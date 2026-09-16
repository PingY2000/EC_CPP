/*
 * scan/scancontroller.h —— 扫描状态机
 *
 * 它是 scan 的**主循环**, 但和 hmi 一样只活在 GUI 线程里: 只通过 BusView 下发目标、
 * 读遥测快照, 一根总线都不碰。所以 2ms 的 CSP 插补循环一行不用改, 也不用新加锁。
 *
 * 节拍是**外部喂的** —— `tick(now_ms)`, 由界面的 30Hz QTimer 驱动, 而不是自己起定时器。
 * 这么做只有一个理由: 单测里换一个手动时钟, 整个状态机就能在**没有硬件**的情况下跑通
 * (见 scan/selftest.cpp)。时间一律用单调钟 (QElapsedTimer), 不用墙上时间 ——
 * 墙上时间会被 NTP 和对时往回拨, 那样"停留了 200ms"可能永远不成立。
 *
 * ── 状态 ──────────────────────────────────────────────────────────────
 *
 *    Idle ──开始──▶ Moving ──到位且稳──▶ Dwelling ──停够──▶ Reading ──采到──┐
 *                     ▲                                                      │
 *                     └──────────────── 下一个点 ◀──────────────────────────┘
 *                     全部走完 ▶ Done
 *
 *    Paused / Aborted 可以从上面任意一个状态进来, 且**立刻生效**。
 *
 * **没有单独的 Settling 状态**: "稳定 settle_ms" 这件事由 ArrivalJudge 的窗口实现,
 * 它就长在 Moving 里。界面想问"现在是在走还是在等稳定", 用 settlingNow() 读,
 * 而不是让状态机多一个状态、多一套进出条件 —— 那样的状态迁移分支是纯粹的自找麻烦。
 *
 * ── 自动中止 ──────────────────────────────────────────────────────────
 *
 * 扫描是**一次一两小时、没人看着**的长动作。所以下面这些条件在**每个 tick** 都查,
 * 命中就立刻自动中止 (而不是继续默默往下扫, 把一整轮数据全采在错的地方):
 *
 *   · 掉出 OP / 连接断了
 *   · 任一轴 6041h bit3 故障  (EcatThread 已经会冻结目标, 但扫描必须停下来)
 *   · 任一轴 6041h bit11 撞硬件限位 (2310h X1/X2) —— 这一条最可能真触发
 *   · WKC 连续若干帧不足 (拔网线就是这个表现)
 *   · 任一轴丢帧 (mirror_ok 掉) 或掉使能
 *   · 有人从**旁路**改了目标 (画布点击 / 回中) —— 扫描期间不允许, 见 externalWantChanged()
 *   · CSV 写失败 (宁可停, 也不要默默少采几千点)
 */
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "busview.h"
#include "powermeter.h"
#include "scanarrive.h"
#include "scanlog.h"
#include "scanplan.h"

namespace scan {

/* 6041h bit11 = Internal limit active。2310h 实测 X1 = 正限位 / X2 = 负限位,
 * 撞上就是这一位置起来。见 docs/ykd2205pe_ci402.md:366-393 */
#define SCAN_LIMIT_BIT   0x0800u

/* 下发一个目标之后, 最多等它出现在遥测里多久 (ms)。见 externalWantChanged() */
#define WANT_CONFIRM_MS   200

class ScanController : public QObject
{
   Q_OBJECT

public:
   enum class State
   {
      Idle,      /* 没在跑; 参数可改 */
      Moving,    /* 已下发目标, 等两轴到位且稳 (含 settle 窗口) */
      Dwelling,  /* 到了, 停 dwell_ms */
      Reading,   /* 已发出读数请求, 等信号回来 */
      Paused,    /* 冻住, 保持力矩; 继续时会重新走完当前点 */
      Aborted,   /* 停住且不打算继续 */
      Done       /* 全部点跑完 */
   };
   Q_ENUM(State)

   /* bus 与 meter 都**不归它管**, 生命周期由窗口负责, 且必须活过本对象 */
   ScanController(BusView *bus, PowerMeter *meter, QObject *parent = nullptr);

   /* 换一个取样源 (界面上的下拉框)。**析构时旧的连接会自动断掉** (QObject 的信号连接
    * 在发送方销毁时断开), 但这里得显式断一次 —— 旧源还活着, 不disconnect 的话它下次
    * 出数会直接喂进状态机, 于是"换过源"这件事在下一点的数据上才被发现。
    * 不允许在跑的时候换, 由窗口把关 (见 ScanWindow::onMeterChanged)。 */
   void setMeter(PowerMeter *meter);

   /* ---------------------------------------------------------- 参数 */

   /* 存下来并重算网格。**不做校验** —— 操作员正在输入时中间态一定是不合法的
    * (把 27 改成 2 之前会先变成空), 每次按键都拒一次没法用。
    * 合不合法由 paramsError() 说, **真正的关口在 start()**。 */
   void setParams(const Params &p);
   const Params &params() const { return m_p; }

   /* validate() 的话; 空 = 可以开始 */
   QString paramsError() const;

   /* 参数变过之后重算网格与预计时长。界面在**按开始之前**就得看见这一趟多长。
    * **几何没变就不动**(不清结果): 跑完 80 分钟刚拿到一张热力图, 手一滑碰到速度框
    * 就把它抹掉, 那是纯粹的破坏。 */
   void rebuildPlan();

   int     gridNx() const { return m_nx; }
   int     gridNy() const { return m_ny; }
   int     totalPoints() const { return (int)m_plan.size(); }
   int64_t estimateTotalMs() const;

   /* ---------------------------------------------------------- 进度 */

   State state() const { return m_st; }
   bool  running() const;              /* Moving/Dwelling/Reading/Paused */
   QString stateText() const;          /* 状态栏那一行中文 */

   /* 正在走的点在 m_plan 里的下标; 不在跑时 -1 */
   int index() const { return m_cur; }
   const Point *currentPoint() const;  /* 不在跑时 nullptr */
   const Point *pointAt(int k) const;  /* k 是 m_plan 下标 */

   /* 已经有数据的格子数 (含采失败的点 —— 那些也算走过, 不该重扫) */
   int completedPoints() const;
   int pendingPoints() const { return m_order.empty() ? 0 : (int)m_order.size() - m_ord_i; }
   int64_t elapsedMs() const { return m_run_ms >= 0 ? m_now_ms - m_run_ms : 0; }

   /* 本状态已经待了多久 (界面拿它做 "等稳定 1.2s" 这种提示) */
   int64_t stateMs() const { return m_state_ms >= 0 ? m_now_ms - m_state_ms : 0; }

   /* 正在 Moving 里开稳定窗口 = 机械已经进容差了, 在等 settle_ms。界面提示用 */
   bool settlingNow() const;

   /* ---------------------------------------------------------- 结果 */

   bool  cellDone(int ix, int iy) const;
   bool  cellHasValue(int ix, int iy) const;
   double cellValue(int ix, int iy) const;   /* 该格**最后一次**采到的功率 */
   const std::vector<char>   &doneMask()  const { return m_done; }
   const std::vector<double> &valueGrid() const { return m_watts; }

   /* 已采数据的功率范围。热力图的色标用它 —— 空/失败的点不算数。
    * 返回 false = 一点都还没采到 */
   bool wattsRange(double *lo, double *hi) const;

   /* ---------------------------------------------------------- 控制 */

   /* 开始新一轮: 新建 CSV (父目录自动建)。过不了 Preflight 就返回 false 并说明 */
   bool start(const QString &csv_path, QString *err);

   /* 断点续扫: 读一个已有 CSV, 校验几何一致 → 只补没采过的点 → **继续追加同一个文件**
    * zero_epoch 不一致时 (中间重连过、零点可能变过) 返回 false, why 里要求操作员确认 */
   bool resume(const QString &csv_path, bool accept_zero_epoch_change,
               QString *err, QString *why);

   /* 单点重测: 走回 (ix,iy) 再采一次, 追加一行 flags=retest。
    * 要求已经有一轮在跑过 (否则没有文件可追加), 且几何参数没被改过 */
   bool retest(int ix, int iy, QString *err);

   void pause();
   void resumeRun();
   void abort(const QString &why);

   /* 外部驱动: 每 tick 喂一次单调钟的毫秒数。**这也是自动中止的唯一检查点** */
   void tick(int64_t now_ms);

   /* CSV 写失败/文件路径, 给状态栏显示 */
   QString csvPath() const { return m_log.path(); }
   int     csvLines() const { return m_log.written(); }

   /*
    * 零点世代。**由窗口维护**, 每次"零点重新确立"就 +1 —— 也就是每次连接、
    * 每次按「设为区域中心」。它只做一件事: 进 CSV 表头, 续扫时对不上就要求操作员确认。
    *
    * 为什么需要它: 连接时 `tryInitOrigin()` 会把零点重设为"当时所在的位置"。
    * 所以断线重连之后, 同一个显示坐标指的**可能已经是另一个物理位置** ——
    * 那下半场的数据会和上半场拼在一张图上, 而图上不会有任何异常的样子。
    */
   void setZeroEpoch(int epoch) { m_zero_epoch = epoch; }
   int  zeroEpoch() const { return m_zero_epoch; }

signals:
   void stateChanged();
   void pointLogged(int ix, int iy, bool ok);
   void runFinished(bool complete);
   /* 自动中止: 界面必须弹**红色横幅**, 不是一闪而过的提示 */
   void autoAborted(const QString &why);

private slots:
   void onReadingReady(double watts);
   void onReadingFailed(const QString &err);

private:
   /* ---- 内部动作 ---- */
   bool armRun(QString *err);           /* Preflight + 进第一个点 */
   void startPoint(int plan_index);
   void beginReading();
   void sendReading();
   void finishPoint(bool ok, const std::string &flags);
   void advance();                      /* 当前点收尾 → 下一个点 / Done */
   void enter(State s);
   void stopMotion();                   /* postStop + 丢掉未决读数 */
   void abortInternal(const QString &why, bool automatic);

   /* 每 tick 的安全检查。返回空串 = 健康; 非空 = 原因 (调用方据此自动中止) */
   QString healthProblem(const BusTelem &t);
   /* 有人从旁路改了目标 (扫描期间不允许) */
   bool externalWantChanged(const BusTelem &t, int axis, int32_t *seen);

   /* ---- 注入 ---- */
   BusView    *m_bus   = nullptr;
   PowerMeter *m_meter = nullptr;

   /* ---- 配置 ---- */
   Params  m_p;
   Params  m_plan_p;             /* 当前网格是照哪份参数建的 —— 见 rebuildPlan() */
   Params  m_run_p;              /* 本轮开始时的一份拷贝 —— 盘中改过参数时要能查出来 */
   int     m_nx = 0, m_ny = 0;
   std::vector<Point> m_plan;

   /* ---- 本轮 ---- */
   State   m_st = State::Idle;
   bool    m_is_retest = false;  /* 本轮是单点重测: 走完这一个点就 Done */
   std::vector<int>   m_order;   /* m_plan 的下标序列 (续扫时已滤掉采过的) */
   size_t  m_ord_i = 0;
   int     m_cur   = -1;         /* m_plan 下标; -1 = 不在点上 */

   /* ---- 每个点的过程数据 ---- */
   ArrivalJudge m_judge[EM_MAX_AXES];
   int      m_nsamp = 0;
   double   m_acc   = 0.0;
   bool     m_pending = false;   /* 读数请求已发出、还没回 */
   int64_t  m_sent_ms = 0;
   int32_t  m_spread[EM_MAX_AXES] = {0, 0};
   int32_t  m_issued[EM_MAX_AXES] = {0, 0};   /* 我们最后下发的目标 (显示坐标) */
   int64_t  m_issued_ms[EM_MAX_AXES] = {0, 0};
   /* 已经亲眼看到 EcatThread 把我们的目标登上了 (下发与 publish 之间差一个 2ms 周期,
    * 不等它出现就比, 会把**自己**当成外部干预)。见 externalWantChanged() */
   bool     m_want_ok[EM_MAX_AXES] = {false, false};

   /* ---- 结果 ---- */
   std::vector<char>   m_done;   /* 走过 (含采失败) */
   std::vector<char>   m_have;   /* 采到了数 */
   std::vector<double> m_watts;  /* 该格最后一次的功率 */

   ScanLog m_log;

   /* ---- 时间 ---- */
   int64_t m_now_ms      = 0;    /* 最近一次 tick 的单调钟 */
   int64_t m_state_ms    = -1;   /* 进入当前状态的时刻 */
   int64_t m_run_ms      = -1;   /* 本轮第一次下发的时刻 (CSV 的 elapsed_ms 基准) */
   int64_t m_deadline_ms = 0;    /* Dwelling 截止 / Reading 超时 */
   int64_t m_move_deadline_ms = 0;  /* 走到位的最后期限 —— 卡住/被夹住时的兜底 */

   /* ---- 自动中止的抖动抑制 ---- */
   int     m_bad_wkc = 0;
   /* CSV 表头里记的零点世代。续扫时对不上 = 中途重连过, 零点可能已经不是同一个物理位置 */
   int     m_zero_epoch = -1;
};

}   /* namespace scan */
