/*
 * scan/ophircom.h —— OphirLMMeasurement COM 对象的一层薄封装
 *
 * 这是 PD300R 探头 → Juno+ 表头 → USB → PC 这条路在软件侧的入口。数据链路:
 *
 *     OphirMeter (ophirmeter.h)
 *          │
 *          ▼
 *     OphirCom            ← 本文件
 *          │  IDispatch
 *          ▼
 *     OphirLMMeasurement.CoLMMeasurement  (StarLab 装的 COM 对象)
 *          │
 *          ▼
 *        Juno+ ──── PD300R
 *
 * ── 为什么不是照抄官方的 C++ 包装 ──────────────────────────────────────────
 *
 * 官方 `Automation Examples\Com object\CPlus_Demo` 里那个 wrapper 靠一行
 *
 *     #import "progid:OphirLMMeasurement.CoLMMeasurement"
 *
 * 让 MSVC 从 typelib 生成 .tlh/.tli 再调。**`#import` 是 MSVC 专属**, GCC 不认 ——
 * 而本仓库的 Qt 上位机是 MSYS2 UCRT64 的 g++ 编的 (见 CMakePresets.json 里那段
 * 为什么不用 Qt 官方 MinGW 的说明)。所以那份示例只能当**签名参考**, 不能原样编进来。
 * 方法名、参数顺序、out 参数类型全部照它和官方手册抄, 没有一处是猜的。
 *
 * ── 为什么不是 GetIDsOfNames ───────────────────────────────────────────────
 *
 * 最省事的写法是 `IDispatch::GetIDsOfNames` 按名字换 DISPID, 再 `IDispatch::Invoke`。
 * **这台机器上那条路是断的** —— 实测四个方法名全部返回 0x8002801D
 * (TYPE_E_LIBNOTREGISTERED), `IDispatch::GetTypeInfo` 也一样。原因在注册表:
 *
 *     HKLM\SOFTWARE\Classes\TypeLib\{F7267688-...}\a.a\0\win64
 *                                                    ^^^ 版本号是字面量 "a.a"
 *
 * 版本号必须是 `major.minor` 的数字, `a.a` 解析不出来, 于是 LoadRegTypeLib 找不到
 * 这个库, 而对象的 GetIDsOfNames / GetTypeInfo 内部**正好是走注册表的**。
 * (同一个坑也让 PowerShell 的 `New-Object -ComObject` 和 MSVC 的 `#import` 一起躺下。
 *  这不是本仓库能替 Ophir 修的东西, 也不该要求每个装机的人都去改注册表。)
 *
 * 所以绕开注册表, 直接从 dll **资源**里读 typelib:
 *
 *     LoadTypeLibEx(<COM x64\OphirLMMeasurement.dll>, REGKIND_NONE, &tl)
 *       → GetTypeInfoOfGuid/按名字找 "ICoLMMeasurement2"
 *       → ITypeInfo::GetIDsOfNames   (只有名字 → DISPID 这一步)
 *       → ITypeInfo::Invoke          (用我们自己加载的 typeinfo 派发)
 *
 * dll 路径从 `HKCR\CLSID\{...}\InprocServer32` 读 —— 那条注册项是**对的** (实测),
 * 坏的只有 typelib 的版本号那一条。整条路一个写死的路径都没有。
 *
 * ── 线程 ──────────────────────────────────────────────────────────────────
 *
 * 服务端是 **Apartment** 线程模型 (InprocServer32 里写的, 官方手册 "Threading issues"
 * 也明说了: "it can be only used directly from the thread it was created in")。
 * 所以: 一个 OphirCom 实例**从头到尾只能在一个线程里用**, 而且那个线程必须先
 * CoInitializeEx。这条约束由调用方 (OphirMeter 的工作线程) 负责。
 *
 * ── 公开头文件里为什么一个 COM 类型都没有 ──────────────────────────────────
 *
 * windows.h 会把 min/max 宏、`interface` 之类的东西泼到整个翻译单元里, 而这个头会被
 * scanwindow.cpp 那样的大文件 include。所以 COM 那边的类型全部关在 .cpp 里的
 * OphirComImpl 后面, 对外只露 QString / QVector。
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace scan {

class OphirCom
{
public:
   /* 一次 GetData 的返回。三个数组**等长**, 第 i 项是同一件事的三个面 */
   struct Data
   {
      QVector<double> values;       /* W 或 J, 看探头和测量模式 */
      QVector<double> timestamps;   /* 设备侧时间, ms */
      QVector<int>    statuses;     /* 0 = 有效; 其余见 .cpp 里的 statusText() */

      int size() const { return (int)values.size(); }
      void clear() { values.clear(); timestamps.clear(); statuses.clear(); }
   };

   struct DeviceInfo { QString name, rom, serial; };
   struct SensorInfo { QString serial, type, name; };

   OphirCom();
   ~OphirCom();

   OphirCom(const OphirCom &)            = delete;
   OphirCom &operator=(const OphirCom &) = delete;

   /*
    * COM 对象在这台机器上注册了没有。**不建对象、不碰设备**, 只查注册表 ——
    * 界面用它来决定"功率计"下拉框里那一项要不要灰掉, 以及自检要不要跑。
    */
   static bool isRegistered();
   static bool isAvailable();      /* isRegistered && typelib 能从 dll 里读出来 */

   /*
    * 建对象并从 dll 资源里读 typelib。**必须在调用线程 CoInitializeEx 之后调**。
    * 失败时 err 是给操作员看的一句话 (哪一步、为什么)。
    */
   bool create(QString *err);
   void destroy();

   bool isCreated() const;

   /* ---- 设备 ---- */
   bool scanUsb(QStringList *serial_numbers, QString *err);
   bool openUsbDevice(const QString &serial_number, long *h_device, QString *err);
   bool closeDevice(long h_device, QString *err);
   bool closeAll(QString *err);
   bool isSensorExists(long h_device, long channel, bool *exists, QString *err);

   /* ---- 信息 ---- */
   bool getVersion(long *version, QString *err);
   bool getDriverVersion(QString *info, QString *err);
   bool getDeviceInfo(long h_device, DeviceInfo *out, QString *err);
   bool getSensorInfo(long h_device, long channel, SensorInfo *out, QString *err);

   /* ---- 探头配置 ----
    *
    * 每对都是 Get 返回**可选项表 + 当前选中项的下标**, Set 按下标选。
    * 手册 Common Parameters: 某项对这个探头不适用时, options 为空、index 为 -1。
    * **配置方法不能在 streaming 时调** —— 调用方负责先 StopStream。
    */
   bool getWavelengths(long h_device, long channel, long *index, QStringList *options,
                       QString *err);
   bool setWavelength(long h_device, long channel, long index, QString *err);
   bool addWavelength(long h_device, long channel, long wavelength, QString *err);

   bool getRanges(long h_device, long channel, long *index, QStringList *options, QString *err);
   bool setRange(long h_device, long channel, long index, QString *err);

   bool getMeasurementMode(long h_device, long channel, long *index, QStringList *options,
                           QString *err);
   bool setMeasurementMode(long h_device, long channel, long index, QString *err);

   /* ---- 测量 ----
    *
    * 手册 Data Streams: StartStream 会**丢掉**之前攒下的数据; 之后设备持续把数据推给
    * COM 对象, 对象攒着, 由 GetData 取走 (取走的就从缓冲里没了)。
    * 取到空数组是**正常情况** (这一瞬间没有新数据), 不是错。
    */
   bool configureStreamMode(long h_device, long channel, long mode, long n_value, QString *err);
   bool startStream(long h_device, long channel, QString *err);
   bool getData(long h_device, long channel, Data *out, QString *err);
   bool stopStream(long h_device, long channel, QString *err);
   bool stopAllStreams(QString *err);

   /* HRESULT → 人话。手册 "Error Codes" 那张表 + 对象自己给的 EXCEPINFO 描述 */
   static QString errorText(long hresult);
   static QString statusText(int status);

private:
   struct Impl;
   Impl *d = nullptr;
};

}   /* namespace scan */
