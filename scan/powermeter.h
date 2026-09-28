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

   QString kind() const override { return QStringLiteral("随机 (噪声)"); }
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

/* ---- 屏幕上的量级换算 (W / mW / μW) ----
 *
 * **只动屏幕, 不动文件。** CSV 那一列与 `# meter_unit=` 照旧是设备报的原值与原单位 ——
 * 读文件的人要能直接解析, 而且一条曲线中途换单位就是一条会骗人的线。
 * 于是**屏幕上显示的单位可能与 CSV 里的 meter_unit 不同**, 这是定下来的口径 (§37)。
 *
 * **只在单位确实是 "W" 时才换。** 判出 J (能量)、dBm (对数)、空 (单位不明) 一律原样:
 * 除 1000 会把 0 dBm 变成 −30, 那是错的; 「单位不明」更不该被加上一个前缀。 */
struct PowerText
{
   double  value = 0.0;      /* 换算后的数值 */
   QString label;            /* "W" / "mW" / "μW" / 原单位 */
};

/* 单个数的量级。|v| >= 1 -> W;  >= 1e-3 -> mW;  更小 -> μW。
 * **v == 0 用 W** (屏幕上写 "0 W", 不写 "0.00 μW"); 负值按 |v| 选档, 符号留着 */
PowerText scalePower(double v, const QString &unit);

/* 一整行的量级: 按这一行里最大的那个 |值| 选一个前缀, **整行共用一个** ——
 * 否则 "最小 0.5 mW  最大 1.5 mW" 这种一行两个单位的读法更难看懂。
 * 返回单位字, 把除数写进 *divisor (调用方自己拿各个数去除: 0.0015 W 除以 1e-3 得 1.5 mW)。
 * 单位不是 "W" 时单位字原样返回、*divisor 写 1 —— 空单位也是 (调用方不必分两种写法) */
QString scaleFor(double max_abs, const QString &unit, double *divisor);

/* 一个数 + 单位, 一次拼好: "1.5 W" / "1.50 mW" / "350 μW" / "单位不明"。
 * 单位空时数值**原样**格式化 (不乘不除) */
QString powerText(double v, const QString &unit);

/* 一个数的显示格式 (量级从 nW 到 W 那一带, 不固定小数位):
 *   **v == 0 -> "0"**;  |v| >= 1e-3 -> 'g' 6 位有效数字;  更小 -> 'e' 4 位有效数字。
 * 零单独写掉: 它**没有量级**, 写 "0" 而不是 "0.0000e+00" —— 挡住光的时候屏幕上正是一片 0,
 * 而 v == 0.0 对 -0.0 也成立, 于是也不会出现 "-0.0000e+00"。
 * **只格式化数字, 不带单位**。给操作员核对用, 入 CSV 的是 double, 一位没少。
 * 「最近读数」与「统计」两处共用它; 统计那一行要四个数共用一个前缀, 所以它单独露出来 */
QString formatReading(double v);

}   /* namespace scan */
