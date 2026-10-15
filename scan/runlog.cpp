#include "runlog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

namespace scan {

namespace {

const char *kTsFormat = "yyyy-MM-dd HH:mm:ss.zzz";

QString nowStamp()
{
   return QDateTime::currentDateTime().toString(QString::fromLatin1(kTsFormat));
}

}   /* namespace */

QString runlogSafe(const QString &s)
{
   QString out = s;
   out.replace(QLatin1Char('|'), QLatin1Char('/'));
   out.replace(QLatin1Char('\r'), QLatin1Char(' '));
   out.replace(QLatin1Char('\n'), QLatin1Char(' '));
   return out;
}

bool splitRunLogLine(const QString &line, QString *ts, QString *tag, QString *event,
                     QMap<QString, QString> *fields, QString *free_text)
{
   /* `#` 开头的是内务行 (启动分隔、格式说明、裁剪标记), 不是日志行 */
   if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      return false;

   /* 自由文本已由 runlogSafe 折掉 `|`, 所以这里最多切出 5 段 */
   const QStringList parts = line.split(QStringLiteral(" | "));
   if (parts.size() < 4)
      return false;

   if (ts != nullptr)    *ts = parts.at(0);
   if (tag != nullptr)   *tag = parts.at(1);
   if (event != nullptr) *event = parts.at(2);
   if (free_text != nullptr)
      *free_text = parts.size() >= 5 ? parts.mid(4).join(QStringLiteral(" | ")) : QString();

   if (fields != nullptr)
   {
      fields->clear();
      const QString f = parts.at(3).trimmed();
      if (f != QLatin1String("-"))
      {
         const QStringList kv = f.split(QLatin1Char(' '), Qt::SkipEmptyParts);
         for (const QString &item : kv)
         {
            const int eq = item.indexOf(QLatin1Char('='));
            if (eq <= 0)
               continue;
            fields->insert(item.left(eq), item.mid(eq + 1));
         }
      }
   }
   return true;
}

bool runlogTrimHead(const QString &path, qint64 max_bytes, qint64 keep_bytes, QString *err)
{
   /* 参数不成立就当"不用裁"。**故意不报错**: 一个配置写错不该把日志本身弄丢 */
   if (max_bytes <= 0 || keep_bytes <= 0 || keep_bytes >= max_bytes)
      return true;

   QFile probe(path);
   if (!probe.open(QIODevice::ReadOnly))
   {
      if (err) *err = QStringLiteral("读取 %1 失败: %2").arg(path, probe.errorString());
      return false;
   }

   const qint64 sz = probe.size();
   if (sz <= max_bytes)
   {
      probe.close();
      return true;
   }

   qint64 start = sz - keep_bytes;
   if (start < 0)
      start = 0;
   if (!probe.seek(start))
   {
      if (err) *err = QStringLiteral("定位 %1 的尾部失败: %2").arg(path, probe.errorString());
      return false;
   }
   QByteArray tail = probe.readAll();
   probe.close();

   /* 从一个换行**之后**开始留 —— 半行留在头上, 后面每一行的结构都会错位 */
   const int nl = tail.indexOf('\n');
   if (nl < 0)
   {
      /* keep 比最长的一行还小, 整段里一个换行都没有。**宁可这次不裁**: 裁下去文件就剩
       * 半行, 比"暂时超一点上限"坏得多 */
      return true;
   }
   tail = tail.mid(nl + 1);

   const qint64 dropped = sz - (qint64)tail.size();

   QFile w(path);
   if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
   {
      if (err) *err = QStringLiteral("重写 %1 失败: %2").arg(path, w.errorString());
      return false;
   }

   /* 丢过历史就说出来, 别静默 */
   const QByteArray marker =
      QStringLiteral("# trimmed %1 B at %2\n").arg(dropped).arg(nowStamp()).toUtf8();

   if (w.write(marker) != (qint64)marker.size() || w.write(tail) != (qint64)tail.size())
   {
      if (err) *err = QStringLiteral("写回 %1 失败: %2").arg(path, w.errorString());
      return false;
   }
   w.flush();
   w.close();
   return true;
}

/* ------------------------------------------------------------------ RunLog */

RunLog::~RunLog()
{
   close();
}

bool RunLog::open(const QString &path, const QStringList &meta, QString *err)
{
   close();

   QFileInfo fi(path);
   if (!fi.absoluteDir().exists())
   {
      if (!QDir().mkpath(fi.absolutePath()))
      {
         m_err = QStringLiteral("新建日志目录失败: %1").arg(fi.absolutePath());
         if (err) *err = m_err;
         return false;
      }
   }

   /* 新建的空文件才写格式说明: 判断要**在打开之前**做, 打开之后 size 就变了 */
   const bool fresh = fi.size() <= 0;

   m_f = new QFile(path);
   if (!m_f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
   {
      m_err = QStringLiteral("打开日志文件失败: %1").arg(m_f->errorString());
      delete m_f;
      m_f = nullptr;
      if (err) *err = m_err;
      return false;
   }

   m_path = path;

   QByteArray head;
   if (fresh)
   {
      head += "# scan 异常日志。格式: <ts> | <TAG> | <event> | <k=v ...> | <自由文本>\n";
      head += "# TAG = LIFE | ECAT | METER; 字段区一个都没有时写 `-`; `#` 开头的是内务行。\n";
   }
   head += QStringLiteral("# ===== %1 启动 (追加) =====\n").arg(nowStamp()).toUtf8();
   for (const QString &m : meta)
      head += QStringLiteral("# %1\n").arg(m).toUtf8();

   if (m_f->write(head) != (qint64)head.size())
   {
      m_err = QStringLiteral("写日志文件头失败: %1").arg(m_f->errorString());
      close();
      if (err) *err = m_err;
      return false;
   }
   m_f->flush();

   {
      QMutexLocker<QMutex> lk(&m_mx);
      m_open = true;
   }
   return true;
}

void RunLog::close()
{
   /* 先把"不再收新行"落下, 再把**已经排进来的**写出去 —— 顺序反过来会丢掉收尾那几行
    * (关窗时 meter_stop / ECAT stop 正是在这一刻排进来的) */
   {
      QMutexLocker<QMutex> lk(&m_mx);
      m_open = false;
   }

   drain(nullptr);

   if (m_f != nullptr)
   {
      m_f->flush();
      m_f->close();
      delete m_f;
      m_f = nullptr;
   }
}

void RunLog::line(const QString &tag, const QString &body)
{
   QByteArray row = (nowStamp() + QStringLiteral(" | ") + tag + QStringLiteral(" | ") + body +
                    QLatin1Char('\n')).toUtf8();

   QMutexLocker<QMutex> lk(&m_mx);

   if (!m_open)
   {
      m_dropped++;
      return;
   }

   /* 背压: 丢**最旧**的整行, 直到装得下。丢整行是关键 —— 半行留在队里会让后面
    * 每一行的结构都错位。生产者永不阻塞, 代价只是最老的那几行 */
   if (m_pending_bytes + row.size() > kRunLogMaxPendingBytes)
   {
      const int excess = m_pending_bytes + row.size() - kRunLogMaxPendingBytes;
      int freed = 0;
      while (!m_pending.isEmpty() && freed < excess)
      {
         const int n = m_pending.first().size();
         freed += n;
         m_pending_bytes -= n;
         m_pending.removeFirst();
         m_dropped++;
      }
   }

   m_pending_bytes += row.size();
   m_pending.append(row);
}

int RunLog::drain(QString *err)
{
   QList<QByteArray> take;
   {
      QMutexLocker<QMutex> lk(&m_mx);
      take.swap(m_pending);
      m_pending_bytes = 0;
   }

   if (take.isEmpty())
      return 0;

   if (m_f == nullptr || !m_f->isOpen())
   {
      QMutexLocker<QMutex> lk(&m_mx);
      m_dropped += (quint64)take.size();
      return 0;
   }

   int n = 0;
   for (const QByteArray &row : take)
   {
      if (m_f->write(row) != (qint64)row.size())
      {
         m_err = QStringLiteral("写日志失败: %1").arg(m_f->errorString());
         if (err) *err = m_err;
         /* 没写出去的那些如实记成"丢了", 别让它们凭空消失 */
         QMutexLocker<QMutex> lk(&m_mx);
         m_dropped += (quint64)(take.size() - n);
         return -1;
      }
      n++;
   }

   m_f->flush();

   /* 裁头放在最后: 此刻队列已排空、而且只有界面线程在写这个文件, 没有并发写者 */
   if (m_f->size() > kRunLogMaxBytes)
   {
      const QString p = m_path;
      m_f->flush();
      m_f->close();
      delete m_f;
      m_f = nullptr;

      QString terr;
      if (!runlogTrimHead(p, kRunLogMaxBytes, kRunLogKeepBytes, &terr))
      {
         m_err = terr;
         if (err) *err = m_err;
      }

      m_f = new QFile(p);
      if (!m_f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
      {
         m_err = QStringLiteral("裁剪后重新打开日志失败: %1").arg(m_f->errorString());
         delete m_f;
         m_f = nullptr;
         if (err) *err = m_err;
         return n;
      }
   }

   return n;
}

quint64 RunLog::dropped() const
{
   QMutexLocker<QMutex> lk(&m_mx);
   return m_dropped;
}

int RunLog::pendingBytes() const
{
   QMutexLocker<QMutex> lk(&m_mx);
   return m_pending_bytes;
}

bool RunLog::isOpen() const
{
   QMutexLocker<QMutex> lk(&m_mx);
   return m_open;
}

QString RunLog::path() const
{
   return m_path;
}

QString RunLog::lastError() const
{
   return m_err;
}

}   /* namespace scan */
