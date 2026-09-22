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
 * 本类是三个请求发起方之一 (另外两个是 ScanController 与参数栏那个「读一次」按钮),
 * 谁能让谁, 由 ScanWindow::refresh() 一处仲裁, 靠两个开关推过来:
 *   · setHold(true) —— 「现在不该由我发请求」: 扫描在跑 (跟着扫描显示), 或手动读在飞。
 *                     running() 照旧是 true (还在采), 只是不发、也不计时。
 *   · 外面看到 issuing() 为 true 时不许启扫 (那是"真的在发")。
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
    * **满了就在丢最旧的**, 所以 full() 要显示出来 (见 refreshMeterReadout) */
   static constexpr int kCapacity          = 20000;
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

   /* 扫描在跑时由界面推过来的一个点 (跟随模式)。不进 CSV —— 那些点本来就写在扫描那份 CSV 里,
    * 再写一份就是同一趟数据两个文件, 迟早对不上 */
   void addFollowSample(int64_t ms, double watts);

signals:
   void sampleAdded();                    /* 曲线要重画 / 统计要刷新 */
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
   int     m_nsamp    = 0;      /* 手上这一批已经收到几个 */
   double  m_acc      = 0.0;    /* 手上这一批的和 */
   int64_t m_now_ms   = 0;      /* 最近一次 tick 的读数 */
   int64_t m_due_ms   = 0;      /* 下一次可以发的时刻 */
   int64_t m_sent_ms  = 0;      /* 这一次是什么时候发的 (看门狗用) */

   QVector<Sample> m_v;
   int64_t m_t0 = 0;            /* 第一笔的 ms, 导出时的 elapsed 基准 */

   QStringList m_meta;          /* 写进文件头的 `#` 行 (裸 key=value) */

   QFile  *m_f = nullptr;
   QString m_path;
   QString m_err;
   int     m_written = 0;
};

}   /* namespace scan */
