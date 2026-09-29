/*
 * scan/meterlog.h —— 「连续读数」那件事的全部逻辑: 定周期向取样源要一个数, 收回来进环形
 * 缓冲、算统计、可选边采边写 CSV。**不含界面**, 也不碰总线与 SOEM。
 *
 * 为什么单独一个文件而不是塞进 scanwindow.cpp: 这里全是时序 —— 到点才发、未决期间不许再发、
 * 回话驱动排下一次、超时要记一笔并续采 —— 而时序错一格在外面看不出来 (只会少一个数)。
 * 时钟由外面喂 (tick), 与 ScanController 同一套做法 (它也不持定时器), 于是
 * scan/selftest.cpp 能用手拨的钟把它整轮跑完, 不需要硬件也不需要界面。
 *
 * 它进 SCAN_COMMON_SRC, 只链 Qt6::Core (scanprefs 的先例)。
 *
 * ---- 与 PowerMeter 的约定 ----
 * powermeter.h 写着: **同一时刻只允许一个未决请求**, 违约不报错、只会静默分错数。
 * 那一类"静默分错数"现在**结构上不可能**了: 本类是**唯一的发起方**。
 *   · 2026-09-28 之前是三个 (本类 / ScanController / 参数栏那个「读一次」按钮), 由
 *     ScanWindow::refresh() 一处仲裁"谁让谁"; 那一年删掉了「读一次」。
 *   · **2026-09-29 起扫描也不再自己发请求** —— 它变成这个流的**消费者**: 一个扫描点的值
 *     就是"该点读取时间段里到齐的采样的平均" (见 ScanController::feedMeterSample), 曲线
 *     画的也是这条流。于是"让位"只剩一个理由: 改设备配置 (setHold(true), 工作线程在停流)。
 *     `setHold` 的判据在 ScanWindow::refresh() 一处算, 与从前一样。
 *   · `issuing()` 今天**没有调用方** (那条"真的在发时不许启扫"的闸随「读一次」一起没了),
 *     只有自检在用; 留着是因为它是"这一路真的占着源"的唯一判据。
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

class QFile;

namespace scan {

class PowerMeter;

/*
 * **一次只有一个未决请求, 超时也不例外**。看门狗到点会记一笔 ok=false 并报错, 但**不重新
 * 发请求** —— 那个超时的请求还在源手上, 它迟早会回话; 要是那时已经又发了一个, 那份迟到的
 * 回话就会冒充新请求的答案 (旧数配新时刻, 一个字节的错都不报)。所以超时之后采集停在
 * "等那一个回话"上, 回话一到就记下来并续采; 源彻底卡死就一直停着 (界面上看得见), 靠
 * 停止/开始或换源出来。代价与理由写全在 meterlog.cpp 的 tick() 里。
 */
class MeterLog : public QObject
{
   Q_OBJECT

public:
   static constexpr int kDefaultIntervalMs = 200;
   static constexpr int kMinIntervalMs     = 20;
   static constexpr int kMaxIntervalMs     = 60000;
   /* 一个请求的看门狗。Ophir 那条自己 1800ms 会报 stale, 这里放宽到 3000 让它先开口 */
   static constexpr int kTimeoutMs         = 3000;
   /* 环形缓冲容量。够画一条看得很清楚的曲线, 又不至于把一个长时间段吃光内存。
    * **满了就在丢最旧的**, 所以 full() 要显示出来 (见 refreshMeterReadout)
    *
    * 2026-09-29 起它不再是唯一的丢弃规则: 下面那个时间窗通常**先到** (缺省 5 分钟 ×
    * 缺省间隔 200 ms 只有 1500 点, 离 20000 远着), 于是 full() 在缺省参数下不可达 ——
    * 它是"长窗口 + 短间隔"那一头才用得上的一道兜底 (间隔 20 ms 时 20000 点 ≈ 6.7 分钟,
    * 所以窗口设到 7 分钟以上才轮得到它)。两条规则都在 append **之后**裁, 顺序无关。 */
   static constexpr int kCapacity          = 20000;

   /* 曲线与统计只保留最近这么久 (分钟)。**内存里也丢掉**, 不只是"不画" —— 统计 (`stats()`)
    * 是现算的, 于是它跟着变成"窗内统计"。
    * 上界 120: 缺省间隔下那个窗口已经比 kCapacity 装得下的那一段还长, 所以不必再给一档
    * "不限" (给了也是同一个结果, 却多一个要解释的状态)。 */
   static constexpr int kDefaultWindowMinutes = 5;
   static constexpr int kMinWindowMinutes     = 1;
   static constexpr int kMaxWindowMinutes     = 120;
   /* 一次采样最多平均几次。与扫描的 samples_per_point 是同一件事, 只是这份没有参数面板 */
   static constexpr int kMaxAverage        = 64;

   /* 一个采样。ok = false 是"要了一次没要回来"(超时/源报错), 不是"读到了 0 W" ——
    * 与扫描 CSV 的 ok 列同一个口径, 曲线上画成断点, 不算进统计。
    *
    * 一次采样可能要 N 个读数 (见 setAverage), 那时 watts 是那 N 个的平均, 时刻取**最后一个** */
   struct Sample
   {
      int64_t ms    = 0;     /* 单调钟 (喂进来的那个), 用来画横轴 */
      double  watts = 0.0;
      bool    ok    = false;
   };

   struct Stats
   {
      int    n    = 0;       /* 只数 ok 的 */
      double last = 0.0;     /* 最后一个 ok 的值 */
      double min  = 0.0;
      double max  = 0.0;
      double mean = 0.0;
      double sd   = 0.0;     /* 样本标准差 (除 n-1); n < 2 时是 0 */
   };

   explicit MeterLog(QObject *parent = nullptr);
   ~MeterLog() override;

   /* 换源。未决请求作废 (旧源照旧会回一次, 靠 sender() 认出来丢掉), **缓冲一起清掉** ——
    * 一条曲线上两段数来自两个不同的东西, 而曲线上只看得出一个源的名字, 那不是数据。
    * 传 nullptr = 没有源。 */
   void setSource(PowerMeter *m);
   PowerMeter *source() const { return m_src; }

   /* 开始。interval_ms 夹到 [kMinIntervalMs, kMaxIntervalMs]; 源没打开 -> false + 原因 */
   bool start(int interval_ms, QString *err);
   void stop();

   bool running() const   { return m_run; }
   /* 真的有未决请求在飞 (**含已经超时的那一个** —— 它还在, 见 tick 的说明) */
   bool pending() const   { return m_pending; }
   bool timedOut() const  { return m_timed_out; }
   /* 正在发请求 (= 占着源)。外面据此决定"能不能启扫" */
   bool issuing() const   { return m_run && !m_hold; }
   bool held() const      { return m_hold; }
   int  intervalMs() const { return m_interval; }

   /* 改间隔。跑着时下一拍就用新的 (不打断当前那个未决请求) */
   void setInterval(int ms);

   /* 每个采样平均几次 (1..kMaxAverage, 缺省 1 = 每次都要)。与扫描的 samples_per_point
    * 同一个口径, 连失败的处理也一样: N 次里**有一次**没读回来, 这一笔就记 ok=false ——
    * 拿半边的数求平均是编出来的, 而它在曲线上看着和别的点一模一样。 */
   void setAverage(int n);
   int  average() const { return m_avg; }

   /* 曲线与统计只保留最近多少分钟 (夹到 kMinWindowMinutes..kMaxWindowMinutes)。
    * **改了立刻按新窗口裁一次**并 emit sampleAdded() —— 采集在 setHold 或被停掉时根本不
    * append, 不立刻裁的话屏幕上会**无限期**留着超窗的数据 (统计里还在数它)。
    * 裁剪**不 emit sampled()**: 裁是丢掉, 不是新到一个采样 (拿旧样本去凑扫描点的平均
    * 是另一种错)。 */
   void setWindowMinutes(int min);
   int  windowMinutes() const { return m_window_minutes; }

   /* 让位 / 收回。hold 期间不发也不排下一次; 放开之后从**当时**重新排, 不补采欠下的 */
   void setHold(bool hold);

   /* 手拨时钟。每拍调一次; 只在这一句里决定"发不发" */
   void tick(int64_t now_ms);

   void clear();                          /* 清空缓冲与统计 (文件不动) */

   const QVector<Sample> &samples() const { return m_v; }
   int   count() const { return (int)m_v.size(); }
   /* 环形缓冲满了 —— 再采一笔就要丢掉最旧的那一个。**这件事必须让人看见**: 屏幕上
    * 那条曲线看着照旧很健康, 只是它已经不完整了 */
   bool  full() const { return m_v.size() >= kCapacity; }
   Stats stats() const;

   /* 写文件时先写的那几行 (`#` 开头) 的内容。裸 key=value, 由本类加 "# " —— 与扫描那份
    * CSV 的 extra_meta 同一个格式 (scanlog.h)。谁采的就该记下是谁采的: 关于探头/波长/量程/
    * 模式/单位/平均次数, 这里一个都不留白, 否则数据回头没法复核。
    * 由界面拼 (meterMetaLines + 间隔/平均次数), 本类不猜里面该有什么。 */
   void setMeta(const QStringList &lines);
   const QStringList &meta() const { return m_meta; }

   /* 边采边写: 建文件 (父目录自动建) + meta 行 + 表头, 之后每个采样追加一行并 flush。
    * 崩了/断电只丢最后一个数, 与 ScanLog 同一个取舍。失败不抛, 返回 false + 原因。 */
   bool beginRecord(const QString &path, QString *err);
   void endRecord();
   bool recording() const;
   QString recordPath() const;
   QString lastError() const { return m_err; }
   int     written() const   { return m_written; }

   /* 把当前缓冲整份导出一份 (不打断正在进行的记录, 也不改 m_f) */
   bool saveBuffer(const QString &path, QString *err) const;

   /* 表头/行格式就这两个, 自检逐字比对。都是 C 区域, 与 scanplan.cpp 的 csvRowLine 同口径 */
   static QString csvHeaderLine();
   static QString csvRowLine(const Sample &s, int64_t t0);

   /* 外部塞一个点到缓冲里 (不经过取样源)。
    * 2026-09-29 起**扫描那条路不走它了** —— 曲线从此画的就是上面这条连续读数的流本身,
    * 扫描点是它的**消费者** (ScanController::feedMeterSample), 不再往这里塞第二份。
    * 留着是因为它仍是"不碰硬件也能往缓冲里放数"的唯一入口 (自检靠它把时间窗那一圈跑完),
    * 与 setAverage 同一个"保留但不露"的处置。
    * 不走 CSV —— 塞进来的点本来就不在这份采集的账上。 */
   void addFollowSample(int64_t ms, double watts);

signals:
   void sampleAdded();                    /* 曲线要重画 / 统计要刷新 */
   /* 真的有一笔新采样进了缓冲。**与 sampleAdded() 不是一件事**: 那一个是"屏幕上该重画了"
    * (清空、裁窗也喊), 这一个只跟着"新到一笔"走 —— 扫描点那笔平均就从这儿取。
    *
    * 于是裁窗与 clear() **都不许喊它**: 拿一批旧样本 (还是假的 ok=true) 去凑一个扫描点,
    * 是安静地出一个错的数。 */
   void sampled(int64_t ms, double watts, bool ok);
   void stateChanged();                   /* running / hold / 记录开关变了 (按钮可用性) */
   /* 出错了, 原文在 err 里, 同时也留在 lastError()。
    * 两种情形共用这一个信号: 一次读没要回来 (已经记成 ok=false 的样本了), 或 CSV 写不进去
    * (采集不停, 但那份文件不再可信) —— 两种都得让人看见, 却都不该打断采集。 */
   void failed(const QString &err);

private slots:
   void onReady(double watts);
   void onFailed(const QString &err);

private:
   /* 记一笔 (成功或失败), 然后排下一次。now 一律取最近一次 tick 喂进来的那个
    * —— 时钟是外面的, 槽里没有别的来源 */
   void record(const Sample &s);
   /* 按 m_window_ms 丢掉太旧的那些 (基准取缓冲自己的最新一笔)。**只在 m_v.append(s) 之后
    * 调** —— 那两句注释里的两个坑 (拿刚到的这一笔当基准会清空缓冲并把 m_t0 搬走; 写在
    * append 之前会让超窗的旧数多留一拍) 都在 meterlog.cpp 里写全了 */
   void trimToWindow();
   bool appendCsv(const Sample &s);
   QString metaBlock() const;          /* m_meta 那几行 + "# " 前缀 + 换行 */
   void resetBatch();                  /* 丢掉手上这一批还没凑够的读数 */
   static int clampInterval(int ms);

   PowerMeter *m_src = nullptr;

   bool m_run       = false;
   bool m_pending   = false;   /* 有一个请求在飞 (含已超时的那一个) */
   bool m_timed_out = false;   /* 这一个已经抱怨过了, 别再每拍喊一次 */
   bool m_hold      = false;

   int     m_interval = kDefaultIntervalMs;
   int     m_avg      = 1;      /* 一次采样平均几个读数 */
   int     m_window_minutes = kDefaultWindowMinutes;
   int64_t m_window_ms      = (int64_t)kDefaultWindowMinutes * 60000;
   int     m_nsamp    = 0;      /* 手上这一批已经收到几个 */
   double  m_acc      = 0.0;    /* 手上这一批的和 */
   int64_t m_now_ms   = 0;      /* 最近一次 tick 的读数 */
   int64_t m_due_ms   = 0;      /* 下一次可以发的时刻 */
   int64_t m_sent_ms  = 0;      /* 这一次是什么时候发的 (看门狗用) */

   QVector<Sample> m_v;
   /* 这场采集第一笔的 ms, CSV 与导出那份的 elapsed 基准。**裁窗一个字都不动它** ——
    * 裁窗是"缓冲从哪一刻起", 它是"这场采集从哪一刻起", 两者只有裁之前才相等 (见 trimToWindow) */
   int64_t m_t0 = 0;

   QStringList m_meta;          /* 写进文件头的 `#` 行 (裸 key=value) */

   QFile  *m_f = nullptr;
   QString m_path;
   QString m_err;
   int     m_written = 0;
};

}   /* namespace scan */
