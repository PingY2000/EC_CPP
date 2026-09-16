/*
 * scan/powermeter.h —— 功率计接口 + 三个模拟实现
 *
 * **功率计型号待定**, 所以这一版只有接口和模拟源。真机协议定下来之后, 要做的只是
 * 再写一个 PowerMeter 的子类 —— 状态机、界面、CSV 一个字都不用动。
 *
 * 接口是**异步**的 (请求 + 信号), 不是 `double read()`:
 * 真机很可能是串口/SCPI, 一次往返几百毫秒。同步接口会把整个界面冻在那儿 ——
 * 而扫描一次要采几千个点, 那就是几千次卡顿。异步之后, 状态机多一个 READING 状态,
 * 换来的是界面自始至终不卡。
 *
 * 三个模拟实现:
 *   ManualMeter —— 界面上现填一个数, 每次采回来都是它。用来手对流程。
 *   ScriptMeter —— 读一个文本文件, 每行一个数, **每次请求取下一行**。可以配一个模拟延迟。
 *                  这是没有功率计时最有用的一个: 它能让整条流水线(状态机 → CSV →
 *                  热力图 → 断点续扫)完整跑一遍, 而且数据是**可预测、可断言**的。
 *   RandomMeter —— 基值 + 噪声, 同样可配延迟。用来压热力图的配色与刷新。
 *
 * **不做工厂 / 注册表 / 配置文件**: 一个基类 + 三个子类 + 界面上一个下拉框的
 * switch 就是终态。也不要支持并发请求 —— 一个时刻只允许一个未决请求,
 * 状态机才能有确定的 WAIT_READING。
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

   /* **恰好**会回一次 readingReady 或 readingFailed (排队投递), 非阻塞。
    * 调用方负责保证同一时刻只有一个未决请求。 */
   virtual void requestReading() = 0;

   /* 扫描前的 Preflight 要问这一句: 功率计没开就不许开始 */
   bool isOpen() const { return m_open; }

signals:
   void readingReady (double watts);
   void readingFailed(const QString &err);

protected:
   /* 三个模拟源都是"设个值就完了"的, 开关状态放基类, 免得抄三份 */
   bool m_open = false;
};

/* ---------------------------------------------------------------- 手动 */

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

/* ---------------------------------------------------------------- 随机 */

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

/* ---------------------------------------------------------------- 脚本 */

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
