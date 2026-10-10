#include "ophirmeter.h"
#include "ophircom.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QMutexLocker>
#include <QSet>
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
   /* 滤片档位也写出来: 它决定这一列数的量级 (滤片在光路里会衰减), 是"这份数据怎么来的"
    * 的一部分。探头没有这一项时 (index = -1) 一个字都不写 —— 与上面三项同一条判据。
    *
    * **要带标签**, 与「量程」同一个道理: 本机那只 PD300R 报回来的选项就是光秃秃的
    * `OUT` / `IN` (2026-10-10 实测, 见 selftest 那条 `INFO … filter:` 行), 一个字搁在这一串里
    * 读不出是指什么 —— 而 "IN" 尤其含糊。标签用界面那一格的同一个词 (Filter),
    * 于是屏幕上两处说的是同一件事 */
   if (i.filter_index >= 0 && i.filter_index < i.filters.size())
      parts << QStringLiteral("Filter ") + i.filters.at(i.filter_index);

   /* 读数单位 (由模式名 / 探头类型判出来的那个)。状态行是操作员一眼看得见的地方, 所以
    * 它也要写出来 —— 而判不出来就照原样写「单位不明」, **不替它填一个 W**。
    * 与 powermeter.cpp 的 unitLabel() 同一个词, 只是那份收的是 PowerMeter* */
   parts << (i.unit.isEmpty() ? QStringLiteral("单位不明")
                              : QStringLiteral("单位 %1").arg(i.unit));

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

/* 由设备自己的两个字判读数单位。**放在匿名空间外面**: 它在 ophirmeter.h 里是给外面用的
 * (scan_selftest 直接调它验那几条判据), 定义留在匿名空间里会与头文件那份声明撞成二义
 * (g++: call of overloaded ... is ambiguous) */
QString unitFromDeviceInfo(const QString &sensor_type, const QString &mode_name)
{
   /* 先看**测量模式名**: 那是设备自己的说法, 而且它随模式改 (Power <-> Energy)。
    * 它说了别的东西 (例如 dBm) 就一律不认 —— 模式名在的时候**不许**探头类型替它翻案:
    * 一只热电堆探头切到 dBm 模式, 报回来的那个数**不是瓦** (是对数), 那时按探头类型
    * 说 W 就会让一个对数值顶着瓦的单位被记进 CSV。 */
   const QString m = mode_name.toLower();
   if (!m.isEmpty())
   {
      if (m.contains(QStringLiteral("energy")))
         return QStringLiteral("J");
      if (m.contains(QStringLiteral("power")))
         return QStringLiteral("W");
      return QString();      /* 模式名说的是别的东西。**不猜** (见 ophirmeter.h) */
   }

   /* 模式这一项根本不存在时 (手册 Common Parameters: 探头可能没有这一项) 才退回探头类型:
    * 热释电探头测的是脉冲能量, 热电堆与光电二极管测的是功率 */
   const QString t = sensor_type.toLower();
   if (t.contains(QStringLiteral("pyro")))
      return QStringLiteral("J");
   if (t.contains(QStringLiteral("thermo")) || t.contains(QStringLiteral("photodiode")))
      return QStringLiteral("W");

   return QString();      /* 认不出来。**不猜**, 见 ophirmeter.h 的说明 */
}

/* 有些非零 status 是通知不是错误 (手册 GetData Status Codes 的 "When and Where" 列标成
 * "informational notification")。注意 0x200000 (过热告警) 不在其列, 那个要停下来。
 *
 * **放在匿名空间外面**: 与下面 unitFromDeviceInfo 同一条理由 —— 它是判据, 自检要直接钉
 * (0x040001 在不在这一列, 决定"操作员拨一下滤片"是安静地刷新界面还是弹一条红横幅)。 */
bool isNotificationStatus(int status)
{
   switch (status)
   {
   case 0x040001:   /* 滤片状态变化 —— 唯一一个还要另作处置的: 见 runSession 里那面 flt_reload */
   case 0x050000:   /* 脉冲频率 */
   case 0x100000:   /* 温度 */
   case 0x300000:   /* 脉宽 */
   case 0x400000:   /* PfP 能量 */
      return true;
   default:
      return false;
   }
}

/* 见 ophirmeter.h: 「探头 (s/n: …) · 表头 (s/n: …)」。纯拼字, 不碰设备 —— 所以自检能直接测
 * 它 (带上真机那一趟验的是"读回来的字对不对", 这个验的是"拼出来的话对不对", 两件事)。 */
QString deviceLabel(const QString &head_name, const QString &head_serial,
                    const QString &sensor_name, const QString &sensor_serial)
{
   /* 半段 = 「名字 (s/n: 序列号)」。缺一半就少写那一半:
    *   名字有、序列号没有 -> 就写名字 (设备没报序列号时别硬凑一个空的括号)
    *   名字没有、序列号有 -> 写 "(s/n: 序列号)" —— 括号里的东西是判据, 留着 */
   auto half = [](const QString &name, const QString &serial) {
      if (name.isEmpty())
         return serial.isEmpty() ? QString() : QStringLiteral("(s/n: %1)").arg(serial);
      if (serial.isEmpty())
         return name;
      return QStringLiteral("%1 (s/n: %2)").arg(name, serial);
   };

   /* **探头在前, 表头在后** —— 现场要的就是这个读法 (先认探头: 量程与波长是它定的) */
   QStringList parts;
   const QString sensor = half(sensor_name, sensor_serial);
   if (!sensor.isEmpty())
      parts << sensor;
   const QString head = half(head_name, head_serial);
   if (!head.isEmpty())
      parts << head;

   return parts.join(QStringLiteral(" · "));
}

/* 见 ophirmeter.h。设备给的选项串格式不由我们定, 所以只认"最前面那一串数字"这一条:
 * 多一个字符都不影响 (nm / NM / 空格), 少一个数字就认不出 (-1)。 */
int wavelengthNm(const QString &option)
{
   int i = 0;
   while (i < option.size() && !option.at(i).isDigit())
      ++i;
   if (i >= option.size())
      return -1;                     /* 一个数字都没有 */

   long v = 0;
   int  digits = 0;
   while (i < option.size() && option.at(i).isDigit())
   {
      v = v * 10 + (option.at(i).unicode() - u'0');
      ++i;
      if (++digits > 6)              /* 一串阿拉伯数字不像波长: 当认不出, 别让它溢出 */
         return -1;
   }
   return (int)v;
}

struct OphirMeter::Private
{
   QThread *thread = nullptr;

   mutable QMutex mx;           /* mutable: wantedSerial() 是 const 的, 它也要加这把锁 */
   QWaitCondition ready;
   bool           open_done = false;
   bool           open_ok   = false;
   QString        open_err;

   /* ★ **open() 必须把它清回 0** —— 它是"给当前这一次会话"的信号, 不是一次性开关。
    * 2026-09-29 前它只被 close() 置 1、从没被清过, 于是"第一次打开失败 (设备没插) →
    * 后来插上 → 按「重试」"这条路永远是坏的: open() 把设备开起来了、报成功了、界面也写着
    * 已连接, 而新线程一进 while (!quit) 就直接跳过整个取数循环, 收摊退场 —— 读数永远不来,
    * 只能重启程序 (那是唯一能让 Private 重新构造、把 quit 变回 0 的路)。
    * 「切换设备」同理 (它先 close() 再 open()) 也是坏的。 */
   QAtomicInt quit  {0};        /* 1 = 该收摊了 (close() 置上, open() 清回 0) */
   QAtomicInt want  {0};        /* 1 = 有一个未决的读数请求 */

   /* 上一次的线程没能在 5 秒内收掉 (close() 那条路, 见下)。**它还在跑**, 所以不能再起一个 ——
    * 两个线程抢同一个表头。只碰 GUI 线程, 不用原子量 (同 m_open)。
    * 它同时是"quit 能不能清 0"的闸: 漏掉的那个线程还站在 while (!quit) 上, 清了它就会被
    * 复活, 于是 close() 那句"该收摊了"静默失效。
    * ★ **记的是指针不是 bool**: 它一跑完就自己清空 (见 close()), 而析构要**看得见**它 ——
    * 线程还活着而 p 被删掉, 就是 use-after-free (那条路今天也在, 只是没人走过)。 */
   QThread *leaked = nullptr;
   qint64     want_ms = 0;      /* 那个请求是什么时候发的 */
   int        want_wl    = -1;  /* -1 = 没有待改的 */
   int        want_range = -1;
   int        want_mode  = -1;
   int        want_filter = -1;
   int        want_add_wl = -1;  /* 要加进设备的那个波长 (nm); -1 = 没有 */
   QString    want_serial;       /* 想打开哪一台; 空 = 枚举到的第一台 */

   /* 下拉那一项的字 (序列号 -> deviceLabel 拼出来的那句话)。**跨会话留着** (它在 p 上, 不在
    * 某一次会话里): 同一台表头一个进程只探一次 —— 见 runSession 里那段。
    * dev_probed 记的是"试过了"而不是"成功了": 探不动的那一台 (别的程序占着 / 没反应) 不再
    * 每按一次「重试」都去撞一次 (撞一次最长 12 秒)。只碰工作线程, 不加锁 */
   QHash<QString, QString> dev_labels;
   QSet<QString>           dev_probed;

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
                   "[ophirmeter] 采集线程 15 s 没收掉 (COM 调用卡在驱动里?), "
                   "故意泄漏它以免 use-after-free。请检查设备与 USB\n");
      return;
   }

   /* close() 早先就漏掉过一个线程 (见 close()): 它还在跑, 而且 runSession 每一轮都要碰 p。
    * p 是它的上下文 —— 删了就是 use-after-free, 所以**跟上面一样漏掉 p**, 只是没有线程指针
    * 可等了。这一支在加「重试」那条路之前就在, 但没有任何状态能走到它。 */
   if (p->leaked != nullptr)
   {
      p->leaked->disconnect();
      std::fprintf(stderr,
                   "[ophirmeter] 早先有一个采集线程没收掉, 析构时它还在 (COM 调用卡在驱动里?), "
                   "故意泄漏状态以免 use-after-free。请检查设备与 USB\n");
      return;
   }

   delete p;
}

QString OphirMeter::kind() const
{
   return QStringLiteral("Ophir 功率计 (PD300R/Juno+)");
}

QString OphirMeter::tag() const
{
   return QStringLiteral("ophir");
}

QString OphirMeter::unit() const
{
   return info().unit;
}

/* 这趟数据是哪个表头、哪个探头、什么波长/量程/模式采的 —— 一行一条 key=value, 进两份
 * CSV 的 `#` 行 (meterMetaLines 会加 meter_source / meter_unit 两行在它前面)。
 *
 * 没打开就**一个空表**: 那时 info() 里全是空的, 记下去只会是一份"说不清来源"的表头。 */
QStringList OphirMeter::configLines() const
{
   const OphirInfo i = info();
   if (!i.valid)
      return QStringList();

   /* 值里可以带空格 (模式名就常是 "Power - CW" 这样): `#` 行是按 key 找的, 空格无妨 */
   QStringList out;
   auto add = [&out](const QString &k, const QString &v) {
      if (!v.isEmpty())
         out << QStringLiteral("%1=%2").arg(k, v);
   };
   add(QStringLiteral("meter_device"),        i.device_name);
   add(QStringLiteral("meter_device_serial"), i.device_serial);
   add(QStringLiteral("meter_device_rom"),    i.rom_version);
   add(QStringLiteral("meter_sensor"),        i.sensor_name);
   add(QStringLiteral("meter_sensor_type"),   i.sensor_type);
   add(QStringLiteral("meter_sensor_serial"), i.sensor_serial);

   /* 这三项记的是**当前选中的那一项**, 不是整张选项表: 数据是这一档采的 */
   if (i.wl_index >= 0 && i.wl_index < i.wavelengths.size())
      add(QStringLiteral("meter_wavelength"), i.wavelengths.at(i.wl_index));
   if (i.range_index >= 0 && i.range_index < i.ranges.size())
      add(QStringLiteral("meter_range"), i.ranges.at(i.range_index));
   if (i.mode_index >= 0 && i.mode_index < i.modes.size())
      add(QStringLiteral("meter_mode"), i.modes.at(i.mode_index));
   /* 滤片档位: 同一份数据换个量级 (滤片在光路里会衰减), 回头复核时它是判据的一部分。
    * 设备给的字原样进文件, 与上面三项同一个写法 */
   if (i.filter_index >= 0 && i.filter_index < i.filters.size())
      add(QStringLiteral("meter_filter"), i.filters.at(i.filter_index));

   add(QStringLiteral("meter_driver"), i.driver_version);
   return out;
}

OphirInfo OphirMeter::info() const
{
   QMutexLocker<QMutex> lk(&p->info_mx);
   return p->info;
}

/* 两个"下一次打开用哪一台"的接口。**必须在 open() 之前设** (工作线程一启动就读它),
 * 形状与 want_wl 那几个待改项一样: 界面写、工作线程读, 用同一把锁。 */
void OphirMeter::setWantedSerial(const QString &serial)
{
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_serial = serial;
}

QString OphirMeter::wantedSerial() const
{
   QMutexLocker<QMutex> lk(&p->mx);
   return p->want_serial;
}

void OphirMeter::addCustomWavelength(int nm)
{
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_add_wl = nm;
}

bool OphirMeter::open(QString *err)
{
   if (isOpen())
      return true;

   if (p->thread || p->leaked != nullptr)
   {
      /* 上一次没收干净就别再起一个: 两个线程抢同一个表头。
       * `leaked` 那一支是 close() 没等到线程退出的情形 —— 指针已经清了, 线程还在。
       * 这句是**拒绝理由**, 按界面规矩要点出现象与出路: 它自己会退, 所以出路是等
       * (close() 那边挂着 finished -> 清标志, 退干净了下一按就能起) */
      if (err)
         *err = QStringLiteral("上一次的采集线程尚未结束。请稍候。");
      return false;
   }

   {
      QMutexLocker<QMutex> lk(&p->mx);
      p->open_done = false;
      p->open_ok   = false;
      p->open_err.clear();
      /* 上次那几条待改项一律作废 (它们是对**上一台**说的); want_serial **不清** ——
       * 它是"我要哪一台", 恰恰要在这一趟里用上 */
      p->want_wl = p->want_range = p->want_mode = -1;
      p->want_add_wl = -1;
      p->want_ms = 0;
   }

   /* ★ **这两个标志都是"给当前这次会话"的, 起线程之前必须复位。** 漏掉 quit 的后果见
    * Private 里那一段: 新线程报了"打开成功"就自己收摊, 界面上写着已连接而读数永远不来。
    * want 是"有没有未决请求", 上一趟那个已经是给上一台设备说的。 */
   p->quit.storeRelease(0);
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
         *err = QStringLiteral("打开功率计等待超过 %1 ms, 设备或 USB 无响应。")
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

   /* 线程还在某个 COM 调用里出不来。不 terminate: COM 对象会带着内部锁死掉。
    * ★ **记下来**: 指针已经清了, 但那个线程还在 while (!quit) 上蹲着 —— open() 靠这个标志
    * 拒绝再起一个 (否则两个线程抢同一个表头), 也靠它保证不把 quit 清 0 把它复活。 */
   p->leaked = t;
   QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
   /* 它**后来**退出来了就把标志放开 —— 不加这一条的话, 一次卡住会让「重试」从此永远
    * 报"上一次的采集线程尚未结束", 而那件事已经不成立了 (只有重启程序能好)。
    * 收在 GUI 线程上: 这个指针是普通成员, 只许 GUI 线程碰。this 没了这条连接自动断掉,
    * 不会去碰已经释放的 p (析构那条漏掉 p 的路上也同理)。 */
   QObject::connect(t, &QThread::finished, this, [this] { p->leaked = nullptr; });
}

void OphirMeter::runSession()
{
   /* Apartment 模型: 本线程必须先 CoInitializeEx, 之后每一次 COM 调用都得在这个线程里 */
   ComApartment apt;

   /* 上一次那一份设备表先作废: 枚举还没跑到, 界面上不该留着**上一次会话**的序列号 ——
    * 它可能已经不在了 (被拔走 / 换了 USB 口), 而那一行是让人照着换设备的 */
   {
      QMutexLocker<QMutex> lk(&p->info_mx);
      p->info.device_serials.clear();
   }

   auto failOpen = [this](const QString &msg) {
      {
         QMutexLocker<QMutex> lk(&p->mx);
         p->open_err  = msg;
         p->open_ok   = false;
         p->open_done = true;
         p->ready.wakeAll();
      }
      /* **打开失败也要报一次**。枚举出来的设备表在打开之前就发布了, 界面正等着它 ——
       * "这台打不开, 换一台再按重试"是那一行唯一的用处, 不给它这一次 infoChanged,
       * 多设备里坏的那台一挡就一台也换不了 (见 ophirmeter.h 的 device_serials) */
      emit infoChanged();
   };

   if (!apt.owned)
   {
      failOpen(QStringLiteral("在该线程上初始化 COM 失败: %1")
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
      failOpen(QStringLiteral("未找到 Ophir USB 设备。请确认 Juno+ 已插好, 并在 StarLab 中确认该表头是否被识别; "
                              "StarLab 中也读不到功率时, 问题在硬件或驱动。"));
      return;
   }

   /* 枚举结果**在打开之前**就发布出去。多设备时"这台打不开, 换一台再试"这条路全指望它:
    * 打不开的时候界面也得看得见列表, 否则一台也换不了 (型号要打开之后才知道, 所以这里
    * 只有序列号 —— Ophir 的枚举接口就只给序列号)。
    *
    * 每一台的字 (名字) 另有一份 device_labels, 与这个表一一对应: 还不知道的那几项是空,
    * 界面那时退回显示序列号。名字由下面那段"探一遍"补上, 补完**再发布一次** */
   auto labelsOf = [&]() {
      QStringList labels;
      for (const QString &s : serials)
         labels << p->dev_labels.value(s, QString());
      return labels;
   };
   auto publishList = [&]() {
      const QStringList labels = labelsOf();
      QMutexLocker<QMutex> lk(&p->info_mx);
      p->info.device_serials = serials;
      p->info.device_labels  = labels;
   };
   publishList();

   QString wanted;
   {
      QMutexLocker<QMutex> lk(&p->mx);
      wanted = p->want_serial;
   }

   /* 指定了哪一台就打哪一台。**指定那台不在时不许悄悄换成第一台** —— 两台表头接在同一个
    * 台面上时, 悄悄换一台就是拿另一个探头的数据当这一个用。
    * **这一步在探别的表头之前**: 那一句要快, 不该先等几个开设备的来回 */
   QString pick = serials.first();
   if (!wanted.isEmpty())
   {
      if (!serials.contains(wanted))
      {
         failOpen(QStringLiteral("未找到上次那台功率计 (%1)。请确认它已插好。").arg(wanted));
         return;
      }
      pick = wanted;
   }

   /* ---- 替**别的**表头各探一遍, 只为把型号写进下拉 ----
    * 枚举接口只给序列号; 型号与探头要**打开**才读得到。想让「设备」下拉里每一项都写着
    * "PD300R (s/n: …) · Juno (s/n: …)", 那些我们本来不会打开的表头就得先探一次:
    * openUsbDevice → getDeviceInfo / getSensorInfo → closeDevice。
    *
    * **只读**: 不写任何东西 (不改配置、不加波长)、不开流, 拿完信息立刻把设备还回去 (表头是
    * 独占的, 不还回去下面那句 openUsbDevice 就撞上自己了)。**要打开的那一台不探** —— 它马上
    * 就要被正式打开一次, 那一次读回来的字比探出来的全 (见下面那句 deviceLabel)。
    *
    * 探不动 (别的程序占着 / USB 没反应) 就让它只有序列号, 而且**一个进程里不再试**: 每按一次
    * 「重试」都去撞一次 12 秒没有意义。代价与已知限制见 docs/scan_sweep.md §37.9 */
   for (const QString &s : serials)
   {
      if (s == pick || p->dev_probed.contains(s))
         continue;
      p->dev_probed.insert(s);

      long h2 = 0;
      QString e2;
      if (!com.openUsbDevice(s, &h2, &e2))
         continue;

      OphirCom::DeviceInfo d2;
      OphirCom::SensorInfo s2;
      const bool ok_dev = com.getDeviceInfo(h2, &d2, &e2);
      const bool ok_sen = com.getSensorInfo(h2, k_channel, &s2, &e2);
      com.closeDevice(h2, nullptr);

      if (!ok_dev)
         continue;                     /* 连它是什么都不知道 -> 只有序列号 */
      p->dev_labels.insert(s, deviceLabel(d2.name, s,
                                          ok_sen ? s2.name : QString(),
                                          ok_sen ? s2.serial : QString()));
   }
   publishList();                       /* 再发布一次: 现在每一项都有型号了 */

   long h = 0;
   if (!com.openUsbDevice(pick, &h, &err))
   {
      failOpen(QStringLiteral("打开设备 %1 失败: %2").arg(pick, err));
      return;
   }

   /* 从这里往后失败都要先把设备还回去: 表头是独占的 */
   auto failOpenWithDevice = [&](const QString &msg) {
      com.closeDevice(h, nullptr);
      failOpen(msg);
   };

   OphirInfo info;
   info.device_serials = serials;      /* 成功那一份也要带上 (下面整份覆盖 p->info) */
   OphirCom::DeviceInfo dinfo;
   if (!com.getDeviceInfo(h, &dinfo, &err))
   {
      failOpenWithDevice(QStringLiteral("读取表头信息失败: %1").arg(err));
      return;
   }
   info.device_name   = dinfo.name;
   info.device_serial = dinfo.serial;
   info.rom_version   = dinfo.rom;

   /* 手册 Conventions: 先确认通道上真有探头再谈别的 */
   bool sensor_ok = false;
   if (com.isSensorExists(h, k_channel, &sensor_ok, &err) && !sensor_ok)
   {
      failOpenWithDevice(QStringLiteral("表头已连接, 但通道 %1 上未检测到探头。请确认 PD300R 已插在 Juno+ 上。").arg(k_channel));
      return;
   }

   OphirCom::SensorInfo sinfo;
   if (!com.getSensorInfo(h, k_channel, &sinfo, &err))
   {
      failOpenWithDevice(QStringLiteral("读取探头信息失败: %1").arg(err));
      return;
   }
   info.sensor_name   = sinfo.name;
   info.sensor_type   = sinfo.type;
   info.sensor_serial = sinfo.serial;

   /* 这一台的真实型号也记进那张表: 下拉里它那一项立刻从光秃秃的序列号变成
    * "PD300R (s/n: …) · Juno (s/n: …)" (界面在 infoChanged 里重填那一栏)。
    * 表头序列号用**枚举到的那个** pick, 不用 dinfo.serial —— 那一项是用 pick 认的 */
   p->dev_labels.insert(pick, deviceLabel(dinfo.name, pick, sinfo.name, sinfo.serial));

   /* ---- 读当前的波长 / 量程 / 测量模式 / 滤片 ----
    * 只读不改 (手册: 不要拿型号自行推断规格)。这几项对某些探头不适用, 那时 index = -1、
    * options 为空 —— 那是正常的, 不是错。设备给的下标是 COM 的 LONG, 先收进 long。
    *
    * 滤片 (手册 §3.6.4.4) 只对光电二极管探头适用, 别的探头多半回 "Not Applicable" 而
    * 整个 Get 失败 —— 与上面三项一样. 失败就留 -1, 不当错。 */
   auto readOptions = [&]() {
      QString e2;
      long idx = -1;

      info.wavelengths.clear();
      info.ranges.clear();
      info.modes.clear();
      info.filters.clear();
      info.wl_index = info.range_index = info.mode_index = info.filter_index = -1;

      if (com.getWavelengths(h, k_channel, &idx, &info.wavelengths, &e2))
         info.wl_index = (int)idx;
      if (com.getRanges(h, k_channel, &idx, &info.ranges, &e2))
         info.range_index = (int)idx;
      if (com.getMeasurementMode(h, k_channel, &idx, &info.modes, &e2))
         info.mode_index = (int)idx;
      if (com.getFilter(h, k_channel, &idx, &info.filters, &e2))
         info.filter_index = (int)idx;

      /* 单位**在这儿判**, 因为它随模式走: Power 那一档报的是 W, 切到 Energy 同一份数组
       * 就是 J 了。判不出来留空 (= 不明), 界面照原样写「单位不明」 */
      const QString mode_now = (info.mode_index >= 0 && info.mode_index < info.modes.size())
                                  ? info.modes.at(info.mode_index)
                                  : QString();
      info.unit = unitFromDeviceInfo(info.sensor_type, mode_now);
   };
   readOptions();

   /* ---- 两个版本号 (纯诊断) ----
    * 取不到就空着: 一句诊断信息不该把设备挡在门外。GetVersion 给的是个整数, 原样记下来
    * —— 怎么解读是 Ophir 的事, 这里不替它编一个 "x.y.z" 的格式 */
   {
      long ver = 0;
      if (com.getVersion(&ver, nullptr))
         info.com_version = QString::number((qlonglong)ver);
      QString drv;
      if (com.getDriverVersion(&drv, nullptr))
         info.driver_version = drv.trimmed();
   }

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
   /* 名字到这儿才齐 (上面那一句刚把 pick 的写进表) */
   info.device_labels = labelsOf();
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

   /* 设备自己报了滤片状态变化 (status 0x040001), 下一轮开头重读一次 GetFilter。
    * **循环内局部量**: 它只由这一条线程读写, 不跨线程, 所以不进 Private 也不用锁。
    * 置位而不当场重读, 是为了把同一批里来的好几次 0x040001 折成一次 (取数那一趟只看最新的
    * 那一项, 见下面 best 那段) —— 也免得在读数那条路上插一次 COM 往返 */
   bool flt_reload = false;

   while (!p->quit.loadAcquire())
   {
      int wl = -1, rg = -1, md = -1, addwl = -1, flt = -1;
      {
         QMutexLocker<QMutex> lk(&p->mx);
         wl = p->want_wl; rg = p->want_range; md = p->want_mode; addwl = p->want_add_wl;
         flt = p->want_filter;
         p->want_wl = p->want_range = p->want_mode = -1;
         p->want_add_wl = -1;
         p->want_filter = -1;
      }

      if (wl >= 0 || rg >= 0 || md >= 0 || addwl >= 0 || flt >= 0)
      {
         /* 手册: "Configuration methods cannot be called while a channel is streaming" */
         QString e2;
         bool ok = com.stopStream(h, k_channel, &e2);

         /* 自定义波长: 先写进设备, 再**当场把列表读回来核对**。
          * 「写成了什么」只有读回来的那一份表说得清 —— 写成功 ≠ 设备接受了这个值,
          * 而找不到就什么都不改 (不进下拉、不进 CSV): 让一个没生效的波长进文件,
          * 那份文件就开始说假话 (§37) */
         if (ok && addwl >= 0)
         {
            if (addwl < k_wl_min_nm || addwl > k_wl_max_nm)
            {
               ok = false;
               e2 = QStringLiteral("波长超出 %1-%2 nm。请换一个值。").arg(k_wl_min_nm).arg(k_wl_max_nm);
            }
            else if (!com.addWavelength(h, k_channel, addwl, &e2))
            {
               ok = false;               /* e2 已是 COM 的原话 */
            }
            else
            {
               QStringList opts;
               long        idx = -1;
               QString     e3;
               if (!com.getWavelengths(h, k_channel, &idx, &opts, &e3))
               {
                  ok = false;
                  e2 = QStringLiteral("写入后读回波长列表失败: %1").arg(e3);
               }
               else
               {
                  int found = -1;
                  for (int i = 0; i < opts.size(); i++)
                  {
                     if (wavelengthNm(opts.at(i)) == addwl)
                     {
                        found = i;
                        break;
                     }
                  }
                  if (found < 0)
                  {
                     ok = false;
                     e2 = QStringLiteral("设备不接受这个波长。请换一个值。");
                  }
                  else
                     wl = found;         /* 落到下面那句 setWavelength —— 顺手选中它 */
               }
            }
         }

         if (ok && wl >= 0) ok = com.setWavelength(h, k_channel, wl, &e2);
         if (ok && rg >= 0) ok = com.setRange(h, k_channel, rg, &e2);
         if (ok && md >= 0) ok = com.setMeasurementMode(h, k_channel, md, &e2);
         if (ok && flt >= 0) ok = com.setFilter(h, k_channel, flt, &e2);

         if (ok)
            ok = com.startStream(h, k_channel, &e2);

         if (ok)
         {
            /* 新流 = 新时间戳, 水位线作废 */
            watermark = -1.0;
            readOptions();                 /* 设备可能把值夹到它接受的范围内 */
            /* 这一趟的 readOptions() 已经把滤片重读过了, 那个待办作废 */
            flt_reload = false;

            {
               QMutexLocker<QMutex> lk(&p->info_mx);
               const QString summary = buildSummary(info);
               p->info.wavelengths  = info.wavelengths;
               p->info.ranges       = info.ranges;
               p->info.modes        = info.modes;
               p->info.filters      = info.filters;
               p->info.wl_index     = info.wl_index;
               p->info.range_index  = info.range_index;
               p->info.mode_index   = info.mode_index;
               p->info.filter_index = info.filter_index;
               /* 单位跟着模式走: 上面那一句可能刚把它改了 (W <-> J) */
               p->info.unit        = info.unit;
               p->info.summary     = summary;
            }
            emit infoChanged();
         }
         else
         {
            /* 配置没成功, 流是停着的: 得开回去, 否则后面每个点都会超时 */
            QString e3;
            if (!com.startStream(h, k_channel, &e3))
               emit configFailed(QStringLiteral("%1; 重新启动数据流也失败: %2").arg(e2, e3));
            else
               emit configFailed(e2);
         }
      }
      else if (flt_reload)
      {
         /* 设备自己报了滤片状态变化 (0x040001)。**只读滤片这一项**, 不走 readOptions():
          * 那一趟会先把波长/量程/模式清成 -1 再读四项, 一次瞬时 COM 失败就会把三行无关的
          * 界面清空 —— 而 0x040001 只说明滤片变了, 别的项没有任何理由动。
          *
          * 只读不改, 所以**不用停流** (手册那条禁令只针对 Set/Configure 那一族; 同一个
          * 道理: 上面配置成功后那一句 readOptions() 也是在 streaming 时调的)。
          * Get 失败就什么都不做 —— 一个通知不该把屏幕上的档位抹掉。 */
         flt_reload = false;

         long        idx = -1;
         QStringList opts;
         QString     e3;
         if (com.getFilter(h, k_channel, &idx, &opts, &e3))
         {
            /* 值真的变了才发信号。操作员拨一下滤片是低频动作, 这段比的是防"设备连着吐
             * 一串 0x040001"时每拍一次的信号风暴 */
            if (opts != info.filters || (int)idx != info.filter_index)
            {
               info.filters      = opts;
               info.filter_index = (int)idx;

               {
                  QMutexLocker<QMutex> lk(&p->info_mx);
                  p->info.filters      = info.filters;
                  p->info.filter_index = info.filter_index;
                  /* 摘要里写着滤片档位 (buildSummary), 所以它也得跟着重算 */
                  p->info.summary      = buildSummary(p->info);
               }

               emit filterChanged();
            }
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
            emit readingFailed(QStringLiteral("读取功率计失败: ") + e2);
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
                  /* 通知类: 水位线已推过去, 等下一个真正的新数; 故意不清标志。
                   *
                   * **只有一个通知要另作处置**: 0x040001 滤片状态变化 —— 那说明设备自己
                   * 的滤片档位变了 (操作员用手拨的), 屏幕上的那一格得跟着走。置个待办,
                   * 下一轮开头重读 (这里不插 COM 往返: 读数那条路正等着新数) */
                  if (st == 0x040001)
                     flt_reload = true;
               }
               else
               {
                  /* 过量程 / 饱和 / 过热: 手册 "Not every data item represents a valid
                   * measurement" —— 宁可报错也不把这个数记进 CSV */
                  p->want.storeRelease(0);
                  emit readingFailed(QStringLiteral("该读数不可用: %1")
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
                  emit readingFailed(QStringLiteral("功率计 %1 ms 未出新数据 (请确认设备在出数、量程是否正确)。")
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
      emit readingFailed(QStringLiteral("功率计未打开"));
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

void OphirMeter::setFilterIndex(int idx)
{
   if (!isOpen())
      return;
   QMutexLocker<QMutex> lk(&p->mx);
   p->want_filter = idx;
}

}   /* namespace scan */
