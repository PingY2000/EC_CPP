#include "scanlog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace scan {

ScanLog::~ScanLog()
{
   close();
}

bool ScanLog::beginNew(const QString &path, const Params &p,
                       const QString &started_iso, int zero_epoch, QString *err)
{
   close();

   QFileInfo fi(path);
   if (!fi.absoluteDir().exists())
   {
      if (!QDir().mkpath(fi.absolutePath()))
      {
         m_err = QStringLiteral("建不出目录: %1").arg(fi.absolutePath());
         if (err) *err = m_err;
         return false;
      }
   }

   m_f = new QFile(path);
   if (!m_f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
   {
      m_err = QStringLiteral("打不开要写的文件: %1").arg(m_f->errorString());
      delete m_f;
      m_f = nullptr;
      if (err) *err = m_err;
      return false;
   }

   m_path    = path;
   m_written = 0;

   std::string head = csvMetaLines(p, started_iso.toStdString(), zero_epoch);
   head += csvColumnHeader();
   head += "\n";

   if (m_f->write(head.c_str(), (qint64)head.size()) != (qint64)head.size())
   {
      m_err = QStringLiteral("写表头失败: %1").arg(m_f->errorString());
      close();
      if (err) *err = m_err;
      return false;
   }
   m_f->flush();
   return true;
}

bool ScanLog::beginAppend(const QString &path, QString *err)
{
   close();

   if (!QFile::exists(path))
   {
      m_err = QStringLiteral("要续写的文件不存在: %1").arg(path);
      if (err) *err = m_err;
      return false;
   }

   if (!trimTail(path, err))
      return false;

   m_f = new QFile(path);
   if (!m_f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
   {
      m_err = QStringLiteral("打不开要续写的文件: %1").arg(m_f->errorString());
      delete m_f;
      m_f = nullptr;
      if (err) *err = m_err;
      return false;
   }

   m_path = path;
   return true;
}

/*
 * 把最后那半行切掉。
 *
 * **这一步是必须的, 不是讲究。** 掉电/被杀的那一刻正好卡在写行的中间, 文件就会
 * 停在一个没有换行的半行上。而续写是纯 append —— 下一个数据行会**粘在这半行后面**,
 * 变成一行字段串了位的垃圾: 网格索引错位, 那一行以后的数据全废。
 *
 * 而且那半行留着也没用: 它本来就是"没写完的那一行"。所以切掉是唯一既安全
 * 又不丢东西的选择 —— 丢的只是一个没写完的字节序列。
 */
bool ScanLog::trimTail(const QString &path, QString *err)
{
   QFile probe(path);
   if (!probe.open(QIODevice::ReadOnly))
   {
      m_err = QStringLiteral("读不了 %1: %2").arg(path, probe.errorString());
      if (err) *err = m_err;
      return false;
   }

   const qint64 sz = probe.size();
   if (sz <= 0)
      return true;

   probe.seek(sz - 1);
   char last = 0;
   if (probe.read(&last, 1) != 1)
   {
      m_err = QStringLiteral("读 %1 的末尾失败").arg(path);
      if (err) *err = m_err;
      return false;
   }
   probe.close();

   if (last == '\n')
      return true;

   QByteArray all;
   {
      QFile r(path);
      if (!r.open(QIODevice::ReadOnly))
      {
         m_err = QStringLiteral("读不了 %1: %2").arg(path, r.errorString());
         if (err) *err = m_err;
         return false;
      }
      all = r.readAll();
   }

   const int cut = all.lastIndexOf('\n');   /* -1 = 整个文件都是那半行 */

   QFile w(path);
   if (!w.open(QIODevice::ReadWrite))
   {
      m_err = QStringLiteral("打不开 %1 去切尾巴: %2").arg(path, w.errorString());
      if (err) *err = m_err;
      return false;
   }
   if (!w.resize(cut + 1))
   {
      m_err = QStringLiteral("切 %1 的尾巴失败: %2").arg(path, w.errorString());
      if (err) *err = m_err;
      return false;
   }
   w.close();
   return true;
}

bool ScanLog::append(const Row &r)
{
   if (m_f == nullptr)
   {
      m_err = QStringLiteral("没有打开的文件");
      return false;
   }

   std::string line = csvRowLine(r);
   if (m_f->write(line.c_str(), (qint64)line.size()) != (qint64)line.size())
   {
      m_err = QStringLiteral("写第 %1 行失败: %2").arg(m_written).arg(m_f->errorString());
      return false;
   }

   /* **每行都 flush**。整个设计就靠这一条: 采到一点就是一点, 崩了不丢 */
   m_f->flush();
   m_written++;
   return true;
}

void ScanLog::close()
{
   if (m_f != nullptr)
   {
      m_f->flush();
      m_f->close();
      delete m_f;
      m_f = nullptr;
   }
}

bool ScanLog::isOpen() const
{
   return m_f != nullptr && m_f->isOpen();
}

/* ---------------------------------------------------------------- 读回 */

bool readCsvText(const QString &path, std::string *text, QString *err)
{
   QFile f(path);
   if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
   {
      if (err) *err = QStringLiteral("打不开 %1: %2").arg(path, f.errorString());
      return false;
   }

   QByteArray raw = f.readAll();
   f.close();

   if (text != nullptr)
      text->assign(raw.constData(), (size_t)raw.size());
   return true;
}

}   /* namespace scan */
