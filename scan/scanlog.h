/*
 * scan/scanlog.h —— CSV 落盘与读回 (格式与解析在 scanplan.cpp)。CSV 本身就是进度文件,
 * 不另存状态文件: 每采一点追加一行 + flush (崩了/断电不丢), 断点续扫读回已有的 (ix,iy)
 * 并继续追加同一个文件, 单点重测再追加一行 (flags=retest); 热力图与统计取该格最后一行。
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <string>
#include <vector>

#include "scanplan.h"

class QFile;

namespace scan {

class ScanLog
{
public:
   ScanLog() = default;
   ~ScanLog();

   ScanLog(const ScanLog &) = delete;
   ScanLog &operator=(const ScanLog &) = delete;

   /* 新建: 写 meta 行 + 列名行。父目录不存在会先建出来。
    *
    * extra_meta 是裸 "key=value" 的几行 (meterMetaLines() 的产出, 见 powermeter.h),
    * 由这里加 "# " 前缀 —— 与连续读数那份 CSV (meterlog) 同一个格式, 两份文件的头长得一样。
    * 它记的是"这趟是哪台仪器、什么探头、什么波长/量程/模式采的": 换一趟数据回头复核时,
    * 靠的就是它。
    *
    * **只在新建时写**: 续写的那一份表头属于它开头那一趟, 不重写 (理由同 ScanLog::beginAppend
    * 的注释)。代价是续写之后如果换了波长/量程/模式, 文件里那几行就说的是**第一趟**的配置
    * —— 所以续写前换过配置的话, 该另起一个文件。 */
   bool beginNew(const QString &path, const Params &p,
                 const QString &started_iso, int zero_epoch,
                 const QStringList &extra_meta, QString *err);

   /* 续写: 文件必须已存在, 不重写表头 (表头记的是那一轮的几何参数) */
   bool beginAppend(const QString &path, QString *err);

   /* 追加一行并立刻 flush。不抛, 失败时返回 false 并把原因留在 lastError() */
   bool append(const Row &r);

   void close();
   bool isOpen() const;

   QString path() const      { return m_path; }
   QString lastError() const { return m_err; }

   /* 已经写进去的行数 (含失败的点和重测) */
   int written() const { return m_written; }

private:
   /* 续写前把没写完的那半行切掉 */
   bool trimTail(const QString &path, QString *err);

   QFile  *m_f = nullptr;
   QString m_path;
   QString m_err;
   int     m_written = 0;
};

/* 把一个 CSV 整个读进内存。只做文件 I/O, 不解析 (解析在 scanplan.cpp);
 * 追加写掉电留下的残行按正常情况处理。 */
bool readCsvText(const QString &path, std::string *text, QString *err);

}   /* namespace scan */
