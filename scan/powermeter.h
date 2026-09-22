/*
 * scan/powermeter.h —— 功率计接口 + 三个模拟实现: ManualMeter (现填一个数) / ScriptMeter
 * (文本文件每行一个数, 每次请求取下一行) / RandomMeter (基值 + 噪声), 后两者可配延迟。
 * 真机协议待定, 定下来后加一个 PowerMeter 子类即可; 接口是异步的 (请求 + 信号):
 * 真机一次往返可能几百毫秒, 同步 read() 会把界面冻住。
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace scan {

class PowerMeter : public QObject
{
   Q_OBJECT

public:
   explicit PowerMeter(QObject *parent = nullptr) : QObject(parent) {}
   ~PowerMeter() override = default;

   virtual QString kind() const = 0;

   /* 这个源的**标识**, 纯 ASCII, 进 CSV 的 `#` 行 ("manual" / "random" / "script" / "ophir")。
    * 不拿 kind() 顶替: 那个是给人看的中文字, 而 CSV 那些 `#` 行一直全是 ASCII */
   virtual QString tag() const = 0;

   /* 读数是什么单位。三个模拟源按定义就是 W (CSV 那一列本来就叫 watts); 真机不是 ——
    * Ophir 那份数据数组是 W 还是 J 由**探头与测量模式**定, 而 COM 一个字段都不给,
    * 所以 ophirmeter.cpp 从设备自己的字里判 (见 unitFromDeviceInfo)。判不出来返回空 =
    * **不猜**: 界面照原样写「单位不明」, 也不许哪一个地方替它写一个 W。
    * 拿它显示一律走 unitLabel()。 */
   virtual QString unit() const { return QStringLiteral("W"); }

   /* 这个源**现在是什么配置** (设备型号 / 探头 / 波长 / 量程 / 模式 …), 一行一条 key=value。
    * 由 meterMetaLines() 收进两份 CSV 的 `#` 行。空 = 这个源没什么可记的 */
   virtual QStringList configLines() const { return QStringList(); }

   virtual bool open(QString *err)   = 0;
   virtual void close()              = 0;

   /* 恰好会回一次 readingReady 或 readingFailed (排队投递), 非阻塞。
    * 同一时刻只允许一个未决请求 (调用方有两个: ScanController 与界面「读一次」按钮,
    * 靠 ScanWindow 里那条闸协调)。违约不报错, 只会静默分错数或让一边白等到超时。 */
   virtual void requestReading() = 0;

   /* 扫描前的 Preflight 要问这一句: 功率计没开就不许开始 */
   bool isOpen() const { return m_open; }

signals:
   void readingReady (double watts);
   void readingFailed(const QString &err);

protected:
   /* 开关状态放基类, 三个模拟源共用 */
   bool m_open = false;
};

class ManualMeter : public PowerMeter
{
   Q_OBJECT

public:
   explicit ManualMeter(QObject *parent = nullptr) : PowerMeter(parent) {}

   QString kind() const override { return QStringLiteral("手动输入"); }
   QString tag()  const override { return QStringLiteral("manual"); }

   bool open(QString *) override { m_open = true; return true; }
   void close() override         { m_open = false; }

   void requestReading() override;

   void setValue(double v) { m_value = v; }
   double value() const    { return m_value; }

private:
   double m_value = 1.0;
};

class RandomMeter : public PowerMeter
{
   Q_OBJECT

public:
   explicit RandomMeter(QObject *parent = nullptr);

   QString kind() const override { return QStringLiteral("随机(噪声)"); }
   QString tag()  const override { return QStringLiteral("random"); }

   bool open(QString *) override { m_open = true; return true; }
   void close() override         { m_open = false; }

   void requestReading() override;

   void setBase(double v)          { m_base = v; }
   void setNoise(double v)         { m_noise = v; }
   void setDelayMs(int ms)         { m_delay_ms = ms; }

   /* 只给界面取缺省值用 (那几个旋钮的初值从源这里取, 别在两处各写一遍 0.05 / 20ms) */
   double base() const    { return m_base; }
   double noise() const   { return m_noise; }
   int    delayMs() const { return m_delay_ms; }

private:
   double m_base     = 1.0;
   double m_noise    = 0.05;
   int    m_delay_ms = 20;
};

class ScriptMeter : public PowerMeter
{
   Q_OBJECT

public:
   explicit ScriptMeter(QObject *parent = nullptr);

   QString kind() const override { return QStringLiteral("脚本文件"); }
   QString tag()  const override { return QStringLiteral("script"); }

   bool open(QString *err) override;
   void close() override;

   void requestReading() override;

   /* 每行一个数; `#` 开头与空行忽略。取完一轮后从头再来(续扫/重测会重复请求同一个点) */
   bool setPath(const QString &path, QString *err);
   const QString &path() const { return m_path; }

   void setDelayMs(int ms) { m_delay_ms = ms; }

   int  count() const { return (int)m_values.size(); }
   int  cursor() const { return m_cursor; }
   int  delayMs() const { return m_delay_ms; }

private:
   QString             m_path;
   QVector<double>     m_values;
   int                 m_cursor   = 0;
   int                 m_delay_ms = 20;
};

/* ---- 两份 CSV 与界面共用的小工具 ----
 * 只有一份实现: 扫描那份 CSV (scanlog) 与连续读数那份 (meterlog) 都从这两个函数取;
 * 各写一遍的话, 迟早一份记一份不记 */
QStringList meterMetaLines(const PowerMeter *m);   /* 裸 "key=value", 由写文件的那方加 "# " */
QString     unitLabel(const PowerMeter *m);        /* 空 -> 「单位不明」 */

}   /* namespace scan */
