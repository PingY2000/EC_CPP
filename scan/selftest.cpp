/*
 * scan/selftest.cpp —— 不需要硬件、不需要界面, 直接把扫描逻辑跑一遍
 *
 * 为什么这个文件不是"可选的": [docs/hmi_click_position.md](../docs/hmi_click_position.md) 的
 * 「尚未验证」一节写得很清楚 —— **hmi 的「连接」那条路一次都没在真机上跑过**。也就是说
 * 这个仓库现在没有可用的真机回路来验扫描状态机。而扫描里真正会出错的东西:
 * 到位的时序、暂停后继续从哪儿接、外部改目标、续扫补哪些点、撞限位之后停不停 ——
 * **全是时序问题, 跟总线没关系**。所以它们在 FakeBus 上能验, 而且只有在这里能验。
 *
 * 时钟是**手拨的**: 状态机的 tick(now_ms) 由这个文件喂, 不是 QTimer。
 * 于是"等 60ms 稳定窗口"这种断言是确定的, 不受机器快慢影响, 整个文件也就跑几秒。
 *
 * 输出刻意全用 ASCII —— 这个程序会在各种控制台里被跑, 中文在不同代码页下会变成乱码,
 * 而"哪一条断言失败了"必须看得见。(失败时打印的参数原因是中文的, 那没办法, 那正是要看的。)
 *
 * 它**不链 SOEM 也不链 Qt Widgets**, 只有 Qt6::Core。
 */
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "scanarrive.h"
#include "scancontroller.h"
#include "scanlog.h"
#include "scanplan.h"
#include "powermeter.h"

using namespace scan;

/* ---------------------------------------------------------------- 断言 */

static int g_fail = 0;
static int g_pass = 0;
static const char *g_case = "";

static void check(bool ok, const char *what, const std::string &extra = std::string())
{
   if (ok)
   {
      g_pass++;
      return;
   }
   g_fail++;
   std::printf("  FAIL [%s] %s", g_case, what);
   if (!extra.empty())
      std::printf("   -- %s", extra.c_str());
   std::printf("\n");
}

static void checkEq(long long got, long long want, const char *what)
{
   char buf[128];
   std::snprintf(buf, sizeof(buf), "got %lld, want %lld", got, want);
   check(got == want, what, buf);
}

static void checkNear(double got, double want, const char *what)
{
   char buf[160];
   std::snprintf(buf, sizeof(buf), "got %.9g, want %.9g", got, want);
   check(std::fabs(got - want) < 1e-6, what, buf);
}

static void caseBegin(const char *name)
{
   g_case = name;
   std::printf("%s\n", name);
}

/* ---------------------------------------------------------------- 假总线 */

/*
 * 自己按速度往前推位置。
 *
 * 关键的一条: `pos` 是**跟出来的**, 不是 setTarget 的瞬间就等于目标 ——
 * 这一条正是要验的东西。真实的滑台在插值目标到位之后还要追一段, 而
 * `doStop()` 会让 at_target 立刻为真。两件事在这里都能复现。
 */
class FakeBus : public BusView
{
public:
   FakeBus()
   {
      t_.connected    = true;
      t_.in_op        = true;
      t_.naxis        = 2;
      t_.range        = 4000000;      /* 够大, 免得量程夹取混进别的断言 */
      t_.wkc          = 2;
      t_.expected_wkc = 2;
      for (int i = 0; i < 2; i++)
      {
         t_.ax[i].valid     = true;
         t_.ax[i].mirror_ok = true;
         t_.ax[i].enabled   = true;
         t_.ax[i].at_target = true;
         t_.ax[i].vel       = 20000;
         t_.ax[i].sw        = 0x0027;   /* 使能且无故障的常见位型 */
      }
   }

   BusTelem telemetry() const override { return t_; }

   void setTarget(int a, int32_t w) override
   {
      if (t_.range > 0)
      {
         if (w >  t_.range) w =  t_.range;
         if (w < -t_.range) w = -t_.range;
      }
      t_.ax[a].want = w;
      t_.ax[a].at_target = (t_.ax[a].want == t_.ax[a].tgt);
   }

   void setSpeed(int a, uint32_t v) override { t_.ax[a].vel = v; }

   /* 与 EcatThread::doStop 同义: 目标就地冻住, **保持使能** */
   void postStop() override
   {
      for (int i = 0; i < 2; i++)
      {
         t_.ax[i].tgt = t_.ax[i].pos;
         t_.ax[i].want = t_.ax[i].tgt;
         t_.ax[i].at_target = true;
      }
   }

   /* 时间流逝 dt 毫秒 */
   void step(int dt_ms)
   {
      if (freeze_)
         return;

      for (int i = 0; i < 2; i++)
      {
         AxisTelem &a = t_.ax[i];

         /* 插值目标按速度追 want */
         if (a.want != a.tgt)
         {
            int64_t d = (int64_t)a.want - (int64_t)a.tgt;
            int64_t s = (int64_t)a.vel * dt_ms / 1000;
            if (s < 1)
               s = 1;

            if (d > 0) a.tgt += (int32_t)std::min<int64_t>(d, s);
            else       a.tgt -= (int32_t)std::min<int64_t>(-d, s);
         }

         /*
          * 实际位置再追插值目标。**插值目标停了它也还得追** ——
          * 真实滑台就是这样 (插补器走完了, 驱动器还要追一段), 也正是
          * ArrivalJudge 不能只看 at_target 的原因。pos_lag 越大追得越慢。
          */
         const int64_t div = (pos_lag_ms_ > 0) ? pos_lag_ms_ : 1;
         int64_t pd = (int64_t)a.tgt - (int64_t)a.pos;
         int64_t ps = (int64_t)a.vel * dt_ms / (1000 * div);
         if (ps < 1) ps = 1;

         if (pd > 0)      a.pos += (int32_t)std::min<int64_t>(pd, ps);
         else if (pd < 0) a.pos -= (int32_t)std::min<int64_t>(-pd, ps);

         a.at_target = (a.want == a.tgt);
      }
   }

   /* ---- 注入 ---- */
   void setFault(int i)        { t_.ax[i].fault = true; t_.fault = true; }
   void setEnabled(int i, bool e) { t_.ax[i].enabled = e; }
   void setLimit(int i, bool on)
   {
      if (on) t_.ax[i].sw |= SCAN_LIMIT_BIT;
      else    t_.ax[i].sw &= (uint16_t)~SCAN_LIMIT_BIT;
   }
   void setDropFrames(int i, bool d) { t_.ax[i].mirror_ok = !d; }
   void setWkc(int w)          { t_.wkc = w; }
   void setInOp(bool v)        { t_.in_op = v; }
   void freezeMotion(bool f)   { freeze_ = f; }
   void setPosLag(int ms)      { pos_lag_ms_ = ms; }
   void setRange(int32_t r)    { t_.range = r; }
   int32_t want(int i) const   { return t_.ax[i].want; }
   int32_t tgt(int i) const    { return t_.ax[i].tgt; }
   int32_t pos(int i) const    { return t_.ax[i].pos; }

private:
   BusTelem t_;
   bool     freeze_ = false;
   int      pos_lag_ms_ = 0;
};

/* ---------------------------------------------------------------- 假功率计 */

/*
 * **回包不由 requestReading() 触发, 而是由测试喂的时钟触发。**
 *
 * 真实现 (powermeter.cpp 里那三个) 用 QTimer::singleShot 延时投递, 那需要事件循环;
 * 这个文件里没有事件循环 —— 时间是被手拨的。所以这里记下"该什么时候回",
 * 由 rig 在拨表的时候把它放出来。行为上等价于一个几百毫秒延迟的真串口功率计。
 */
class FakeMeter : public PowerMeter
{
public:
   QString kind() const override { return QStringLiteral("fake"); }
   bool open(QString *) override { m_open = true; return true; }
   void close() override         { m_open = false; }

   void requestReading() override
   {
      if (!m_open)
      {
         emit readingFailed(QStringLiteral("没开"));
         return;
      }
      armed_ = true;
      fails_ = fail_next_;
      fail_next_ = false;
      due_   = now_ + latency_ms_;
   }

   void setNow(int64_t ms) { now_ = ms; }
   void setLatency(int ms) { latency_ms_ = ms; }
   void setValue(double v) { value_ = v; }
   void failNext()         { fail_next_ = true; }

   /* 把到期的回包放出去。由 rig 在每个 tick 之后调用 */
   void pump(int64_t now)
   {
      now_ = now;
      if (!armed_ || now < due_)
         return;

      armed_ = false;
      if (fails_)
      {
         fails_ = false;
         emit readingFailed(QStringLiteral("模拟失败"));
         return;
      }

      readings_++;
      emit readingReady(value_);
   }

   int readings() const { return readings_; }

private:
   double  value_      = 1.0;
   int64_t now_        = 0;
   int64_t due_        = 0;
   int     latency_ms_ = 60;
   int     readings_   = 0;
   bool    armed_      = false;
   bool    fails_      = false;
   bool    fail_next_  = false;
};

/* ---------------------------------------------------------------- 台架 */

struct Rig
{
   FakeBus        bus;
   FakeMeter      meter;
   ScanController ctrl { &bus, &meter };
   int64_t        now = 1000;

   Rig()
   {
      meter.setNow(now);
      meter.setLatency(60);
      meter.open(nullptr);
      ctrl.tick(now);            /* 让状态机的钟和这里的钟对齐, 再开始 */
   }

   /* 拨一格表: 机械走 → 状态机看一眼 → 功率计回包 */
   void stepOnce(int dt = 20)
   {
      bus.step(dt);
      meter.setNow(now);
      ctrl.tick(now);
      meter.pump(now);
      now += dt;
   }

   /* 这三个都要先对齐时钟 —— 状态机记的是**最近一次 tick 的时刻**,
    * 而 start/resume/retest 都会拿它算期限 (见 scancontroller.cpp 的 startPoint) */
   bool startScan(const QString &csv, QString *err)
   {
      ctrl.tick(now);
      return ctrl.start(csv, err);
   }

   bool resumeScan(const QString &csv, bool accept, QString *err, QString *why)
   {
      ctrl.tick(now);
      return ctrl.resume(csv, accept, err, why);
   }

   bool retestAt(int ix, int iy, QString *err)
   {
      ctrl.tick(now);
      return ctrl.retest(ix, iy, err);
   }

   /* 一直拨到条件成立, 或超时。返回是否成立 */
   bool runUntil(const std::function<bool()> &done, int64_t max_ms = 60000, int dt = 20)
   {
      const int64_t stop = now + max_ms;
      while (now < stop)
      {
         stepOnce(dt);
         if (done())
            return true;
      }
      return done();
   }

   bool runToIdle(int64_t max_ms = 120000)
   {
      return runUntil([this] { return ctrl.state() == ScanController::State::Done
                                   || ctrl.state() == ScanController::State::Aborted; }, max_ms);
   }

   /* 小网格, 跑得快: 2×2 单位 @ 0.5 → 5×5 = 25 点 */
   static Params smallParams()
   {
      Params p;
      p.area_x_unit = 2.0;
      p.area_y_unit = 2.0;
      p.res_unit    = 0.5;
      p.speed_pul_s = 100000;   /* 上限, 于是 0.5 单位 = 25000 pul 只要 0.25s */
      p.dwell_ms    = 40;
      p.settle_ms   = 60;
      p.samples_per_point = 1;
      return p;
   }
};

/* ================================================================ 用例 */

static void test_grid()
{
   caseBegin("grid: 27x27 @ 0.5 -> 55x55");

   Params p;
   checkEq(axisCount(p.area_x_unit, p.res_unit), 55, "nx");
   checkEq(axisCount(p.area_y_unit, p.res_unit), 55, "ny");

   std::vector<Point> plan = buildPlan(p);
   checkEq((long long)plan.size(), 3025, "point count");

   std::vector<double> xs = axisCoords(27.0, 0.5);
   checkNear(xs.front(), -13.5, "first coord");
   checkNear(xs.back(),   13.5, "last coord");

   /* 蛇形: 第 0 行正向, 第 1 行反向 */
   checkEq(plan[0].ix, 0,  "row0 first ix");
   checkEq(plan[54].ix, 54, "row0 last ix");
   checkEq(plan[55].ix, 54, "row1 first ix (serpentine)");
   checkEq(plan[109].ix, 0, "row1 last ix (serpentine)");
   checkEq(plan[54].iy, 0,  "row0 iy");
   checkEq(plan[55].iy, 1,  "row1 iy");

   /* 每一步只动一根轴 —— 这是蛇形的意义所在, 也是时间估算成立的依据 */
   int diag = 0;
   for (size_t i = 1; i < plan.size(); i++)
   {
      if (plan[i].ix != plan[i - 1].ix && plan[i].iy != plan[i - 1].iy)
         diag++;
   }
   checkEq(diag, 0, "no diagonal steps in a serpentine path");

   /* 除不尽时网格居中且不越界 */
   std::vector<double> odd = axisCoords(10.0, 3.0);   /* n = 4, span = 9 < 10 */
   checkEq((long long)odd.size(), 4, "10/3 -> 4 points");
   checkNear(odd.front(), -4.5, "10/3 first");
   checkNear(odd.back(),   4.5, "10/3 last");
   check(std::fabs(odd.back()) <= 5.0, "grid stays inside the area");

   caseBegin("grid: 单位换算与量程");
   checkEq(pulseOf(-13.5, 50000.0), -675000, "pulseOf(-13.5)");
   checkNear(unitOf(-675000, 50000.0), -13.5, "unitOf(round trip)");
   checkEq(autoRangePul(p), 725000, "auto range = (13.5 + 1) * 50000");
   checkEq(posTolPul(p), 1250, "pos tol = step/20");

   /* 小步长时容差不该大到"允许偏一整个格子" */
   Params tiny = p;
   tiny.res_unit = 0.002;                              /* 步长 100 pul */
   int32_t tt = posTolPul(tiny);
   check(tt >= 1 && tt <= 100 / 4 + 1, "pos tol stays below a quarter step");

   caseBegin("grid: 参数校验");
   Params bad = p;
   bad.res_unit = 0.0;
   check(!validate(bad).empty(), "res = 0 rejected");

   bad = p;
   bad.res_unit = 100.0;
   check(!validate(bad).empty(), "res larger than the area rejected");

   bad = p;
   bad.res_unit = 0.001;                                /* 27000 x 27000 > 200000 */
   check(!validate(bad).empty(), "too many points rejected");

   bad = p;
   bad.speed_pul_s = 10;
   check(!validate(bad).empty(), "speed out of range rejected");

   check(validate(p).empty(), "the default parameters pass", validate(p));

   std::string why;
   Params narrow = p;
   narrow.range_pul = 100000;                            /* 只有 ±2 单位 */
   check(!fitsRange(narrow, &why), "area outside the range rejected", why);
}

static void test_csv()
{
   caseBegin("csv: meta + rows round trip");

   Params p;
   std::string text = csvMetaLines(p, "2026-09-17T10:00:00", 3);
   text += csvColumnHeader();
   text += "\n";

   std::vector<Point> plan = buildPlan(p);
   for (size_t i = 0; i < 10; i++)
   {
      Row r;
      r.index = (int)i;
      r.pt    = plan[i];
      r.watts = 1.0 + (double)i * 0.5;
      r.ok    = true;
      r.unix_ms = 1000 + (int64_t)i;
      r.elapsed_ms = (int64_t)i * 100;
      text += csvRowLine(r);
   }

   /* 掉电那一刻的现场: 最后一行写了一半, 连换行都没有。
    * 单独留一份 —— 后面那些"读回数值"的断言要在完整文件上做 */
   const std::string half = text + "11,1";

   std::vector<char> mask;
   int max_index = -1;
   std::string iso;
   int epoch = -1;
   std::string diff = csvParseForResume(half, p, &mask, &max_index, &iso, &epoch);

   check(diff.empty(), "same geometry parses clean", diff);
   checkEq(max_index, 9, "max_index");            /* 残行不算数 */
   checkEq(epoch, 3, "zero_epoch survives the round trip");
   check(iso == "2026-09-17T10:00:00", "started survives");
   checkEq((long long)mask.size(), 3025, "mask size");
   checkEq(mask[0], 1, "cell 0 marked");
   checkEq(mask[1], 1, "cell 1 marked");

   /* 末尾残行也不该把数值读乱 */
   std::vector<char>   have;
   std::vector<double> watts;
   csvLoadGrid(half, p, &have, &watts);
   checkNear(watts[0], 1.0, "a half-written tail does not disturb the values");

   caseBegin("csv: 几何一变就拒绝续扫, 而且指名道姓");
   Params moved = p;
   moved.res_unit = 0.25;
   std::string d2 = csvParseForResume(half, moved, nullptr, nullptr, nullptr, nullptr);
   check(!d2.empty(), "different resolution rejected");
   check(d2.find("0.5") != std::string::npos || d2.find("分辨率") != std::string::npos,
         "the message says which parameter changed", d2);

   Params moved2 = p;
   moved2.pulses_per_unit = 10000.0;
   check(!csvParseForResume(half, moved2, nullptr, nullptr, nullptr, nullptr).empty(),
         "different pulses-per-unit rejected");

   caseBegin("csv: 数值读回, 重测取最后一行");

   csvLoadGrid(text, p, &have, &watts);
   checkEq((long long)have.size(), 3025, "grid size");
   checkEq(have[0], 1, "cell 0 has a value");
   checkNear(watts[0], 1.0, "cell 0 value");
   checkNear(watts[9], 5.5, "cell 9 value");
   checkEq(have[100], 0, "untouched cell is empty");

   /* 同一格再来一行 = 单点重测, 图上要的是后一行 */
   Row rt;
   rt.pt = plan[0];
   rt.watts = 42.0;
   rt.ok = true;
   rt.flags = "retest";
   text += csvRowLine(rt);

   csvLoadGrid(text, p, &have, &watts);
   checkNear(watts[0], 42.0, "retest overwrites the value");

   /* 采失败的点 (功率列空) 不该算"有值" */
   Row bad;
   bad.pt = plan[3];
   bad.ok = false;
   bad.flags = "timeout";
   text += csvRowLine(bad);

   csvLoadGrid(text, p, &have, &watts);
   checkEq(have[3], 0, "a failed point has no value");

   caseBegin("csv: 真损坏要报出来");
   std::string broken = csvMetaLines(p, "x", 1) + csvColumnHeader() + "\n";
   broken += "0,0,0,-13.5,-13.5,-675000,-675000,1,1,,0,0,0,0,0,0\n";
   broken += "1,0\n";                                    /* 中间出现残行 */
   broken += "2,1,0,-13,-13.5,-650000,-675000,1,1,,0,0,0,0,0,0\n";
   check(!csvParseForResume(broken, p, nullptr, nullptr, nullptr, nullptr).empty(),
         "a truncated middle line is corruption, not a partial write");

   std::string outside = csvMetaLines(p, "x", 1) + csvColumnHeader() + "\n";
   outside += "0,99,0,0,0,0,0,1,1,,0,0,0,0,0,0\n";       /* ix 超出 55 */
   check(!csvParseForResume(outside, p, nullptr, nullptr, nullptr, nullptr).empty(),
         "an out-of-grid index rejected");
}

static void test_arrive()
{
   caseBegin("arrive: 到位判据");

   ArrivalJudge j;
   std::string why;

   ArrivalObs o;
   o.in_op = o.valid = o.mirror_ok = o.enabled = true;

   j.begin(1000, 50, 100, 0);

   /* 头一帧: 机械还没动, at_target 已经是真 (下发之前的状态) —— 不能算到位 */
   o.at_target = true;
   o.pos = 0;
   check(j.feed(o, 0, &why) == ArrivalJudge::Verdict::Waiting,
         "must not arrive before we have seen it move");

   o.at_target = false;                                  /* 门开 */
   j.feed(o, 20, &why);

   /* 在容差外 */
   o.at_target = true;
   o.pos = 900;
   check(j.feed(o, 40, &why) == ArrivalJudge::Verdict::Waiting,
         "outside the tolerance is not arrival");

   /* 进容差, 但只持续了一帧 */
   o.pos = 1000;
   check(j.feed(o, 60, &why) == ArrivalJudge::Verdict::Waiting,
         "one frame inside is not enough");
   check(j.windowOpen(), "window is open");

   check(j.feed(o, 120, &why) == ArrivalJudge::Verdict::Waiting,
         "still short of the settle window");

   check(j.feed(o, 170, &why) == ArrivalJudge::Verdict::Arrived,
         "arrived once the window has held long enough");

   caseBegin("arrive: 窗口被打破就要重新计时");
   j.begin(1000, 50, 100, 0);
   o.at_target = false; o.pos = 0;
   j.feed(o, 0, &why);
   o.at_target = true;  o.pos = 1000;
   j.feed(o, 20, &why);
   o.at_target = false;                                  /* 抖了一下 */
   j.feed(o, 40, &why);
   o.at_target = true;
   check(j.feed(o, 100, &why) == ArrivalJudge::Verdict::Waiting,
         "the clock restarts after a break");
   check(j.feed(o, 160, &why) == ArrivalJudge::Verdict::Waiting,
         "and it is not done yet");

   caseBegin("arrive: 该报错的要报错, 而不是一直等");
   j.begin(1000, 50, 100, 0);
   o.at_target = false; o.pos = 0;
   j.feed(o, 0, &why);

   ArrivalObs off = o;
   off.enabled = false;
   check(j.feed(off, 20, &why) == ArrivalJudge::Verdict::Faulted,
         "a mid-scan disable is a fault, not a wait", why);

   ArrivalObs flt = o;
   flt.fault = true;
   j.begin(1000, 50, 100, 0);
   j.feed(o, 0, &why);
   check(j.feed(flt, 20, &why) == ArrivalJudge::Verdict::Faulted, "a drive fault aborts");

   ArrivalObs noop = o;
   noop.in_op = false;
   j.begin(1000, 50, 100, 0);
   j.feed(o, 0, &why);
   check(j.feed(noop, 20, &why) == ArrivalJudge::Verdict::Faulted, "leaving OP aborts");

   ArrivalObs stale = o;
   stale.mirror_ok = false;
   j.begin(1000, 50, 100, 0);
   j.feed(o, 0, &why);
   check(j.feed(stale, 20, &why) == ArrivalJudge::Verdict::Waiting,
         "a dropped frame is just a wait (the position is stale, not wrong)");

   caseBegin("arrive: 位置没到但 at_target 已经为真 (doStop 的陷阱)");
   j.begin(1000, 50, 100, 0);
   o.at_target = false; o.pos = 0;
   j.feed(o, 0, &why);
   o.at_target = true; o.pos = 400;                      /* 停止之后惯性还在滑 */
   j.feed(o, 20, &why);
   check(j.feed(o, 200, &why) == ArrivalJudge::Verdict::Waiting,
         "at_target alone does not mean it stopped");
   o.pos = 1000;
   j.feed(o, 220, &why);                                 /* 窗口在这里开 */
   check(j.feed(o, 280, &why) == ArrivalJudge::Verdict::Waiting,
         "the settle window counts from when it actually got there");
   check(j.feed(o, 330, &why) == ArrivalJudge::Verdict::Arrived,
         "arrives once the position really is there");
}

static void test_run()
{
   caseBegin("run: 一整轮扫完 (FakeBus + FakeMeter)");

   QTemporaryDir dir;
   check(dir.isValid(), "temp dir");
   const QString csv = dir.filePath("run.csv");

   Rig rig;
   rig.ctrl.setParams(Rig::smallParams());
   rig.ctrl.rebuildPlan();
   checkEq(rig.ctrl.totalPoints(), 25, "5x5 = 25 points");

   rig.meter.setLatency(60);
   rig.meter.setValue(2.5);

   QString err;
   check(rig.startScan(csv, &err), "start accepted", err.toStdString());

   bool done = rig.runToIdle();
   check(done, "the run reaches Done");
   check(rig.ctrl.state() == ScanController::State::Done, "state is Done");
   checkEq(rig.ctrl.completedPoints(), 25, "25 cells completed");
   checkEq(rig.meter.readings(), 25, "25 readings taken");

   /* 每个格子都该有数, 且都在正确的位置上 */
   int valued = 0;
   for (int iy = 0; iy < 5; iy++)
      for (int ix = 0; ix < 5; ix++)
         if (rig.ctrl.cellHasValue(ix, iy))
            valued++;
   checkEq(valued, 25, "every cell has a value");

   checkNear(rig.ctrl.cellValue(0, 0), 2.5, "the value landed in the grid");

   double lo = 0, hi = 0;
   check(rig.ctrl.wattsRange(&lo, &hi), "watts range available");
   checkNear(lo, 2.5, "min");
   checkNear(hi, 2.5, "max");

   /* 扫完之后滑台应该停在最后一个点上, 而且是真的到了 */
   const Point *last = rig.ctrl.pointAt(24);
   check(last != nullptr, "last point exists");
   if (last != nullptr)
   {
      checkNear((double)rig.bus.pos(0), (double)last->x_pul, "axis 0 parked on the last point");
      checkNear((double)rig.bus.pos(1), (double)last->y_pul, "axis 1 parked on the last point");
   }

   /* CSV: 表头 + 25 行 */
   std::string text;
   check(readCsvText(csv, &text, &err), "csv readable", err.toStdString());
   int lines = 0;
   for (char c : text)
      if (c == '\n')
         lines++;
   check(lines >= 30, "csv has the header and 25 rows");

   std::vector<char> mask;
   std::string diff = csvParseForResume(text, rig.ctrl.params(), &mask, nullptr, nullptr, nullptr);
   check(diff.empty(), "the file we just wrote parses back clean", diff);

   int marked = 0;
   for (char c : mask)
      if (c)
         marked++;
   checkEq(marked, 25, "all 25 cells are marked done in the file");

   caseBegin("run: 暂停冻在原地, 继续会重走当前点");

   Rig r2;
   r2.ctrl.setParams(Rig::smallParams());
   r2.ctrl.rebuildPlan();
   r2.meter.setValue(7.0);
   check(r2.startScan(dir.filePath("pause.csv"), &err), "start", err.toStdString());

   /* 跑到第三个点 */
   r2.runUntil([&] { return r2.ctrl.completedPoints() >= 3; });
   checkEq(r2.ctrl.completedPoints() >= 3, 1, "at least 3 points done");

   r2.ctrl.pause();
   check(r2.ctrl.state() == ScanController::State::Paused, "paused");

   const int32_t px = r2.bus.pos(0), py = r2.bus.pos(1);
   for (int k = 0; k < 20; k++)
      r2.stepOnce();
   checkEq(r2.bus.pos(0), px, "axis 0 frozen while paused");
   checkEq(r2.bus.pos(1), py, "axis 1 frozen while paused");
   checkEq(r2.bus.want(0), r2.bus.tgt(0), "axis 0 target frozen at the interpolated point");
   check(r2.bus.telemetry().ax[0].enabled, "still enabled (holding torque) while paused");

   const int done_at_pause = r2.ctrl.completedPoints();

   r2.ctrl.resumeRun();
   check(r2.ctrl.state() == ScanController::State::Moving, "back to Moving");
   check(r2.runToIdle(), "finishes after resuming");
   checkEq(r2.ctrl.completedPoints(), 25, "all 25 done after resume");
   check(r2.ctrl.completedPoints() > done_at_pause, "made progress after resuming");

   caseBegin("run: 中止 = 停住 + 不写半点");

   Rig r3;
   r3.ctrl.setParams(Rig::smallParams());
   r3.ctrl.rebuildPlan();
   const QString csv3 = dir.filePath("abort.csv");
   check(r3.startScan(csv3, &err), "start", err.toStdString());
   r3.runUntil([&] { return r3.ctrl.completedPoints() >= 2; });

   r3.ctrl.abort(QStringLiteral("操作员按了中止"));
   check(r3.ctrl.state() == ScanController::State::Aborted, "aborted");

   const int32_t ax = r3.bus.pos(0), ay = r3.bus.pos(1);
   for (int k = 0; k < 20; k++)
      r3.stepOnce();
   checkEq(r3.bus.pos(0), ax, "axis 0 stopped");
   checkEq(r3.bus.pos(1), ay, "axis 1 stopped");

   std::string t3;
   readCsvText(csv3, &t3, nullptr);
   int rows3 = 0;
   for (char c : t3)
      if (c == '\n')
         rows3++;
   check(rows3 < 30, "the abort left a partial file (that is the point)");

   caseBegin("run: 续扫只补没采过的点");

   std::vector<char>   m4;
   std::vector<double> w4;
   Params p4 = Rig::smallParams();
   std::string diff4 = csvParseForResume(t3, p4, &m4, nullptr, nullptr, nullptr);
   check(diff4.empty(), "the partial file is resumable", diff4);

   Rig r4;
   r4.ctrl.setParams(p4);
   r4.ctrl.rebuildPlan();
   r4.meter.setValue(3.0);
   r4.ctrl.setZeroEpoch(0);      /* 窗口在「连接 / 设为区域中心」时做的事 */

   QString why;
   check(r4.resumeScan(csv3, false, &err, &why), "resume accepted",
         (err + " | " + why).toStdString());
   checkEq(r4.ctrl.completedPoints(), r3.ctrl.completedPoints(),
           "already-done points are loaded as progress");

   check(r4.runToIdle(), "the resume finishes");
   checkEq(r4.ctrl.completedPoints(), 25, "and the grid ends up complete");

   std::string t4;
   readCsvText(csv3, &t4, nullptr);
   int rows4 = 0;
   for (char c : t4)
      if (c == '\n')
         rows4++;
   check(rows4 > rows3, "appended to the same file rather than starting a new one");
}

static void test_aborts()
{
   caseBegin("abort: 撞硬件限位 -> 自动中止");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_limit.csv", &err), "start", err.toStdString());

      bool fired = false;
      QString what;
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { fired = true; what = s; });

      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });
      r.bus.setLimit(1, true);              /* 轴1 撞上负限位 */
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);

      check(fired, "autoAborted fired");
      check(what.contains(QStringLiteral("bit11")) || what.contains(QStringLiteral("限位")),
            "the message names the limit switch", what.toStdString());
   }

   caseBegin("abort: 驱动器故障");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_fault.csv", &err), "start", err.toStdString());
      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      bool fired = false;
      QObject::connect(&r.ctrl, &ScanController::autoAborted, [&](const QString &) { fired = true; });
      r.bus.setFault(0);
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);
      check(fired, "a drive fault aborts the scan");
   }

   caseBegin("abort: 掉使能");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_dis.csv", &err), "start", err.toStdString());
      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      r.bus.setEnabled(1, false);
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);
      check(r.ctrl.state() == ScanController::State::Aborted, "losing an enable aborts");
   }

   caseBegin("abort: 拔网线 (WKC 连续不足)");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_wkc.csv", &err), "start", err.toStdString());
      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      r.bus.setWkc(0);
      /* 单帧抖动不该中止 —— 要连续 10 帧 */
      r.stepOnce();
      check(r.ctrl.state() != ScanController::State::Aborted, "one bad frame is not enough");
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);
      check(r.ctrl.state() == ScanController::State::Aborted, "a sustained shortfall aborts");
   }

   caseBegin("abort: 有人从旁路改了目标");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_ext.csv", &err), "start", err.toStdString());

      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      bool fired = false;
      QObject::connect(&r.ctrl, &ScanController::autoAborted, [&](const QString &) { fired = true; });
      r.bus.setTarget(0, 123456);           /* 画布点击/回中 走的就是这条路 */
      r.runUntil([&] { return fired; }, 5000);
      check(fired, "an external target change aborts the scan");
   }

   caseBegin("abort: 走不到就中止, 而不是一声不响地等下去");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_stuck.csv", &err), "start", err.toStdString());

      r.bus.freezeMotion(true);             /* 机械卡住 */
      bool fired = false;
      QString what;
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { fired = true; what = s; });
      r.runUntil([&] { return fired; }, 30000);
      check(fired, "a stuck axis aborts instead of hanging", what.toStdString());
   }

   caseBegin("abort: 到位判据要的是实测位置, 不只是插补走完");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;

      /* 位置滞后于插值目标 —— 真实滑台就是这样, 而 at_target 会提前为真 */
      r.bus.setPosLag(4);
      check(r.startScan(QDir::tempPath() + "/scan_lag.csv", &err), "start", err.toStdString());
      check(r.runToIdle(120000), "still finishes with position lag");

      /* 采数那一刻, 实测位置必须真的在目标上 —— 这就是 pos_tol 那道判据的意义 */
      check(r.ctrl.state() == ScanController::State::Done, "completed");
      checkEq(r.ctrl.completedPoints(), 25, "25 points despite the lag");
   }
}

static void test_retest()
{
   caseBegin("retest: 单点重测追加一行, 且不改动别的格子");

   QTemporaryDir dir;
   const QString csv = dir.filePath("retest.csv");

   Rig rig;
   rig.ctrl.setParams(Rig::smallParams());
   rig.ctrl.rebuildPlan();
   rig.meter.setValue(1.0);

   QString err;
   check(rig.startScan(csv, &err), "start", err.toStdString());
   check(rig.runToIdle(), "first pass finishes");

   std::string before;
   check(readCsvText(csv, &before, &err), "read back", err.toStdString());
   int rows_before = 0;
   for (char c : before)
      if (c == '\n')
         rows_before++;

   /* 重测 (2,3) */
   rig.meter.setValue(99.0);
   check(rig.retestAt(2, 3, &err), "retest accepted", err.toStdString());
   check(rig.runToIdle(), "retest finishes");

   checkNear(rig.ctrl.cellValue(2, 3), 99.0, "the retested cell took the new value");
   checkNear(rig.ctrl.cellValue(0, 0), 1.0, "other cells untouched");

   std::string after;
   check(readCsvText(csv, &after, &err), "read back", err.toStdString());
   int rows_after = 0;
   for (char c : after)
      if (c == '\n')
         rows_after++;

   checkEq(rows_after, rows_before + 1, "exactly one row appended");
   check(after.find("retest") != std::string::npos, "the row is flagged as a retest");

   /* 参数改过之后不许重测 —— 坐标系已经对不上了 */
   Params moved = rig.ctrl.params();
   moved.res_unit = 0.25;
   rig.ctrl.setParams(moved);
   check(!rig.retestAt(0, 0, &err), "retest refused after the geometry changed");

   /* 没有在跑的一轮就没有文件可追加 */
   Rig fresh;
   fresh.ctrl.setParams(Rig::smallParams());
   check(!fresh.ctrl.retest(0, 0, &err), "retest refused with no run in progress");
}

static void test_preflight()
{
   caseBegin("preflight: 缺一条都不许动");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      QString err;

      r.bus.setEnabled(0, false);
      check(!r.startScan(QDir::tempPath() + "/pf1.csv", &err), "not enabled -> refused");
      check(err.contains(QStringLiteral("使能")), "the reason says so", err.toStdString());

      r.bus.setEnabled(0, true);
      r.meter.close();
      check(!r.startScan(QDir::tempPath() + "/pf2.csv", &err), "no meter -> refused");
      check(err.contains(QStringLiteral("功率计")), "the reason mentions the meter", err.toStdString());

      r.meter.open(nullptr);
      r.bus.setLimit(1, true);
      check(!r.startScan(QDir::tempPath() + "/pf3.csv", &err),
            "already sitting on a limit switch -> refused");
      check(err.contains(QStringLiteral("bit11")), "the reason mentions bit11", err.toStdString());

      r.bus.setLimit(1, false);
      r.bus.setInOp(false);
      check(!r.startScan(QDir::tempPath() + "/pf4.csv", &err), "not in OP -> refused");

      /* 一切正常时**必须放行** —— 只会拒绝的 Preflight 跟没写一样 */
      r.bus.setInOp(true);
      check(r.startScan(QDir::tempPath() + "/pf5.csv", &err), "healthy -> accepted", err.toStdString());
      r.ctrl.abort(QString());
   }

   caseBegin("preflight: 量程装不下区域就拒绝 (边缘被静默夹掉是最坏的一类 bug)");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      QString err;
      r.bus.setRange(1000);                  /* 量程比网格小得多 */
      check(!r.startScan(QDir::tempPath() + "/pf6.csv", &err), "refused");
      check(err.contains(QStringLiteral("量程")), "the reason mentions the range", err.toStdString());
   }
}

int main(int argc, char **argv)
{
   QCoreApplication app(argc, argv);

   std::printf("scan selftest -- no hardware, no GUI, hand-driven clock\n\n");

   test_grid();
   test_csv();
   test_arrive();
   test_run();
   test_aborts();
   test_retest();
   test_preflight();

   std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
   return g_fail == 0 ? 0 : 1;
}
