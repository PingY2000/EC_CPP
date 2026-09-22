/*
 * scan/scancontroller.h —— 扫描状态机: 只活在 GUI 线程里 (不碰总线, 不用加锁), 节拍由外部
 * 30Hz QTimer 喂 tick(now_ms), 时间一律用单调钟 (QElapsedTimer)。
 * 状态 Idle/Moving/Dwelling/Reading → 下一个点 / Done; Paused / Aborted 可从任意状态进入。
 * 自动中止的条件每个 tick 都查 (含暂停中), 见 healthProblem() 与 externalWantChanged()。
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

/* 撞限位不在这里判定: 读 AxisTelem::limit_active 就够了, 它由 ecatcmd::limit_hit()
 * 一处算出 (6041h bit11, 见 ecatworker.h 的 LimitRule)。中止只由它决定,
 * 开关本身压没压着是另一回事 (看 60FDh 的 dig_home/dig_pos/dig_neg)。 */

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
      Reading,
      Paused,    /* 冻住, 保持力矩; 继续时会重新走完当前点 */
      Aborted,
      Done       /* 全部点跑完 */
   };
   Q_ENUM(State)

   /* bus 与 meter 不归它管, 由窗口负责, 且必须活过本对象 */
   ScanController(BusView *bus, PowerMeter *meter, QObject *parent = nullptr);

   /* 换一个取样源。旧的必须显式 disconnect (旧源还活着, 它下次出数会直接喂进状态机);
    * 不允许在跑的时候换, 由窗口把关。 */
   void setMeter(PowerMeter *meter);

   /* 存下来并重算网格, 不做校验 (输入中间态一定不合法); 关口在 start() */
   void setParams(const Params &p);
   const Params &params() const { return m_p; }

   /* validate() 的话; 空 = 可以开始 */
   QString paramsError() const;

   /* 重算网格与预计时长。几何没变就不动 —— 不清掉刚采完的结果 */
   void rebuildPlan();

   int     gridNx() const { return m_nx; }
   int     gridNy() const { return m_ny; }
   int     totalPoints() const { return (int)m_plan.size(); }
   int64_t estimateTotalMs() const;

   State state() const { return m_st; }
   bool  running() const;              /* Moving/Dwelling/Reading/Paused */
   QString stateText() const;          /* 状态栏那一行中文 */

   /* 正在走的点在 m_plan 里的下标; 不在跑时 -1 */
   int index() const { return m_cur; }
   const Point *currentPoint() const;  /* 不在跑时 nullptr */
   const Point *pointAt(int k) const;  /* k 是 m_plan 下标 */

   /* 已经有数据的格子数 (含采失败的点 —— 它们也算走过) */
   int completedPoints() const;
   int pendingPoints() const { return m_order.empty() ? 0 : (int)m_order.size() - m_ord_i; }
   int64_t elapsedMs() const { return m_run_ms >= 0 ? m_now_ms - m_run_ms : 0; }

   /* 本状态已经待了多久 (界面拿它做 "等稳定 1.2s" 这种提示) */
   int64_t stateMs() const { return m_state_ms >= 0 ? m_now_ms - m_state_ms : 0; }

   /* 正在 Moving 里开稳定窗口 = 机械已经进容差了, 在等 settle_ms。界面提示用 */
   bool settlingNow() const;

   bool  cellDone(int ix, int iy) const;
   bool  cellHasValue(int ix, int iy) const;
   double cellValue(int ix, int iy) const;   /* 该格**最后一次**采到的功率 */
   const std::vector<char>   &doneMask()  const { return m_done; }
   const std::vector<double> &valueGrid() const { return m_watts; }

   /* 已采数据的功率范围 (热力图色标用), 空/失败的点不算数; false = 一点都没采到 */
   bool wattsRange(double *lo, double *hi) const;

   /* 开始新一轮: 新建 CSV (父目录自动建)。过不了 Preflight 就返回 false 并说明 */
   bool start(const QString &csv_path, QString *err);

   /* 断点续扫: 读一个已有 CSV, 校验几何一致 → 只补没采过的点 → 继续追加同一个文件。
    * zero_epoch 不一致时 (中间重连过、零点可能变过) 返回 false, why 里要求操作员确认 */
   bool resume(const QString &csv_path, bool accept_zero_epoch_change,
               QString *err, QString *why);

   /* 单点重测: 走回 (ix,iy) 再采一次, 追加一行 flags=retest。
    * 要求已经有一轮在跑 (否则没有文件可追加), 且几何参数没被改过 */
   bool retest(int ix, int iy, QString *err);

   void pause();
   void resumeRun();
   void abort(const QString &why);

   /* 外部驱动: 每 tick 喂一次单调钟的毫秒数。**这也是自动中止的唯一检查点** */
   void tick(int64_t now_ms);

   /* CSV 写失败/文件路径, 给状态栏显示 */
   QString csvPath() const { return m_log.path(); }
   int     csvLines() const { return m_log.written(); }

   /* 零点世代: 零点真被搬过一次就 +1。**由工作线程计, 窗口从遥测同步** (只许往前), 这里只收。
    * 只进 CSV 表头, 续扫时对不上就要求操作员确认。
    * **连接不再推它** (2026-09-22 改): scan 现在跨重连沿用零点, 连接本身不动零点; 真搬零点的
    * 是回零 / 「设为区域中心」/ 第一次取零点 / 沿用不了只好重取, 那四处都在工作线程里。 */
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

   BusView    *m_bus   = nullptr;
   PowerMeter *m_meter = nullptr;

   Params  m_p;
   Params  m_plan_p;             /* 当前网格是照哪份参数建的 —— 见 rebuildPlan() */
   Params  m_run_p;              /* 本轮开始时的一份拷贝 —— 盘中改过参数时要能查出来 */
   int     m_nx = 0, m_ny = 0;
   std::vector<Point> m_plan;

   State   m_st = State::Idle;
   bool    m_is_retest = false;  /* 本轮是单点重测: 走完这一个点就 Done */
   std::vector<int>   m_order;   /* m_plan 的下标序列 (续扫时已滤掉采过的) */
   size_t  m_ord_i = 0;
   int     m_cur   = -1;         /* m_plan 下标; -1 = 不在点上 */

   ArrivalJudge m_judge[EM_MAX_AXES];
   int      m_nsamp = 0;
   double   m_acc   = 0.0;
   bool     m_pending = false;   /* 读数请求已发出、还没回 */
   int64_t  m_sent_ms = 0;
   int32_t  m_spread[EM_MAX_AXES] = {0, 0};
   int32_t  m_issued[EM_MAX_AXES] = {0, 0};   /* 最后下发的目标 (显示坐标) */
   int64_t  m_issued_ms[EM_MAX_AXES] = {0, 0};
   /* 已经看到 EcatThread 把目标登上了 (下发与 publish 相隔一个 2ms 周期) */
   bool     m_want_ok[EM_MAX_AXES] = {false, false};

   std::vector<char>   m_done;   /* 走过 (含采失败) */
   std::vector<char>   m_have;   /* 采到了数 */
   std::vector<double> m_watts;  /* 该格最后一次的功率 */

   ScanLog m_log;

   int64_t m_now_ms      = 0;    /* 最近一次 tick 的单调钟 */
   int64_t m_state_ms    = -1;   /* 进入当前状态的时刻 */
   int64_t m_run_ms      = -1;   /* 本轮第一次下发的时刻 (CSV 的 elapsed_ms 基准) */
   int64_t m_deadline_ms = 0;    /* Dwelling 截止 / Reading 超时 */
   int64_t m_move_deadline_ms = 0;  /* 走到位的最后期限 —— 卡住/被夹住时的兜底 */

   int     m_bad_wkc = 0;
   /* CSV 表头里记的零点世代; 续扫时对不上 = 中途重连过 */
   int     m_zero_epoch = -1;
};

}   /* namespace scan */
