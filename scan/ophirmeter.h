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

   /* 读数的单位。**COM 一个字段都不给**: 那份数组是 W 还是 J 由探头与测量模式定。
    * 这里是从设备自己的两个字判出来的 (见 unitFromDeviceInfo); 空 = 认不出来 */
   QString unit;

   /* 诊断用的两个版本号。取不到就空着 (那不影响读数, 所以不该因为它打不开设备) */
   QString com_version;      /* getVersion: COM 对象自己的版本 */
   QString driver_version;   /* getDriverVersion: 驱动那一串, 原样来自设备 */

   QStringList wavelengths;  /* 下拉框的选项, 原样来自设备 */
   QStringList ranges;
   QStringList modes;
   int wl_index    = -1;     /* -1 = 这个探头没有这一项 (手册 Common Parameters) */
   int range_index = -1;
   int mode_index  = -1;

   QString summary;          /* 状态行那句话, 由工作线程拼好 */
};

/* 由**设备自己的两个字**判读数单位: 当前的测量模式名 (Ophir 的模式名里带 Power / Energy)
 * 与探头类型 (热释电测的是脉冲能量)。
 *
 * **优先看模式名**: 它说了 Power / Energy 就是 W / J; 它说了别的东西 (dBm …) 就直接返回空,
 * 不让探头类型替它翻案 —— 同一只探头换个模式报的就是另一个量纲, 而对数值顶着 W 进 CSV
 * 是最坏的一种错。探头类型只在这一项**不存在**时兜底。
 *
 * 认不出来就返回空, **不猜** —— 空的意思是"我不知道", 界面照原样写「单位不明」。
 * 见 docs/scan_sweep.md §25。 */
QString unitFromDeviceInfo(const QString &sensor_type, const QString &mode_name);

class OphirMeter : public PowerMeter
{
   Q_OBJECT

public:
   explicit OphirMeter(QObject *parent = nullptr);
   ~OphirMeter() override;

   QString kind() const override;
   QString tag()  const override;      /* "ophir" */

   /* 单位与配置都从 info() 取 (工作线程读回来的设备状态)。没打开 / 认不出来 -> 空 */
   QString unit() const override;
   QStringList configLines() const override;

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
