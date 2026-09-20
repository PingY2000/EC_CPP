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

   bool open(QString *) override { m_open = true; return true; }
   void close() override         { m_open = false; }

   void requestReading() override;

   void setBase(double v)          { m_base = v; }
   void setNoise(double v)         { m_noise = v; }
   void setDelayMs(int ms)         { m_delay_ms = ms; }

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

   bool open(QString *err) override;
   void close() override;

   void requestReading() override;

   /* 每行一个数; `#` 开头与空行忽略。取完一轮后从头再来(续扫/重测会重复请求同一个点) */
   bool setPath(const QString &path, QString *err);
   const QString &path() const { return m_path; }

   void setDelayMs(int ms) { m_delay_ms = ms; }

   int  count() const { return (int)m_values.size(); }
   int  cursor() const { return m_cursor; }

private:
   QString             m_path;
   QVector<double>     m_values;
   int                 m_cursor   = 0;
   int                 m_delay_ms = 20;
};

}   /* namespace scan */
