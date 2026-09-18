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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "scanarrive.h"
#include "scancontroller.h"
#include "scanlog.h"
#include "scanplan.h"
#include "scanprefs.h"
#include "powermeter.h"

/* test_ophir 那条腿要用 COM。这只是个头文件, 没有把 SOEM/Widgets 拖进来 ——
 * 本文件"不链 SOEM 也不链 Qt Widgets"那条不变量没破 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "ophircom.h"
#include "ophirmeter.h"

using namespace scan;

/* ---------------------------------------------------------------- 断言 */

static int g_fail = 0;
static int g_pass = 0;
static int g_skip = 0;
static const char *g_case = "";

/*
 * 「这条**没验**」和「这条验失败了」是两回事, 报出来的数必须分开数。
 *
 * 用在需要外部东西的那几条上 —— 目前只有真机功率计那条 (要这台机器装了 Ophir 的
 * StarLab)。没装就把这条标成 SKIP: 它不是失败的, 但**也不能算通过** ——
 * 把没跑的当跑过了, 是自检最容易骗到自己的地方。
 */
static void skipCase(const char *why)
{
   g_skip++;
   std::printf("  SKIP [%s] %s\n", g_case, why);
}

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

   /*
    * 撞限位。**sw 摆好之后, limit_active 是用真判据算出来的**, 不是照抄一个 true ——
    * EcatThread::publish() 里就是 `limit_active = ecatcmd::limit_hit(sw, dig..., di_invert)`。
    * 这个假总线必须跟着**同一条函数**走: 照抄的话, 反转那一档 (sw 与 dig 谁说了算正好
    * 相反) 就会在测试里退化成一个"两样总是一致"的假世界, 而那种不一致恰恰是最该测的。
    */
   void setLimit(int i, bool on)
   {
      if (on) t_.ax[i].sw |= EM_SW_INTLIMIT;
      else    t_.ax[i].sw &= (uint16_t)~EM_SW_INTLIMIT;
      recomputeLimit(i);
   }

   /*
    * 「输入电平反转 (NPN)」。**总线级** —— 一次改全部轴, 与 publish() 里"一次 load 出
    * 一个局部量、所有轴共用"是同一件事。
    */
   void setDiInvert(bool on)
   {
      t_.di_invert = on;
      for (int i = 0; i < EM_MAX_AXES; i++)
         recomputeLimit(i);
   }

   /*
    * 三个限位开关本身 (60FDh)。**dig_known 单独一个开关**, 不靠"三个都 false"推 ——
    * 读不到 60FDh 时那三位也是 false, 而"三个都没压住"是个看起来完全正常的结论。
    * 默认 unknown: 真机上生效的 1A00h 里没有 60FDh, 那才是常态。
    *
    * **写进来的值要先反相**, 与 publish() 里那三行 `a.dig_x = !a.dig_x` 对着 ——
    * 这个注入口给的是**驱动器 60FDh 的原始读数**, 不是反转之后的值。反相放在这一层,
    * 测试就能写"60FDh 说两个限位都压着, 而反转开着 -> 实际一个都没压着"这种句子。
    */
   void setDigKnown(int i, bool k) { t_.ax[i].dig_known = k; recomputeLimit(i); }
   void setDig(int i, bool home, bool pos, bool neg)
   {
      t_.ax[i].dig_known = true;
      t_.ax[i].dig_home  = t_.di_invert ? !home : home;
      t_.ax[i].dig_pos   = t_.di_invert ? !pos  : pos;
      t_.ax[i].dig_neg   = t_.di_invert ? !neg  : neg;
      recomputeLimit(i);
   }
   void setDropFrames(int i, bool d) { t_.ax[i].mirror_ok = !d; }
   void setWkc(int w)          { t_.wkc = w; }
   void setInOp(bool v)        { t_.in_op = v; }
   /* 总线正在回零。**只影响 armRun 那道闸** —— 真回零是工作线程在跑, 这个假总线
    * 不假装能复现它, 只复现"控制器看得到的那一位" */
   void setHoming(bool v)      { t_.homing = v; }
   void freezeMotion(bool f)   { freeze_ = f; }
   void setPosLag(int ms)      { pos_lag_ms_ = ms; }
   void setRange(int32_t r)    { t_.range = r; }
   int32_t want(int i) const   { return t_.ax[i].want; }
   int32_t tgt(int i) const    { return t_.ax[i].tgt; }
   int32_t pos(int i) const    { return t_.ax[i].pos; }

private:
   /* 唯一一处把 sw / dig / 反转合成 limit_active 的地方 —— 与 publish() 同一个函数 */
   void recomputeLimit(int i)
   {
      const AxisTelem &a = t_.ax[i];
      t_.ax[i].limit_active = ecatcmd::limit_hit(a.sw, a.dig_known, a.dig_pos,
                                                 a.dig_neg, t_.di_invert);
   }

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

/* ------------------------------------------------- 点数上限 (一道资源闸) */

/*
 * 用户报的那条"参数设到某些值程序就卡死", 根子在这里。
 *
 * **在修好之前, 这个文件里的一行 check 都写不出来** —— 因为"卡死"发生在 setParams 里:
 * buildPlan 会给 floor(区域/分辨率)+1 的平方个 Point 开空间。区域 500 × 分辨率 0.001
 * 是 2.5e11 个点 (几十 TB); 小一点的那些**分配得下来**, 然后填满它要几秒 ——
 * 而界面在这期间一帧都刷不出来, 操作员看到的就是"卡死", 而且回不去 (下一次按键又要
 * 在同一个坑里再走一遍)。
 *
 * 所以现在断言的是: **超上限时一个 Point 都不建** —— 网格按 0×0 报, 三个结果数组清空,
 * 开始按钮那一关 (validate) 照样拦。而"正好卡在上限上"必须建得出来: 闸太紧会误伤
 * 合法的大网格, 那比不闸更坏 (人会把参数改小到能跑为止, 而扫出来的是错的区域)。
 */
static void test_plancap()
{
   caseBegin("plan cap: 太大就不建网格 (卡死的根子)");
   {
      Rig r;
      Params p = Rig::smallParams();
      p.area_x_unit = 500.0;
      p.area_y_unit = 500.0;
      p.res_unit    = 0.001;

      /* 先确认这份参数**真的**是超大的那份 —— 否则下面几条是空验 */
      checkEq(axisCount(p.area_x_unit, p.res_unit), 500001, "the requested grid is huge");
      checkEq((long long)500001 * 500001, 250001000001LL, "and its point count is astronomic");

      r.ctrl.setParams(p);

      check(!r.ctrl.paramsError().isEmpty(), "validate rejects it");
      checkEq(r.ctrl.totalPoints(), 0, "no plan was built");
      checkEq(r.ctrl.gridNx(), 0, "grid reported empty (nx)");
      checkEq(r.ctrl.gridNy(), 0, "grid reported empty (ny)");
      check(!r.ctrl.cellDone(0, 0), "cellDone on an empty grid answers false");
      check(!r.ctrl.cellHasValue(0, 0), "cellHasValue on an empty grid answers false");
      checkNear(r.ctrl.cellValue(0, 0), 0.0, "cellValue on an empty grid answers 0");
      checkEq(r.ctrl.estimateTotalMs(), 0, "estimate is 0, not an overflow");

      QString err;
      check(!r.startScan(QDir::tempPath() + "/cap1.csv", &err), "start is refused");
      check(err.contains(QStringLiteral("点数")), "the reason is the point count",
            err.toStdString());
   }

   caseBegin("plan cap: 正好到上限必须建得出来, 退回去必须能恢复");
   {
      Rig r;

      /* 400 × 500 = 200000, 正好是上限 */
      Params p = Rig::smallParams();
      p.res_unit    = 0.5;
      p.area_x_unit = 199.5;                 /* floor(199.5/0.5)+1 = 400 */
      p.area_y_unit = 249.5;                 /* floor(249.5/0.5)+1 = 500 */
      checkEq(axisCount(p.area_x_unit, p.res_unit), 400, "nx at the limit");
      checkEq(axisCount(p.area_y_unit, p.res_unit), 500, "ny at the limit");

      r.ctrl.setParams(p);
      check(r.ctrl.paramsError().isEmpty(), "exactly at the limit is legal");
      checkEq(r.ctrl.gridNx(), 400, "grid built (nx)");
      checkEq(r.ctrl.gridNy(), 500, "grid built (ny)");
      checkEq(r.ctrl.totalPoints(), 200000, "grid built (points)");

      /* 多一格就拒 */
      Params q = p;
      q.area_x_unit = 200.0;                 /* floor(200/0.5)+1 = 401 -> 200500 点 */
      r.ctrl.setParams(q);
      check(!r.ctrl.paramsError().isEmpty(), "one column over the limit is refused");
      checkEq(r.ctrl.totalPoints(), 0, "and its plan is gone, not half-built");

      /* 退回来必须重建 —— 否则"改坏了再改回来"就永远回不到能扫的状态 */
      r.ctrl.setParams(Rig::smallParams());
      checkEq(r.ctrl.totalPoints(), 25, "back to a small grid rebuilds the plan");
      checkEq(r.ctrl.gridNx(), 5, "back to a small grid (nx)");

      QString err;
      check(r.startScan(QDir::tempPath() + "/cap2.csv", &err), "and it scans again",
            err.toStdString());
      r.ctrl.abort(QString());
   }
}

/* ------------------------------------------------- 参数记忆 (scan.ini) */

/*
 * 这一层最容易**悄悄**错: 键名错一个字母不报错, 只是那一项永远记不住 ——
 * 而"哪张网卡"这件事要等到下次开机才发现, 那时没人会想到是 ini 的锅。
 *
 * 网卡名 (Npcap 的 `\Device\NPF_{GUID}`) 是这条路上唯一的坑: **INI 是有转义字符的格式**,
 * 反斜杠写进去再读回来会不会变成别的样子, 只有真跑一遍才知道。下面那一条断言就钉这个。
 */
static void test_prefs()
{
   caseBegin("prefs: 文件不存在时给缺省 (第一次运行与读了个半截的 ini 是同一条路)");
   {
      QTemporaryDir dir;
      check(dir.isValid(), "temp dir");
      const QString ini = dir.filePath(QStringLiteral("scan.ini"));

      const Prefs none = prefsLoad(ini);
      checkNear(none.params.area_x_unit, 27.0, "default area_x");
      checkNear(none.params.res_unit, 0.5, "default res");
      checkEq((long long)none.params.speed_pul_s, 20000, "default speed");
      checkEq(none.params.samples_per_point, 1, "default samples");
      check(none.params.serpentine, "default serpentine");
      checkEq(none.manual_speed, -1, "manual speed unset");
      check(none.nic.isEmpty(), "no nic yet");
      checkEq(none.params.range_pul, 0, "range is never remembered");
   }

   caseBegin("prefs: 存进去再读回来, 逐项相等");
   {
      QTemporaryDir dir;
      const QString ini = dir.filePath(QStringLiteral("scan.ini"));

      Prefs p;
      p.params.area_x_unit      = 12.5;
      p.params.area_y_unit      = 7.25;
      p.params.res_unit         = 0.125;
      p.params.pulses_per_unit  = 4096.0;
      p.params.speed_pul_s      = 33333;
      p.params.dwell_ms         = 150;
      p.params.settle_ms        = 80;
      p.params.samples_per_point = 4;
      p.params.serpentine       = false;
      p.params.start_positive   = false;
      p.nic = QStringLiteral("\\Device\\NPF_{9A3C1E7B-4D2F-4A18-9C55-6B0E2F7A1D43}");
      p.manual_speed = 12345;

      prefsSave(ini, p);
      const Prefs b = prefsLoad(ini);

      checkNear(b.params.area_x_unit, 12.5, "area_x");
      checkNear(b.params.area_y_unit, 7.25, "area_y");
      checkNear(b.params.res_unit, 0.125, "res");
      checkNear(b.params.pulses_per_unit, 4096.0, "pulses_per_unit");
      checkEq((long long)b.params.speed_pul_s, 33333, "speed");
      checkEq(b.params.dwell_ms, 150, "dwell");
      checkEq(b.params.settle_ms, 80, "settle");
      checkEq(b.params.samples_per_point, 4, "samples");
      check(!b.params.serpentine, "serpentine=false survives");
      check(!b.params.start_positive, "start_positive=false survives");
      checkEq(b.manual_speed, 12345, "manual speed");
      check(b.nic == p.nic, "the Npcap device path survives INI escaping",
            b.nic.toStdString());
   }

   caseBegin("prefs: 不能扫的参数盖不掉上一次能用的那份");
   {
      Prefs store;
      const Params good = Rig::smallParams();
      prefsMergeParams(&store, good);
      checkNear(store.params.area_x_unit, 2.0, "a scannable set is remembered");

      /* 手滑: 分辨率少打一位 —— 这份过不了 validate */
      Params bad = Rig::smallParams();
      bad.res_unit = 0.0001;
      check(!validate(bad).empty(), "the slipped set really is illegal");
      prefsMergeParams(&store, bad);
      checkNear(store.params.res_unit, 0.5, "the illegal set did NOT overwrite the old one");
      checkNear(store.params.area_x_unit, 2.0, "and the rest of the old set is intact");

      /* 量程一律归零 —— 它是算出来的 */
      Params withr = Rig::smallParams();
      withr.range_pul = 725000;
      prefsMergeParams(&store, withr);
      checkEq(store.params.range_pul, 0, "range is dropped, not remembered");
   }
}

/* ------------------------------------------------- 故障复位 (6040h bit7 上升沿) */

/*
 * 这些判据全在 ecatcmd 里 (头文件, inline) —— **故意放在头文件**:
 * scan_selftest 不编 ecatworker.cpp, 逻辑写在 .cpp 里就等于永远验不到,
 * 而下面第一条恰好是这个程序里最不能错的一行。
 */
static void test_faultreset()
{
   using namespace ecatcmd;

   /*
    * 「该不该复位这根轴」= 全部安全性所在。
    *
    * em_fault_reset() 的动作顺序是"先写 6040h = 0x0000 (卸力) 打十帧, 再抬 bit7"
    * (bit7 是上升沿触发, 不先压 0 构不成沿), 而它**到函数末尾**才报告"本来就没有故障"。
    * 所以对一根健康的保持轴做这件事会真的松开保持力矩 —— 竖直滑台会掉下来。
    * 这道闸必须在**调用之前**, 而它就是下面这四行。
    */
   caseBegin("faultreset: 只有「可信 + 有故障」才许碰");
   check(!axis_needs_reset(false, true,  true),  "invalid axis is skipped");
   check(!axis_needs_reset(false, false, true),  "invalid + unknown is skipped");
   check(!axis_needs_reset(true,  false, true),  "**unknown (mirror_ok=false) is skipped, not guessed**");
   check(!axis_needs_reset(true,  true,  false), "healthy axis is never written to");
   check(!axis_needs_reset(true,  false, false), "unknown + healthy is skipped");
   check( axis_needs_reset(true,  true,  true),  "trusted + faulted -> reset");

   /* ---- 从一份遥测里挑出该复位的轴 ---- */
   caseBegin("faultreset: 挑轴 — 挑不到就一个字节都不写");
   {
      BusTelem t;
      t.naxis = 2;
      for (int i = 0; i < 2; i++)
      {
         t.ax[i].valid     = true;
         t.ax[i].mirror_ok = true;
      }

      int out[EM_MAX_AXES];

      /* 一根都没故障 —— 这是最要紧的一种情况: 返回 0 = 一个字节都不该写 */
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "no fault -> nothing to do");

      /* 两根都故障 */
      t.ax[0].fault = t.ax[1].fault = true;
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 2, "both faulted");
      checkEq(out[0], 0, "first is axis 0");
      checkEq(out[1], 1, "second is axis 1");

      /* 轴0 有故障、轴1 状态未知 —— **未知的跳过, 不猜**。猜错 = 对它卸力 */
      t.ax[1].fault     = true;
      t.ax[1].mirror_ok = false;
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 1, "an unknown axis is skipped");
      checkEq(out[0], 0, "and only the trusted one is left");

      /* 状态未知的轴线上一根都不碰 */
      t.ax[0].mirror_ok = false;
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "all unknown -> nothing");

      /* valid=false 同理 */
      t.ax[0].mirror_ok = true;
      t.ax[0].valid     = false;
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "invalid -> nothing");

      /* 没有轴 */
      BusTelem none;
      none.naxis = 0;
      checkEq(pick_faulted_axes(none, out, EM_MAX_AXES), 0, "no axes -> nothing");

      /* out 装不下时**仍返回真实条数**: 调用方靠这个数决定该说什么 */
      t.ax[0].valid = true;
      t.ax[0].fault = t.ax[1].fault = true;
      t.ax[1].mirror_ok = true; t.ax[1].valid = true;
      checkEq(pick_faulted_axes(t, out, 1), 2, "max is respected but the count is honest");
      checkEq(out[0], 0, "and the first one is still reported");

      /* out = nullptr: 只要个数 */
      checkEq(pick_faulted_axes(t, nullptr, 0), 2, "count only");
   }
}

/* ------------------------------------------------- 三个限位开关 (60FDh) */

/*
 * 撞限位的判定**只有一处** (ecatcmd::limit_hit)。这里把**三条**规则都钉住 ——
 * 包括还没打开的那一条, 和 2026-09-18 才加的反转那一条。
 *
 * 「还没打开的那一条」为什么也要测: kRefineLimitWithDigIn 从 0 改成 1 是一次
 * **有证据的改动** (见 ecatworker.h), 改的那一刻不该再补测试。所以规则写成带
 * 参数的函数, 三条分支在同一次构建里都跑得到 —— 否则"要改一个 #define 重编一次
 * 才能验另一条", 而安全关键的逻辑不能那样测。
 */
static void test_limitsw()
{
   using namespace ecatcmd;

   const uint16_t LIM = EM_SW_INTLIMIT;

   caseBegin("limitsw: 今天的判定 = bit11 单独, 一个比特没变");
   check( limit_hit(LIM, false, false, false, false), "bit11 + nothing known -> still hit");
   check( limit_hit(LIM, true,  false, false, false), "bit11 + both switches released -> still hit");
   check( limit_hit(LIM, true,  true,  false, false), "bit11 + positive switch");
   check( limit_hit(LIM, true,  false, true,  false), "bit11 + negative switch");
   check(!limit_hit(0,   true,  true,  true,  false), "no bit11 -> never a hit");
   check(!limit_hit(0,   false, false, false, false), "no bit11, nothing known");

   /*
    * 「原点不算」这件事**在类型上就成立了**: limit_hit 的参数里根本没有原点那一位
    * (dig_home 传不进来), 所以它不可能影响中止判定 —— 这不是靠一条 if 记得写对。
    * 于是"只压住原点"在这里长的就是 (pos=false, neg=false) 这一组。
    */
   caseBegin("limitsw: 精判据 (开关关着的那条分支)");
   check(!limit_hit_rule(LIM, true,  false, false, LIMIT_RULE_REFINED),
         "**bit11 + only the home switch pressed (pos/neg both released) -> NOT a hit**");
   check( limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_REFINED), "bit11 + positive switch -> hit");
   check( limit_hit_rule(LIM, true,  false, true,  LIMIT_RULE_REFINED), "bit11 + negative switch -> hit");
   check( limit_hit_rule(LIM, true,  true,  true,  LIMIT_RULE_REFINED), "bit11 + both -> hit");
   /* **这一条是"不弱化"的保证**: 不知道 60FDh 就退回旧判据, 保护一点不减 */
   check( limit_hit_rule(LIM, false, false, false, LIMIT_RULE_REFINED),
         "bit11 + unknown 60FDh -> falls back to bit11 alone");
   check(!limit_hit_rule(0,   true,  false, false, LIMIT_RULE_REFINED), "no bit11 -> still no hit");

   /*
    * ---- 第三条判据: 输入反转 (NPN)。2026-09-18 真机那条 ----
    *
    * 那台机器上 2300h 配反了, bit11 **恒为 1** —— 它不是判据了, 是个常数。
    * 所以这一条判据的全部意义就在下面第一组断言里: **bit11 置起而开关都松开 -> 不中止**。
    * 这与 REFINED 那条方向正好相反, 也是这台机器唯一能跑起来的走法。
    *
    * 反转的语义是"把限位判定从**驱动器的意见**换成**开关的真实状态**", 那 bit11 就必须
    * 整个退场。写成本条判据而不是复用 REFINED (`bit11 && (...)`), 是为了不留下那个耦合:
    * bit11 恒 1 时两者等价, 而哪天 2300h 被改对、反转忘了关, bit11 一变 0 就会把
    * `bit11 && ...` 整条**恒置为 false** —— 保护静悄悄地全没, 而这个分支不会。
    */
   caseBegin("limitsw: 反转那条判据 —— bit11 不参与, 保护压在开关上");
   check(!limit_hit_rule(LIM, true,  false, false, LIMIT_RULE_INVERT),
         "**bit11 set but both switches released -> NOT a hit** (bit11 is stuck-1 here)");
   check( limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_INVERT),
         "inverted: positive switch pressed -> hit");
   check( limit_hit_rule(LIM, true,  false, true,  LIMIT_RULE_INVERT),
         "inverted: negative switch pressed -> hit");
   /* 反转开着时 bit11 连"多一层保险"都算不上 —— 它不参与, 置不置起结果一样 */
   check( limit_hit_rule(0,   true,  true,  false, LIMIT_RULE_INVERT)
       == limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_INVERT),
         "inverted: bit11 makes no difference at all");
   /*
    * **三条判据里只有这一条把「未知」判成中止。** 因为此时退无可退: bit11 已经不用了,
    * 开关又读不到。REFINED 那条能退回 bit11, 这条没有可退的东西 —— 那就不动。
    */
   check( limit_hit_rule(0,   false, false, false, LIMIT_RULE_INVERT),
         "inverted + unknown 60FDh -> abort, because there is no judge left");

   caseBegin("limitsw: 哪个开关选哪条判据 —— 只此一处 (limit_rule_for)");
   checkEq((int)limit_rule_for(false),
           (int)(kRefineLimitWithDigIn != 0 ? LIMIT_RULE_REFINED : LIMIT_RULE_BIT11),
           "反转关着 -> 由 kRefineLimitWithDigIn 那个编译期开关决定");
   checkEq((int)limit_rule_for(true), (int)LIMIT_RULE_INVERT,
           "反转开着 -> 一定是 INVERT, 与那个编译期开关无关");
   /* 端到端那一条: 同一份输入, 反转一开一关必须得到**相反**的结论 —— 否则这个开关没接上 */
   check( limit_hit(LIM, true, false, false, false)
       && !limit_hit(LIM, true, false, false, true),
         "bit11 + switches released: hit without invert, NOT a hit with invert");

   caseBegin("limitsw: 现场诊断那句话不许把「不知道」说成「都没压着」");
   {
      const char *unk = limit_switch_text(false, false, false, false);
      check(std::strstr(unk, "无从得知") != nullptr,
            "unknown is reported as unknown, not as 'nothing pressed'", unk);
      /* 反转开着而读不到 60FDh: 比"不知道"还严重一层, 那句话得说出来 */
      const char *unk_inv = limit_switch_text(false, false, false, true);
      check(std::strstr(unk_inv, "反转") != nullptr,
            "unknown + invert is called out as one judge short", unk_inv);
      check(std::strcmp(unk, unk_inv) != 0, "...and it is not the same sentence");

      const char *none_p = limit_switch_text(true, false, false, false);
      check(std::strstr(none_p, "都没压着") != nullptr, "known + released says so", none_p);
      const char *pos = limit_switch_text(true, true, false, false);
      check(std::strstr(pos, "正限位") != nullptr, "positive limit is named", pos);
      const char *neg = limit_switch_text(true, false, true, false);
      check(std::strstr(neg, "负限位") != nullptr, "negative limit is named", neg);

      /* 反转开着时每一条都要说明"这是反相之后的值" —— 不然那个值没有来处 */
      check(std::strstr(limit_switch_text(true, true, false, true), "反相") != nullptr,
            "inverted: the value is labelled as post-inversion",
            limit_switch_text(true, true, false, true));
   }

   /*
    * 2026-09-18 真机那条: X0~X3 接的是 **NPN** 传感器 (高电平 = 未触发), 而 2300h
    * (输入有效电平逻辑) 按常开配着 —— 于是两个限位输入常年读成"压着", bit11 恒置起,
    * **扫描一次都开不起来**, 而现场的文案只说"两个都压着, 先手动走离限位" ——
    * 那句话把人支去追一个不存在的限位。
    *
    * 这一条钉的是**措辞**, 不是逻辑: 正负限位同时压着物理上不成立, 所以那句话说出口
    * 时必须带上"这多半不是真的"和"往 2300h 查"。逻辑一个字没改 —— bit11 照样挡住启扫,
    * 该挡就得挡。
    */
   caseBegin("limitsw: 正负限位同时压着 = 不可能, 文案必须点破并指向 2300h");
   {
      const char *both = limit_switch_text(true, true, true, false);
      check(std::strstr(both, "2300h") != nullptr,
            "the impossible combination points at the polarity parameter", both);
      check(std::strstr(both, "同时") != nullptr, "it says the two are simultaneous", both);

      const char *adv = limit_hit_advice(true, true, true, false, false);
      check(std::strstr(adv, "2300h") != nullptr, "the advice names 2300h too", adv);
      /* 这一句是这次要根治的东西: 不许再叫人去走离一个不存在的限位 */
      check(std::strstr(adv, "先手动把它走离") == nullptr,
            "and it does NOT tell the operator to walk off a limit that is not there", adv);
      /* 反转这个新出路也要点一下, 但必须连带说清它只治软件那一侧 */
      check(std::strstr(adv, "输入电平反转") != nullptr, "the advice mentions the new way out",
            adv);
      check(std::strstr(adv, "只治软件") != nullptr,
            "...and says plainly that it only fixes this side", adv);

      /* 真正的单边压着 —— 那句"走离限位"在**这一支**上仍然是对的 */
      const char *one = limit_hit_advice(true, true, false, false, false);
      check(std::strstr(one, "走离") != nullptr, "a real single limit still says walk off it",
            one);

      /* 三种情形的建议必须彼此不同 —— 同一条建议套在四种成因上就是原来那个毛病 */
      const char *a1 = limit_hit_advice(false, false, false, false, false);
      const char *a2 = limit_hit_advice(true, false, false, false, false);
      const char *a3 = limit_hit_advice(true, false, false, true, false);
      check(std::strcmp(a1, a2) != 0 && std::strcmp(a2, a3) != 0 && std::strcmp(a1, a3) != 0,
            "unknown / neither-pressed / home-only each get their own advice");
      check(std::strstr(a2, "607Dh") != nullptr, "neither pressed -> look at the soft limits",
            a2);
      check(std::strstr(a3, "原点") != nullptr, "home only -> says it is the home switch", a3);
   }

   /*
    * ---- 反转开着时, 上面那几句的**意思全变了**, 所以必须是另外几句话 ----
    *
    * 「正负限位同时压着」在两种语境下是两个病:
    *   反转关着 -> 极性配反 (两边一起反相), 该去查 2300h;
    *   反转开着 -> dig_* 已经是反相之后的值, 两路还同时为真就意味着 60FDh 的 bit1/bit0
    *               同时为 0, 也就是两个输入端**真的**都被读成低电平。往 2300h 上找
    *               是找不到的 —— 极性错只会让两边一起反相, 反相完就该松开了。
    * 同一条建议套在两种成因上, 就是这次要根治的那个毛病本身, 只是换了个方向。
    */
   caseBegin("limitsw: 反转开着时「同时压着」换了个意思, 文案必须跟着换");
   {
      const char *t_off = limit_switch_text(true, true, true, false);
      const char *t_on  = limit_switch_text(true, true, true, true);
      check(std::strcmp(t_off, t_on) != 0, "same input, two different sentences");
      check(std::strstr(t_on, "2300h") == nullptr,
            "**with invert on it must NOT send you to 2300h** — polarity is already handled",
            t_on);
      check(std::strstr(t_on, "反相") != nullptr, "it says the values are post-inversion", t_on);

      const char *a_on = limit_hit_advice(true, true, true, false, true);
      check(std::strstr(a_on, "2300h") == nullptr,
            "nor does the advice: that lever is already pulled", a_on);
      check(std::strstr(a_on, "供电") != nullptr,
            "it points at wiring / sensor power instead", a_on);

      /*
       * 反转开着时**单边压着反而是可信的** —— 判定用的就是它。这一句比反转关着时更强,
       * 因为它不再是"多半是", 而是"就是这个"。措辞不同 = 两种语境分得开。
       */
      const char *one_on  = limit_hit_advice(true, true, false, false, true);
      const char *one_off = limit_hit_advice(true, true, false, false, false);
      check(std::strcmp(one_on, one_off) != 0, "single limit: invert changes the wording");
      check(std::strstr(one_on, "可信") != nullptr,
            "with invert on the reading is stated as trustworthy, not as a guess", one_on);

      /*
       * 反转开着而读不到 60FDh: 这是**必然开不了扫描**, 不是"可能有问题" ——
       * 措辞里必须有那两条出路, 而且不能跟"反转关着时的未知"用同一句话。
       */
      const char *u_on  = limit_hit_advice(false, false, false, false, true);
      const char *u_off = limit_hit_advice(false, false, false, false, false);
      check(std::strcmp(u_on, u_off) != 0, "unknown 60FDh: invert changes the advice");
      check(std::strstr(u_on, "永远开不了") != nullptr,
            "**with invert on, unknown 60FDh means the scan can never start**", u_on);
      check(std::strstr(u_on, "关掉") != nullptr, "and one way out is to turn the invert off",
            u_on);

      /*
       * 开场白也要跟着判据换: 反转开着时 limit_active 与 bit11 毫无关系,
       * 还说"bit11 置起"就是把人支去查一个决定不了任何事的位。
       *
       * 注意它**仍然提到** bit11 —— 但说的是"无关"。这是故意的: 操作员前面看到的
       * 每一句横幅、文档里每一条待验证问题都在讲 bit11, 不主动回答"那 bit11 呢"
       * 反而是把疑问留在那儿。所以这里钉的不是"不许出现这个词", 而是**不许说它置起**。
       */
      const char *h_off = limit_hit_headline(false);
      const char *h_on  = limit_hit_headline(true);
      check(std::strstr(h_off, "bit11") != nullptr
            && std::strstr(h_off, "置起") != nullptr,
            "invert off: the headline says bit11 is set", h_off);
      check(std::strstr(h_on, "置起") == nullptr,
            "**invert on: the headline must NOT claim bit11 is set** — it is not the judge",
            h_on);

      /*
       * 措辞必须落在**信号**上, 不能落到"撞上了"。
       *
       * ykd 手册 V2.4 对 6041h bit11 的定义是「硬件限位信号有效时置 1」—— 它是那路
       * 信号**此刻的电平**, 既不是驱动器的判断, 也不是一次已经发生的碰撞。回零时它
       * 本来就该是 1 (motor_api/ec_motor_motion.c 的回零分支里就写着"bit11 硬件限位
       * 有效, 仍在找原点")。把这个区别说丢, 现场就会去"清故障 / 重新使能", 或者
       * 干脆不敢回零。
       *
       * 这个仓库里从前每一句旧文案都是"撞上了", 所以必须有东西钉住 —— 不然下一次
       * 改文案的人(是未来的我, 也是别人)一定会飘回去。
       */
      check(std::strstr(h_off, "硬件限位信号有效") != nullptr,
            "the headline uses the manual's own wording for bit11", h_off);
      check(std::strstr(h_off, "撞") == nullptr,
            "**and it does not say the axis crashed into a limit** — bit11 is a level, "
            "not a collision", h_off);
      check(std::strstr(h_off, "驱动器认为") == nullptr,
            "...nor that the drive 'thinks' anything: nothing here is a verdict", h_off);
      check(std::strstr(h_on, "无关") != nullptr,
            "...and it answers the obvious question by saying bit11 is unrelated", h_on);
      check(std::strcmp(h_off, h_on) != 0, "two headlines, not one");
   }

   /*
    * ---- 新灯**不参与任何中止判据** ----
    *
    * 这是本轮那个决定的回归护栏: 只有原点开关压着时, 扫描必须一路跑到 Done,
    * autoAborted 一次都不能响。会中止的仍然只有 6041h bit11。
    * (扫描区域本来就可能正好停在一个开关上; 用监视量去触发会白中止一趟一小时的活。)
    */
   caseBegin("limitsw: 只压住原点开关 -> 扫描照跑, 一次都不中止");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();

      r.bus.setDig(1, /*home=*/true, /*pos=*/false, /*neg=*/false);
      r.bus.setDig(0, /*home=*/true, /*pos=*/false, /*neg=*/false);

      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_home.csv", &err), "start", err.toStdString());

      bool fired = false;
      QString what;
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { fired = true; what = s; });

      check(r.runToIdle(120000), "runs to the end");
      check(!fired, "**the home switch never aborts a scan**", what.toStdString());
      checkEq(r.ctrl.completedPoints(), 25, "all 25 points collected");
      check(r.ctrl.state() == ScanController::State::Done, "Done, not Aborted");
   }

   /*
    * ---- 2026-09-18 那台机器: 反转打开之后, 扫描必须真的能开起来 ----
    *
    * 现场原样搬进来: 2300h (输入有效电平逻辑) 配反了 —— 60FDh 说正限位与负限位
    * **同时**压着, 而 6041h bit11 **恒为 1**。反转关着时上面前后每一条都拦着,
    * 那是对的; 打开反转之后, 那两路读到的其实是"两个都松开着", 扫描就该照常跑。
    *
    * 这一条是**用户报的那个 bug 的回归护栏**: 它同时钉住三件事 ——
    *   · 反转关着时拦得住 (拦不住才是真出事);
    *   · 拦的时候话里指向 2300h (不然人不知道该动哪儿);
    *   · 反转打开后放得行 (这个功能的**全部意义**就在这一条上)。
    * 少了第三条, 上面那些纯判据的断言全过, 而功能仍然没用。
    */
   caseBegin("limitsw: NPN 那台机器 —— 反转打开后, bit11 恒置起也不再挡启扫");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();

      /* 驱动器原样: bit11 置起; 60FDh 原始读数说两个限位都压着 (高电平被当成触发) */
      r.bus.setLimit(0, true); r.bus.setDig(0, false, true, true);
      r.bus.setLimit(1, true); r.bus.setDig(1, false, true, true);

      QString err;
      check(!r.startScan(QDir::tempPath() + "/scan_npn_off.csv", &err),
            "反转关着 -> 拒绝启扫 (拦得住, 这是对的)", err.toStdString());
      {
         const QByteArray eb = err.toUtf8();
         check(std::strstr(eb.constData(), "2300h") != nullptr,
               "拒绝的话里指向 2300h", err.toStdString());
      }

      /*
       * 打开反转。**setDig 要再叫一次**: 它注入的是 60FDh 的**原始**读数, 而
       * setDiInvert 只重算 limit_active, 不会回头去翻已经摆好的那三位
       * (和 publish() 里"先读原始值、再统一反相"是同一个顺序)。
       */
      r.bus.setDiInvert(true);
      r.bus.setDig(0, false, true, true);
      r.bus.setDig(1, false, true, true);

      err.clear();
      check(r.startScan(QDir::tempPath() + "/scan_npn_on.csv", &err),
            "**反转打开 -> 能启扫** (这才是这台机器要的)", err.toStdString());
   }

   /*
    * ---- 旧判据没退化 ----
    *
    * 60FDh 读不到的机器 (本机的常态: 生效的 1A00h 里没有它) 上, bit11 置起**照样中止**。
    * 这一条与上面那条是一对: 把原点排除掉, 不等于把保护削弱。
    */
   caseBegin("limitsw: bit11 置起而 60FDh 未知 -> 照样中止 (旧判据没退化)");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_noDig.csv", &err), "start", err.toStdString());

      bool fired = false;
      QString what;
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { fired = true; what = s; });

      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });
      r.bus.setDigKnown(1, false);
      r.bus.setLimit(1, true);
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);

      check(fired, "still aborts");
      check(what.contains(QStringLiteral("bit11")), "names bit11", what.toStdString());
      check(what.contains(QStringLiteral("无从得知")),
            "**and says the switch state is unknown instead of pretending it is known**",
            what.toStdString());
   }

   /*
    * ---- 文案与开关状态对得上 ----
    *
    * 中止那行字是操作员事后唯一还能看到的东西 (面板灯早就过去了), 所以它必须说出
    * 到底是哪个开关压着 —— 那正是「bit11 什么时候置起」这个待验证问题的现场答案。
    */
   caseBegin("limitsw: 中止文案点名压住的那个开关");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_named.csv", &err), "start", err.toStdString());

      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      QString what;
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { what = s; });

      r.bus.setDig(1, /*home=*/false, /*pos=*/false, /*neg=*/true);
      r.bus.setLimit(1, true);
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);

      check(what.contains(QStringLiteral("负限位")), "names the negative limit", what.toStdString());
   }
}

/*
 * ---------------------------------------------------------------- 回零 (HM)
 *
 * 回零是**唯一一个软件兜不住的动作**: 一旦发起, 朝哪走、什么时候停、撞不撞开关, 全由
 * 驱动器按 6098h 自己决定。所以这里钉的不是"回零能不能成功" (那要插上机器才知道),
 * 而是**发起之前那道闸**与**收尾之后那句话** —— 这两样是纯判据, 也正是操作员唯一能
 * 依赖的东西。
 *
 * 尤其是措辞: 「被停止中止」和「失败」是两件事 (前者是人让它停的), 「故障」和
 * 「状态未知」也是两件事 (后者要人去做的是完全不同的一件事)。这些话都由这里钉住。
 *
 * 真正的 em_home() 调用链 (doHome / 收尾顺序 / g_stop 纪律) **一条都没法在这里验** ——
 * 它们要 em_bus_t 和网卡。那些条目记在 docs/scan_sweep.md §13 的硬件清单里。
 */
static void test_homing()
{
   /* 本地小工具: 判"这句话里有这个词"。判据是**给人看的话**, 所以措辞也是被测的东西 */
   auto has = [](const char *p, const char *w) {
      return p != nullptr && std::string(p).find(w) != std::string::npos;
   };

   /* ---- 方向 ---------------------------------------------------- */
   caseBegin("回零: 正/反向 -> 6098h（24 / 29）");
   {
      checkEq(ecatcmd::home_method_for(false), 24, "正向 = 方式 24");
      checkEq(ecatcmd::home_method_for(true),  29, "反向 = 方式 29");

      check(std::string(ecatcmd::home_dir_text(false)) == "正向", "正向的叫法");
      check(std::string(ecatcmd::home_dir_text(true))  == "反向", "反向的叫法");

      /* 范围守卫。上面两条只钉住"这个数没变", 钉不住"这个数是对的" —— 将来有人把
       * 29 打成 30 或者 4, 那两条照样过。35 是"以当前位置为机械原点", 它**不去找**,
       * 不是这两个按钮的意思 (见 docs/ykd2205pe_ci402.md 那张表) */
      for (int neg = 0; neg < 2; neg++)
      {
         const int m = ecatcmd::home_method_for(neg != 0);
         check(m >= 4 && m <= 30 && m != 35, "方式号落在手册的 HM 表里, 且不是 35");
      }
   }

   /* ---- 返回速度派生 -------------------------------------------- */
   caseBegin("回零: 6099h:02 = 6099h:01 / 4（下限 1）");
   {
      checkEq(ecatcmd::home_vel_slow(2000), 500, "2000 -> 500");
      checkEq(ecatcmd::home_vel_slow(1000), 250, "1000 -> 250");
      checkEq(ecatcmd::home_vel_slow(4),      1, "4 -> 1");
      checkEq(ecatcmd::home_vel_slow(3),      1, "3 -> 整除到 0, 由下限救回 1");
      checkEq(ecatcmd::home_vel_slow(0),      1, "0 -> 1（写 0 是什么语义手册没写, 而"
                                                "「返回速度是 0」绝不该是它的意思）");

      /* 全域不变式, 比逐个例子管用: 返回速度永远 >= 1 (绝不许发 0 出去), 且 <= 找原点
       * 速度 (返回段是"慢慢回到那个点", 比找段还快没有道理) */
      bool inv = true;
      for (uint32_t v = 1; v <= 4000 && inv; v++)
      {
         const uint32_t s = ecatcmd::home_vel_slow(v);
         if (s < 1u || s > v)
            inv = false;
      }
      check(inv, "1..4000 全域: 1 <= 返回速度 <= 找原点速度");
   }

   /* ---- 夹取 ---------------------------------------------------- */
   caseBegin("回零: 速度夹取与输入框的上下限是同一个宏");
   {
      checkEq(ecatcmd::home_vel_clamp(0),         HMI_HOME_VEL_MIN, "0 -> 下限");
      checkEq(ecatcmd::home_vel_clamp(99),        HMI_HOME_VEL_MIN, "99 -> 下限");
      /* **比上限高一点点**, 不写一个具体的数 —— 上一个版本这里写的是 2001, 而 2001
       * 在上限从 2000 提到 100000 之后就成了一个**合法值**, 这条断言会从"验夹取"
       * 变成"验上限还是 2000"。夹取测的是边界关系, 不是某一个数 */
      checkEq(ecatcmd::home_vel_clamp((int32_t)HMI_HOME_VEL_MAX + 1), HMI_HOME_VEL_MAX,
              "MAX + 1 -> 上限");
      checkEq(ecatcmd::home_vel_clamp(2147483647), HMI_HOME_VEL_MAX, "INT32_MAX -> 上限");
      checkEq(ecatcmd::home_vel_clamp(HMI_HOME_VEL_DEF), HMI_HOME_VEL_DEF, "缺省值原样通过");

      /* 这一条锁的是"**界面上显示的数就是线上发的数**": 输入框的 range 与这道夹取读的
       * 是同一对宏。哪天有人只放宽一边 (比如把 range 开到 5000 好"跑快点"), 这条会响 */
      checkEq(ecatcmd::home_vel_clamp(HMI_HOME_VEL_MAX), HMI_HOME_VEL_MAX,
              "clamp(MAX) == MAX —— range 与夹取没有漂移");
      checkEq(ecatcmd::home_vel_clamp(HMI_HOME_VEL_MIN), HMI_HOME_VEL_MIN, "clamp(MIN) == MIN");

      /* 回零速度的头上**不许高于程序里别的运动** —— 「手动速度」+ 点画布本来就能以
       * HMI_VEL_MAX 朝同一个开关走, 回零比它快没有任何理由, 而慢是白慢 (见 §18) */
      check(HMI_HOME_VEL_MAX <= HMI_VEL_MAX, "回零速度的上限不超过 HMI_VEL_MAX");
      check(HMI_HOME_VEL_DEF >= HMI_HOME_VEL_MIN && HMI_HOME_VEL_DEF <= HMI_HOME_VEL_MAX,
            "缺省值落在上下限之内");
   }

   /* ---- 加减速 -------------------------------------------------- */
   caseBegin("回零: 609Ah 由速度派生 —— 斜坡时间 0.1 秒, 加速度封顶");
   {
      /* 驱动器自己那一对实测值: 6099h:01 = 50000, 609Ah = 500000。这一条钉的是
       * "斜坡时间 = 0.1 秒"这个约定的源头 —— 它一变, 这行就该响 */
      checkEq(ecatcmd::home_accel_for(50000), HMI_HOME_ACC_MAX,
              "驱动器自己那一对 (50000 / 500000) 原样复现");

      /* 在 50000 及以下: 斜坡时间恒为 0.1 秒 (acc == v * 10)。
       * **这是这组测试里最要紧的一条** —— 没有它, 一个"加速度写死"的回归
       * (比如又改回 5000) 会让低速那几条断言照样全过 */
      bool ramp = true;
      for (uint32_t v = HMI_HOME_VEL_MIN; v <= HMI_HOME_ACC_MAX / 10u; v++)
         if (ecatcmd::home_accel_for(v) != v * 10u)
            ramp = false;
      check(ramp, "下限..50000 全域: 斜坡时间恒为 0.1 秒 (acc == v * 10)");

      /* 封顶: 一个很高的速度**不许**换来一个比机器自己配的还硬的加速度 */
      checkEq(ecatcmd::home_accel_for(50001), HMI_HOME_ACC_MAX, "刚过 50000 -> 封顶");
      checkEq(ecatcmd::home_accel_for(HMI_HOME_VEL_MAX), HMI_HOME_ACC_MAX,
              "上限速度 -> 还是封顶 (斜坡变长到 0.2 秒, 而不是加速度翻倍)");
      /* 没有那个除法守卫的话 v * 10 会回绕, 于是"很大的速度"算出"很小的加速度" */
      checkEq(ecatcmd::home_accel_for(4294967295u), HMI_HOME_ACC_MAX,
              "UINT32_MAX -> 封顶, 不回绕");

      bool mono = true, cap = true;
      for (uint32_t v = HMI_HOME_VEL_MIN; v <= HMI_HOME_VEL_MAX; v++)
      {
         if (ecatcmd::home_accel_for(v) > HMI_HOME_ACC_MAX)
            cap = false;
         if (v > HMI_HOME_VEL_MIN && ecatcmd::home_accel_for(v) < ecatcmd::home_accel_for(v - 1))
            mono = false;
      }
      check(cap,  "下限..上限 全域: 加速度不超过 HMI_HOME_ACC_MAX");
      check(mono, "下限..上限 全域: 加速度随速度单调不减");
   }

   /* ---- 闸 ------------------------------------------------------ */
   caseBegin("回零闸: 分支, 以及分支的**顺序**");
   {
      check(ecatcmd::home_refusal(true, true, true, false, false) == nullptr,
            "全清 -> 放行 (nullptr)");

      check(has(ecatcmd::home_refusal(false, true, true, false, false), "总线"),
            "没连上 -> 说总线");
      check(has(ecatcmd::home_refusal(false, true, true, true, false), "总线"),
            "没连上 + 有故障 -> 还是先说总线: 那时连状态字都没有, 说故障是在猜");

      check(has(ecatcmd::home_refusal(true, false, true, false, false), "位置"),
            "一笔 6064h 都没取到 -> 说位置未知");

      /* **顺序的要害**: 一个从没收到过的状态字里的 bit3 不是信息。这一条与
       * axis_needs_reset 编码的是同一条规矩 */
      const char *r = ecatcmd::home_refusal(true, true, false, true, false);
      check(has(r, "未知"), "丢帧 + 有故障 -> 说「状态未知」");
      check(!has(r, "bit3"), "丢帧时不许拿一个没收到过的状态字里的 bit3 说事");

      r = ecatcmd::home_refusal(true, true, true, true, false);
      check(has(r, "bit3") && has(r, "故障复位"), "有故障 -> 点名 bit3, 并指到「故障复位」");

      r = ecatcmd::home_refusal(true, true, true, false, true);
      check(has(r, "停止"), "还有轴在走 -> 让人先按「停止」");
      check(has(r, "回零期间插补器是停的") || has(r, "停在半途"),
            "还要说明白**为什么** —— 不然那句话看着像没道理的门槛");
   }

   /* ---- 收尾结局 ------------------------------------------------ */
   caseBegin("回零收尾: 结局由**实测状态**定, 不由返回码排列组合");
   {
      check(ecatcmd::home_end_state(false, false, true,  0) == ecatcmd::HOME_END_NEVER_STARTED,
            "没发起过就是没发起过");
      check(ecatcmd::home_end_state(false, true,  true, -1) == ecatcmd::HOME_END_NEVER_STARTED,
            "闸拦下时哪怕现场有故障, 也不是「这次回零把它搞坏了」");

      check(ecatcmd::home_end_state(true, true, false, 0) == ecatcmd::HOME_END_FAULTED,
            "bit3 还在 -> 故障");
      /* 按 CiA402 这两条不该同时成立; 万一真同时读到, 该报的是**故障** —— 那才是要人
       * 动手的那一件事。反过来说成"保持中"就是漏掉一个真故障 */
      check(ecatcmd::home_end_state(true, true, true, 0) == ecatcmd::HOME_END_FAULTED,
            "故障与使能同时读到 -> 报故障");

      check(ecatcmd::home_end_state(true, false, false, 0) == ecatcmd::HOME_END_STRANDED,
            "没使能也没故障 -> 状态不明");
      /* 这一格是这道函数存在的**主要理由**: 轴可能确实带电, 但驱动器不按 CSP 解释
       * 607Ah, 而 interpolate() 每周期都在往 607Ah 里写 —— "带力矩停着"和
       * "带电但模式不对"是两件事, 说成 HOLDING 就是撒谎 */
      check(ecatcmd::home_end_state(true, false, true, -1) == ecatcmd::HOME_END_STRANDED,
            "已使能但没切回 CSP -> 不能说 HOLDING");
      check(ecatcmd::home_end_state(true, false, false, -1) == ecatcmd::HOME_END_STRANDED,
            "两样都不成立");

      check(ecatcmd::home_end_state(true, false, true, 0) == ecatcmd::HOME_END_HOLDING,
            "已使能 + 已是 CSP -> 保持中 (用户要的那一档)");
   }

   /* ---- 措辞 ---------------------------------------------------- */
   caseBegin("回零措辞: 每一种结局说的话都对得上");
   {
      check(has(ecatcmd::home_end_text(ecatcmd::HOME_END_STRANDED), "可能仍带电"),
            "STRANDED 必须把「可能仍带电」说出来");
      check(has(ecatcmd::home_end_text(ecatcmd::HOME_END_FAULTED), "故障复位"),
            "FAULTED 要指到「故障复位」 —— 而不是让人去拉总闸");
      check(!has(ecatcmd::home_end_text(ecatcmd::HOME_END_HOLDING), "可能仍带电"),
            "HOLDING 不许说带电不明: 那会让人白跑一趟动力电源");
      check(has(ecatcmd::home_end_text(ecatcmd::HOME_END_HOLDING), "保持"),
            "HOLDING 要明说「带保持力矩」 —— 用户就是照这句决定敢不敢松手");

      check(has(ecatcmd::home_cause_text(0), "到位"), "rc = 0 -> 到位");
      check(has(ecatcmd::home_cause_text(1), "停止"), "rc = 1 点名「停止」");
      /* **被「停止」中止不是失败。** 那是人让它停的, 说成失败会让人去找一个不存在
       * 的毛病 (这条路径本来就是本功能的半个需求) */
      check(!has(ecatcmd::home_cause_text(1), "失败"), "被「停止」中止不许说成失败");
      check(has(ecatcmd::home_cause_text(-1), "方向"), "真失败时给换方向的建议");
      check(has(ecatcmd::home_cause_text(-1), "硬顶"),
            "失败建议里要写明**别硬顶** —— 撞着开关还硬回, 才是真会伤机器的做法");
   }

   /* ---- 控制器那道闸 -------------------------------------------- */
   caseBegin("回零中不起扫 (armRun), 回零结束立刻能起扫");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      QString err;

      r.bus.setHoming(true);
      check(!r.startScan(QDir::tempPath() + "/hm1.csv", &err), "回零中 -> 拒绝起扫");
      check(err.contains(QStringLiteral("回零")), "理由说的是回零", err.toStdString());

      /* 这道闸是"等一下", 不是"这份数据坏了" —— 回零做完必须马上能接着扫。
       * 只会拒绝的闸跟没写一样 (同 preflight 那条的规矩) */
      r.bus.setHoming(false);
      check(r.startScan(QDir::tempPath() + "/hm2.csv", &err), "回零结束 -> 放行",
            err.toStdString());
      r.ctrl.abort(QString());
   }
}

/*
 * ---------------------------------------------------------------- 三个模拟源
 *
 * 它们从前没有一条测试 —— 而界面上那个「读一次」按钮**四个源都能点**, 于是它们的
 * 行为第一次直接摆在操作员面前。这里钉的就一件事, 而且正是那条按钮的闸门所依赖的:
 *
 *   **一次请求恰好回一次** (readingReady 或 readingFailed, 不多不少), 且值对得上。
 *
 * 为什么这条值得单独钉: PowerMeter 的约定把"同一时刻只允许一个未决请求"交给了
 * **调用方** (powermeter.h), 而"回话分得清是哪一次的"就靠"一请求一回话"这个配对关系。
 * 哪天某个源改成回两次 (或一次都不回), 出错的不是它自己, 而是**扫描的 CSV 里悄悄
 * 少一个点或者错一个点** —— 不报任何错。所以配对关系要有测试守着。
 *
 * 全是 QTimer::singleShot 投递的, 所以要真转一次事件循环才收得到回话。
 * 每个请求**自己起一个 QEventLoop**: 复用同一个的话, 上一次那个超时定时器会在
 * 下一次 exec() 里提前把它按停, 收到的东西就说不清是哪一次的了。
 */

/* 一次请求的回话 */
struct Reply
{
   int     ready  = 0;
   int     failed = 0;
   double  watts  = 0.0;
   QString err;
};

/*
 * 发一个请求, 把事件循环转到有回话 (或超时) 为止。
 *
 * **连接挂在这个 loop 上** —— connect 的第三个参数 (context) 传 &loop, 于是 loop 一析构
 * 连接就跟着断。这一步不是装饰: 不传 context 的话连接的宿主是**信号发送方** (那个源的
 * 生存期), 而 lambda 里按引用捕获的 loop 早就析构了 —— 下一次请求回话时, 上一次留下的
 * 那个 lambda 也会被叫起来, 碰的正是那个已经没了的 QEventLoop。**本文件踩过这个坑**
 * (自检直接段错误, 而且因为 stdout 是块缓冲, 连一行输出都没留下), 所以收进一个函数,
 * 只写一遍、只对一次。
 */
static Reply ask(PowerMeter *m, int timeout_ms = 2000)
{
   Reply r;
   QEventLoop loop;
   QObject::connect(m, &PowerMeter::readingReady, &loop, [&](double w) {
      r.ready++; r.watts = w; loop.quit();
   });
   QObject::connect(m, &PowerMeter::readingFailed, &loop, [&](const QString &e) {
      r.failed++; r.err = e; loop.quit();
   });
   m->requestReading();

   /*
    * 已经回了就不再进循环。**有的源是同步回话的** —— 没打开时那几个实现都是直接
    * `emit readingFailed(...)` (见 powermeter.cpp), 那时上面两个 lambda 已经跑过了。
    * 而 loop.quit() 在 exec() 之前调是**没有用**的 (Qt: 循环没在跑, 这个调用什么也不做),
    * 所以照样进 exec() 的话会白等到超时 —— 不报错, 只是每一次都白花两秒。
    */
   if (r.ready == 0 && r.failed == 0)
   {
      QTimer::singleShot(timeout_ms, &loop, &QEventLoop::quit);
      loop.exec();
   }
   return r;
}

static void test_meter_sources()
{
   caseBegin("meter: 手动源 —— 一次请求一个数, 没打开就恰好回一次失败");
   {
      ManualMeter man;
      man.setValue(0.25);

      /* 没打开: 请求必须得到**恰好一次** readingFailed (不是 0 次, 也不是 2 次) */
      const Reply shut = ask(&man);
      check(shut.ready == 0 && shut.failed == 1,
            "closed -> exactly one failure, no reading", shut.err.toStdString());
      check(!man.isOpen(), "and it stays closed");

      QString e;
      check(man.open(&e), "open()", e.toStdString());

      const Reply r = ask(&man);
      checkEq(r.ready, 1, "open -> exactly one reading");
      checkNear(r.watts, 0.25, "and it is the value that was set");
   }

   caseBegin("meter: 随机源 —— 回话在 基值±噪声 之内");
   {
      RandomMeter rnd;
      rnd.setBase(2.0);
      rnd.setNoise(0.1);
      rnd.setDelayMs(1);

      QString e;
      check(rnd.open(&e), "open()", e.toStdString());

      /* 跑十次: 每一次都只许回一个数, 而且必须落在 ±噪声 的范围内。
       * 这里不用 checkNear —— 它是随机的, 该验的是**界**, 不是某个具体值 */
      int    bad_n = 0, out_of_band = 0;
      double lo = 1e9, hi = -1e9;
      for (int k = 0; k < 10; k++)
      {
         const Reply r = ask(&rnd);
         if (r.ready != 1)
            bad_n++;
         if (r.watts < 2.0 - 0.1 - 1e-9 || r.watts > 2.0 + 0.1 + 1e-9)
            out_of_band++;
         lo = std::min(lo, r.watts);
         hi = std::max(hi, r.watts);
      }

      char buf[160];
      std::snprintf(buf, sizeof(buf), "10 次里 %d 次回话数不对, %d 次出了 [1.9,2.1]",
                    bad_n, out_of_band);
      check(bad_n == 0, "every request answers exactly once", buf);
      check(out_of_band == 0, "every reading is inside base±noise", buf);
      /* 十次全都撞在同一个数上 = 噪声坏了 (setNoise 没接上), 那热力图就没得看了 */
      check(hi - lo > 0.0, "the noise actually varies between readings", buf);
   }

   caseBegin("meter: 脚本源 —— 按行取, 取完一轮从头, 游标对得上");
   {
      QTemporaryDir dir;
      const QString f = dir.filePath(QStringLiteral("vals.txt"));
      {
         QFile w(f);
         check(w.open(QIODevice::WriteOnly | QIODevice::Text), "write the script file");
         /* 夹一行 # 注释和一个空行 —— 两种都该被跳过 (见 setPath) */
         w.write("# 注释行\n1.5\n\n2.5\n3.5\n");
      }

      ScriptMeter scr;
      scr.setDelayMs(1);
      QString e;
      check(scr.setPath(f, &e), "setPath reads the file", e.toStdString());
      checkEq(scr.count(), 3, "three values, comment and blank line skipped");
      check(scr.open(&e), "open()", e.toStdString());

      const double want[4] = {1.5, 2.5, 3.5, 1.5};   /* 第 4 次绕回第一行 */
      for (int k = 0; k < 4; k++)
      {
         const Reply r = ask(&scr);
         char buf[96];
         std::snprintf(buf, sizeof(buf), "第 %d 次: got %.9g, want %.9g",
                       k + 1, r.watts, want[k]);
         check(r.ready == 1 && std::fabs(r.watts - want[k]) < 1e-9,
               "reads the next line, then wraps around", buf);
      }
      checkEq(scr.cursor(), 1, "cursor is one past the wrap-around value");
   }

   /*
    * 这条**不是**在验某个源的行为, 是在验那条约定本身 —— 也就是界面上那个按钮为什么
    * 必须在扫描期间禁用: 一次请求回一次, 配的是"同一时刻只有一个未决请求"。
    * 两个请求撞在一起时, 源会**老老实实回两次**, 谁也不知道哪个数属于哪一次 ——
    * 而真机那条 (Ophir) 更狠: 它按时间戳只认严格更新的采样, 于是其中一边白等到超时。
    */
   caseBegin("meter: 两个未决请求撞在一起 -> 源回两次, 所以闸门必须由调用方把");
   {
      ManualMeter man;
      man.setValue(1.0);
      QString e;
      man.open(&e);

      int n = 0;
      QObject::connect(&man, &PowerMeter::readingReady, [&](double) { n++; });

      man.requestReading();
      man.requestReading();          /* 调用方违约 —— 这里就是要看它会发生什么 */
      {
         QEventLoop loop;
         QTimer::singleShot(200, &loop, &QEventLoop::quit);
         loop.exec();
      }

      checkEq(n, 2, "two overlapping requests -> two readings, unresolvable by the caller");
   }

   /* 这就是那个闸门要挡的东西 —— 而闸门在界面上 (ScanWindow::refresh 里那条
    * setEnabled), 那层要 Qt Widgets, 本文件按约定不链。所以这条约定是**靠上面这条
    * 测试说明为什么必须挡**, 而不是靠断言。 */
}

/*
 * ---------------------------------------------------------------- 真机功率计
 *
 * PD300R + Juno+ 这条路 (见 ophircom.h 顶部)。**这条腿不需要插表头** —— 它验的是
 * 上半截: COM 对象在这台机器上注册了没有、那套绕开注册表的 typelib 加载走不走得通、
 * 没插设备时会不会**干净地**报错 (不是崩, 也不是卡住)。
 *
 * 下半截 (真读到功率) 只在表头真插着的时候跑 —— 而那时它跑的是**外圈那套**
 * (OphirMeter 的线程 + 异步请求), 正是最终要用的那一套。所以插上表头再跑一次这个
 * 程序, 它就从"验没坏"变成"验能用"。
 *
 * 这台机器没装 StarLab -> SKIP, 不是 FAIL。理由见 skipCase()。
 */
static void test_ophir()
{
   caseBegin("ophir: COM object, no device attached");

   if (!OphirCom::isRegistered())
   {
      skipCase("StarLab COM object not registered on this machine "
               "(install Ophir StarLab to run this case)");
      return;
   }

   /* 主线自己的 COM 环境。**跟 OphirMeter 的工作线程无关** —— 那边是它自己在
    * runSession() 里起的 (服务端是 Apartment, 谁用谁初始化) */
   if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
   {
      skipCase("CoInitializeEx(APARTMENTTHREADED) failed on this thread");
      return;
   }

   OphirCom com;
   QString  err;

   const bool created = com.create(&err);
   check(created, "create the COM object", err.toStdString());
   if (!created)
   {
      CoUninitialize();
      return;
   }

   /*
    * GetVersion 这一条是**整条路的关键证据**。
    *
    * 这台机器上 IDispatch::GetIDsOfNames / GetTypeInfo / Invoke 全返回
    * 0x8002801D, 因为注册表里 typelib 的版本号是字面量 "a.a"。oPhirCom 绕开注册表,
    * 直接从 dll 资源里 LoadTypeLibEx 再走 ITypeInfo::Invoke。所以"GetVersion 能
    * 拿到数"证明的不是"设备在", 而是"那套绕法成立、名字解析和派发都通"。
    */
   long ver = 0;
   check(com.getVersion(&ver, &err) && ver != 0, "GetVersion via the DLL-resource typelib",
         err.toStdString());

   /* 枚举 USB: **成功**是硬性的, 设备个数不是 */
   QStringList serials;
   const bool scanned = com.scanUsb(&serials, &err);
   check(scanned, "ScanUSB returns without error", err.toStdString());

   if (!scanned)
   {
      com.destroy();
      CoUninitialize();
      return;
   }

   std::printf("  INFO [%s] ScanUSB found %d device(s)\n", g_case, (int)serials.size());

   if (!serials.isEmpty())
   {
      /* 表头真插着 —— 但外圈那套要自成一趟, 别在这儿把设备先占住 */
      std::printf("  INFO [%s] a head is attached; the real read path is exercised below\n",
                  g_case);
      com.destroy();
      CoUninitialize();

      caseBegin("ophir: live reading through OphirMeter");
      {
         OphirMeter meter;
         check(meter.kind().contains(QStringLiteral("Ophir")), "kind() names the device");

         QString e;
         check(meter.open(&e), "open() with a head attached", e.toStdString());
         if (meter.isOpen())
         {
            const OphirInfo i = meter.info();
            check(i.valid, "info() is valid after open");
            check(!i.sensor_name.isEmpty() || !i.device_name.isEmpty(),
                  "info() names the head and the sensor", i.summary.toStdString());

            /* 异步请求 -> 事件循环里等它回来 (跨线程是排队投递的) */
            double        got = -1.0;
            QString       fail;
            int           ready = 0, failed = 0;
            QEventLoop    loop;
            QObject::connect(&meter, &PowerMeter::readingReady, [&](double w) {
               ready++; got = w; loop.quit();
            });
            QObject::connect(&meter, &PowerMeter::readingFailed, [&](const QString &m) {
               failed++; fail = m; loop.quit();
            });
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);

            meter.requestReading();
            loop.exec();

            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d ready, %d failed, %s", ready, failed,
                          fail.isEmpty() ? "-" : fail.toUtf8().constData());
            check(ready == 1, "exactly one reading came back", buf);
            check(!std::isnan(got) && got >= -1e-12, "the reading is a plausible wattage", buf);

            meter.close();
            check(!meter.isOpen(), "close() leaves it closed");
         }
      }
      return;
   }

   /* ---- 没插表头: 该报错的地方必须报错, 而且必须快 ---- */
   com.destroy();
   CoUninitialize();

   caseBegin("ophir: no device attached -- fails cleanly, does not hang");
   {
      OphirMeter meter;
      check(meter.kind().contains(QStringLiteral("Ophir")), "kind() names the device");
      check(!meter.isOpen(), "not open before open()");

      /* 没打开就请求读数: 接口约定是**恰好回一次**, 不能一个都不回 */
      int     failed = 0;
      QString fail;
      QObject::connect(&meter, &PowerMeter::readingFailed, [&](const QString &m) {
         failed++; fail = m;
      });
      meter.requestReading();
      check(failed == 1, "requestReading() while closed answers exactly once",
            fail.toStdString());

      QElapsedTimer t;
      t.start();
      QString err2;
      const bool opened = meter.open(&err2);
      const qint64 ms = t.elapsed();

      if (opened)
      {
         /* 跑到这儿说明表头其实插着 (上面那趟和这趟之间插上的), 不算失败 */
         std::printf("  INFO [%s] a head appeared mid-run; skipping the absent-device checks\n",
                     g_case);
         meter.close();
         return;
      }

      check(!err2.isEmpty(), "open() said why it failed");
      check(!meter.isOpen(), "isOpen() stays false after a failed open");

      /* 12s 是 open() 自己的天花板。没设备时它是**立刻**失败的, 不该真等那么久 ——
       * 这一条挡的是"没设备变成了等超时", 那样每次都白搭 12 秒 */
      char buf[128];
      std::snprintf(buf, sizeof(buf), "took %lld ms", (long long)ms);
      check(ms < 4000, "the failure came back promptly, not on the 12s cap", buf);

      /* 失败之后线程有没有收干净 —— 收不干净的话第二次就起不来了 */
      QString err3;
      const bool again = meter.open(&err3);
      check(!again && !meter.isOpen(), "a second open() also fails cleanly (thread reaped)",
            err3.toStdString());

      meter.close();      /* 没收干净的话这里会卡 5 秒再走那条"放手"的路 */
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
   test_plancap();
   test_prefs();
   test_faultreset();
   test_limitsw();
   test_homing();
   test_meter_sources();
   test_ophir();

   std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
   return g_fail == 0 ? 0 : 1;
}
