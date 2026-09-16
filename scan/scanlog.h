/*
 * scan/scanlog.h —— CSV 落盘与读回
 *
 * **CSV 本身就是进度文件**, 不另存 sidecar 状态文件。
 * 边采边写的数据行里已经有全部信息, 再维护一份"扫到哪了"就是两份真相, 迟早对不上。
 *
 * 三个后果, 都是想要的:
 *   · 每采一点追加一行 + flush ⇒ 中途崩了/断电了, 已采的数据一行不丢
 *   · 断点续扫 = 读回已有的 (ix,iy) 集合, 剩下没采的补扫, **继续往同一个文件追加**
 *   · 单点重测 = 再追加一行 (同 index, flags=retest); 热力图与统计一律取该格最后一行
 *
 * 格式与解析在 scanplan.cpp (纯逻辑, 可单测); 这里只负责文件 I/O。
 */
#pragma once

#include <QString>
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

   /* 新建: 写 meta 两行 + 列名行。父目录不存在会先建出来。 */
   bool beginNew(const QString &path, const Params &p,
                 const QString &started_iso, int zero_epoch, QString *err);

   /* 续写: 文件必须已经存在, **不重写表头** (表头里的几何参数是那一轮的事实) */
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
   /* 续写前把没写完的那半行切掉。见 scanlog.cpp 里的理由 */
   bool trimTail(const QString &path, QString *err);

   QFile  *m_f = nullptr;
   QString m_path;
   QString m_err;
   int     m_written = 0;
};

/*
 * 把一个 CSV 整个读进内存。**只做文件 I/O, 不解析** ——
 * 解析全在 scanplan.cpp 里 (纯逻辑, 能脱离 Qt 单测), 这里再包一层解析函数
 * 只会多一条没人单测的路径。
 *
 * 几千行一次读完最省事, 也免得自己处理跨块的半行 —— 追加写掉电留下的残行是**正常情况**,
 * 不是异常情况。
 */
bool readCsvText(const QString &path, std::string *text, QString *err);

}   /* namespace scan */
