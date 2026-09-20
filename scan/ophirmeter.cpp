#include "ophirmeter.h"
#include "ophircom.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

namespace scan {

namespace {

/* 取数轮询间隔。手册说 Standard 流下对象最多每 50ms 触发一次 DataReady */
const int k_poll_ms = 10;

/* open() 最多阻塞多久。手册那套流程 (枚举 USB → 开设备 → 读探头 → 配置 → 开流) 正常
 * 几百毫秒, 12s 是留给 USB 半死不活的天花板 */
const int k_open_timeout_ms = 12000;

/* 一个未决的读数请求最多等多久。故意比控制器的 meter_timeout_ms (缺省 2000) 小,
 * 好让先开口的是"设备一直没出新数"而不是控制器那句笼统的"功率计超时" */
const int k_stale_ms = 1800;

/* Juno+ 是**单通道**设备。手册 Conventions: "For devices with only one channel
 *  <channel> should be 0" */
const long k_channel = 0;

long long nowMs()
{
   return QDateTime::currentMSecsSinceEpoch();
}

/* 有些非零 status 是通知不是错误 (手册 GetData Status Codes 的 "When and Where" 列标成
 * "informational notification")。注意 0x200000 (过热告警) 不在其列, 那个要停下来。 */
bool isNotificationStatus(int status)
{
   switch (status)
   {
   case 0x040001:   /* 滤片状态变化 */
   case 0x050000:   /* 脉冲频率 */
   case 0x100000:   /* 温度 */
   case 0x300000:   /* 脉宽 */
   case 0x400000:   /* PfP 能量 */
      return true;
   default:
      return false;
   }
}

QString buildSummary(const OphirInfo &i)
{
   QStringList parts;

   QString dev = i.device_name;
   if (!i.device_serial.isEmpty())
      dev += QStringLiteral(" (SN %1)").arg(i.device_serial);
   if (!dev.isEmpty())
      parts << dev;

   QString sens = i.sensor_name;
   if (!i.sensor_type.isEmpty())
      sens += QStringLiteral(" · %1").arg(i.sensor_type);
   if (!i.sensor_serial.isEmpty())
      sens += QStringLiteral(" · SN %1").arg(i.sensor_serial);
   if (!sens.isEmpty())
      parts << sens;

   if (i.wl_index >= 0 && i.wl_index < i.wavelengths.size())
      parts << i.wavelengths.at(i.wl_index);
   if (i.range_index >= 0 && i.range_index < i.ranges.size())
      parts << QStringLiteral("量程 ") + i.ranges.at(i.range_index);
   if (i.mode_index >= 0 && i.mode_index < i.modes.size())
      parts << i.modes.at(i.mode_index);

   return parts.join(QStringLiteral("  ·  "));
}

/* 每个成功的 CoInitializeEx 都要配一个 CoUninitialize (含提前 return 的路) */
struct ComApartment
{
   HRESULT hr;
   bool    owned;
   ComApartment() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
                    owned(SUCCEEDED(hr)) {}
   ~ComApartment() { if (owned) CoUninitialize(); }

   ComApartment(const ComApartment &)            = delete;
   ComApartment &operator=(const ComApartment &) = delete;
};

}   /* namespace */

struct OphirMeter::Private
{
   QThread *thread = nullptr;

   QMutex         mx;
   QWaitCondition ready;
   bool           open_done = false;
   bool           open_ok   = false;
   QString        open_err;

   QAtomicInt quit  {0};        /* 1 = 该收摊了 (close() 置上) */
   QAtomicInt want  {0};        /* 1 = 有一个未决的读数请求 */
   qint64     want_ms = 0;      /* 那个请求是什么时候发的 */
   int        want_wl    = -1;  /* -1 = 没有待改的 */
   int        want_range = -1;
   int        want_mode  = -1;

   mutable QMutex info_mx;
   OphirInfo      info;
};

OphirMeter::OphirMeter(QObject *parent) : PowerMeter(parent), p(new Private)
{
}

/* 析构比 close() 多等一会儿 (15 秒): 工作线程的每一次 emit 都要碰 this, 线程还没停就删
 * 对象是 use-after-free。收不掉就把线程和 p 故意漏掉 (泄漏优于崩溃), 并打一行 stderr。 */
OphirMeter::~OphirMeter()
{
   m_open = false;
   p->want.storeRelease(0);

   QThread *t = p->thread;
   if (t != nullptr)
   {
      p->thread = nullptr;
      p->quit.storeRelease(1);

      if (t->wait(15000))
      {
         delete t;
         delete p;
         return;
      }

      /* 断掉它的所有连接 (免得它往一个正在死的对象上投信号), 然后把线程和 p 一起漏掉 */
      t->disconnect();
      std::fprintf(stderr,
                   "[ophirmeter] 采集线程 15s 没收掉 (COM 调用卡在驱动里?), "
                   "故意泄漏它以免 use-after-free。请检查设备与 USB\n");
      return;
   }

   delete p;
}

QString OphirMeter::kind() const
{
   return QStringLiteral("Ophir 功率计 (PD300R/Juno+)");
}

OphirInfo OphirMeter::info() const
{
   QMutexLocker<QMutex> lk(&p->info_mx);
   return p->info;
}

bool OphirMeter::open(QString *err)
{
   if (isOpen())
      return true;

   if (p->thread)
   {
      /* 上一次没收干净就别再起一个: 两个线程抢同一个表头 */
      if (err)
         *err = QStringLiteral("上一次的采集线程还没收掉");
      return false;
   }

   {
      QMutexLocker<QMutex> lk(&p->mx);
      p->open_done = false;
      p->open_ok   = false;
      p->open_err.clear();
      p->want_wl = p->want_range = p->want_mode = -1;
      p->want_ms = 0;
   }
   p->want.storeRelease(0);

   p->thread = QThread::create([this] { runSession(); });
   p->thread->setObjectName(QStringLiteral("ophir-meter"));
   p->thread->start();

   if (!waitForOpen(err))
   {
      close();                       /* 把没收干净的东西收掉 */
      return false;
   }

   m_open = true;
   return true;
}

bool OphirMeter::waitForOpen(QString *err)
{
   QMutexLocker<QMutex> lk(&p->mx);

   if (!p->open_done)
      p->ready.wait(&p->mx, QDeadlineTimer(k_open_timeout_ms));

   if (!p->open_done)
   {
      if (err)
         *err = QStringLiteral("等功率计打开超过 %1 ms —— 设备或 USB 卡住了")
                   .arg(k_open_timeout_ms);
      return false;
   }

   if (!p->open_ok)
   {
      if (err)
         *err = p->open_err;
      return false;
   }
   return true;
}

void OphirMeter::close()
{
   m_open = false;

   /* 未决的请求作废: 停线程时不会再回它了 */
   p->want.storeRelease(0);

   QThread *t = p->thread;
   if (!t)
      return;
   p->thread = nullptr;

   p->quit.storeRelease(1);

   if (t->wait(5000))
   {
      delete t;
      return;
   }

   /* 线程还在某个 COM 调用里出不来。不 terminate: COM 对象会带着内部锁死掉 */
   QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
}

void OphirMeter::runSession()
{
   /* Apartment 模型: 本线程必须先 CoInitializeEx, 之后每一次 COM 调用都得在这个线程里 */
   ComApartment apt;

   auto failOpen = [this](const QString &msg) {
      QMutexLocker<QMutex> lk(&p->mx);
      p->open_err  = msg;
      p->open_ok   = false;
      p->open_done = true;
      p->ready.wakeAll();
   };

   if (!apt.owned)
   {
      failOpen(QStringLiteral("在这个线程上起 COM 失败: %1")
                  .arg(OphirCom::errorText((long)apt.hr)));
      return;
   }

   OphirCom com;
   QString  err;

   if (!com.create(&err))
   {
      failOpen(err);
      return;
   }

   QStringList serials;
   if (!com.scanUsb(&serials, &err))
   {
      failOpen(QStringLiteral("枚举 Ophir USB 设备失败: %1").arg(err));
      return;
   }
   if (serials.isEmpty())
   {
      /* 手册的排查顺序: 线 / 供电 / Windows 认不认 / StarLab 认不认, 不是先改代码 */
      failOpen(QStringLiteral("没找到 Ophir USB 设备。Juno+ 插好了吗? "
                              "先开 StarLab 看看它认不认这块表头 —— "
                              "StarLab 里都读不到功率的话, 问题在硬件或驱动, 不在这个程序里"));
      return;
   }

   long h = 0;
   if (!com.openUsbDevice(serials.first(), &h, &err))
   {
      failOpen(QStringLiteral("打开 %1 失败: %2").arg(serials.first(), err));
      return;
   }

   /* 从这里往后失败都要先把设备还回去: 表头是独占的 */
   auto failOpenWithDevice = [&](const QString &msg) {
      com.closeDevice(h, nullptr);
      failOpen(msg);
   };

   OphirInfo info;
   OphirCom::DeviceInfo dinfo;
   if (!com.getDeviceInfo(h, &dinfo, &err))
   {
      failOpenWithDevice(QStringLiteral("读表头信息失败: %1").arg(err));
      return;
   }
   info.device_name   = dinfo.name;
   info.device_serial = dinfo.serial;
   info.rom_version   = dinfo.rom;

   /* 手册 Conventions: 先确认通道上真有探头再谈别的 */
   bool sensor_ok = false;
   if (com.isSensorExists(h, k_channel, &sensor_ok, &err) && !sensor_ok)
   {
      failOpenWithDevice(QStringLiteral("表头在, 但通道 %1 上没有探头 —— "
                                        "PD300R 插到 Juno+ 上了吗?").arg(k_channel));
      return;
   }

   OphirCom::SensorInfo sinfo;
   if (!com.getSensorInfo(h, k_channel, &sinfo, &err))
   {
      failOpenWithDevice(QStringLiteral("读探头信息失败: %1").arg(err));
      return;
   }
   info.sensor_name   = sinfo.name;
   info.sensor_type   = sinfo.type;
   info.sensor_serial = sinfo.serial;

   /* ---- 读当前的波长 / 量程 / 测量模式 ----
    * 只读不改 (手册: 不要拿型号自行推断规格)。这三项对某些探头不适用, 那时 index = -1、
    * options 为空 —— 那是正常的, 不是错。设备给的下标是 COM 的 LONG, 先收进 long。 */
   auto readOptions = [&]() {
      QString e2;
      long idx = -1;

      info.wavelengths.clear();
      info.ranges.clear();
      info.modes.clear();
      info.wl_index = info.range_index = info.mode_index = -1;

      if (com.getWavelengths(h, k_channel, &idx, &info.wavelengths, &e2))
         info.wl_index = (int)idx;
      if (com.getRanges(h, k_channel, &idx, &info.ranges, &e2))
         info.range_index = (int)idx;
      if (com.getMeasurementMode(h, k_channel, &idx, &info.modes, &e2))
         info.mode_index = (int)idx;
   };
   readOptions();

   /* ---- 开流 ----
    * 不调 ConfigureStreamMode: 缺省的 Standard 就是这里要的 (对象把数据攒起来, 由
    * GetData 取走); Turbo 只对 Vega/Nova-II 有意义。 */
   if (!com.startStream(h, k_channel, &err))
   {
      failOpenWithDevice(QStringLiteral("启动测量失败: %1").arg(err));
      return;
   }

   info.valid   = true;
   info.summary = buildSummary(info);
   {
      QMutexLocker<QMutex> lk(&p->info_mx);
      p->info = info;
   }

   {
      QMutexLocker<QMutex> lk(&p->mx);
      p->open_ok   = true;
      p->open_done = true;
      p->ready.wakeAll();
   }
   emit infoChanged();

   /* 水位线: 只认严格比它新的采样 —— 否则 samples_per_point > 1 求平均时会把同一个数
    * 加 N 次, 看着正常其实是假的 */
   double watermark = -1.0;

   while (!p->quit.loadAcquire())
   {
      int wl = -1, rg = -1, md = -1;
      {
         QMutexLocker<QMutex> lk(&p->mx);
         wl = p->want_wl; rg = p->want_range; md = p->want_mode;
         p->want_wl = p->want_range = p->want_mode = -1;
      }

      if (wl >= 0 || rg >= 0 || md >= 0)
      {
         /* 手册: "Configuration methods cannot be called while a channel is streaming" */
         QString e2;
         bool ok = com.stopStream(h, k_channel, &e2);

         if (ok && wl >= 0) ok = com.setWavelength(h, k_channel, wl, &e2);
         if (ok && rg >= 0) ok = com.setRange(h, k_channel, rg, &e2);
         if (ok && md >= 0) ok = com.setMeasurementMode(h, k_channel, md, &e2);

         if (ok)
            ok = com.startStream(h, k_channel, &e2);

         if (ok)
         {
            /* 新流 = 新时间戳, 水位线作废 */
            watermark = -1.0;
            readOptions();                 /* 设备可能把值夹到它接受的范围内 */

            {
               QMutexLocker<QMutex> lk(&p->info_mx);
               const QString summary = buildSummary(info);
               p->info.wavelengths = info.wavelengths;
               p->info.ranges      = info.ranges;
               p->info.modes       = info.modes;
               p->info.wl_index    = info.wl_index;
               p->info.range_index = info.range_index;
               p->info.mode_index  = info.mode_index;
               p->info.summary     = summary;
            }
            emit infoChanged();
         }
         else
         {
            /* 配置没成功, 流是停着的: 得开回去, 否则后面每个点都会超时 */
            QString e3;
            if (!com.startStream(h, k_channel, &e3))
               emit configFailed(QStringLiteral("%1; 而且重开流也失败了: %2").arg(e2, e3));
            else
               emit configFailed(e2);
         }
      }

      /* ---- 有未决的读数请求吗 ----
       * 清标志必须在 emit 之前: 信号是排队投递的, 界面收到 readingReady 后可能立刻
       * 又发来下一个请求 (samples_per_point > 1), 先 emit 再清会把那个新请求丢掉。 */
      if (p->want.loadAcquire())
      {
         OphirCom::Data data;
         QString e2;

         if (!com.getData(h, k_channel, &data, &e2))
         {
            p->want.storeRelease(0);
            emit readingFailed(QStringLiteral("读功率计失败: ") + e2);
         }
         else
         {
            /* 取最新的那一项, 不是最老的 (缓冲区里可能压着滑台移动期间采的数) */
            int best = -1;
            for (int i = 0; i < data.size(); i++)
            {
               if (data.timestamps[i] <= watermark)
                  continue;
               if (best < 0 || data.timestamps[i] > data.timestamps[best])
                  best = i;
            }

            if (best >= 0)
            {
               watermark = data.timestamps[best];
               const int st = data.statuses[best];

               if (st == 0)
               {
                  p->want.storeRelease(0);
                  emit readingReady(data.values[best]);
               }
               else if (isNotificationStatus(st))
               {
                  /* 通知类: 水位线已推过去, 等下一个真正的新数; 故意不清标志 */
               }
               else
               {
                  /* 过量程 / 饱和 / 过热: 手册 "Not every data item represents a valid
                   * measurement" —— 宁可报错也不把这个数记进 CSV */
                  p->want.storeRelease(0);
                  emit readingFailed(QStringLiteral("这一读数不可用: %1")
                                        .arg(OphirCom::statusText(st)));
               }
            }
            else
            {
               /* 还没有更新的数, 不急着报错: 等太久了才开口, 给一句比"超时"更有用的原因 */
               qint64 asked = 0;
               {
                  QMutexLocker<QMutex> lk(&p->mx);
                  asked = p->want_ms;
               }
               if (asked > 0 && nowMs() - asked > k_stale_ms)
               {
                  p->want.storeRelease(0);
                  emit readingFailed(QStringLiteral("功率计 %1 ms 没有出新数 "
                                                    "(设备在出数吗? 量程选对了吗?)")
                                        .arg(k_stale_ms));
               }
            }
         }
      }

      QThread::msleep((unsigned long)k_poll_ms);
   }

   QString e4;
   com.stopStream(h, k_channel, &e4);
   com.closeDevice(h, &e4);
}

void OphirMeter::requestReading()
{
   if (!isOpen())
   {
      /* 必须回一个: 接口约定恰好回一次, 不回控制器会一直等到它自己的超时 */
      emit readingFailed(QStringLiteral("功率计没打开"));
      return;
   }

   {
      QMutexLocker<QMutex> lk(&p->mx);
      p->want_ms = nowMs();
   }
   p->want.storeRelease(1);
}

void OphirMeter::setWavelengthIndex(int idx)
{
   if (!isOpen())
      return;
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_wl = idx;
}

void OphirMeter::setRangeIndex(int idx)
{
   if (!isOpen())
      return;
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_range = idx;
}

void OphirMeter::setModeIndex(int idx)
{
   if (!isOpen())
      return;
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_mode = idx;
}

}   /* namespace scan */
