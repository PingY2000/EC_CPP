/*
 * scan/ophircom.h —— OphirLMMeasurement.CoLMMeasurement (StarLab 装的 COM 对象) 的薄封装:
 * 链路 OphirMeter → OphirCom → IDispatch → 该对象 → Juno+ → PD300R。
 * 本机 typelib 的注册项坏了 (0x8002801D TYPE_E_LIBNOTREGISTERED), 改从 dll 资源里读;
 * 服务端是 Apartment 模型 (手册 "Threading issues"): 实例只能在一个线程里用, 该线程须先
 * CoInitializeEx。官方 CPlus_Demo 示例的 `#import` 是 MSVC 专属, 只作签名与参数顺序参考。
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace scan {

class OphirCom
{
public:
   /* 一次 GetData 的返回, 三个数组等长, 第 i 项是同一件事的三个面 */
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

   /* COM 对象注册了没有。只查注册表, 不建对象、不碰设备 */
   static bool isRegistered();
   static bool isAvailable();      /* isRegistered && typelib 能从 dll 里读出来 */

   /* 建对象并从 dll 资源里读 typelib。必须在调用线程 CoInitializeEx 之后调;
    * 失败时 err 是给操作员看的一句话 */
   bool create(QString *err);
   void destroy();

   bool isCreated() const;

   bool scanUsb(QStringList *serial_numbers, QString *err);
   bool openUsbDevice(const QString &serial_number, long *h_device, QString *err);
   bool closeDevice(long h_device, QString *err);
   bool closeAll(QString *err);
   bool isSensorExists(long h_device, long channel, bool *exists, QString *err);

   bool getVersion(long *version, QString *err);
   bool getDriverVersion(QString *info, QString *err);
   bool getDeviceInfo(long h_device, DeviceInfo *out, QString *err);
   bool getSensorInfo(long h_device, long channel, SensorInfo *out, QString *err);

   /* ---- 探头配置 ----
    * Get 返回可选项表 + 当前选中项下标, Set 按下标选; 某项对这个探头不适用时 options
    * 为空、index 为 -1 (手册 Common Parameters)。配置方法不能在 streaming 时调。
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
    * 手册 Data Streams: StartStream 会丢掉之前攒下的数据; 之后设备持续把数据推给 COM
    * 对象, 由 GetData 取走 (取走的就从缓冲里没了)。
    * 取到空数组是正常情况 (这一瞬间没有新数据), 不是错。详见 ophirmeter.cpp。
    */
   bool configureStreamMode(long h_device, long channel, long mode, long n_value, QString *err);
   bool startStream(long h_device, long channel, QString *err);
   bool getData(long h_device, long channel, Data *out, QString *err);
   bool stopStream(long h_device, long channel, QString *err);
   bool stopAllStreams(QString *err);

   /* HRESULT → 人话。手册 "Error Codes" 表 + 对象给的 EXCEPINFO 描述 */
   static QString errorText(long hresult);
   static QString statusText(int status);

private:
   struct Impl;
   Impl *d = nullptr;
};

}   /* namespace scan */
