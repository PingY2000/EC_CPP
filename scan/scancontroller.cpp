#include "scancontroller.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scan {

/* ---------------------------------------------------------------- 小工具 */

static int64_t wallMs()
{
   return QDateTime::currentMSecsSinceEpoch();
}

static QString isoNow()
{
   return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
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

/* 我们下发的目标可能被 EcatThread::setTarget 按量程夹过 —— 拿夹过的值去比,
 * 否则区域贴着量程时会自己报自己 */
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

/* ---------------------------------------------------------------- 构造 */

ScanController::ScanController(BusView *bus, PowerMeter *meter, QObject *parent)
   : QObject(parent), m_bus(bus), m_meter(meter)
{
   connect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
   connect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);

   rebuildPlan();
}

void ScanController::setMeter(PowerMeter *meter)
{
   if (meter == nullptr || meter == m_meter)
      return;

   /* 旧的必须显式断开: 它**还活着**, 只是不再被选中 —— 不disconnect 的话它下次出数
    * 会直接喂进状态机, 而且那条路径只有在下一个点采回来时才看得出来 */
   if (m_meter != nullptr)
   {
      disconnect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
      disconnect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);
   }

   m_meter = meter;
   connect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
   connect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);
}

/* ---------------------------------------------------------------- 参数 */

void ScanController::setParams(const Params &p)
{
   m_p = p;
   rebuildPlan();
}

QString ScanController::paramsError() const
{
   return fromStd(validate(m_p));
}

void ScanController::rebuildPlan()
{
   /* 几何没变就什么都别动 —— 网格还是那个网格, 已有结果还是对的。
    * 这一条是为了让"跑完之后手滑碰一下速度框"不会把刚采完的一整张图抹掉。 */
   if (m_nx > 0 && !m_plan.empty() && sameGeom(m_p, m_plan_p))
      return;

   m_nx   = axisCount(m_p.area_x_unit, m_p.res_unit);
   m_ny   = axisCount(m_p.area_y_unit, m_p.res_unit);
   m_plan = buildPlan(m_p);
   m_plan_p = m_p;

   const size_t n = (size_t)std::max(0, m_nx) * (size_t)std::max(0, m_ny);
   m_done.assign(n, 0);
   m_have.assign(n, 0);
   m_watts.assign(n, 0.0);
}

int64_t ScanController::estimateTotalMs() const
{
   return (int64_t)m_plan.size() * estimatePerPointMs(m_p);
}

int ScanController::completedPoints() const
{
   return (int)std::count(m_done.begin(), m_done.end(), (char)1);
}

/* ---------------------------------------------------------------- 查询 */

bool ScanController::running() const
{
   return m_st == State::Moving || m_st == State::Dwelling
       || m_st == State::Reading || m_st == State::Paused;
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
   case State::Idle:     return QStringLiteral("空闲 —— 参数可改");
   case State::Moving:   return settlingNow() ? QStringLiteral("到位中 (在稳定窗口里)")
                                              : QStringLiteral("移动中");
   case State::Dwelling: return QStringLiteral("停留 (等机械余振过去)");
   case State::Reading:  return QStringLiteral("读功率计…");
   case State::Paused:   return QStringLiteral("已暂停 —— 目标冻在当前位置, 保持力矩");
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

/* ---------------------------------------------------------------- 状态迁移 */

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
   /* 单据作废: 这一点的读数回包到了也不要了 (重走这一点时会重新发请求) */
   m_pending = false;
}

/* ---------------------------------------------------------------- 开始 */

bool ScanController::armRun(QString *err)
{
   const BusTelem t = m_bus->telemetry();

   /* ---- Preflight。缺一条都不动 ---- */
   if (!t.connected)
      return fail(err, QStringLiteral("没连上总线 —— 先点「连接」"));
   if (!t.in_op)
      return fail(err, QStringLiteral("总线不在 OP 状态"));

   if (m_meter == nullptr || !m_meter->isOpen())
      return fail(err, QStringLiteral("功率计没打开 —— 扫描要采数, 不能没有源"));

   if (m_order.empty())
      return fail(err, QStringLiteral("没有要扫的点"));

   QString pe = paramsError();
   if (!pe.isEmpty())
      return fail(err, pe);

   /* 轴数: 必须正好两根。扫描的语义就是 X=轴0 / Y=轴1, 少一根多的那根没意义 */
   if (t.naxis != 2)
      return fail(err, QStringLiteral("总线报到 %1 根轴, 扫描需要正好两根 (轴0 = X, 轴1 = Y)")
                            .arg(t.naxis));

   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      if (!a.valid || !a.mirror_ok)
         return fail(err, QStringLiteral("轴%1 还没收到完整的过程数据帧 —— 位置不可信").arg(i));
      if (!a.enabled)
         return fail(err, QStringLiteral("轴%1 没使能。「使能」是唯一让电机带电的按钮, 得先按它").arg(i));
      if (a.fault || t.fault)
         return fail(err, QStringLiteral("轴%1 有故障位 (6041h bit3) —— 先清故障再扫").arg(i));
      if ((a.sw & SCAN_LIMIT_BIT) != 0)
         return fail(err, QStringLiteral(
            "轴%1 的 6041h bit11 已经置起 —— 现在正压在硬件限位开关上 (2310h X1/X2)。\n"
            "这个状态开始扫描, 一头撞上去是必然的。先手动把它走离限位。").arg(i));
   }

   if (t.expected_wkc > 0 && t.wkc < t.expected_wkc)
      return fail(err, QStringLiteral("工作计数器不足 (%1/%2) —— 过程数据不完整, 别开始")
                            .arg(t.wkc).arg(t.expected_wkc));

   /*
    * 量程: 拿**实际生效的**那个, 而不是参数里算出来的。
    * 这两个会不一致 —— 量程是按「连接时」的参数设进 EcatThread 的, 之后把区域放大就
    * 对不上了。而这种不一致的表现是**边缘被静默夹掉**: 扫描照跑, 只是最外圈那几个点
    * 永远停在原地。所以这里必须用 telemetry 里的真值挡一道。
    */
   int32_t far = 0;
   for (size_t k = 0; k < m_order.size(); k++)
   {
      const Point &p = m_plan[(size_t)m_order[k]];
      far = std::max(far, std::abs(p.x_pul));
      far = std::max(far, std::abs(p.y_pul));
   }
   if (t.range > 0 && far > t.range)
      return fail(err, QStringLiteral(
         "最远的网格点是 %1 脉冲, 而当前量程只有 ±%2。\n"
         "超出量程的目标会被**静默夹掉** —— 那几条边永远扫不到, 而且不报错。\n"
         "区域改小一点, 或者断开重连一次 (量程是在连接时按区域参数设的)。")
         .arg(far).arg(t.range));

   /* ---- 过了。锁参数、设速度、进第一个点 ---- */
   m_run_p   = m_p;
   m_bad_wkc = 0;
   m_ord_i   = 0;

   /* 速度只在这里设一次 —— 一轮扫描从头到尾一个速度, 免得中途有人在动滑块 */
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
   m_pending  = false;
   m_spread[0] = m_spread[1] = 0;

   m_bus->setTarget(0, pt.x_pul);
   m_bus->setTarget(1, pt.y_pul);

   const int32_t tol = posTolPul(m_p);
   m_judge[0].begin(pt.x_pul, tol, m_p.settle_ms, m_now_ms);
   m_judge[1].begin(pt.y_pul, tol, m_p.settle_ms, m_now_ms);

   /*
    * 走不到就中止的最后期限。**这不是可有可无的**:
    * 目标被夹、机械卡住、驱动器跟不上, 表现都是"到位判据永远不成立" ——
    * 没有这一条, 扫描会一声不响地停在原地, 而操作员以为它还在跑。
    * 距离按**当前位置**量, 所以从上一行末尾折返的这一格也算得对。
    */
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

/* ---------------------------------------------------------------- 一轮 */

bool ScanController::start(const QString &csv_path, QString *err)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中 —— 先「中止」"));

   QString pe = paramsError();
   if (!pe.isEmpty())
      return fail(err, pe);

   rebuildPlan();
   if (m_plan.empty())
      return fail(err, QStringLiteral("网格是空的"));

   m_log.close();

   m_order.resize(m_plan.size());
   std::iota(m_order.begin(), m_order.end(), 0);
   m_is_retest = false;
   m_cur       = -1;
   m_run_ms    = -1;

   /* 新一轮 = 新的零点世代 (零点就是此刻的物理位置, 由操作员「设为区域中心」确立) */
   if (m_zero_epoch < 0)
      m_zero_epoch = 0;

   if (!m_log.beginNew(csv_path, m_p, isoNow(), m_zero_epoch, err))
      return false;

   if (!armRun(err))
   {
      m_log.close();
      return false;
   }
   return true;
}

bool ScanController::resume(const QString &csv_path, bool accept_zero_epoch_change,
                            QString *err, QString *why)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中 —— 先「中止」"));

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

   /*
    * 零点世代对不上 = 中间重连过。**不静默继续** —— 见 scancontroller.h 的说明。
    *
    * 比法是刻意不对称的: 只有"文件里有世代 (>=0) 且跟现在的不一样"才拦。
    * "自己这边不知道是第几代" (-1) **也算不一样**, 于是照样拦 —— 不确定的时候
    * 多问一句的成本是一次点击, 而猜错的成本是半张图的数据采在错的位置上。
    * 反过来, 文件里没写世代的老文件不拦 (那时也没什么可对的)。
    */
   if (csv_epoch >= 0 && csv_epoch != m_zero_epoch && !accept_zero_epoch_change)
   {
      return fail(why != nullptr ? why : err,
         QStringLiteral(
            "这个 CSV 是在**另一次零点**下采的 (文件里是第 %1 次, 现在是第 %2 次)。\n\n"
            "连接时零点会被重设为「当时所在的位置」, 所以断线重连之后同一个坐标\n"
            "指的**可能已经是另一个物理位置**。就这么接着扫, 下半场会和上半场拼在\n"
            "一张图上, 而图上不会有任何异常的样子。\n\n"
            "请先确认: 滑台现在的位置和「上一次零点确立时」是同一个物理位置\n"
            "(比如都停在同一个机械靠块 / 同一个对位标记上)。确认了再选「继续」。\n\n"
            "文件: %3 (开始于 %4)")
            .arg(csv_epoch).arg(m_zero_epoch)
            .arg(csv_path, fromStd(started_iso)));
   }

   /* 只补没采过的点 */
   m_order.clear();
   for (size_t i = 0; i < m_plan.size() && i < mask.size(); i++)
   {
      if (!mask[i])
         m_order.push_back((int)i);
   }

   if (m_order.empty())
      return fail(why != nullptr ? why : err,
                  QStringLiteral("这个 CSV 里的点已经全采完了 —— 没有要补的。"
                                 "要重来一轮就换个新文件"));

   /* 把已有进度**连数值一起**装进结果网格 —— 这样续扫一开始, 图上就已经有上半场了 */
   for (size_t i = 0; i < mask.size() && i < m_done.size(); i++)
      m_done[i] = mask[i];
   csvLoadGrid(text, m_p, &m_have, &m_watts);

   m_log.close();
   if (!m_log.beginAppend(csv_path, err))
      return false;

   m_is_retest = false;
   m_cur       = -1;
   m_run_ms    = -1;      /* elapsed_ms 从这次接上的那一刻算起 */

   if (!armRun(err))
   {
      m_log.close();
      return false;
   }
   return true;
}

bool ScanController::retest(int ix, int iy, QString *err)
{
   if (running())
      return fail(err, QStringLiteral("扫描进行中 —— 单点重测要等它停下来"));

   if (!m_log.isOpen())
      return fail(err, QStringLiteral(
         "还没有在跑的一轮 —— 单点重测是**往那个 CSV 里再追加一行**, 没有文件可追加。\n"
         "先「开始」或「续扫」。"));

   if (!sameGeom(m_run_p, m_p))
      return fail(err, QStringLiteral(
         "区域/分辨率/每单位脉冲数被改过了 —— 现在这个 (ix,iy) 和 CSV 里的那一点\n"
         "已经不是同一个地方, 追加进去会把两个坐标混在一个文件里。\n"
         "要重测就把参数改回去, 或者另开一轮。"));

   if (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny)
      return fail(err, QStringLiteral("格子 (%1,%2) 超出 %3×%4 的网格")
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
      return fail(err, QStringLiteral("网格里找不到 (%1,%2) —— 不该发生, 参数可能刚被改过")
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

/* ---------------------------------------------------------------- 暂停/中止 */

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

   /* **重新走完当前点并重新采样** —— 而不是从半截接着采。
    * 否则会写进一行"位置还没停稳时采的数", 而那一行在 CSV 里看不出任何异常。 */
   startPoint(m_cur);
}

void ScanController::abort(const QString &why)
{
   abortInternal(why, false);
}

void ScanController::abortInternal(const QString &why, bool automatic)
{
   if (!running())
   {
      if (automatic && !why.isEmpty())
         emit autoAborted(why);
      return;
   }

   stopMotion();
   m_cur = -1;
   enter(State::Aborted);

   /* **不关文件**: 中止之后最常见的动作就是"看一眼热力图, 把可疑的那几个点重测一遍",
    * 而重测正是往这个文件里追加。每行都已经 flush 过了, 留着它不会丢任何东西。 */
   if (automatic && !why.isEmpty())
      emit autoAborted(why);
   emit runFinished(false);
}

/* ---------------------------------------------------------------- 读数 */

void ScanController::beginReading()
{
   m_nsamp = 0;
   m_acc   = 0.0;
   enter(State::Reading);
   sendReading();
}

void ScanController::sendReading()
{
   m_pending = true;
   m_sent_ms = m_now_ms;
   m_meter->requestReading();
}

void ScanController::onReadingReady(double watts)
{
   /* 状态不对就是迟到的回包 (暂停时丢的那个), 扔掉 */
   if (m_st != State::Reading || !m_pending)
      return;

   m_pending = false;
   m_acc += watts;
   m_nsamp++;

   if (m_nsamp < m_p.samples_per_point)
   {
      sendReading();
      return;
   }

   m_acc /= (double)m_nsamp;
   finishPoint(true, std::string());
}

void ScanController::onReadingFailed(const QString &err)
{
   if (m_st != State::Reading || !m_pending)
      return;

   m_pending = false;

   /* 一个点读不到**不该毁掉整轮** —— 记下来继续走, 回头可以单点重测 */
   finishPoint(false, err.toStdString());
}

void ScanController::finishPoint(bool ok, const std::string &flags)
{
   if (m_cur < 0 || (size_t)m_cur >= m_plan.size())
   {
      abortInternal(QStringLiteral("内部错误: 收尾时当前点下标越界"), true);
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
   r.unix_ms      = wallMs();
   r.elapsed_ms   = (m_run_ms >= 0) ? (m_now_ms - m_run_ms) : 0;

   if (!m_log.isOpen() || !m_log.append(r))
   {
      abortInternal(QStringLiteral("CSV 写失败, 已自动中止 (后面的数据会丢): %1")
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

   /* 走完了 (整轮或单点重测)。**文件不关** —— 刚跑完看一眼图、随手重测几个可疑点,
    * 是最自然的动作, 而重测正是往这个文件里追加。 */
   m_is_retest = false;
   enter(State::Done);
   emit runFinished(true);
}

/* ---------------------------------------------------------------- 节拍 */

void ScanController::tick(int64_t now_ms)
{
   m_now_ms = now_ms;

   if (!running())
      return;

   /* 安全检查在**每个 tick**、每个状态下都跑 —— 包括暂停中。
    * 暂停不等于安全: 驱动器照样可能报故障、照样可能撞限位。 */
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
            "走不到位: 下发目标 (%1, %2) 已等 %3 秒, 实测位置 (%4, %5) 一直没进容差 ±%6。\n"
            "多半是卡住、被限位挡住, 或者目标被量程夹掉了。")
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
      if (!m_pending)
         return;                              /* 回包在事件队列里, 等它 */
      if (m_now_ms - m_sent_ms <= m_p.meter_timeout_ms)
         return;

      m_pending = false;
      finishPoint(false, "功率计超时 " + std::to_string(m_p.meter_timeout_ms) + " ms");
      return;

   case State::Idle:
   case State::Aborted:
   case State::Done:
      return;
   }
}

/* ---------------------------------------------------------------- 自动中止 */

bool ScanController::externalWantChanged(const BusTelem &t, int axis, int32_t *seen)
{
   if (m_cur < 0)
      return false;

   /*
    * 暂停时目标**本来就**不是我们下发的那个了 —— postStop() 会把它冻在当前位置
    * (want := tgt), 而那正是我们要的。不在这里放行的话, 一按暂停就会被自己
    * 判定成"有人从别处改了目标"然后自动中止 —— 暂停按钮变成了中止按钮。
    *
    * 继续的时候 startPoint() 会重新下发并把确认标志清掉, 检查自然接上。
    */
   if (m_st == State::Paused)
      return false;

   const int32_t w = t.ax[axis].want;
   if (seen != nullptr)
      *seen = w;

   const int32_t want = asClamped(m_issued[axis], t.range);

   /*
    * 下发到 publish 之间差一个 2ms 周期, 头几帧看到的还是**上一次**的目标,
    * 不等自己的值出现就比, 会把自已当成外部干预。所以先等它登上去。
    *
    * **但等待必须有上限。** 没有上限的话: 如果有人在"我们刚下发、还没登上去"的那
    * 几十毫秒里改了目标, 我们的值就永远不会出现 —— 这个门就永远开着, 于是这次外部
    * 干预**一次都不会被认出来**。那不报错的后果是滑台照着别人的目标走, 而扫描
    * 一直在等到位, 直到 m_move_deadline_ms (最长 120 秒) 才因为"走不到"停下来。
    *
    * 2ms 的发布周期下, 200ms 是 100 个周期 —— 够宽裕, 又不至于让人觉得卡住。
    */
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
      return QStringLiteral("总线已断开 —— 扫描自动中止");

   if (!t.in_op)
      return QStringLiteral("掉出了 OP 状态 —— 过程数据已经不可信, 扫描自动中止");

   if (t.fault)
      return QStringLiteral("驱动器报故障 (6041h bit3) —— 目标已被冻结, 扫描自动中止");

   /* WKC 不足是"拔网线/供电掉了"最直接的表现。但单帧抖动不值得中止,
    * 所以连续 10 帧 (30Hz 下约 1/3 秒) 才认。 */
   if (t.expected_wkc > 0 && t.wkc < t.expected_wkc)
   {
      m_bad_wkc++;
      if (m_bad_wkc >= 10)
         return QStringLiteral("工作计数器连续 %1 帧不足 (%2/%3) —— 检查网线与驱动器供电")
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
         return QStringLiteral("轴%1 丢了过程数据帧 —— 位置是陈值, 扫下去会采在错的地方").arg(i);

      if (!a.enabled)
         return QStringLiteral("轴%1 掉使能 (6041h bit2) —— 扫描自动中止").arg(i);

      /* 这一条是本轮最可能真触发的: 区域算错就会一头撞上去 */
      if ((a.sw & SCAN_LIMIT_BIT) != 0)
         return QStringLiteral(
            "轴%1 的 6041h bit11 置起 —— 撞上硬件限位开关了 (2310h X1 = 正 / X2 = 负)。\n"
            "扫描已自动中止。手动把它走离限位之后, 用「续扫」接着采。").arg(i);

      int32_t seen = 0;
      if (externalWantChanged(t, i, &seen))
         return QStringLiteral(
            "有人从别处改了轴%1 的目标 (现在 %2, 本点应该是 %3)。\n"
            "扫描期间不允许手动干预 —— 已自动中止。要手动控制就先「中止」。")
            .arg(i).arg(seen).arg(asClamped(m_issued[i], t.range));
   }

   return QString();
}

}   /* namespace scan */
