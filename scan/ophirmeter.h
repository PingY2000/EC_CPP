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

   /* 这一次 ScanUSB 看到的**所有**表头序列号 (Ophir COM 只给序列号, 型号要打开之后才知道)。
    * **打开成功与否都发布** —— 多设备时"这台打不开, 换一台再试"这条路全靠它 */
   QStringList device_serials;

   QString summary;          /* 状态行那句话, 由工作线程拼好 */
};

/* 自定义波长的允许范围。**单一出处**: 界面那个旋钮的范围与工作线程的越界拒绝都读它 */
const int k_wl_min_nm = 330;
const int k_wl_max_nm = 1100;

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

/* 从设备给的**波长选项串**里取出 nm 数: "1064nm" / "1064 nm" / "532" -> 1064 / 1064 / 532。
 * 认法只有一条: 取最前面那一串连续数字。**格式由设备定, 这里不假设** (CSV 里见过 "1064nm"
 * 这一种); 一个数字都没有 -> -1, 调用方当"认不出"。
 *
 * 它有两个用处: 在"写进去的那个值读回来了没有"这件事上做比对, 以及找出它在新列表里的下标
 * (setWavelength 只吃下标)。两处必须用同一个解析, 否则自己写的值自己认不出来 */
int wavelengthNm(const QString &option);

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

   /*
    * 往设备里**加**一个波长并选中它 (异步, 范围 k_wl_min_nm ~ k_wl_max_nm)。
    *
    * **这是本程序唯一一处写设备。** 设备那张波长表是只读的选项表, 表里没有的值根本没有下标
    * 可用 —— 所以"自定义波长"只有这一条路。写完**立刻把列表读回来核对**: 只有新值真的出现
    * 在读回来的那份表里才算成功 (那一份才是"设备接受了什么"), 否则 configFailed 且什么都不改。
    * 界面侧与 setWavelengthIndex 同一套: 置 m_cfgBusy, 等 infoChanged / configFailed。
    */
   void addCustomWavelength(int nm);

   /* 想打开哪一台 (序列号, 来自 info().device_serials)。空 = 没指定, 用枚举到的第一台。
    * **必须在 open() 之前设**: 工作线程一启动就按它选设备。设了但枚举不到那一台会快失败,
    * 并明说"未找到上次那台" —— 悄悄换成第一台是最坏的一种做法 */
   void setWantedSerial(const QString &serial);
   QString wantedSerial() const;

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
