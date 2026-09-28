#include "powermeter.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

#include <cmath>

namespace scan {

QStringList meterMetaLines(const PowerMeter *m)
{
   QStringList out;
   if (m == nullptr)
      return out;

   /* tag() 是 ASCII 的 (见 powermeter.h): 这两行会原样进 CSV, 而 CSV 的 `#` 行一直全是
    * ASCII —— 中文字进去, 读的人就得先猜编码 */
   out << QStringLiteral("meter_source=%1").arg(m->tag());

   /* 单位**一定记**: 那一列叫 watts, 而真机报的可能是 J。文件格式一个字节都没改
    * (老文件照旧读得进来), 但新写的文件里有一行说得清那一列到底是什么 */
   out << QStringLiteral("meter_unit=%1").arg(m->unit().isEmpty()
                                                 ? QStringLiteral("unknown")
                                                 : m->unit());
   out += m->configLines();
   return out;
}

QString unitLabel(const PowerMeter *m)
{
   if (m == nullptr || m->unit().isEmpty())
      return QStringLiteral("单位不明");
   return m->unit();
}

/* ---------------------------------------------------------------- 屏幕上的量级换算 */

/* 档位: |v| >= 1 -> W,  >= 1e-3 -> mW,  更小 -> μW。
 * 两个界都用 1 的幂, 于是 0.001 W 显示 1 mW、0.999 W 显示 999 mW 而不是 0.999 W ——
 * **同一量级里口径一致**比"尽量短"要紧。v == 0 归 W (屏幕写 "0 W")。
 *
 * `*divisor` 是**除数** (0.0015 W 除掉 1e-3 得 1.5 mW), 与 powermeter.h / metercurve.h
 * 那两句"调用方拿各个数去除"是同一个约定 —— 写成 1e3 再让调用方乘的话, 三处的读法就分了岔。
 *
 * 唯一的前置条件: 单位**确实是 "W"**。这不是一句防御性代码, 是这个换算的**全部依据** ——
 * J 是能量、dBm 是对数、空是"不知道", 三种都不许乘除 (见 powermeter.h)。 */
static void pickPrefix(double v, double *divisor, QString *label)
{
   const double a = std::fabs(v);

   *divisor = 1.0;
   *label   = QStringLiteral("W");
   if (v == 0.0)
      return;
   if (a < 1e-3)
   {
      *divisor = 1e-6;
      *label   = QStringLiteral("μW");
   }
   else if (a < 1.0)
   {
      *divisor = 1e-3;
      *label   = QStringLiteral("mW");
   }
}

PowerText scalePower(double v, const QString &unit)
{
   PowerText out;
   out.value = v;
   out.label = unit;

   if (unit != QLatin1String("W"))
      return out;                       /* J / dBm / 空: 原样 */

   double  div = 1.0;
   QString lab;
   pickPrefix(v, &div, &lab);
   out.value = v / div;
   out.label = lab;
   return out;
}

QString scaleFor(double max_abs, const QString &unit, double *divisor)
{
   if (divisor != nullptr)
      *divisor = 1.0;
   if (unit != QLatin1String("W"))
      return unit;                      /* 不换前缀: 单位字也原样 (空 -> 空) */

   double  div = 1.0;
   QString lab;
   pickPrefix(max_abs, &div, &lab);
   if (divisor != nullptr)
      *divisor = div;
   return lab;
}

QString formatReading(double v)
{
   /* 0 先单独写掉: 零**没有量级**, 科学计数法在它身上只是噪声 —— 而挡住光的时候屏幕上正是
    * 一片 0 (大字读数、统计那几个数), "0.0000e+00 W" 比 "0 W" 难读得多。
    * v == 0.0 对 -0.0 也成立 (IEEE), 于是不会出现 "-0.0000e+00" 这种写法 */
   if (v == 0.0)
      return QStringLiteral("0");

   if (std::fabs(v) >= 1e-3)
      return QString::number(v, 'g', 6);
   return QString::number(v, 'e', 4);
}

QString powerText(double v, const QString &unit)
{
   if (unit.isEmpty())
      return formatReading(v) + QStringLiteral(" 单位不明");

   const PowerText t = scalePower(v, unit);
   return formatReading(t.value) + QLatin1Char(' ') + t.label;
}

/* 三个模拟实现都用 QTimer::singleShot 把 emit 挪出调用栈: 直接 emit 会让状态机的槽在
 * requestReading() 内部嵌套执行; 延时投递后与真机 (串口回来才 emit) 时序一致。 */

void ManualMeter::requestReading()
{
   if (!m_open)
   {
      emit readingFailed(QStringLiteral("功率计未打开"));
      return;
   }

   double v = m_value;
   QTimer::singleShot(0, this, [this, v] { emit readingReady(v); });
}

RandomMeter::RandomMeter(QObject *parent) : PowerMeter(parent) {}

void RandomMeter::requestReading()
{
   if (!m_open)
   {
      emit readingFailed(QStringLiteral("功率计未打开"));
      return;
   }

   /* 均匀噪声。用 QRandomGenerator: rand() 在多线程里不保证可重入 */
   double r = QRandomGenerator::global()->generateDouble();   /* [0,1) */
   double v = m_base + (r * 2.0 - 1.0) * m_noise;

   int d = m_delay_ms;
   QTimer::singleShot(d, this, [this, v] { emit readingReady(v); });
}

ScriptMeter::ScriptMeter(QObject *parent) : PowerMeter(parent) {}

bool ScriptMeter::setPath(const QString &path, QString *err)
{
   m_values.clear();
   m_cursor = 0;
   m_path   = path;

   if (path.isEmpty())
      return true;

   QFile f(path);
   if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
   {
      if (err) *err = QStringLiteral("无法打开脚本文件: %1").arg(path);
      return false;
   }

   QTextStream in(&f);
   while (!in.atEnd())
   {
      QString line = in.readLine().trimmed();
      if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
         continue;

      bool ok = false;
      double v = line.toDouble(&ok);
      if (!ok)
      {
         if (err)
            *err = QStringLiteral("脚本文件第 %1 行不是数字: %2")
                      .arg(m_values.size() + 1).arg(line);
         m_values.clear();
         return false;
      }
      m_values.append(v);
   }

   if (m_values.isEmpty())
   {
      if (err) *err = QStringLiteral("脚本文件中没有数字: %1").arg(path);
      return false;
   }
   return true;
}

bool ScriptMeter::open(QString *err)
{
   if (m_path.isEmpty())
   {
      if (err) *err = QStringLiteral("未选择脚本文件");
      return false;
   }
   /* 打开时重读一遍: 改完脚本不必重启程序 */
   if (!setPath(m_path, err))
      return false;

   m_open = true;
   return true;
}

void ScriptMeter::close() { m_open = false; }

void ScriptMeter::requestReading()
{
   if (!m_open)
   {
      emit readingFailed(QStringLiteral("功率计未打开"));
      return;
   }
   if (m_values.isEmpty())
   {
      emit readingFailed(QStringLiteral("脚本文件中没有值"));
      return;
   }

   if (m_cursor >= m_values.size())
      m_cursor = 0;                 /* 取完一轮从头来 (重测与续扫会重复请求同一点) */

   double v = m_values[(size_t)m_cursor];
   m_cursor++;

   int d = m_delay_ms;
   QTimer::singleShot(d, this, [this, v] { emit readingReady(v); });
}

}   /* namespace scan */
