/*
 * scan/runlog.h —— 长时间稳定性调查用的异常日志 (EtherCAT 总线 + USB 功率计)。
 *
 * **它要回答的是"跑了十几个小时之后, 什么时候开始不对的"**, 所以它记两样东西:
 *   · 出事那一刻的结构化现场 (谁, 什么判据, 当时的数是多少);
 *   · 每隔若干秒一行的健康摘要 —— 因为最常见的坏法不是"爆了一个错", 而是**什么错都没爆,
 *     节拍却在慢慢变差**。
 *
 * 三条硬规矩 (改动这个文件之前先读):
 *
 * 1. **生产者不写盘。** `line()` 可以由任何线程调 (总线的 2 ms 循环、功率计采集线程、
 *    界面线程), 它只取时间戳、拼一行、进内存队列就返回。`QFile` 只在 `drain()` / `close()`
 *    里被碰, 而那两个**只许界面线程调**。理由: 仓库文档记过 30 分钟跑下来帧节拍劣化的案例,
 *    往 2 ms 的控制循环里塞一次可能被磁盘 (或杀毒) 拖住上百毫秒的写盘, 是拿被测对象换日志。
 *
 * 2. **队列有界。** 超过 kRunLogMaxPendingBytes 就丢**最旧**的整行并累加 dropped ——
 *    生产者永不阻塞、内存不会涨爆。丢了多少会出现在之后每一条摘要的 `dropped=` 里,
 *    所以"丢过"这件事不会被静默掉。
 *
 * 3. **失败不抛、不刷屏。** 与 ScanLog 同一条约定: `bool` + `QString *err`, 不抛异常。
 *    打不开就报一次红横幅, 之后静默丢弃 (计数照加)。
 *
 * 行格式 (机器解析到第 4 段为止, 自由文本是最后一段, 可含中文):
 *
 *     <ts> | <TAG> | <event> | <k=v k=v ...> | <自由文本(可选)>
 *     2026-10-15 14:05:11.004 | METER | meter_stale | n=0 wm=88421 kind=empty | 功率计 1800 ms 未出新数据
 *
 *   · ts    定宽 `yyyy-MM-dd HH:mm:ss.zzz`, 字典序 = 时间序;
 *   · TAG   ∈ LIFE | ECAT | METER;
 *   · event 稳定的小写下划线词 (不中英混写, 便于 grep 与画图);
 *   · 字段区空格分隔的 k=v, v 是纯 ASCII、无空格无 `|`, 未知写 `-`; 一个字段都没有时写 `-`;
 *   · 自由文本**必须由调用方**过 runlogSafe() 折掉 `|` 与换行, 保证前四段的结构不被撑破。
 *     (line() 自己**不能**替你折 —— 它拿到的是整条 body, 而 body 里那两个 ` | ` 正是
 *      结构本身。要折只能是写自由文本的那一处折, 所以这一条是调用方的责任。)
 *
 * 文件名固定、只追加 (用户的选择)。**不生成第二个文件** —— 长得太大时裁头保尾,
 * 见 runlogTrimHead()。
 */
#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <cstdint>

class QFile;

namespace scan {

/* 队列上限 (字节)。到顶丢最旧, 生产者永不阻塞 */
const int kRunLogMaxPendingBytes = 4 * 1024 * 1024;

/* 文件上限与裁到多少。**必须 keep < max**, 否则裁完还是超 */
const qint64 kRunLogMaxBytes  = 64 * 1024 * 1024;
const qint64 kRunLogKeepBytes = 48 * 1024 * 1024;

/* 自由文本里不许出现 `|` 与换行 (它们会把前四段的结构撑破), 一律换成 `/`。
 * 纯函数, 自检直接钉。 */
QString runlogSafe(const QString &s);

/* 把一行拆回 (ts, TAG, event, 字段表, 自由文本)。`#` 开头的内务行与空行 -> false。
 *
 * 字段区拆成 k=v; 值为 `-` 或整个字段区是 `-` 时, 对应的键不进表 (所以 `meter_req_closed | -`
 * 拆出来是一张空表, 而不是一个键为 "-" 的怪项)。
 *
 * 它存在的理由与 runlogSafe 是同一个: 让自检能"写进去再读回来对一遍", 而不是靠肉眼看格式。 */
bool splitRunLogLine(const QString &line, QString *ts, QString *tag, QString *event,
                     QMap<QString, QString> *fields, QString *free_text);

/* 文件超过 max_bytes 时, 丢掉最旧的头部、只保留尾部 keep_bytes 左右。
 *
 * **文件名不变、仍然是同一个追加文件** (用户选的就是这个): 裁的时候从
 * `size - keep_bytes` 处往后找第一个换行 (免得留半行), 从那里开始留, 并在最前面插一行
 * `# trimmed …` 标记 —— 所以"丢过历史"这件事在文件里看得见, 不是静默的。
 *
 * 没超上限就是个空动作 (返回 true)。不抛, 失败返回 false + err。 */
bool runlogTrimHead(const QString &path, qint64 max_bytes, qint64 keep_bytes, QString *err);

class RunLog
{
public:
   RunLog() = default;
   ~RunLog();

   RunLog(const RunLog &)            = delete;
   RunLog &operator=(const RunLog &) = delete;

   /* 打开 (固定名, 追加) 并写一段启动现场。父目录不存在会先 mkpath。
    *
    * meta 是启动那几行裸 "key=value" (exe / 版本 / 网卡 / 设备序列号 …), 由这里加 `# ` 前缀。
    * 换一趟数据回头复核时靠的就是它。文件是新建的还会先写一行格式说明。 */
   bool open(const QString &path, const QStringList &meta, QString *err);

   /* 排空队列 + flush + 关闭。之后再来的 line() 静默丢弃 (计数照加) */
   void close();

   /* **任何线程**都能调: 取时间戳、拼一行、入队。不碰磁盘、不抛、不阻塞。
    * body 形如 "event | k=v k=v" (自由文本前自己过 runlogSafe) */
   void line(const QString &tag, const QString &body);

   /* **只许界面线程调**: 把队列整段换出来写盘 + flush, 顺带按上限裁头。
    * 返回写出的行数。失败返回 -1 并把原因留在 lastError() (不弹窗、不刷屏) */
   int drain(QString *err = nullptr);

   quint64 dropped() const;          /* 因背压或未打开而丢的行数 */
   int     pendingBytes() const;
   bool    isOpen() const;
   QString path() const;
   QString lastError() const;

private:
   /* m_open / m_pending / m_pending_bytes / m_dropped 全在 m_mx 下 —— 生产者是多线程的。
    * m_f 只有 drain()/close() 碰 (界面线程), 所以它不进锁。 */
   mutable QMutex      m_mx;
   QList<QByteArray>   m_pending;
   int                 m_pending_bytes = 0;
   quint64             m_dropped       = 0;
   bool                m_open          = false;

   QFile  *m_f = nullptr;            /* 界面线程独占 */
   QString m_path;
   QString m_err;
};

}   /* namespace scan */
