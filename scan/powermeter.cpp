#include "powermeter.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

namespace scan {

/* 三个模拟实现都用 QTimer::singleShot 把 emit 挪出调用栈: 直接 emit 会让状态机的槽在
 * requestReading() 内部嵌套执行; 延时投递后与真机 (串口回来才 emit) 时序一致。 */

void ManualMeter::requestReading()
{
   if (!m_open)
   {
      emit readingFailed(QStringLiteral("功率计没打开"));
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
      emit readingFailed(QStringLiteral("功率计没打开"));
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
      if (err) *err = QStringLiteral("打不开脚本文件: %1").arg(path);
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
      if (err) *err = QStringLiteral("脚本文件里没有一个数字: %1").arg(path);
      return false;
   }
   return true;
}

bool ScriptMeter::open(QString *err)
{
   if (m_path.isEmpty())
   {
      if (err) *err = QStringLiteral("还没选脚本文件");
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
      emit readingFailed(QStringLiteral("功率计没打开"));
      return;
   }
   if (m_values.isEmpty())
   {
      emit readingFailed(QStringLiteral("脚本文件里没有值"));
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
