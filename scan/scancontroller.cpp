#include "scancontroller.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scan {

/* `# started=` 与每一行的 `time_local` 共用的**同一个**格式串 (2026-09-30)。
 * 两处必须逐字相同: 一个是"这趟几点开始的", 另一个是"这一点几点采的", 格式不一样就得对表。
 * 都是 QDateTime::currentDateTime() 来的**本机本地时间、不带时区后缀**。 */
static QString isoFmt()
{
   return QStringLiteral("yyyy-MM-ddTHH:mm:ss");
}

static QString isoOf(const QDateTime &t)
{
   return t.toString(isoFmt());
}

static QString isoNow()
{
   return isoOf(QDateTime::currentDateTime());
}

static bool fail(QString *err, const QString &msg)
{
   if (err != nullptr)
      *err = msg;
   return false;
}

static QString fromStd(const std::string &s)
{
   return QString::fromStdString(s);
}

/* 下发值可能被 EcatThread::setTarget 按量程夹过, 比较要用夹过的值 */
static int32_t asClamped(int32_t want, int32_t range)
{
   if (range <= 0)
      return want;
   if (want >  range) return  range;
   if (want < -range) return -range;
   return want;
}

static ArrivalObs obsOf(const BusTelem &t, int i)
{
   ArrivalObs o;
   o.in_op     = t.in_op;
   o.valid     = t.ax[i].valid;
   o.mirror_ok = t.ax[i].mirror_ok;
   o.enabled   = t.ax[i].enabled;
   o.fault     = t.fault || t.ax[i].fault;
   o.at_target = t.ax[i].at_target;
   o.pos       = t.ax[i].pos;
   return o;
}

ScanController::ScanController(BusView *bus, PowerMeter *meter, QObject *parent)
   : QObject(parent), m_bus(bus), m_meter(meter)
{
   /* 没有 connect: 本类不再向功率计发请求, 收数走 feedMeterSample()
    * (那条线由窗口把 MeterLog::sampled 接过来 —— 2026-09-29, 见头文件) */

   rebuildPlan();
}

void ScanController::setParams(const Params &p)
{
   m_p = p;
   rebuildPlan();
}

QString ScanController::paramsError() const
{
   return fromStd(validate(m_p));
}

void ScanController::setLimitLines(const limitguard::LimitAxis *ax)
{
   /* 整份换掉, 不是逐条改 —— 推过来的那一份永远比这里的新 (界面是主)。传 nullptr 就是
    * "一条都没有", 与那时候的操作员意图 (刚回过零) 一致。 */
   for (int i = 0; i < 2; i++)
      m_lim[i] = (ax != nullptr) ? ax[i] : limitguard::LimitAxis();
}

/* 开扫前把点列按**这次的走法**重排一次 (2026-10-11)。start() 与 loadResume() 各调一次。
 *
 * rebuildPlan() 只在**几何**变了时重建点列, 走法变了它只重排不重建 (见它开头那一支) ——
 * 于是到这里手里那份点列是"按这种走法"的基准顺序, 但**不是随机的那个顺序**: 随机那两种
 * 每轮得现洗一遍, 而且**每次开扫换一个新种子** (同一片区域连着跑两轮, 顺序本来就该不一样,
 * 这正是用户要的"每次随机一个点")。
 *
 * 洗的是 m_plan 本身, 不动 m_nx/m_ny 与三个结果数组: 热力图按 (ix, iy) 索引, 与顺序无关,
 * 而"上一轮图上还留着格子"是既有的、与本轮无关的行为 (§45 末尾那条), 这里不顺手改它。
 * 按行的两种一个字都不做 —— 它们的顺序是算出来的, 洗它没有意义。 */
void ScanController::prepareRunPlan()
{
   if (!modeRandom(m_p.mode) || m_plan.empty())
      return;

   m_seed   = QRandomGenerator::global()->generate();
   m_plan   = buildPlan(m_p);
   shufflePlan(&m_plan, m_seed++);      /* ++ : 第一遍循环里再洗时用的是另一个子种子 */
   m_plan_p = m_p;
}

void ScanController::rebuildPlan()
{
   /* 几何没变就什么都别动: 网格还是那个网格, 已有结果还是对的 */
   if (m_nx > 0 && !m_plan.empty() && sameGeom(m_p, m_plan_p))
   {
      /* 但**走法**变了要把点列重排一次 (2026-10-11)。几何不变就整段跳过的话, 屏幕上写着
       * 「逐行往返」而点列还是上一轮随机那个顺序 —— 画布上那条预览折线当场就露馅。
       * 重排**只换顺序**: 网格尺寸与三个结果数组一个都不动, 于是"上一轮跑完了图上还留着
       * 上一轮的格子"这条既有行为原样保留 (它有自己的账, 见 docs/scan_sweep.md §45)。
       *
       * 「起始方向」不在这里比 —— 它也是"走法"的一部分, 但它改了之后要不要重排是 §45 末尾
       * 那条既有取舍, 本轮不顺手改 (本轮的账: 只有 mode)。 */
      if (m_p.mode != m_plan_p.mode)
      {
         m_plan   = buildPlan(m_p);
         m_plan_p = m_p;
      }
      return;
   }

   const int nx = axisCount(m_p.area_x_unit, m_p.res_unit);
   const int ny = axisCount(m_p.area_y_unit, m_p.res_unit);
   const long long total = (long long)std::max(0, nx) * (long long)std::max(0, ny);

   /* 点数上限的闸在这里 (不是 validate() 那句提示): 参数是边输边算的, 超过上限时一个
    * Point 都不能建, 否则分配几 GB 把界面卡死。网格当成 0×0 并把三个结果数组一起清掉 ——
    * 它们按 nx*ny 索引 (cellDone 那条 m_done[iy*m_nx+ix]), 长度对不上就是越界读。 */
   if (nx <= 0 || ny <= 0 || total > kMaxPlanPoints)
   {
      m_nx = m_ny = 0;
      m_plan.clear();
      m_plan_p = m_p;
      m_done.clear();
      m_have.clear();
      m_watts.clear();
      return;
   }

   m_nx   = nx;
   m_ny   = ny;
   m_plan = buildPlan(m_p);
   m_plan_p = m_p;

   const size_t n = (size_t)m_nx * (size_t)m_ny;
   m_done.assign(n, 0);
   m_have.assign(n, 0);
   m_watts.assign(n, 0.0);
}

/* 一轮 (m_plan 一个来回) 的估计。**「随机 (可重复)」下它仍然只是"一轮"**: 那一轮不会
 * 自己结束, 所以界面那一行写的是「全程: 不限」, 不拿它当整轮耗时用 (scanwindow.cpp)。 */
int64_t ScanController::estimateTotalMs() const
{
   return (int64_t)m_plan.size() * estimatePerPointMs(m_p);
}

int ScanController::completedPoints() const
{
   return (int)std::count(m_done.begin(), m_done.end(), (char)1);
}

bool ScanController::running() const
{
   return m_st == State::Moving || m_st == State::Dwelling
       || m_st == State::Reading || m_st == State::Paused;
}

/* 为什么 Loaded **不在** running() 里 (2026-09-29)。
 *
 * 它是"归族"问题, 不是"少改几处": 装载态**没有扫描循环**, 与 Idle 同类 —— 手动那条路
 * (回零 / 使能 / 画布点动) 在装载态本来就是活的, 与空闲态一模一样。塞进 running() 会立刻
 * 长出两处错: tick() 开始跑 healthProblem() (驱动器报一个限位位就自动中止一次**根本没跑过**
 * 的扫描), pause()/abort() 的语义变成"暂停/中止了一轮不存在的扫描"。
 *
 * 代价是"装载态"这件事要在各判据处**显式点名** (窗口那几处按钮、refreshEditability、
 * refreshMeterPanel、stateText 与 tick 的两个 switch)。换来的是 tick/pause/resumeRun/abort
 * 那几条路一个字都不用动。 */

void ScanController::clearResults()
{
   /* 尺寸不变 (m_done.size() 就是 nx*ny): cellDone() 是按 m_nx 索引进来的, clear() 会让
    * 一个本该为假的判断变成越界读 */
   m_done.assign(m_done.size(), 0);
   m_have.assign(m_have.size(), 0);
   m_watts.assign(m_watts.size(), 0.0);
}

/* 把"当前这一轮"整个收掉。两个调用方: abortInternal 的装载那一支与 newFile()。
 * 不碰状态 (enter(Idle) 由调用方来), 也不碰 m_plan / m_nx / m_ny —— 网格尺寸要留着,
 * clearResults() 那一份注释说了为什么。 */
void ScanController::resetRunState()
{
   m_log.close();
   m_order.clear();

   /* 这三行是"让不可能变成不可能", 不是顺手 (2026-09-30): pendingPoints() 是
    * m_order.size() - m_ord_i, m_order 空了而 m_ord_i 不是 0 就是**负数** —— 而从这一轮起
    * 它成了「继续」与「重测选中点」两个按钮的判据 (见 scanwindow.cpp 的 refresh)。
    * 装载那一支从前没重置它们, 当时没事 (loadResume 刚清零过), 但那靠的是调用顺序。 */
   m_ord_i      = 0;
   m_run_ms     = -1;
   m_load_epoch = -1;

   m_cur       = -1;
   m_is_retest = false;
   m_endless   = false;      /* 这一轮收掉了, endless() 不许对着一轮过去的事点头 (见 advance) */

   clearResults();

   /* 当前那份文件已经不再是要写的目标了 —— 窗口那个路径框得换名, 否则下一次「开始扫描」
    * 会把它截断 (见 takeStaleCsvName) */
   m_stale_csv_name = true;
}

bool ScanController::newFile(QString *err)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中, 请先「中止」。"));

   /* **不复用 abortInternal**: 它的第三个分支 (中止一轮) 刻意不关文件 —— 那是为了让中止之后
    * 还能单点重测; 而"新建"要的恰恰是把那份文件放掉, 否则新建之后重测还能往上一份文件追加。
    * 装载 / Done / Aborted / Idle 四种来路在这里走的是**同一条**。 */
   resetRunState();
   enter(State::Idle);
   return true;
}

bool ScanController::takeStaleCsvName()
{
   const bool v = m_stale_csv_name;
   m_stale_csv_name = false;
   return v;
}

const Point *ScanController::currentPoint() const
{
   if (m_cur < 0 || (size_t)m_cur >= m_plan.size())
      return nullptr;
   return &m_plan[(size_t)m_cur];
}

const Point *ScanController::pointAt(int k) const
{
   if (k < 0 || (size_t)k >= m_plan.size())
      return nullptr;
   return &m_plan[(size_t)k];
}

bool ScanController::settlingNow() const
{
   return m_st == State::Moving
       && (m_judge[0].windowOpen() || m_judge[1].windowOpen());
}

QString ScanController::stateText() const
{
   switch (m_st)
   {
   case State::Idle:     return QStringLiteral("空闲, 参数可改");
   /* 两支 (2026-09-30): 待补的点在不在, 决定操作员此刻能做的是"继续补点"还是"重测单点" ——
    * 而这两件事在屏幕上是两个不同的按钮。字数上限 ~15 (这一句同时进画布 HUD, 那个框单行
    * 不换行直接裁, 而画布能被拖窄), 所以只说最要紧的那半句 */
   case State::Loaded:   return m_order.empty()
                                ? QStringLiteral("已打开 CSV (此文件已采完)")
                                : QStringLiteral("已打开 CSV (未启动)");
   case State::Moving:   return settlingNow() ? QStringLiteral("到位中 (已进稳定窗口)")
                                              : QStringLiteral("移动中");
   case State::Dwelling: return QStringLiteral("停留 (等待机械余振衰减)");
   case State::Reading:  return QStringLiteral("读功率计…");
   case State::Paused:   return QStringLiteral("已暂停 (目标冻结, 保持力矩)");
   case State::Aborted:  return QStringLiteral("已中止");
   case State::Done:     return QStringLiteral("本轮结束");
   }
   return QString();
}

bool ScanController::cellDone(int ix, int iy) const
{
   if (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny)
      return false;
   return m_done[(size_t)iy * (size_t)m_nx + (size_t)ix] != 0;
}

bool ScanController::cellHasValue(int ix, int iy) const
{
   if (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny)
      return false;
   return m_have[(size_t)iy * (size_t)m_nx + (size_t)ix] != 0;
}

double ScanController::cellValue(int ix, int iy) const
{
   if (!cellHasValue(ix, iy))
      return 0.0;
   return m_watts[(size_t)iy * (size_t)m_nx + (size_t)ix];
}

bool ScanController::wattsRange(double *lo, double *hi) const
{
   bool any = false;
   double a = 0.0, b = 0.0;

   for (size_t i = 0; i < m_have.size(); i++)
   {
      if (!m_have[i])
         continue;
      double v = m_watts[i];
      if (!any) { a = b = v; any = true; }
      else      { if (v < a) a = v; if (v > b) b = v; }
   }

   if (!any)
      return false;
   if (lo != nullptr) *lo = a;
   if (hi != nullptr) *hi = b;
   return true;
}

void ScanController::enter(State s)
{
   if (m_st == s)
      return;
   m_st       = s;
   m_state_ms = m_now_ms;
   emit stateChanged();
}

void ScanController::stopMotion()
{
   m_bus->postStop();
   /* 丢掉手上那半份采样: 到齐的那两笔是**停之前**的位置上采的, 留着会与重走这一点之后的
    * 数混成一个平均值 —— 而它在 CSV 里和别的行长得一模一样。
    * 采集那边没有"我们的请求"要撤 (请求是 MeterLog 发的), 所以只清这一对计数器 */
   m_nsamp = 0;
   m_acc   = 0.0;
}

bool ScanController::armRun(QString *err)
{
   const BusTelem t = m_bus->telemetry();

   if (!t.connected)
      return fail(err, QStringLiteral("未连接总线"));
   if (!t.in_op)
      return fail(err, QStringLiteral("总线不在 OP 状态"));

   /* 回零把轴留在使能 + HM: 自动中止判据抓不到它, 插补也不推进 (总线线程阻塞在 em_home
    * 里), 状态机会以为点到了而滑台没动 */
   if (t.homing)
      return fail(err, QStringLiteral("总线正在回零, 请等回零结束后再启动扫描。"));

   /* 这一条留在控制器里 (没搬去界面): 源没开 = MeterLog 起不来 = 这条流上**一笔采样都不会
    * 到**, 每个点都得空等到期限才收尾成 ok=false —— 那趟扫描能走完, 却是一份全失败的 CSV。
    * 起点拦掉比走完再解释便宜得多, 文案照旧 */
   if (m_meter == nullptr || !m_meter->isOpen())
      return fail(err, QStringLiteral("功率计未打开, 扫描无法采集数据。"));

   if (m_order.empty())
      return fail(err, QStringLiteral("网格中没有要扫的点。"));

   QString pe = paramsError();
   if (!pe.isEmpty())
      return fail(err, pe);

   /* 轴数: 必须正好两根。扫描的语义就是 X=轴0 / Y=轴1, 少一根多的那根没意义 */
   if (t.naxis != 2)
      return fail(err, QStringLiteral("总线报告 %1 根轴, 扫描需要正好两根 (轴0 = X, 轴1 = Y)。")
                            .arg(t.naxis));

   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      if (!a.valid || !a.mirror_ok)
         return fail(err, QStringLiteral("轴%1 未收到完整过程数据帧, 位置不可信。").arg(i));
      if (!a.enabled)
         return fail(err, QStringLiteral(
            "轴%1 未使能。请先「使能」或「回零」。").arg(i));
      if (a.fault || t.fault)
         return fail(err, QStringLiteral("轴%1 有故障位 (6041h bit3), 请先「故障复位」。").arg(i));
      if (a.limit_active)
         return fail(err, QStringLiteral(
            "%1。\n"
            "%2。\n"
            "%3。\n"
            /* 收尾只讲后果: 扫描期间限位成立就自动中止, 所以现在拒绝。
             * 不许说"会撞上去" —— %3 里有一种成因正是那限位根本不存在 (极性配反) */
            "扫描期间限位成立即自动中止, 本次扫描未启动。")
               .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)).arg(i))
               .arg(QString::fromUtf8(ecatcmd::limit_switch_text(
                       a.dig_known, a.dig_pos, a.dig_neg, t.di_invert)))
               .arg(QString::fromUtf8(ecatcmd::limit_hit_advice(
                       a.dig_known, a.dig_pos, a.dig_neg, a.dig_home, t.di_invert))));
   }

   if (t.expected_wkc > 0 && t.wkc < t.expected_wkc)
      return fail(err, QStringLiteral("工作计数器不足 (%1/%2), 过程数据不完整, 扫描未启动。")
                            .arg(t.wkc).arg(t.expected_wkc));

   /* 量程用 telemetry 里的**真值**, 不用参数算的 —— 这里防的是"遥测里的量程还没跟上参数"
    * 那个时间差 (量程是每次参数一变就重投的, 但投过去要等工作线程转一圈), 这个窗口很窄但
    * 真的存在: 刚把区域放大、立刻按开始, 最外圈的点就会被静默夹掉。 */
   int32_t far = 0;
   /* 逐轴的 [最小, 最大] (脉冲)。与 far 同一趟算出来 —— 那道量程闸要的是"最远多少",
    * 限位那道闸要的是"每一侧最远到哪"; 分两趟走容易有一趟忘了跟着改。 */
   int32_t lo[2] = {INT32_MAX, INT32_MAX};
   int32_t hi[2] = {INT32_MIN, INT32_MIN};
   for (size_t k = 0; k < m_order.size(); k++)
   {
      const Point &p = m_plan[(size_t)m_order[k]];
      const int32_t xy[2] = {p.x_pul, p.y_pul};
      for (int i = 0; i < 2; i++)
      {
         lo[i] = std::min(lo[i], xy[i]);
         hi[i] = std::max(hi[i], xy[i]);
      }

      far = std::max(far, std::abs(p.x_pul));
      far = std::max(far, std::abs(p.y_pul));
   }
   if (t.range > 0 && far > t.range)
      return fail(err, QStringLiteral(
         "最远的网格点是 %1 pul, 当前量程只有 ±%2。\n"
         "超出量程的目标会被静默夹掉: 那几条边采不到, 且不报错。\n"
         "参数刚改过时量程要等工作线程转一圈才跟上, 此时启扫会撞上这一条; \n"
         "持续出现请把区域改小。")
         .arg(far).arg(t.range));

   /* 限位记录那道闸 (2026-09-29, 见 scan/limitguard.h)。**排在量程之后**: 量程是既有的
    * 判据, 两条都中时先报那一条, 不多说一句。
    *
    * 拦在**起点**而不是走到那一格才停: 走一半停会留下一张缺了一角的图, 而那个缺口还得
    * 靠人看出来 —— 起扫前一句话说清楚便宜得多。理由的措辞在 limitguard.h 里 (那句要与
    * 界面上的提示同源, 所以它是个纯函数, 自检钉得住)。 */
   {
      const std::string lw = limitguard::limitPlanWhy(m_lim, lo, hi);
      if (!lw.empty())
         return fail(err, QString::fromStdString(lw));
   }

   m_run_p   = m_p;
   m_bad_wkc = 0;
   m_ord_i   = 0;

   /* 速度只在这里设一次: 一轮扫描从头到尾一个速度 */
   m_bus->setSpeed(0, m_p.speed_pul_s);
   m_bus->setSpeed(1, m_p.speed_pul_s);

   startPoint(m_order[0]);
   return true;
}

void ScanController::startPoint(int plan_index)
{
   const Point &pt = m_plan[(size_t)plan_index];
   const BusTelem t = m_bus->telemetry();

   m_cur      = plan_index;
   m_issued[0] = pt.x_pul;
   m_issued[1] = pt.y_pul;
   m_issued_ms[0] = m_issued_ms[1] = m_now_ms;
   m_want_ok[0] = m_want_ok[1] = false;
   m_nsamp    = 0;
   m_acc      = 0.0;
   m_spread[0] = m_spread[1] = 0;

   m_bus->setTarget(0, pt.x_pul);
   m_bus->setTarget(1, pt.y_pul);

   const int32_t tol = posTolPul(m_p);
   m_judge[0].begin(pt.x_pul, tol, m_p.settle_ms, m_now_ms);
   m_judge[1].begin(pt.y_pul, tol, m_p.settle_ms, m_now_ms);

   /* 走不到就中止的最后期限 (目标被夹 / 卡住 / 跟不上时到位判据永远不成立);
    * 距离按当前位置量, 折返的那一格也算得对 */
   int64_t dx = std::abs((int64_t)pt.x_pul - (int64_t)t.ax[0].pos);
   int64_t dy = std::abs((int64_t)pt.y_pul - (int64_t)t.ax[1].pos);
   int64_t d  = std::max(dx, dy);
   int64_t ms = (m_p.speed_pul_s > 0) ? (d * 1000 / (int64_t)m_p.speed_pul_s) : 0;
   ms = ms * 3 + 3000;
   if (ms <  5000)   ms =  5000;
   if (ms > 120000)  ms = 120000;
   m_move_deadline_ms = m_now_ms + ms;

   if (m_run_ms < 0)
      m_run_ms = m_now_ms;

   enter(State::Moving);
}

bool ScanController::start(const QString &csv_path, QString *err)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中, 请先「中止」。"));

   /* 打开态下"新建一轮"没有意义 —— 一份打开的文件正等着补点或重测, 而这一按会把它的
    * 上半场与待补集合一起丢掉。界面那边「开始扫描」是灰的, 这一句是照 armRun 那个体例
    * 留的第二道。出路写「中止」而不写「新建 CSV」: 两个都能出去, 而「中止」在任何状态下
    * 都在 (「新建 CSV」是这一轮新加的, 不指望读这句话的人已经知道它) */
   if (loaded())
      return fail(err, QStringLiteral("已打开一份 CSV, 请先「中止」。"));

   QString pe = paramsError();
   if (!pe.isEmpty())
      return fail(err, pe);

   rebuildPlan();
   if (m_plan.empty())
      return fail(err, QStringLiteral("网格为空。"));

   /* 按这次的走法重排一次点列 (见 prepareRunPlan)。新一轮没有任何格子采过, 所以 m_order
    * 一律是整片网格 —— 「随机 (可重复)」那个"永远整片"的池子在这里也就自动成立了 */
   prepareRunPlan();
   m_endless = modeEndless(m_p.mode);

   m_log.close();

   m_order.resize(m_plan.size());
   std::iota(m_order.begin(), m_order.end(), 0);
   m_is_retest = false;
   m_cur       = -1;
   m_run_ms    = -1;

   /* 新一轮 = 新的零点世代 (零点就是此刻的物理位置) */
   if (m_zero_epoch < 0)
      m_zero_epoch = 0;

   /* extra_meta: 这趟是哪台仪器、什么探头/波长/量程/模式采的 (powermeter.h 的
    * meterMetaLines)。CSV 里那一列叫 watts, 而真机报的可能不是 W —— 这一句就是答案 */
   if (!m_log.beginNew(csv_path, m_p, isoNow(), m_zero_epoch, meterMetaLines(m_meter), err))
      return false;

   if (!armRun(err))
   {
      m_log.close();
      return false;
   }
   return true;
}

/* 「打开 CSV」第一步: 把那份 CSV 读进来 (2026-09-29 与"开跑"拆开)。
 *
 * 今天打开的路径只到这里为止 —— 上半场画在画布上、要补的点算好、句柄按追加开着, 而滑台
 * **一步没走**。开始走是 beginLoadedRun() 的事, 由操作员按「继续」触发。
 *
 * **可重入 (2026-09-30, §49.9)**: 打开态下再打开一份 = **换一份** —— 界面那边「打开 CSV」
 * 在打开态是亮的, 用户原话是"打开 csv 的时候不要只有一个中止按钮"。这条路从前是不通的
 * (一句 `if (loaded()) return fail(...)`), 理由是它的后半段**破坏性**: 清 m_order / 覆写
 * m_done / 关旧句柄开新句柄, 而新旧句柄之间没有退路 —— 新的没开成, 旧的就回不来, 留下一个
 * "状态写着装载态、数据没了、句柄关着"的撕裂态。把这条路堵掉是最省事的做法, 代价是"想换
 * 一份"只剩「中止」这一条路。
 *
 * 现在它通, 靠的是**把顺序反过来**: 凡是可能失败的都排在前面 (读文件 / 解析 / 世代比对),
 * 换句柄是唯一不可逆的一步, 于是给它配一条回滚 (关掉新开的、把旧的装回去 —— 重开一次是
 * 幂等的); 换成功之后剩下的**全是内存里的赋值**, 一步都失败不了。 */
bool ScanController::loadResume(const QString &csv_path, bool accept_zero_epoch_change,
                                QString *err, QString *why)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中, 请先「中止」。"));

   if (why != nullptr) why->clear();

   /* 参数先自洽, 否则读回来的网格索引无处可比 */
   QString pe = paramsError();
   if (!pe.isEmpty())
      return fail(err, pe);

   rebuildPlan();

   std::string text;
   if (!readCsvText(csv_path, &text, err))
      return false;

   std::vector<char> mask;
   int         max_index = -1;
   std::string diff;
   std::string started_iso;
   int         csv_epoch = -1;

   diff = csvParseForResume(text, m_p, &mask, &max_index, &started_iso, &csv_epoch);

   if (!diff.empty())
      return fail(why != nullptr ? why : err, fromStd(diff));

   /* 零点世代对不上 = 中间零点被搬过 (回零 / 「设为区域中心」), 不静默继续。比法刻意不对称:
    * 文件里写了世代 (>=0) 且与现在不同才拦, 自己这边 -1 也算不同; 没写世代的老文件不拦。
    * **连接不算搬零点**了 (2026-09-22 起 scan 跨重连沿用零点), 所以那句报错里不再提连接。 */
   if (csv_epoch >= 0 && csv_epoch != m_zero_epoch && !accept_zero_epoch_change)
   {
      /* 一句「现象 + 出路」, 五行压成一行 (2026-09-30, §49.11)。
       *
       * 从前这里是**五段** —— 它落到屏幕上就是一条五行的红横幅, 压在画布顶上, 把刚打开
       * 的那半场数据整个盖住。而中间那三段是**成因**(回零与「设为区域中心」会搬零点、同一个
       * 坐标可能指向别处), 按 §26.3 那套本来就该住在 docs/scan_messages.md §8.2 里, 不在
       * 屏幕上。出路也只剩一条, 就是操作员下一步真会按的那个键 (按「继续」之前先看一眼滑台
       * 在不在原处)。 */
      return fail(why != nullptr ? why : err,
         QStringLiteral("该 CSV 采于另一次零点 (第 %1 次 → 第 %2 次)。"
                        "请确认滑台还在上次确立零点时的位置, 再按「继续」。")
            .arg(csv_epoch).arg(m_zero_epoch));
   }

   /* 只补没采过的点 (2026-09-30: **空也照装**)。
    *
    * 从前 m_order 为空这里就 fail 了, 于是"打开一份已经采完的 CSV"只有一个红横幅。而用户要的
    * 是**打开来看 / 拿来重测单点** —— 那是两份不同的用途, 采完的文件完全配得上一个能用的
    * "打开"。所以这里不再拒, 后面照旧往下走: m_done 由 mask 铺满、csvLoadGrid 把值填上,
    * 于是画布上是一整张图, 而 pendingPoints() == 0。
    *
    * 它的代价落在两处判据上, 都在下面各自的地方: beginLoadedRun() 空 order 拒绝 (那份文案
    * 从前住在这里), retest() 反过来**只在有待补的点时才拒**。 */

   /* ---- 到这儿为止一个字节都没动。下面是**唯一不可逆的一步**: 换写出句柄 ---------
    *
    * ScanLog 只有一个句柄 (换一份就是换它), 而它一失败就没有回头路 —— 所以先把旧的那份
    * 记下来, 新句柄没开成就把旧的**装回去**。重开一次是幂等的: 那半行残行早被 trimTail
    * 切过, 而每一次 append 都写满行。于是"换一份"没换成就等于什么都没发生 —— 屏幕上还是
    * 原来那一份, 画布上还是它那半场图。 */
   const QString old_path = m_log.path();
   const bool    old_open = m_log.isOpen();

   if (!m_log.beginAppend(csv_path, err))
   {
      QString back_err;
      if (old_open && !old_path.isEmpty())
         m_log.beginAppend(old_path, &back_err);
      return false;
   }

   /* ---- 句柄已经换好了, 下面全是内存里的赋值, 一步都失败不了 ------------------- */

   /* 已有进度连数值一起装进结果网格, 续扫一开始图上就有上半场。
    * 装进两个临时数组再换进来: csvLoadGrid 本来就是"整份覆写" (它先 assign 再填), 用临时
    * 数组只是让"哪一段会失败、哪一段不会"在代码上看得见。 */
   std::vector<char>   have;
   std::vector<double> watts;
   csvLoadGrid(text, m_p, &have, &watts);

   /* 走法在这里也重排一次: 补点该按这次选的走法走 (随机那两种还会洗一遍) */
   prepareRunPlan();
   m_endless = modeEndless(m_p.mode);

   m_order.clear();
   if (m_endless)
   {
      /* 「随机 (可重复)」的池子**永远是整片网格** —— 不管扫没扫过都进池子, 所以这里刻意
       * 不按 mask 过滤 (那是别的走法的"只补没采过的点")。残留数据照旧画在图上, 由上面的
       * m_done 重铺决定。 */
      m_order.resize(m_plan.size());
      std::iota(m_order.begin(), m_order.end(), 0);
   }
   else
   {
      /* **点列下标不是网格下标** (2026-10-11 修): mask 是按网格 `iy*m_nx+ix` 排的, 而 m_plan
       * 的下标是"走的第几个点" —— 蛇形在行内是反的 (row1 的第一个点是 ix=54), 随机那两种更
       * 是一整个置换。以前这里直接拿 `mask[i]` 配 `m_plan[i]`, 只在"整行整行地采过"时才恰好
       * 对得上 (行内是一个集合内的对折); 文件停在一行中间时它就会**重采已经采过的格、漏掉
       * 没采过的格**。走 (ix, iy) 换算一下才是那一格真正的下标。 */
      for (size_t i = 0; i < m_plan.size(); i++)
      {
         const Point &q = m_plan[i];
         const size_t cell = (size_t)q.iy * (size_t)m_nx + (size_t)q.ix;
         if (cell < mask.size() && !mask[cell])
            m_order.push_back((int)i);
      }
   }

   /* m_done **整份重铺**, 不是"把采过的格子贴上去" (2026-09-30): 打开态下换一份时, 新文件里
    * 没有的格子必须**变回未采** —— 留着上一份的值就是把两份数据的图拼在一起。mask 与 m_done
    * 同尺寸 (都是 nx*ny), 所以逐格赋值就是整份覆写。 */
   m_done.assign(m_done.size(), 0);
   for (size_t i = 0; i < mask.size() && i < m_done.size(); i++)
      m_done[i] = mask[i];
   m_have  = std::move(have);
   m_watts = std::move(watts);

   m_is_retest = false;
   m_cur       = -1;
   m_run_ms    = -1;      /* elapsed_ms 从这次接上的那一刻算起 */

   /* 这三行原本在 armRun 的收尾里 (与"走起来"同一口气), 装载时必须自己来 ——
    * 尤其 m_ord_i: pendingPoints() 是 m_order.size() - m_ord_i, 而它唯一被清零的地方就是
    * armRun 那一行。漏掉的话装载完"待补点数"会从上一轮的下标上接着算, 出负数 (画布左上角
    * 那行「N / M 点, 剩 K」是无条件读它的)。 */
   m_run_p   = m_p;       /* 装载这一份几何 = 后面两道重查的基线 */
   m_bad_wkc = 0;
   m_ord_i   = 0;
   m_load_epoch = m_zero_epoch;   /* 零点世代基线, beginLoadedRun 重查 */

   enter(State::Loaded);
   return true;
}

/* 「打开 CSV」第二步: 操作员按了「继续」。
 *
 * 开工之前把**装载那一刻的两条前提**重查一遍。这两件事都能在"装载好、还没按继续"那段
 * 时间里变掉, 而 m_order 里那些 (ix, iy) 绑的就是它们:
 *   1. 几何 —— 界面那几项是锁着的, 这一道是控制器侧的兜底 (armRun 只查 paramsError(),
 *      **不查 m_order 的下标还对不对得上 m_plan**);
 *   2. 零点世代 —— 界面锁不住这个: 工作线程自己会在"沿用不了只好重取"那条路上搬零点,
 *      而零点一搬, 补的点就整体平移到别的物理位置, 却与上半场拼进同一张图。
 * 拦下时留在装载态 (一个字节不动), 接好线 / 重开一份再按一次就行。 */
bool ScanController::beginLoadedRun(QString *err)
{
   if (m_st != State::Loaded)
      return fail(err, QStringLiteral("还没打开一份 CSV, 请先「打开 CSV」。"));

   /* **排在 sameGeom 与 armRun 前面** (2026-09-30): armRun 里同样有一句空 order 的拒绝,
    * 但它排在"未连接总线 / 不在 OP / 正在回零 / 功率计未打开"**之后** —— 打开一份采完的文件
    * 又恰好掉过线时, 屏幕上先看到的会是「未连接总线」, 一句与现象毫不相干的话。 */
   if (m_order.empty())
      return fail(err, QStringLiteral(
         "这份 CSV 的点已全部采完, 没有要补的点。请改用「重测选中点」。"));

   if (!sameGeom(m_run_p, m_p))
      return fail(err, QStringLiteral("打开时那份几何已被改过, 请「中止」后重新打开这份 CSV。"));

   if (m_zero_epoch != m_load_epoch)
      return fail(err,
         QStringLiteral("打开之后零点被搬动过 (第 %1 次 → 第 %2 次)。"
                        "请「中止」后重新打开这份 CSV。")
            .arg(m_load_epoch).arg(m_zero_epoch));

   return armRun(err);   /* 成功时 armRun 已经 startPoint → enter(Moving) */
}

bool ScanController::retest(int ix, int iy, QString *err)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中, 单点重测需等扫描停止。"));

   /* 打开一份 CSV 之后重测会把 m_order 换成那一个点 —— **"要补哪些点"当场没了, 一声不响**。
    * 而这里**特别容易漏**: 下面那句 !m_log.isOpen() 在装载态恰好**不成立** (追加句柄正开着),
    * 所以它不是这道闸。界面那边「重测选中点」是灰的。
    *
    * 判据是**有没有待补的点**, 不是"在不在装载态" (2026-09-30): 采完的文件里 m_order 本来就
    * 是空的, 没有集合可顶 —— 而那正是"打开一份旧数据、挑几格重测"这条用途 (用户原话:
    * 「扫完了可以重扫单点并把数据加在最后」)。界面那一侧的判据是同一个算式。 */
   if (loaded() && pendingPoints() > 0)
      return fail(err, QStringLiteral("已打开一份 CSV, 还有 %1 个点待补, 请先「继续」或「中止」。")
                            .arg(pendingPoints()));

   if (!m_log.isOpen())
      return fail(err, QStringLiteral(
         "当前没有进行中的一轮, 单点重测需要已打开的 CSV 文件, 无文件可追加。\n"
         "请先「开始扫描」或「打开 CSV」。"));

   if (!sameGeom(m_run_p, m_p))
      return fail(err, QStringLiteral(
         "区域 / 分辨率 / 每 mm 脉冲数已改动, 当前 (ix, iy) 已不是 CSV 中的那一点,\n"
         "追加会在一份文件里混入两个坐标。请把参数改回, 或另开一轮。"));

   if (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny)
      return fail(err, QStringLiteral("格子 (%1, %2) 超出 %3×%4 的网格。")
                            .arg(ix).arg(iy).arg(m_nx).arg(m_ny));

   int k = -1;
   for (size_t i = 0; i < m_plan.size(); i++)
   {
      if (m_plan[i].ix == ix && m_plan[i].iy == iy)
      {
         k = (int)i;
         break;
      }
   }
   if (k < 0)
      return fail(err, QStringLiteral("网格中找不到 (%1, %2)。参数可能刚被改动。")
                            .arg(ix).arg(iy));

   m_order.assign(1, k);
   m_is_retest = true;

   if (!armRun(err))
   {
      m_is_retest = false;
      return false;
   }
   return true;
}

void ScanController::pause()
{
   if (!running() || m_st == State::Paused)
      return;

   stopMotion();
   enter(State::Paused);
}

void ScanController::resumeRun()
{
   if (m_st != State::Paused)
      return;

   if (m_cur < 0)
   {
      enter(State::Done);
      emit runFinished(false);
      return;
   }

   /* 重新走完当前点并重新采样: 从半截接着采会写进位置还没停稳的数 */
   startPoint(m_cur);
}

void ScanController::abort(const QString &why)
{
   abortInternal(why, false);
}

void ScanController::abortInternal(const QString &why, bool automatic)
{
   /* 装载态 = **放弃这次打开**, 不是"中止一轮" (2026-09-29)。一个点都没跑过, 所以:
    * 关掉追加句柄、丢掉要补的点、把结果数组清干净 (否则「放弃打开 → 开始扫描」之后,
    * 图上这一轮还没测过的格子画的是那份被放弃文件里的值 —— rebuildPlan 几何没变就提前
    * return, start() 只重排 m_order, 那三个数组谁都不会替它清), 回到 Idle。
    * **不发 runFinished**: 那一路的槽会弹「扫描已结束 (未完成)」, 而这里没有"结束"可言。
    * 2026-09-30 起这一段搬进 resetRunState() (与 newFile() 共用), 做的还是这几件事。 */
   if (m_st == State::Loaded)
   {
      resetRunState();
      enter(State::Idle);
      return;
   }

   if (!running())
   {
      if (automatic && !why.isEmpty())
         emit autoAborted(why);
      return;
   }

   stopMotion();
   m_cur = -1;
   enter(State::Aborted);

   /* 不关文件: 中止之后常要单点重测, 而重测就是往这个文件里追加 (每行都已 flush)。
    * 「重测」走的是 m_log 手上那个句柄, 与路径框写着什么无关 —— 所以下面那件事安全。 */
   m_stale_csv_name = true;   /* 见 takeStaleCsvName() */

   if (automatic && !why.isEmpty())
      emit autoAborted(why);
   emit runFinished(false);
}

void ScanController::beginReading()
{
   m_nsamp = 0;
   m_acc   = 0.0;

   /* 这一个点的收尾期限。采样按连续读数那个「间隔」一笔笔到齐, N 笔至少要 N 个间隔,
    * 再给一整笔的余量 (第一笔可能要等下一次排拍) —— 加上 meter_timeout_ms 那一段,
    * 于是"一笔都没到齐"与"到齐了但源卡住"两种情况都等得起。
    * 这个数是**算出来的**: 它跟着「间隔」走, 而「间隔」是界面上那个旋钮 (见 Params) */
   m_read_budget_ms = (int64_t)m_p.meter_timeout_ms
                    + (int64_t)std::max(1, m_p.samples_per_point) * (int64_t)m_p.meter_interval_ms;
   m_deadline_ms = m_now_ms + m_read_budget_ms;

   enter(State::Reading);
}

void ScanController::feedMeterSample(int64_t ms, double watts, bool ok)
{
   Q_UNUSED(ms);           /* 用 tick 那条单调钟, 见头文件 */

   /* 不在读数期间到齐的采样 (移动/停留/暂停/收尾之后) 属于曲线上那条流, 不属于任何一个点。
    * 这一道守卫是"上一个点的数混进下一个点"的唯一防线 —— 少了它, 停止/暂停之前手上那半份
    * 会被算进重走的那一点, 而且不报错 */
   if (m_st != State::Reading)
      return;

   /* 没要回来的那一笔: 不算进平均, 也不当场判死 —— 期限到了自然收尾成 ok=false */
   if (!ok)
      return;

   m_acc += watts;
   m_nsamp++;

   if (m_nsamp < m_p.samples_per_point)
      return;

   m_acc /= (double)m_nsamp;
   finishPoint(true, std::string());
}

void ScanController::finishPoint(bool ok, const std::string &flags)
{
   if (m_cur < 0 || (size_t)m_cur >= m_plan.size())
   {
      abortInternal(QStringLiteral("内部错误: 收尾时当前点下标越界。"), true);
      return;
   }

   const Point &pt   = m_plan[(size_t)m_cur];
   const BusTelem t  = m_bus->telemetry();

   Row r;
   r.index        = m_cur;
   r.pt           = pt;
   r.watts        = ok ? m_acc : 0.0;
   r.ok           = ok;
   r.flags        = flags;
   if (m_is_retest)
      r.flags = r.flags.empty() ? std::string("retest") : (r.flags + "|retest");
   r.pos_x_pul    = t.ax[0].pos;
   r.pos_y_pul    = t.ax[1].pos;
   r.spread_x_pul = m_spread[0];
   r.spread_y_pul = m_spread[1];
   /* 两个时间字段从**同一个** QDateTime 来 (2026-09-30): 分两次 currentDateTime() 会跨秒 ——
    * unix_ms 落在 …:29.998 而 time_local 落在 …:30, 于是那一行自己跟自己对不上 */
   const QDateTime now = QDateTime::currentDateTime();
   r.unix_ms      = now.toMSecsSinceEpoch();
   r.time_local   = isoOf(now).toStdString();
   r.elapsed_ms   = (m_run_ms >= 0) ? (m_now_ms - m_run_ms) : 0;

   if (!m_log.isOpen() || !m_log.append(r))
   {
      abortInternal(QStringLiteral("CSV 写入失败, 已自动中止 (此后数据丢失): %1")
                       .arg(m_log.lastError()), true);
      return;
   }

   const size_t cell = (size_t)pt.iy * (size_t)m_nx + (size_t)pt.ix;
   if (cell < m_done.size())
   {
      m_done[cell] = 1;
      if (ok)
      {
         m_have[cell]  = 1;
         m_watts[cell] = m_acc;
      }
   }

   emit pointLogged(pt.ix, pt.iy, ok);
   advance();
}

void ScanController::advance()
{
   m_cur = -1;

   if (!m_is_retest)
   {
      m_ord_i++;

      if (m_ord_i < m_order.size())
      {
         startPoint(m_order[m_ord_i]);
         return;
      }
   }

   /* 「随机 (可重复)」: 这一遍点列走完**不判 Done**, 把点列重洗一遍接着走 —— 这一轮永远
    * 到不了头, 只有「暂停」「中止」能停 (用户要的"不管扫没扫过都进随机池子, 不停歇地扫")。
    * 洗的是 m_plan 本身, 而 m_order 是它的下标全集, 于是新顺序自动覆盖整片网格。此刻
    * m_cur 还是 -1 (函数第一行), 不在任何点上, 换顺序不会让谁指错格子。
    *
    * **它必须住在这儿, 不能搬到上面那句 `m_ord_i++` 之后**: 那样就成了"每走完一个点就重洗、
    * 再从 m_order[0] 起步", 也就是**每次独立抽一个点** —— 那不是用户选的 (他要的是"洗一遍
    * 走一遍, 走完再洗"), 而且集齐整片网格要多花近四倍的点 (25 格 ≈ 95 次抽取)。
    * 自检里"走完第一遍恰好 25 个点"那一条钉的就是这个位置。
    *
    * `!m_is_retest` 不能省: 单点重测走完也落到这儿, 而它是**一个点**, 不该把整轮重开。
    * (重测之前可能刚跑过一轮可重复, 那时 m_endless 还是 true。) */
   if (m_endless && !m_is_retest)
   {
      shufflePlan(&m_plan, m_seed++);
      m_ord_i = 0;
      startPoint(m_order[0]);
      return;
   }

   /* 走完了 (整轮或单点重测)。文件不关: 重测要往同一个文件追加 */
   m_is_retest = false;
   m_endless   = false;      /* 这一轮已经收掉, endless() 不许对着一轮过去的事点头 */

   /* 点列清掉 (2026-09-30)。单点重测走完之后 m_order 是 {k} 而 m_ord_i 是 0 (重测那一路
    * 跳过上面那句 m_ord_i++), 于是 pendingPoints() 报 **1** —— 画布左上角那行「N / M 点,
    * 剩 K」是无条件读它的, 每次重测之后都会多出一句"剩 1 个点"。而这一轮起它还成了
    * 「继续」与「重测」两个按钮的判据, 留着一个假的数会给下一个人假的线索。
    * start() 与 retest() 都会重填 m_order, 所以清掉安全。 */
   m_order.clear();
   m_ord_i = 0;

   /* 本轮那份文件不再是"要写的目标"了 —— 窗口那个路径框得换成新的时间戳名, 否则再按
    * 「开始扫描」会把它截断 (见 takeStaleCsvName) */
   m_stale_csv_name = true;

   enter(State::Done);
   emit runFinished(true);
}

void ScanController::tick(int64_t now_ms)
{
   m_now_ms = now_ms;

   if (!running())
      return;

   /* 安全检查每个 tick 都跑, 包括暂停中 —— 暂停时驱动器仍可能报故障或撞限位 */
   const BusTelem t = m_bus->telemetry();
   const QString bad = healthProblem(t);
   if (!bad.isEmpty())
   {
      abortInternal(bad, true);
      return;
   }

   switch (m_st)
   {
   case State::Paused:
      return;

   case State::Moving:
   {
      bool all = true;
      for (int i = 0; i < 2; i++)
      {
         std::string why;
         const ArrivalJudge::Verdict v = m_judge[i].feed(obsOf(t, i), m_now_ms, &why);
         if (v == ArrivalJudge::Verdict::Faulted)
         {
            abortInternal(fromStd(why), true);
            return;
         }
         if (v != ArrivalJudge::Verdict::Arrived)
            all = false;
         m_spread[i] = m_judge[i].spread();
      }

      if (m_now_ms > m_move_deadline_ms)
      {
         abortInternal(QStringLiteral(
            "未到位: 下发目标 (%1, %2) 已等待 %3 s, 实测位置 (%4, %5) 始终未进入容差 ±%6。\n"
            "可能原因: 机械卡住 / 被限位挡住 / 目标被量程夹掉。")
            .arg(m_issued[0]).arg(m_issued[1])
            .arg(m_state_ms >= 0 ? (m_now_ms - m_state_ms) / 1000 : 0)
            .arg(t.ax[0].pos).arg(t.ax[1].pos).arg(posTolPul(m_p)), true);
         return;
      }

      if (!all)
         return;

      enter(State::Dwelling);
      m_deadline_ms = m_now_ms + m_p.dwell_ms;
      if (m_now_ms >= m_deadline_ms)          /* dwell = 0 时直接过 */
         beginReading();
      return;
   }

   case State::Dwelling:
      if (m_now_ms < m_deadline_ms)
         return;
      beginReading();
      return;

   case State::Reading:
      /* 手上"欠"的是采样笔数, 不是未决请求 —— 一笔都没到齐就一直等, 等到期限 */
      if (m_now_ms < m_deadline_ms)
         return;

      /* 记下到了几笔: 0 笔 = 这条流根本没在跑 (源没开 / 被 hold 住), 那是最常见的一种;
       * 报出来比只报一个时限有用。两处都不用逗号 (它要进 CSV 的 flags 列) */
      finishPoint(false, "功率计超时 " + std::to_string(m_read_budget_ms) + " ms (到 "
                         + std::to_string(m_nsamp) + "/"
                         + std::to_string(std::max(1, m_p.samples_per_point)) + " 笔采样)");
      return;

   case State::Idle:
   case State::Aborted:
   case State::Done:
   /* Loaded 到不了这儿: 上面那句 if (!running()) 已经 return 了 —— 装载态没有扫描循环,
    * 安全检查归 armRun (按「继续」时把每一条重查一遍)。列在这里是让 switch 保持穷尽 */
   case State::Loaded:
      return;
   }
}

bool ScanController::externalWantChanged(const BusTelem &t, int axis, int32_t *seen)
{
   if (m_cur < 0)
      return false;

   /* 暂停时目标本来就不是下发值 (postStop 把 want := tgt), 必须放行, 否则一按
    * 暂停就被判成有人从旁路改了目标而自动中止 */
   if (m_st == State::Paused)
      return false;

   const int32_t w = t.ax[axis].want;
   if (seen != nullptr)
      *seen = w;

   const int32_t want = asClamped(m_issued[axis], t.range);

   /* 下发到 publish 之间差一个 2ms 周期, 先等自己的值登上去再比, 否则会把自己当成外部
    * 干预。等待必须有上限 (WANT_CONFIRM_MS): 没上限的话, 下发与登上去之间被人改了目标
    * 就永远认不出来 */
   if (!m_want_ok[axis])
   {
      if (w == want)
      {
         m_want_ok[axis] = true;
         return false;
      }
      return m_now_ms - m_issued_ms[axis] > WANT_CONFIRM_MS;
   }

   return w != want;
}

QString ScanController::healthProblem(const BusTelem &t)
{
   if (!t.connected)
      return QStringLiteral("总线已断开, 扫描自动中止。");

   if (!t.in_op)
      return QStringLiteral("已退出 OP 状态, 过程数据不可信, 扫描自动中止。");

   if (t.fault)
   {
      /* 6041h bit3 只说"有故障"; **说清是哪一个靠 603Fh** (ecatcmd::faulted_axes_text)。
       * 读不回来时那一句自己会说"还没读到 / 读不到", 不会编一个码出来 ——
       * 但这句自动中止是当场弹的, 那一刻码通常还没到, 所以中止窗口里那份"还没读到"
       * 是常态, 不是异常; 后面红横幅上会补上。 */
      const QString codes = ecatcmd::faulted_axes_text(t);

      return QStringLiteral("驱动器自报故障 (6041h bit3), 目标已冻结, 扫描自动中止%1")
                .arg(codes.isEmpty() ? QString()
                                     : QStringLiteral("。故障码: ") + codes);
   }

   /* WKC 不足 = 拔网线 / 掉了供电; 单帧抖动不值得中止, 连续 10 帧 (30Hz 下约 1/3 秒) 才认 */
   if (t.expected_wkc > 0 && t.wkc < t.expected_wkc)
   {
      m_bad_wkc++;
      if (m_bad_wkc >= 10)
         return QStringLiteral("工作计数器连续 %1 帧不足 (%2/%3), 过程数据不完整, 扫描自动中止。")
                   .arg(m_bad_wkc).arg(t.wkc).arg(t.expected_wkc);
   }
   else
   {
      m_bad_wkc = 0;
   }

   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      if (!a.valid || !a.mirror_ok)
         return QStringLiteral("轴%1 丢失过程数据帧, 位置为陈旧值, 扫描自动中止。").arg(i);

      if (!a.enabled)
         return QStringLiteral("轴%1 掉使能 (6041h bit2), 扫描自动中止。").arg(i);

      /* 这一条最可能真触发。后两句是现场诊断: bit11 报的是硬件限位信号有效, 未必真有
       * 个开关触发 (见 ecatworker.h) */
      if (a.limit_active)
         return QStringLiteral(
            "%1。\n"
            "%2。\n"
            "%3。\n"
            "扫描已自动中止, 已采的点不会重采。")
               .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)).arg(i))
               .arg(QString::fromUtf8(ecatcmd::limit_switch_text(
                       a.dig_known, a.dig_pos, a.dig_neg, t.di_invert)))
               .arg(QString::fromUtf8(ecatcmd::limit_hit_advice(
                       a.dig_known, a.dig_pos, a.dig_neg, a.dig_home, t.di_invert)));

      int32_t seen = 0;
      if (externalWantChanged(t, i, &seen))
         return QStringLiteral(
            "轴%1 的目标被外部改动 (当前 %2, 本点应为 %3)。\n"
            "扫描期间不允许手动干预, 已自动中止。")
            .arg(i).arg(seen).arg(asClamped(m_issued[i], t.range));
   }

   return QString();
}

}   /* namespace scan */
