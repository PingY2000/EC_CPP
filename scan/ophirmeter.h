/*
 * scan/ophirmeter.h —— PD300R + Juno+ 的功率计, 接到 PowerMeter 接口上
 *
 * 硬件链路: PD300R (光电二极管探头) → Juno+ (USB 表头) → PC → OphirLMMeasurement COM
 *
 * ── 为什么有一个自己的线程 ────────────────────────────────────────────────
 *
 * 两条约束撞在一起:
 *   · 服务端是 **Apartment** 线程模型 (官方手册 "Threading issues"): "it can be only
 *     used directly from the thread it was created in"。所以一个 COM 对象只能在
 *     **创建它的那个线程**里用, 不能今天这儿调一下明天那儿调一下。
 *   · 界面线程不能被 USB 往返堵住。手册的规则 11/12 也明说了别让 UI 线程做采集。
 *
 * 两条加起来只剩一条路: **所有 COM 调用集中在一个专属线程里**, 界面通过原子标志
 * (请求读数) 和信号 (结果) 跟它说话, 一次直接调用都没有。
 *
 * ── open() 为什么要阻塞 ───────────────────────────────────────────────────
 *
 * PowerMeter 的接口就是这么定的 (`bool open(QString *err)`, 调用方拿到返回值就要用),
 * 三个模拟源也是"设一下就好了"。真机做不到 —— 枚举 USB、开设备、读探头、配置、
 * 开流可能要几百毫秒到一两秒。所以这里**阻塞着等**, 但**有上限**: 超时就把线程收掉
 * 并报错, 不让界面永远停在一句 open 里回不来 (USB 挂了的时候那种卡是没法自救的)。
 *
 * ── 读数的语义 (这一条最容易糊弄过去) ─────────────────────────────────────
 *
 * 设备是**一直在推**数据的, 而扫描控制器是"到位了, 给我一个数"。于是:
 *
 *   · 每次请求返回**缓冲区里最新的那个** —— 不是最老的。缓冲区里可能还压着滑台
 *     移动过程中采的数, 那些跟当前这点没关系。
 *   · **同一个采样不会返回两次**。samples_per_point > 1 时控制器会连着要几个数
 *     求平均, 如果回回给同一个, 那个"平均"就是假的。所以按设备时间戳记一个水位线,
 *     只认严格更新的。
 *   · 等不到新数就**继续等**, 不急着报错 —— 控制器的 meter_timeout_ms (缺省 2000ms)
 *     本来就是干这个的。这里 1800ms 先报一次, 是为了给一句比"超时"更有用的原因。
 *   · **status 不是 0 的项不能当成测量值**。手册原话: "Not every data item represents a
 *     valid measurement. The status code must always be checked"。过量程/饱和就明确
 *     报错, 而不是把一个不可用的数记进 CSV。
 */
#pragma once

#include <QAtomicInt>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "powermeter.h"

class QThread;

namespace scan {

/* 打开之后才有的东西。界面靠它填状态行和三个下拉框 */
struct OphirInfo
{
   bool    valid       = false;
   QString device_name;      /* 表头型号, 例如 "Juno+" */
   QString device_serial;
   QString rom_version;
   QString sensor_name;      /* 探头型号, 例如 "PD300R" */
   QString sensor_type;      /* thermopile / photodiode / pyroelectric */
   QString sensor_serial;

   QStringList wavelengths;  /* 下拉框的选项, 原样来自设备 */
   QStringList ranges;
   QStringList modes;
   int wl_index    = -1;     /* -1 = 这个探头没有这一项 (手册 Common Parameters) */
   int range_index = -1;
   int mode_index  = -1;

   QString summary;          /* 状态行那句话, 由工作线程拼好 */
};

class OphirMeter : public PowerMeter
{
   Q_OBJECT

public:
   explicit OphirMeter(QObject *parent = nullptr);
   ~OphirMeter() override;

   QString kind() const override;

   bool open(QString *err) override;
   void close() override;
   void requestReading() override;

   /* 加锁拷一份。工作线程在写, 界面在读 */
   OphirInfo info() const;

   /*
    * 换波长 / 量程 / 测量模式。**异步** —— 手册规定配置方法不能在 streaming 时调,
    * 所以工作线程收到请求后是 停流 → 改 → 重新开流 三步。改完报 infoChanged(),
    * 改不动报 configFailed()。
    *
    * 传 -1 表示"不要求这一项"。设备可能把值夹到它接受的范围内, 所以**以回来的
    * info() 为准**, 不要拿这里传进去的下标当结果。
    */
   void setWavelengthIndex(int idx);
   void setRangeIndex(int idx);
   void setModeIndex(int idx);

signals:
   void infoChanged();
   void configFailed(const QString &err);

private:
   void runSession();          /* 工作线程的主体 —— 所有 COM 都在这里面 */
   bool waitForOpen(QString *err);

   struct Private;
   Private *p = nullptr;
};

}   /* namespace scan */
