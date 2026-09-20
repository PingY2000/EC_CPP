/*
 * scan/ophirmeter.h —— PD300R + Juno+ 的功率计, 接到 PowerMeter 接口上。
 * 所有 COM 调用集中在专属线程里 (服务端是 Apartment 模型, 手册 "Threading issues"),
 * 界面通过原子标志与信号跟它说话; open() 阻塞着等但有上限, 超时就把线程收掉并报错。
 * 每次请求返回缓冲区里最新的那个采样; status 不是 0 的项不能当测量值 (手册必查)。
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
    * 换波长 / 量程 / 测量模式, 异步: 手册规定配置方法不能在 streaming 时调, 工作线程收到
    * 请求后是 停流 → 改 → 重新开流 三步。传 -1 = 不要求这一项, 以回来的 info() 为准。
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
