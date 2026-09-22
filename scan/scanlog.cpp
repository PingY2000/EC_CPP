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
                       const QString &started_iso, int zero_epoch,
                       const QStringList &extra_meta, QString *err)
{
   close();

   QFileInfo fi(path);
   if (!fi.absoluteDir().exists())
   {
      if (!QDir().mkpath(fi.absolutePath()))
      {
         m_err = QStringLiteral("新建目录失败: %1").arg(fi.absolutePath());
         if (err) *err = m_err;
         return false;
      }
   }

   m_f = new QFile(path);
   if (!m_f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
   {
      m_err = QStringLiteral("打开写入文件失败: %1").arg(m_f->errorString());
      delete m_f;
      m_f = nullptr;
      if (err) *err = m_err;
      return false;
   }

   m_path    = path;
   m_written = 0;

   std::string head = csvMetaLines(p, started_iso.toStdString(), zero_epoch);

   /* 仪器那几行 (探头/波长/量程/模式/单位 …)。加了前缀就是 `#` 注释, 而读回时只认那几个
    * 几何 key (scanplan.cpp 的 metaGet), 多几行不影响续扫的兼容性判定 —— 自检里有这一条 */
   for (const QString &l : extra_meta)
   {
      head += "# ";
      head += l.toStdString();
      head += "\n";
   }

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
      m_err = QStringLiteral("续写目标文件不存在: %1").arg(path);
      if (err) *err = m_err;
      return false;
   }

   if (!trimTail(path, err))
      return false;

   m_f = new QFile(path);
   if (!m_f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
   {
      m_err = QStringLiteral("打开续写文件失败: %1").arg(m_f->errorString());
      delete m_f;
      m_f = nullptr;
      if (err) *err = m_err;
      return false;
   }

   m_path = path;
   return true;
}

/* 把最后那半行切掉 (文件末尾没有换行 = 掉电时写了一半)。不切的话续写的下一个
 * 数据行会粘在它后面, 字段错位, 那一行以后的数据全废。 */
bool ScanLog::trimTail(const QString &path, QString *err)
{
   QFile probe(path);
   if (!probe.open(QIODevice::ReadOnly))
   {
      m_err = QStringLiteral("读取 %1 失败: %2").arg(path, probe.errorString());
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
         m_err = QStringLiteral("读取 %1 失败: %2").arg(path, r.errorString());
         if (err) *err = m_err;
         return false;
      }
      all = r.readAll();
   }

   const int cut = all.lastIndexOf('\n');   /* -1 = 整个文件都是那半行 */

   QFile w(path);
   if (!w.open(QIODevice::ReadWrite))
   {
      m_err = QStringLiteral("打开 %1 以截断尾部失败: %2").arg(path, w.errorString());
      if (err) *err = m_err;
      return false;
   }
   if (!w.resize(cut + 1))
   {
      m_err = QStringLiteral("截断 %1 失败: %2").arg(path, w.errorString());
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
      m_err = QStringLiteral("当前没有打开的文件");
      return false;
   }

   std::string line = csvRowLine(r);
   if (m_f->write(line.c_str(), (qint64)line.size()) != (qint64)line.size())
   {
      m_err = QStringLiteral("写第 %1 行失败: %2").arg(m_written).arg(m_f->errorString());
      return false;
   }

   /* 每行都 flush: 崩了不丢已采的点 */
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

bool readCsvText(const QString &path, std::string *text, QString *err)
{
   QFile f(path);
   if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
   {
      if (err) *err = QStringLiteral("打开 %1 失败: %2").arg(path, f.errorString());
      return false;
   }

   QByteArray raw = f.readAll();
   f.close();

   if (text != nullptr)
      text->assign(raw.constData(), (size_t)raw.size());
   return true;
}

}   /* namespace scan */
