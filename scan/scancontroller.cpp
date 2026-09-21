#include "scancontroller.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scan {

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
   connect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
   connect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);

   rebuildPlan();
}

void ScanController::setMeter(PowerMeter *meter)
{
   if (meter == nullptr || meter == m_meter)
      return;

   /* 旧的必须显式断开: 它还活着, 不 disconnect 的话它下次出数会直接喂进状态机 */
   if (m_meter != nullptr)
   {
      disconnect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
      disconnect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);
   }

   m_meter = meter;
   connect(m_meter, &PowerMeter::readingReady,  this, &ScanController::onReadingReady);
   connect(m_meter, &PowerMeter::readingFailed, this, &ScanController::onReadingFailed);
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

void ScanController::rebuildPlan()
{
   /* 几何没变就什么都别动: 网格还是那个网格, 已有结果还是对的 */
   if (m_nx > 0 && !m_plan.empty() && sameGeom(m_p, m_plan_p))
      return;

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
   /* 作废未决读数: 这一点的回包到了也不要了 (重走这一点时会重新发请求) */
   m_pending = false;
}

bool ScanController::armRun(QString *err)
{
   const BusTelem t = m_bus->telemetry();

   if (!t.connected)
      return fail(err, QStringLiteral("没连上总线 —— 先点「连接」"));
   if (!t.in_op)
      return fail(err, QStringLiteral("总线不在 OP 状态"));

   /* 回零把轴留在使能 + HM: 自动中止判据抓不到它, 插补也不推进 (总线线程阻塞在 em_home
    * 里), 状态机会以为点到了而滑台没动 */
   if (t.homing)
      return fail(err, QStringLiteral("总线正在回零 —— 等它做完再启扫"));

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
         return fail(err, QStringLiteral(
            "轴%1 没使能。「使能」或「回零」都会让电机带电, 得先按其中一个").arg(i));
      if (a.fault || t.fault)
         return fail(err, QStringLiteral("轴%1 有故障位 (6041h bit3) —— 先清故障再扫").arg(i));
      if (a.limit_active)
         return fail(err, QStringLiteral(
            "%1。\n"
            "%2。\n"
            "%3。\n"
            /* 收尾只讲后果: 扫描期间限位成立就自动中止, 所以现在拒绝。
             * 不许说"会撞上去" —— %3 里有一种成因正是那限位根本不存在 (极性配反) */
            "扫描期间它是每 tick 都查、成立就自动中止的那一类, "
            "与其采到一半停在同一格, 不如这一趟现在就不开始。")
               .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)).arg(i))
               .arg(QString::fromUtf8(ecatcmd::limit_switch_text(
                       a.dig_known, a.dig_pos, a.dig_neg, t.di_invert)))
               .arg(QString::fromUtf8(ecatcmd::limit_hit_advice(
                       a.dig_known, a.dig_pos, a.dig_neg, a.dig_home, t.di_invert))));
   }

   if (t.expected_wkc > 0 && t.wkc < t.expected_wkc)
      return fail(err, QStringLiteral("工作计数器不足 (%1/%2) —— 过程数据不完整, 别开始")
                            .arg(t.wkc).arg(t.expected_wkc));

   /* 量程用 telemetry 里的真值, 不用参数算的: 量程是按连接时的参数设进 EcatThread 的,
    * 之后放大区域就对不上, 最外圈的点会被静默夹掉 */
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
   m_pending  = false;
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

   /* 新一轮 = 新的零点世代 (零点就是此刻的物理位置) */
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

   /* 零点世代对不上 = 中间重连过, 不静默继续。比法刻意不对称: 文件里写了世代 (>=0) 且
    * 与现在不同才拦, 自己这边 -1 也算不同; 没写世代的老文件不拦 */
   if (csv_epoch >= 0 && csv_epoch != m_zero_epoch && !accept_zero_epoch_change)
   {
      return fail(why != nullptr ? why : err,
         QStringLiteral(
            "这个 CSV 是在**另一次零点**下采的 (文件里是第 %1 次, 现在是第 %2 次)。\n\n"
            "连接时零点会被重设为「当时所在的位置」, 回零也会把零点整个搬到驱动器\n"
            "自报的那个原点 —— 这两种事之后, 同一个坐标指的**可能已经是另一个物理\n"
            "位置**了。就这么接着扫, 下半场会和上半场拼在一张图上, 而图上不会有\n"
            "任何异常的样子。\n\n"
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

   /* 已有进度连数值一起装进结果网格, 续扫一开始图上就有上半场 */
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
   if (!running())
   {
      if (automatic && !why.isEmpty())
         emit autoAborted(why);
      return;
   }

   stopMotion();
   m_cur = -1;
   enter(State::Aborted);

   /* 不关文件: 中止之后常要单点重测, 而重测就是往这个文件里追加 (每行都已 flush) */
   if (automatic && !why.isEmpty())
      emit autoAborted(why);
   emit runFinished(false);
}

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

   /* 一个点读不到不毁掉整轮: 记下来继续走, 回头可以单点重测 */
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

   /* 走完了 (整轮或单点重测)。文件不关: 重测要往同一个文件追加 */
   m_is_retest = false;
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
      return QStringLiteral("总线已断开 —— 扫描自动中止");

   if (!t.in_op)
      return QStringLiteral("掉出了 OP 状态 —— 过程数据已经不可信, 扫描自动中止");

   if (t.fault)
   {
      /* 6041h bit3 只说"有故障"; **说清是哪一个靠 603Fh** (ecatcmd::faulted_axes_text)。
       * 读不回来时那一句自己会说"还没读到 / 读不到", 不会编一个码出来 ——
       * 但这句自动中止是当场弹的, 那一刻码通常还没到, 所以中止窗口里那份"还没读到"
       * 是常态, 不是异常; 后面红横幅上会补上。 */
      const QString codes = ecatcmd::faulted_axes_text(t);

      return QStringLiteral("驱动器报故障 (6041h bit3) —— 目标已被冻结, 扫描自动中止%1")
                .arg(codes.isEmpty() ? QString()
                                     : QStringLiteral("。  故障码: ") + codes);
   }

   /* WKC 不足 = 拔网线 / 掉了供电; 单帧抖动不值得中止, 连续 10 帧 (30Hz 下约 1/3 秒) 才认 */
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

      /* 这一条最可能真触发。后两句是现场诊断: bit11 报的是硬件限位信号有效, 未必真有
       * 个开关压着 (见 ecatworker.h) */
      if (a.limit_active)
         return QStringLiteral(
            "%1。\n"
            "%2。\n"
            "%3。\n"
            "扫描已自动中止。处理完之后用「续扫」接着采, 已经采过的点不会重采。")
               .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)).arg(i))
               .arg(QString::fromUtf8(ecatcmd::limit_switch_text(
                       a.dig_known, a.dig_pos, a.dig_neg, t.di_invert)))
               .arg(QString::fromUtf8(ecatcmd::limit_hit_advice(
                       a.dig_known, a.dig_pos, a.dig_neg, a.dig_home, t.di_invert)));

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
