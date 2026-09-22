/*
 * scan/selftest.cpp —— 不需要硬件、不需要界面, 直接把扫描逻辑跑一遍
 *
 * 覆盖的到位的时序、暂停后从哪儿接、外部改目标、续扫补哪些点、撞限位停不停, 全是时序问题,
 * 跟总线没关系, 所以在 FakeBus 上能验。
 *
 * 时钟是手拨的: 状态机的 tick(now_ms) 由这个文件喂, 不是 QTimer, 于是那些"等 60ms 稳定
 * 窗口"的断言是确定的, 不受机器快慢影响。
 * 输出的组名与断言说明是中文 (按 UTF-8 打)。**"哪一条断言失败了"必须看得见** ——
 * 所以失败那一行除了断言说明, 还会把当时的参数原文一起打出来 (见 check 的 extra 参数),
 * 那才是回去查问题用的东西。
 *
 * 它不链 SOEM 也不链 Qt Widgets, 只有 Qt6::Core。
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

#include "editgate.h"
#include "meterlog.h"
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

/* 「这条没验」和「这条验失败了」是两回事, 报出来的数必须分开数。
 * 用在需要外部东西的那几条上 (目前只有真机功率计那条, 要这台机器装了 Ophir 的 StarLab):
 * 没装就标成 SKIP —— 它不是失败的, 但也不能算通过。 */
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

/* 自己按速度往前推位置: pos 是跟出来的, 不是 setTarget 的瞬间就等于目标。 */
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

         /* 实际位置再追插值目标 —— 插值目标停了它也还得追。pos_lag 越大追得越慢。 */
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

   /* 603Fh。**单独一个注入点**: 真实的码是故障沿之后一拍才到的, 所以"故障起来了但码还没到"
    * (UNREAD) 是常态 —— 测试要能各造出这三种状态 */
   void setFaultCode(int i, int code) { t_.ax[i].fault_code = code; }
   void setEnabled(int i, bool e) { t_.ax[i].enabled = e; }

   /* 撞限位。limit_active 走真判据 ecatcmd::limit_hit(), 与 publish() 同一条函数 —— 照抄一个
    * true 会把"sw 与 dig 谁说了算相反"那种不一致在测试里抹平, 而那正是该测的。 */
   void setLimit(int i, bool on)
   {
      if (on) t_.ax[i].sw |= EM_SW_INTLIMIT;
      else    t_.ax[i].sw &= (uint16_t)~EM_SW_INTLIMIT;
      recomputeLimit(i);
   }

   /* 「上位机侧取反」。总线级: 一次改全部轴。 */
   void setDiInvert(bool on)
   {
      t_.di_invert = on;
      for (int i = 0; i < EM_MAX_AXES; i++)
         recomputeLimit(i);
   }

   /* 三个限位开关本身 (60FDh)。dig_known 单独一个开关: 读不到时那三位也是 false。
    * 默认 unknown —— 真机上生效的 1A00h 里没有 60FDh。注入口给的是驱动器原始读数,
    * 与 publish() 一样先反相再存。 */
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
   /* 总线正在回零。只影响 armRun 那道闸: 复现的是"控制器看得到的那一位" */
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

/* 回包不由 requestReading() 触发, 而由测试喂的时钟触发 —— 本文件没有事件循环, 时间是被手拨的:
 * 这里记下"该什么时候回", 由 rig 在拨表时放出来, 等价于一个几百毫秒延迟的真串口功率计。 */
class FakeMeter : public PowerMeter
{
public:
   QString kind() const override { return QStringLiteral("fake"); }
   /* tag() 是纯虚的 (powermeter.h): 进 CSV 的那个标识, 纯 ASCII */
   QString tag()  const override { return QStringLiteral("fake"); }
   bool open(QString *) override { m_open = true; return true; }
   void close() override         { m_open = false; }

   void requestReading() override
   {
      if (!m_open)
      {
         emit readingFailed(QStringLiteral("没开"));
         return;
      }
      /* 上一个还没回就又来一个 —— 这正是 powermeter.h 那条"同一时刻只允许一个未决请求"
       * 禁止的事。记下来: 测试靠它判"仲裁漏了一拍", 因为别的症状全是静默的
       * (真机上这一下会让一份回话分给两个调用方, 或者一边白等到超时) */
      if (armed_)
         overlaps_++;

      requests_++;
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
   /* 收到过几个请求 (与 readings 不同: 发了但还没回的那些只算在这里) */
   int requests() const { return requests_; }
   /* 上一个未决请求还没回, 就又收到了一个请求的次数。**正常必须恒为 0** */
   int overlaps() const { return overlaps_; }

private:
   double  value_      = 1.0;
   int64_t now_        = 0;
   int64_t due_        = 0;
   int     latency_ms_ = 60;
   int     readings_   = 0;
   int     requests_   = 0;
   int     overlaps_   = 0;
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

/* ---------------------------------------------------------------- 用例 */

static void test_grid()
{
   caseBegin("grid: 27x27 @ 0.5 → 55x55");

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
   checkEq((long long)odd.size(), 4, "10/3 → 4 points");
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

static void test_meter_meta()
{
   caseBegin("meter: 读数单位由设备自己的字判, 判不出来就不猜");
   {
      /* 探头类型与测量模式名, 两个来源任一说得清就说得清 */
      check(unitFromDeviceInfo(QStringLiteral("pyroelectric"), QStringLiteral("Energy"))
               == QStringLiteral("J"),
            "热释电 + Energy → J");
      check(unitFromDeviceInfo(QStringLiteral("thermopile"), QStringLiteral("Power"))
               == QStringLiteral("W"),
            "热电堆 + Power → W");
      check(unitFromDeviceInfo(QStringLiteral("photodiode"), QStringLiteral("Power"))
               == QStringLiteral("W"),
            "光电二极管 + Power → W");

      /* 模式名优先于探头类型: 同一只探头在 Energy 模式下报的是 J */
      check(unitFromDeviceInfo(QStringLiteral("thermopile"), QStringLiteral("Energy"))
               == QStringLiteral("J"),
            "模式名说了算");

      /* **认不出来返回空**, 一个字符都不许编 */
      check(unitFromDeviceInfo(QString(), QString()).isEmpty(), "两个都空 → 空");
      check(unitFromDeviceInfo(QStringLiteral("pyroelectric"), QStringLiteral("dBm")).isEmpty(),
            "dBm 算认不出来 —— 外面判不出那份数组到底是 dBm 还是 W");
      check(unitFromDeviceInfo(QStringLiteral("unknown-type"), QStringLiteral("Power"))
               == QStringLiteral("W"),
            "模式名认得出来就够 (不需要探头也认得)");
   }

   caseBegin("meter: 两份 CSV 的 meta 行 —— 谁采的 / 什么单位 / 什么配置");
   {
      QTemporaryDir dir;
      check(dir.isValid(), "temp dir");

      Params p;
      const QString path = dir.filePath(QStringLiteral("s.csv"));

      ScanLog log;
      QString err;

      /* 界面推过来的是 meterMetaLines() 的产出 (纯 ASCII), 这里原样摆几行 */
      checkEq((long long)meterMetaLines(nullptr).size(), 0, "没有源 → 一行都不加");

      QStringList extra = meterMetaLines(nullptr);
      extra << QStringLiteral("meter_source=ophir")
            << QStringLiteral("meter_unit=J")
            << QStringLiteral("meter_mode=Energy");

      check(log.beginNew(path, p, QStringLiteral("2026-09-17T10:00:00"), 3, extra, &err),
            "beginNew with extra meta", err.toStdString());

      QFile f(path);
      check(f.open(QIODevice::ReadOnly | QIODevice::Text), "read it back");
      const QString text = QString::fromUtf8(f.readAll());
      f.close();

      check(text.contains(QStringLiteral("# meter_unit=J\n")), "单位那一行在文件里");
      check(text.contains(QStringLiteral("# meter_source=ophir\n")), "来源那一行在文件里");

      /* 位置: 夹在几何那几行与列表头之间 —— 头几行永远是"这份文件是怎么来的" */
      const int at_meta = text.indexOf(QStringLiteral("# meter_source="));
      const int at_head = text.indexOf(QStringLiteral("index,ix,iy"));
      check(at_meta > 0 && at_head > at_meta, "在列表头之前",
            text.left(300).toStdString());
      check(text.indexOf(QStringLiteral("# started=")) < at_meta, "几何那几行照旧在前面");

      /* 关键的一条: 头里多了几行, **续扫的兼容性判定一个字都没变** ——
       * 读回时只认那几个几何 key (scanplan.cpp 的 metaGet), 多的行没人看 */
      std::vector<char> mask;
      int max_index = -1;
      std::string iso;
      int epoch = -1;
      const std::string diff =
         csvParseForResume(text.toStdString(), p, &mask, &max_index, &iso, &epoch);
      check(diff.empty(), "带 meter 行的文件照样能续扫", diff);
      checkEq(max_index, -1, "一个点都还没采");
      checkEq(epoch, 3, "zero_epoch 照旧读得回来");
      check(iso == "2026-09-17T10:00:00", "started 照旧读得回来", iso);
   }
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
   caseBegin("abort: 撞硬件限位 → 自动中止");
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

   caseBegin("abort: 驱动器故障 (中止那一句要带上 603Fh 故障码)");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_fault.csv", &err), "start", err.toStdString());
      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      bool fired = false;
      QString why;

      /* **先接好再注入** —— autoAborted 只在源那一帧发一次, 接晚了就永远收不到 */
      QObject::connect(&r.ctrl, &ScanController::autoAborted,
                       [&](const QString &s) { fired = true; why = s; });

      r.bus.setFault(0);
      /* 过压。**与过流的处置办法完全不搭界** (一个查供电, 一个查机械) ——
       * 这一句是无人值守时唯一的现场记录, 说错码就是把人支到错的地方去 */
      r.bus.setFaultCode(0, 0xFF02);
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);

      check(fired, "a drive fault aborts the scan");
      check(why.contains(QStringLiteral("6041h bit3")), "说清是哪个位", why.toStdString());
      check(why.contains(QStringLiteral("0xFF02")), "中止理由里带上了故障码", why.toStdString());
      check(why.contains(QStringLiteral("过压")), "还带上了码的意思", why.toStdString());
      check(why.contains(QStringLiteral("轴X")), "说清是哪一根轴", why.toStdString());
      /* 另一根没报故障, 不许被一起点名 —— 点错了人就跑去查那根好的 */
      check(!why.contains(QStringLiteral("轴Y")), "没故障的轴不许被点名", why.toStdString());
   }

   caseBegin("abort: 驱动器故障 — 码还没到那一拍也要读得通");
   {
      /* 真实时序: 码是故障沿**之后**一拍才读回来的 (SDO 在下一圈的圈顶做),
       * 所以自动中止那一刻 fault_code 多半还是 UNREAD。那一句在这种缺的情况下
       * 也必须完整 —— 界面上不许留半个 "603Fh = "。 */
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      r.ctrl.rebuildPlan();
      QString err;
      check(r.startScan(QDir::tempPath() + "/scan_fault_unread.csv", &err), "start",
            err.toStdString());
      r.runUntil([&] { return r.ctrl.completedPoints() >= 1; });

      QString why;
      QObject::connect(&r.ctrl, &ScanController::autoAborted, [&](const QString &s) { why = s; });

      r.bus.setFault(0);                 /* 码刻意不设 -> 停在 UNREAD, 就是故障沿那一拍 */
      r.runUntil([&] { return r.ctrl.state() == ScanController::State::Aborted; }, 5000);

      check(why.contains(QStringLiteral("603Fh")), "把 603Fh 这块牌子举起来", why.toStdString());
      check(why.contains(QStringLiteral("还没读到")), "照实说还没读到, 不编一个码",
            why.toStdString());
      check(!why.contains(QStringLiteral("0x")), "没有码就不许出现十六进制数",
            why.toStdString());
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
      check(!r.startScan(QDir::tempPath() + "/pf1.csv", &err), "not enabled → refused");
      check(err.contains(QStringLiteral("使能")), "the reason says so", err.toStdString());

      r.bus.setEnabled(0, true);
      r.meter.close();
      check(!r.startScan(QDir::tempPath() + "/pf2.csv", &err), "no meter → refused");
      check(err.contains(QStringLiteral("功率计")), "the reason mentions the meter", err.toStdString());

      r.meter.open(nullptr);
      r.bus.setLimit(1, true);
      check(!r.startScan(QDir::tempPath() + "/pf3.csv", &err),
            "already sitting on a limit switch → refused");
      check(err.contains(QStringLiteral("bit11")), "the reason mentions bit11", err.toStdString());

      r.bus.setLimit(1, false);
      r.bus.setInOp(false);
      check(!r.startScan(QDir::tempPath() + "/pf4.csv", &err), "not in OP → refused");

      /* 一切正常时**必须放行** —— 只会拒绝的 Preflight 跟没写一样 */
      r.bus.setInOp(true);
      check(r.startScan(QDir::tempPath() + "/pf5.csv", &err), "healthy → accepted", err.toStdString());
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

/* setParams 里的 buildPlan 会给 floor(区域/分辨率)+1 的平方个 Point 开空间 —— 区域 500 × 分辨率
 * 0.001 是 2.5e11 个点, 界面在这期间一帧都刷不出来。断言: 超上限时一个 Point 都不建 (网格按
 * 0×0 报, validate 照样拦); "正好卡在上限上"必须建得出来 —— 闸太紧会误伤合法的大网格。 */
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

/* 键名错一个字母不报错, 只是那一项永远记不住。网卡名 (Npcap 的 \Device\NPF_{GUID}) 是这条路上
 * 唯一的坑: INI 是有转义字符的格式, 反斜杠写进去再读回来会不会变样, 只有真跑一遍才知道。 */
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

/* ---------------------------------------------------------------- 编辑门控 */

/* 状态机本身没有 Qt 依赖, 但下面每条判据对应的都是界面上看得见的行为 */
static void test_editgate()
{
   /* 本地小工具: 判"这句话里有这个词" */
   auto has = [](const char *p, const char *w) {
      return p != nullptr && std::string(p).find(w) != std::string::npos;
   };

   caseBegin("editgate: 不在编辑态 — 没有标记, 也点不脏");
   {
      editgate::Gate g;
      check(!g.editing, "新框不在编辑态");
      check(std::string(editgate::titleMark(g)).empty(), "标题不拼任何后缀");

      editgate::markDirty(&g);   /* 载入 ini / 恢复默认时顺手碰控件 */
      check(!g.dirty, "非编辑态下 markDirty 无效");
      check(std::string(editgate::titleMark(g)).empty(), "于是标记也没冒出来");
   }

   caseBegin("editgate: 进了编辑态但没改 → 有编辑权, 没有未保存的东西");
   {
      editgate::Gate g;
      check(editgate::begin(&g), "第一次 begin 成功");
      check(!editgate::begin(&g), "重复点「编辑」是 no-op (绝不重拍快照)");
      check(std::string(editgate::titleMark(g)).empty(), "没改过就不说「未保存」");

      editgate::markDirty(&g);
      check(has(editgate::titleMark(g), "未保存"), "改一下就出现「未保存」");
   }

   caseBegin("editgate: 改回原值 → 标记自己消失");
   {
      editgate::Gate g;
      editgate::begin(&g);
      editgate::markDirty(&g);
      check(has(editgate::titleMark(g), "未保存"), "改一下就有标记");

      editgate::undirty(&g);
      check(!g.dirty, "界面侧逐项比对发现与快照一致 → 标记落下");
      check(std::string(editgate::titleMark(g)).empty(), "标题回到干净的那一份");
      check(g.editing, "但还在编辑态: 清标记不等于退出编辑");

      editgate::save(&g);
      g.dirty = true;      /* 直接置位, 绕过 markDirty 的编辑态守卫 */
      editgate::undirty(&g);
      check(g.dirty, "非编辑态下 undirty 不动手 —— 标记只在编辑态里有意义");
      check(!g.editing && !g.discarded, "别的标志一个不动");
   }

   caseBegin("editgate: 保存与取消都清干净");
   {
      editgate::Gate g;
      editgate::begin(&g);
      editgate::markDirty(&g);
      editgate::save(&g);
      check(!g.editing && !g.dirty && !g.discarded, "save 之后三个标志全清");
      check(std::string(editgate::titleMark(g)).empty(), "标记也清掉");

      editgate::begin(&g);
      editgate::markDirty(&g);
      editgate::cancel(&g);
      check(!g.editing && !g.dirty && !g.discarded,
            "cancel 与 save 的区别只在调用方要不要回滚控件");
   }

   caseBegin("editgate: 静默丢弃 — 真有改动才留痕");
   {
      editgate::Gate g;
      editgate::begin(&g);
      check(!editgate::drop(&g), "进了编辑态又没改 → 调用方什么都不用做");
      check(!has(editgate::titleMark(g), "已丢弃"), "没改过就不留「已丢弃」");

      editgate::begin(&g);
      editgate::markDirty(&g);
      check(editgate::drop(&g), "有改动 → 调用方必须回滚控件并重新下推");
      check(!g.editing, "丢弃也退出编辑态");
      check(has(editgate::titleMark(g), "未保存"), "痕迹说清「未保存」");
      check(has(editgate::titleMark(g), "已丢弃"), "并说清它已经被丢掉");

      editgate::begin(&g);
      check(!has(editgate::titleMark(g), "已丢弃"),
            "「已丢弃」留到这块框下次进编辑态为止");
   }

   caseBegin("editgate: 双反相警告 — 不是「等于没反」, 是判据恒成立");
   {
      check(editgate::doubleInvertWarning(true,  false) == nullptr, "只勾写驱动器: 无事");
      check(editgate::doubleInvertWarning(false, true)  == nullptr, "只勾上位机侧取反: 无事");
      check(editgate::doubleInvertWarning(false, false) == nullptr, "两个都不勾: 无事");

      const char *w = editgate::doubleInvertWarning(true, true);
      check(w != nullptr, "两个都勾 → 必须给警告");
      check(has(w, "恒成立"), "措辞必须点明判据恒成立", w ? w : "");
      check(has(w, "上位机侧取反"), "并指出该关掉哪一个", w ? w : "");
   }

   caseBegin("editgate: 2300h 的掩码比较 — 高几位无关");
   {
      check(!EM_DI_LOGIC_EQ(0x0000u, EM_DI_LOGIC_NPN), "常开 → 要写");
      check( EM_DI_LOGIC_EQ(0x0007u, EM_DI_LOGIC_NPN), "已经是 NPN → 一个字节都不写");
      check( EM_DI_LOGIC_EQ(0xF007u, EM_DI_LOGIC_NPN), "高位有别的位不关这三位的事");
      check(!EM_DI_LOGIC_EQ(0x0006u, EM_DI_LOGIC_NPN), "只对两位 → 还要写");
      check( EM_DI_LOGIC_EQ(EM_DI_LOGIC_MASK, EM_DI_LOGIC_NPN), "掩码就是 0x0007");
   }
}

/* ------------------------------------------------------------ 高级选项的记忆 */

static void test_advprefs()
{
   caseBegin("advprefs: 缺省就是「开 / 开 / 关」");
   {
      const Prefs p;
      check(p.want_dig_in,      "默认让 60FDh 进 TxPDO");
      check(p.npn_write_drive,  "默认写驱动器 2300h");
      check(!p.npn_sw_invert,   "上位机侧取反默认关");
      checkEq(p.home_vel, -1,   "回零速度: 还没记过");

      QTemporaryDir dir;
      const Prefs fresh = prefsLoad(dir.filePath(QStringLiteral("scan.ini")));
      check(fresh.want_dig_in && fresh.npn_write_drive && !fresh.npn_sw_invert,
            "文件压根不存在时也走同一份缺省");
   }

   caseBegin("advprefs: 旧 ini (没有这三个键) 读回来仍是开 / 开 / 关");
   {
      QTemporaryDir dir;
      const QString ini = dir.filePath(QStringLiteral("scan.ini"));

      QFile f(ini);
      check(f.open(QIODevice::WriteOnly | QIODevice::Text), "手写一份只有老键的 ini");
      f.write("[scan]\narea_x_unit=12.5\nspeed_pul_s=33333\n"
              "[ui]\nmanual_speed=12345\n");
      f.close();

      const Prefs p = prefsLoad(ini);
      checkNear(p.params.area_x_unit, 12.5, "老键照旧读回来");
      checkEq(p.manual_speed, 12345, "老键照旧读回来 (2)");

      /* 本用例的正题: QSettings 对缺项给的是无效 QVariant, toBool() 一律 false ——
       * 不显式带缺省的话, 升级前的 ini 会把"默认开"静默读成"用户把它关了" */
      check(p.want_dig_in,     "缺 adv/want_dig_in 必须回落到默认开");
      check(p.npn_write_drive, "缺 adv/npn_write_drive 必须回落到默认开");
      check(!p.npn_sw_invert,  "缺 adv/npn_sw_invert 回落到默认关");
      checkEq(p.home_vel, -1,  "缺 ui/home_vel 回落到「没记过」");
   }

   caseBegin("advprefs: 四项反着设, 存进去再读回来逐项相等");
   {
      QTemporaryDir dir;
      const QString ini = dir.filePath(QStringLiteral("scan.ini"));

      Prefs p;
      p.want_dig_in     = false;
      p.npn_write_drive = false;
      p.npn_sw_invert   = true;
      p.home_vel        = 30000;
      prefsSave(ini, p);

      const Prefs b = prefsLoad(ini);
      check(!b.want_dig_in,     "want_dig_in=false 存得住");
      check(!b.npn_write_drive, "npn_write_drive=false 存得住");
      check(b.npn_sw_invert,    "npn_sw_invert=true 存得住");
      checkEq(b.home_vel, 30000, "回零速度存得住");
   }

   /* 「色标上下限不记, 但『是不是自动跟随』要记」—— 前者是一个数 (套到下一趟数据上就是错的),
    * 后者是一个模式 (记不住就得每次开程序重新勾一遍)。上下限压根不在 Prefs 里, 所以这里只钉
    * 得住模式那一半。 */
   caseBegin("advprefs: 色标「自动跟随数据」是个模式 —— 缺省关、缺项回落到关、存得住");
   {
      const Prefs p;
      check(!p.shade_auto, "缺省关 (锁定的色标才是能拿两张图对比的那一种)");

      QTemporaryDir dir;
      const QString ini = dir.filePath(QStringLiteral("scan.ini"));
      check(!prefsLoad(ini).shade_auto, "文件压根不存在时也走同一份缺省");

      /* 手写一份老 ini: 没有 shade/auto_fit 这一节。QSettings 对缺项给无效 QVariant,
       * toBool() 一律 false —— 不显式带缺省的话, 升级前的 ini 会把"默认关"读成别的 */
      QFile f(ini);
      check(f.open(QIODevice::WriteOnly | QIODevice::Text), "手写一份只有老键的 ini");
      f.write("[scan]\narea_x_unit=12.5\n");
      f.close();
      const Prefs old = prefsLoad(ini);
      check(!old.shade_auto, "缺 shade/auto_fit 回落到默认关");
      checkNear(old.params.area_x_unit, 12.5, "老键照旧读回来");

      Prefs q;
      q.shade_auto = true;
      prefsSave(ini, q);
      const Prefs b = prefsLoad(ini);
      check(b.shade_auto, "shade_auto=true 存得住");
   }

   caseBegin("advprefs: 被手改坏的 ini 不许直接把速度拿去用");
   {
      checkEq(ecatcmd::home_vel_from_pref(-1), HMI_HOME_VEL_DEF,
              "-1 = 没记过 → 用驱动器实测值");
      checkEq(ecatcmd::home_vel_from_pref(0),  HMI_HOME_VEL_DEF, "0 也算没记过");
      checkEq(ecatcmd::home_vel_from_pref(30000), 30000, "正常值原样用");
      checkEq(ecatcmd::home_vel_from_pref(1), HMI_HOME_VEL_MIN, "太小 → 夹到下限");
      checkEq(ecatcmd::home_vel_from_pref(999999999), HMI_HOME_VEL_MAX, "太大 → 夹到上限");
   }
}

/* ------------------------------------------------- 故障复位 (6040h bit7 上升沿) */

/* 这些判据全在 ecatcmd 里 (头文件, inline) —— 故意放在头文件: scan_selftest 不编
 * ecatworker.cpp, 逻辑写在 .cpp 里就等于永远验不到, 而下面第一条恰好是这个程序里最不能错
 * 的一行。 */
static void test_faultreset()
{
   using namespace ecatcmd;

   /* 「该不该复位这根轴」= 全部安全性所在。em_fault_reset() 先写 6040h = 0x0000 (卸力) 打十帧,
    * 再抬 bit7 (上升沿触发, 不先压 0 构不成沿), 而它到末尾才报告"本来就没有故障" —— 对一根健康
    * 的保持轴做这件事会真的松开保持力矩, 竖直滑台会掉下来。这道闸必须在调用之前。 */
   caseBegin("faultreset: 只有「可信 + 有故障」才许碰");
   check(!axis_needs_reset(false, true,  true),  "invalid axis is skipped");
   check(!axis_needs_reset(false, false, true),  "invalid + unknown is skipped");
   check(!axis_needs_reset(true,  false, true),  "unknown (mirror_ok=false) is skipped, not guessed");
   check(!axis_needs_reset(true,  true,  false), "healthy axis is never written to");
   check(!axis_needs_reset(true,  false, false), "unknown + healthy is skipped");
   check( axis_needs_reset(true,  true,  true),  "trusted + faulted → reset");

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
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "no fault → nothing to do");

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
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "all unknown → nothing");

      /* valid=false 同理 */
      t.ax[0].mirror_ok = true;
      t.ax[0].valid     = false;
      checkEq(pick_faulted_axes(t, out, EM_MAX_AXES), 0, "invalid → nothing");

      /* 没有轴 */
      BusTelem none;
      none.naxis = 0;
      checkEq(pick_faulted_axes(none, out, EM_MAX_AXES), 0, "no axes → nothing");

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

/* 撞限位的判定只有一处 (ecatcmd::limit_hit)。规则写成带参数的函数, 三条分支 (含反转那条) 在
 * 同一次构建里都跑得到 —— 否则"要改一个 #define 重编一次才能验另一条"。 */
static void test_limitsw()
{
   using namespace ecatcmd;

   const uint16_t LIM = EM_SW_INTLIMIT;

   caseBegin("limitsw: 今天的判定 = bit11 单独, 一个比特没变");
   check( limit_hit(LIM, false, false, false, false), "bit11 + nothing known → still hit");
   check( limit_hit(LIM, true,  false, false, false), "bit11 + both switches released → still hit");
   check( limit_hit(LIM, true,  true,  false, false), "bit11 + positive switch");
   check( limit_hit(LIM, true,  false, true,  false), "bit11 + negative switch");
   check(!limit_hit(0,   true,  true,  true,  false), "no bit11 → never a hit");
   check(!limit_hit(0,   false, false, false, false), "no bit11, nothing known");

   /* 「原点不算」在类型上就成立: limit_hit 的参数里没有原点那一位, 它不可能影响中止判定。
    * 故"只压住原点"长的就是 (pos=false, neg=false) 这一组。 */
   caseBegin("limitsw: 精判据 (开关关着的那条分支)");
   check(!limit_hit_rule(LIM, true,  false, false, LIMIT_RULE_REFINED),
         "bit11 + only the home switch pressed (pos/neg both released) → NOT a hit");
   check( limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_REFINED), "bit11 + positive switch → hit");
   check( limit_hit_rule(LIM, true,  false, true,  LIMIT_RULE_REFINED), "bit11 + negative switch → hit");
   check( limit_hit_rule(LIM, true,  true,  true,  LIMIT_RULE_REFINED), "bit11 + both → hit");
   /* **这一条是"不弱化"的保证**: 不知道 60FDh 就退回旧判据, 保护一点不减 */
   check( limit_hit_rule(LIM, false, false, false, LIMIT_RULE_REFINED),
         "bit11 + unknown 60FDh → falls back to bit11 alone");
   check(!limit_hit_rule(0,   true,  false, false, LIMIT_RULE_REFINED), "no bit11 → still no hit");

   /* ---- 第三条判据: 输入反转 (NPN) ----
    * 反转的语义是用开关的真实状态替掉驱动器的意见, bit11 必须整个退场 —— 故不复用 REFINED:
    * 哪天 2300h 改对而反转忘了关, bit11 变 0 会把 `bit11 && ...` 恒置为 false, 保护静悄悄地全没。 */
   caseBegin("limitsw: 反转那条判据 —— bit11 不参与, 保护压在开关上");
   check(!limit_hit_rule(LIM, true,  false, false, LIMIT_RULE_INVERT),
         "bit11 set but both switches released → NOT a hit (bit11 is stuck-1 here)");
   check( limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_INVERT),
         "inverted: positive switch pressed → hit");
   check( limit_hit_rule(LIM, true,  false, true,  LIMIT_RULE_INVERT),
         "inverted: negative switch pressed → hit");
   /* 反转开着时 bit11 不参与判据, 置不置起结果一样 */
   check( limit_hit_rule(0,   true,  true,  false, LIMIT_RULE_INVERT)
       == limit_hit_rule(LIM, true,  true,  false, LIMIT_RULE_INVERT),
         "inverted: bit11 makes no difference at all");
   /* 三条判据里只有这条把「未知」判成中止: bit11 不用了, 开关又读不到, 退无可退。 */
   check( limit_hit_rule(0,   false, false, false, LIMIT_RULE_INVERT),
         "inverted + unknown 60FDh → abort, because there is no judge left");

   caseBegin("limitsw: 哪个开关选哪条判据 —— 只此一处 (limit_rule_for)");
   checkEq((int)limit_rule_for(false),
           (int)(kRefineLimitWithDigIn != 0 ? LIMIT_RULE_REFINED : LIMIT_RULE_BIT11),
           "反转关着 → 由 kRefineLimitWithDigIn 那个编译期开关决定");
   checkEq((int)limit_rule_for(true), (int)LIMIT_RULE_INVERT,
           "反转开着 → 一定是 INVERT, 与那个编译期开关无关");
   /* 端到端那一条: 同一份输入, 反转一开一关必须得到**相反**的结论 —— 否则这个开关没接上 */
   check( limit_hit(LIM, true, false, false, false)
       && !limit_hit(LIM, true, false, false, true),
         "bit11 + switches released: hit without invert, NOT a hit with invert");

   caseBegin("limitsw: 现场诊断那句话不许把「不知道」说成「都没压着」");
   {
      const char *unk = limit_switch_text(false, false, false, false);
      check(std::strstr(unk, "无从得知") != nullptr,
            "unknown is reported as unknown, not as 'nothing pressed'", unk);
      /* 取反开着而读不到 60FDh: 比"不知道"还严重一层, 那句话得说出来 */
      const char *unk_inv = limit_switch_text(false, false, false, true);
      check(std::strstr(unk_inv, "取反") != nullptr,
            "unknown + invert is called out as one judge short", unk_inv);
      check(std::strcmp(unk, unk_inv) != 0, "…and it is not the same sentence");

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

   /* 真机那条: X0~X3 接 NPN 传感器而 2300h 按常开配, 两个限位输入常年读成"压着"、bit11 恒置起,
    * 而文案只说"先手动走离限位" —— 把人支去追一个不存在的限位。正负限位同时压着物理上不成立,
    * 那句话说出口时必须带上"这多半不是真的"和"往 2300h 查"。这条钉的是措辞, 逻辑一个字没改。 */
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
      /* 取反这个新出路也要点一下, 但必须连带说清它只治软件那一侧 */
      check(std::strstr(adv, "上位机侧取反") != nullptr, "the advice mentions the new way out",
            adv);
      check(std::strstr(adv, "只治软件") != nullptr,
            "…and says plainly that it only fixes this side", adv);

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
      check(std::strstr(a2, "607Dh") != nullptr, "neither pressed → look at the soft limits",
            a2);
      check(std::strstr(a3, "原点") != nullptr, "home only → says it is the home switch", a3);
   }

   /* ---- 反转开着时, 上面那几句的意思全变了, 必须是另外几句话 ----
    * dig_* 已是反相之后的值, 两路还同时为真与 2300h 无关 (极性错只会两边一起反相, 反相完就该
    * 松开), 所以那几句里不许再出现指向 2300h 的话。 */
   caseBegin("limitsw: 反转开着时「同时压着」换了个意思, 文案必须跟着换");
   {
      const char *t_off = limit_switch_text(true, true, true, false);
      const char *t_on  = limit_switch_text(true, true, true, true);
      check(std::strcmp(t_off, t_on) != 0, "same input, two different sentences");
      check(std::strstr(t_on, "2300h") == nullptr,
            "with invert on it must NOT send you to 2300h — polarity is already handled",
            t_on);
      check(std::strstr(t_on, "反相") != nullptr, "it says the values are post-inversion", t_on);

      const char *a_on = limit_hit_advice(true, true, true, false, true);
      check(std::strstr(a_on, "2300h") == nullptr,
            "nor does the advice: that lever is already pulled", a_on);
      check(std::strstr(a_on, "供电") != nullptr,
            "it points at wiring / sensor power instead", a_on);

      /* 反转开着时单边压着反而是可信的 —— 判定用的就是它, 措辞该更强 */
      const char *one_on  = limit_hit_advice(true, true, false, false, true);
      const char *one_off = limit_hit_advice(true, true, false, false, false);
      check(std::strcmp(one_on, one_off) != 0, "single limit: invert changes the wording");
      check(std::strstr(one_on, "可信") != nullptr,
            "with invert on the reading is stated as trustworthy, not as a guess", one_on);

      /* 反转开着而读不到 60FDh: 必然开不了扫描, 不是"可能有问题"。措辞里必须有那两条出路 */
      const char *u_on  = limit_hit_advice(false, false, false, false, true);
      const char *u_off = limit_hit_advice(false, false, false, false, false);
      check(std::strcmp(u_on, u_off) != 0, "unknown 60FDh: invert changes the advice");
      check(std::strstr(u_on, "永远开不了") != nullptr,
            "with invert on, unknown 60FDh means the scan can never start", u_on);
      check(std::strstr(u_on, "关掉") != nullptr, "and one way out is to turn the invert off",
            u_on);

      /* 开场白也要跟着判据换: 反转开着时 limit_active 与 bit11 无关, 还说"bit11 置起"就是把人
       * 支去查一个决定不了任何事的位。钉的不是"不许出现这个词", 而是不许说它置起。 */
      const char *h_off = limit_hit_headline(false);
      const char *h_on  = limit_hit_headline(true);
      check(std::strstr(h_off, "bit11") != nullptr
            && std::strstr(h_off, "置起") != nullptr,
            "invert off: the headline says bit11 is set", h_off);
      check(std::strstr(h_on, "置起") == nullptr,
            "invert on: the headline must NOT claim bit11 is set — it is not the judge",
            h_on);

      /* 措辞必须落在信号上, 不能落到"撞上了"。ykd 手册 V2.4: 6041h bit11 = 「硬件限位信号有效时
       * 置 1」, 是那路信号此刻的电平, 不是已经发生的碰撞 (回零时它本来就该是 1)。 */
      check(std::strstr(h_off, "硬件限位信号有效") != nullptr,
            "the headline uses the manual's own wording for bit11", h_off);
      check(std::strstr(h_off, "撞") == nullptr,
            "and it does not say the axis crashed into a limit — bit11 is a level, "
            "not a collision", h_off);
      check(std::strstr(h_off, "驱动器认为") == nullptr,
            "…nor that the drive 'thinks' anything: nothing here is a verdict", h_off);
      check(std::strstr(h_on, "无关") != nullptr,
            "…and it answers the obvious question by saying bit11 is unrelated", h_on);
      check(std::strcmp(h_off, h_on) != 0, "two headlines, not one");
   }

   /* ---- 新灯不参与任何中止判据 ----
    * 只有原点开关压着时扫描必须一路跑到 Done。扫描区域本来就可能停在一个开关上, 用监视量去
    * 触发会白中止一趟一小时的活。 */
   caseBegin("limitsw: 只压住原点开关 → 扫描照跑, 一次都不中止");
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
      check(!fired, "the home switch never aborts a scan", what.toStdString());
      checkEq(r.ctrl.completedPoints(), 25, "all 25 points collected");
      check(r.ctrl.state() == ScanController::State::Done, "Done, not Aborted");
   }

   /* ---- 反转打开之后, 扫描必须真的能开起来 ----
    * 现场原样: 2300h 配反了 —— 60FDh 说正负限位同时压着, bit11 恒为 1。钉三件事: 反转关着时
    * 拦得住; 拦的时候话里指向 2300h; 打开后放得行。少了第三条, 上面那些断言全过而功能没用。 */
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
            "反转关着 → 拒绝启扫 (拦得住, 这是对的)", err.toStdString());
      {
         const QByteArray eb = err.toUtf8();
         check(std::strstr(eb.constData(), "2300h") != nullptr,
               "拒绝的话里指向 2300h", err.toStdString());
      }

      /* 打开反转。setDig 要再叫一次: 它注入的是 60FDh 的原始读数, 而 setDiInvert 只重算
       * limit_active, 不会回头去翻已经摆好的那三位 (和 publish() 里"先读原始值、再统一反相"
       * 是同一个顺序)。 */
      r.bus.setDiInvert(true);
      r.bus.setDig(0, false, true, true);
      r.bus.setDig(1, false, true, true);

      err.clear();
      check(r.startScan(QDir::tempPath() + "/scan_npn_on.csv", &err),
            "反转打开 → 能启扫 (这才是这台机器要的)", err.toStdString());
   }

   /* ---- 旧判据没退化 ----
    * 60FDh 读不到的机器 (本机的常态: 生效的 1A00h 里没有它) 上, bit11 置起照样中止。这一条
    * 与上面那条是一对: 把原点排除掉, 不等于把保护削弱。 */
   caseBegin("limitsw: bit11 置起而 60FDh 未知 → 照样中止 (旧判据没退化)");
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
            "and says the switch state is unknown instead of pretending it is known",
            what.toStdString());
   }

   /* ---- 文案与开关状态对得上 ----
    * 中止那行字是操作员事后唯一还能看到的东西 (面板灯早就过去了), 所以它必须说出到底是哪个
    * 开关压着。 */
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

/* ---------------------------------------------------------------- 回零 (HM) */

/* 回零是唯一一个软件兜不住的动作: 一旦发起, 朝哪走、什么时候停、撞不撞开关, 全由驱动器按
 * 6098h 自己决定。所以这里钉的是发起之前那道闸与收尾之后那句话 (纯判据, 也是操作员唯一能
 * 依赖的东西); 真正的 em_home() 调用链要 em_bus_t 和网卡才能验。 */
static void test_homing()
{
   /* 本地小工具: 判"这句话里有这个词"。判据是**给人看的话**, 所以措辞也是被测的东西 */
   auto has = [](const char *p, const char *w) {
      return p != nullptr && std::string(p).find(w) != std::string::npos;
   };

   /* ---- 方向 ---------------------------------------------------- */
   caseBegin("回零: 正/反向 → 6098h（24 / 29）");
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

   /* ---- 找限位: 方式号与白名单 ---------------------------------- */
   caseBegin("找限位: 正/负 → 6098h（18 / 17）, 以及四方式的白名单");
   {
      checkEq(ecatcmd::home_lim_method_for(false), 18, "正限位 = 方式 18");
      checkEq(ecatcmd::home_lim_method_for(true),  17, "负限位 = 方式 17");

      /* 找限位与找原点**必须是四个不同的数** —— 17/18 里任意一个与 24/29 撞上, 就是
       * "按找限位结果去找了原点开关", 而这个错误在界面上看不出来 (两边都叫"回零") */
      const int lim[2] = { ecatcmd::home_lim_method_for(false),
                           ecatcmd::home_lim_method_for(true) };
      for (int i = 0; i < 2; i++)
      {
         check(lim[i] != ecatcmd::home_method_for(false) &&
               lim[i] != ecatcmd::home_method_for(true),
               "找限位的方式号不与找原点的方式号重合");
      }
      check(lim[0] != lim[1], "正/负限位是两个不同的方式号");

      /* 白名单 = 恰好这四个。**多一个都不许** —— 白名单宽一格, 就是一个"没验过的
       * 回零方式能被发起"的口子, 而回零是软件唯一兜不住的动作 */
      check(ecatcmd::home_method_allowed(24), "24 放行");
      check(ecatcmd::home_method_allowed(29), "29 放行");
      check(ecatcmd::home_method_allowed(18), "18 放行");
      check(ecatcmd::home_method_allowed(17), "17 放行");
      check(!ecatcmd::home_method_allowed(35),
            "35 不放行 —— 它是「以当前位置为机械原点」, 根本不去找");
      check(!ecatcmd::home_method_allowed(0),  "0 不放行");
      check(!ecatcmd::home_method_allowed(19), "19 不放行");
      check(!ecatcmd::home_method_allowed(30), "30 不放行");

      /* 白名单与两个构造函数**对得上**: 这四个数必须正好是那两条函数能算出来的全集。
       * 否则会出现"界面给得出、worker 不认"或者反过来的死格 */
      int n_allowed = 0;
      for (int m = 0; m <= 40; m++)
         if (ecatcmd::home_method_allowed(m))
            n_allowed++;
      checkEq(n_allowed, 4, "0..40 里放行的恰好四个");

      check(ecatcmd::home_method_is_limit(17), "17 是找限位");
      check(ecatcmd::home_method_is_limit(18), "18 是找限位");
      check(!ecatcmd::home_method_is_limit(24), "24 不是找限位");
      check(!ecatcmd::home_method_is_limit(29), "29 不是找限位");
      check(!ecatcmd::home_method_is_limit(0),  "0 不是找限位");

      check(std::string(ecatcmd::home_method_short(18)) == "找正限位", "18 的叫法");
      check(std::string(ecatcmd::home_method_short(17)) == "找负限位", "17 的叫法");
      check(std::string(ecatcmd::home_method_short(24)) == "找原点",   "24 的叫法");
      check(std::string(ecatcmd::home_method_short(29)) == "找原点",   "29 的叫法");

      /* **动作**的名字与**开关**的名字是两样东西: "找正限位" / "正限位", 差一个"找"字。
       * 横幅要用后者 ("正限位信号此刻有效"), 而拿前者 .mid(1) 去切是看不见地依赖那个字的
       * 长度 —— 哪天动作名改成"回零到正限位", 切出来就成了"零到正限位"。 */
      check(std::string(ecatcmd::home_lim_switch_name(18)) == "正限位", "18 的开关名");
      check(std::string(ecatcmd::home_lim_switch_name(17)) == "负限位", "17 的开关名");
      check(std::string(ecatcmd::home_lim_switch_name(18)) !=
               std::string(ecatcmd::home_method_short(18)),
            "开关名与动作名不是同一个字符串");
   }

   /* ---- 找限位: 首段方向 (手册 V2.4 p46~p48 的 a)/b) 两条分支) ---- */
   caseBegin("找限位: 首段方向真值表（a) 没压着 / b) 已压着）");
   {
      /* 手册原文那四条:
       *   17 a) 反向高速 → 遇上升沿减速停止 → 正向低速 → 遇下降沿后停机
       *   17 b) 正向低速运行, 遇下降沿后停机
       *   18 a) 正向高速 → 遇上升沿减速停止 → 反向低速 → 遇下降沿后停机
       *   18 b) 反向低速运行, 遇下降沿后停机
       * **17 与 18 的首段方向是反的, 而 a) 与 b) 之间又各反一次** —— 这一格单独看
       * 任何一条都说得通, 只有摆成整张表才看得出哪条是错的。 */
      check(has(ecatcmd::home_method_first_dir(18, false), "正"), "18(a) 首段正向高速");
      check(has(ecatcmd::home_method_first_dir(18, true),  "反"), "18(b) 首段反向低速");
      check(has(ecatcmd::home_method_first_dir(17, false), "反"), "17(a) 首段反向高速");
      check(has(ecatcmd::home_method_first_dir(17, true),  "正"), "17(b) 首段正向低速");

      /* a) 与 b) 的方向**必须相反**: b) 的全部意义就是"从压着的开关上退开", 方向与
       * "朝它去"相反。哪天有人把 b) 也写成朝开关走, 那条路会一直顶着开关跑到超时 */
      check(std::string(ecatcmd::home_method_first_dir(18, false)) !=
               std::string(ecatcmd::home_method_first_dir(18, true)),
            "18 的 a) 与 b) 方向相反");
      check(std::string(ecatcmd::home_method_first_dir(17, false)) !=
               std::string(ecatcmd::home_method_first_dir(17, true)),
            "17 的 a) 与 b) 方向相反");

      /* 找原点这两条与 tgt_active 无关 (24/29 只有一条分支, 手册没给它写 b) ——
       * 原点开关在启动时压着不改变它的走法)。**挡住"顺手把 17/18 那套套到 24/29 上"** */
      check(std::string(ecatcmd::home_method_first_dir(24, false)) ==
            std::string(ecatcmd::home_method_first_dir(24, true)),
            "24 的方向不随「启动时压着」变");
      check(std::string(ecatcmd::home_method_first_dir(29, false)) ==
            std::string(ecatcmd::home_method_first_dir(29, true)),
            "29 的方向不随「启动时压着」变");

      /* 与老那两条对得上: 24 就是 home_dir_text(false), 29 就是 home_dir_text(true) */
      for (int neg = 0; neg < 2; neg++)
      {
         const int m = ecatcmd::home_method_for(neg != 0);
         check(std::string(ecatcmd::home_method_first_dir(m, false)) ==
               std::string(ecatcmd::home_dir_text(neg != 0)),
               "24/29 的首段方向与 home_dir_text 一致（不许出现两套说法）");
      }

      /* 四个方式**每个都得有名有姓**, 不许落到那个 "?" 兜底里 —— 兜底是给非法方式号的 */
      const int all[4] = { 24, 29, 18, 17 };
      for (int i = 0; i < 4; i++)
      {
         check(std::string(ecatcmd::home_method_first_dir(all[i], false)) != "?",
               "合法方式号都有首段方向");
         check(std::string(ecatcmd::home_method_first_dir(all[i], true)) != "?",
               "合法方式号都有首段方向（含 b) 那一格）");
      }
   }

   /* ---- 找限位: 目标/另一侧是哪两位 ----------------------------- */
   caseBegin("找限位: 目标开关 → 60FDh 的哪一位（只此一处）");
   {
      /* 这两条是"哪一位是目标"的唯一定义。允否、分支预告、横幅都读它, 所以它错了
       * 就是三处一起错, 而且**方向会正好反过来** */
      check(ecatcmd::home_lim_target_active(18, true, false),  "18 看正限位位");
      check(!ecatcmd::home_lim_target_active(18, false, true), "18 不看负限位位");
      check(ecatcmd::home_lim_target_active(17, false, true),  "17 看负限位位");
      check(!ecatcmd::home_lim_target_active(17, true, false), "17 不看正限位位");

      check(ecatcmd::home_lim_other_active(18, false, true),   "18 的另一侧是负限位");
      check(ecatcmd::home_lim_other_active(17, true, false),   "17 的另一侧是正限位");

      /* 上面四条是逐格举的例子, 容易被"恰好举到那两格"蒙过去。这一组把**两位的四种
       * 组合全走一遍**, 钉的是"哪一位进哪个角色"本身 —— 写反 (18 去看 dig_neg) 在单看
       * 一格时说得通 (都叫"限位位"), 只有三个组合一起看才认得出。 */
      for (int p = 0; p < 2; p++)
      {
         for (int n = 0; n < 2; n++)
         {
            const bool pos = (p != 0), neg = (n != 0);
            check(ecatcmd::home_lim_target_active(18, pos, neg) == pos,
                  "18 的目标就是正限位位本身");
            check(ecatcmd::home_lim_other_active(18, pos, neg) == neg,
                  "18 的另一侧就是负限位位本身");
            check(ecatcmd::home_lim_target_active(17, pos, neg) == neg,
                  "17 的目标就是负限位位本身");
            check(ecatcmd::home_lim_other_active(17, pos, neg) == pos,
                  "17 的另一侧就是正限位位本身");
         }
      }

      /* 两位**同时**压着 = 那道否决要拦的形状, 此时两者确实同时为真 —— 写死下来,
       * 免得哪天有人把"互斥"当成不变式, 顺手把这道否决的条件改成恒假 */
      check(ecatcmd::home_lim_target_active(18, true, true) &&
            ecatcmd::home_lim_other_active(18, true, true),
            "两位都压着时 18 的目标与另一侧同时为真（这正是被拦的那种形状）");

      /* 24/29 没有"目标限位" —— 它们的基准是原点开关 X0。这里是挡住"顺手也给它算一个" */
      check(!ecatcmd::home_lim_target_active(24, true, true), "24 没有目标限位");
      check(!ecatcmd::home_lim_other_active(24, true, true),  "24 没有另一侧限位");
   }

   /* ---- 找限位: 两道否决 ---------------------------------------- */
   caseBegin("找限位闸: 只否决「60FDh 读不到」与「两侧同时有效」");
   {
      check(ecatcmd::home_lim_refusal(true, false, false) == nullptr,
            "读得到 + 两侧都没压着 → 放行 (a) 分支)");
      check(ecatcmd::home_lim_refusal(true, true, false) == nullptr,
            "读得到 + 目标压着 → 放行 (b) 分支) —— b) 是正规做法, 不是要拦的情形");
      check(ecatcmd::home_lim_refusal(true, false, true) == nullptr,
            "读得到 + 另一侧压着 → 放行 (朝目标去正好是远离那一侧)");

      const char *r = ecatcmd::home_lim_refusal(false, false, false);
      check(r != nullptr, "60FDh 读不到 → 不放行");
      check(has(r, "60FDh"), "说清楚是 60FDh 的事");
      check(has(r, "TxPDO"), "指到「让 60FDh 进 TxPDO」这条路");
      check(has(r, "拒绝"), "必须有「拒绝」两个字 —— 界面横幅就是照它上红色的");

      r = ecatcmd::home_lim_refusal(true, true, true);
      check(r != nullptr, "两侧同时有效 → 不放行");
      check(has(r, "2300h"), "指到 2300h（极性配反是本机实测过的那个原因）");
      check(has(r, "同时"), "说清楚「同时」才是问题");
      check(has(r, "拒绝"), "必须有「拒绝」两个字");

      /* 读不到与两侧同时有效**是两句不同的话**。同一句话会让操作员去查错的东西:
       * 前者是"没打开一个勾", 后者是"接线/极性与读数不符" */
      check(std::string(ecatcmd::home_lim_refusal(false, false, false)) !=
               std::string(ecatcmd::home_lim_refusal(true, true, true)),
            "两道否决不是同一句话");

      /* 两条否决的文案都要点名「拒绝找限位」—— 让人一眼看出拦的是哪一个动作 */
      check(has(r, "拒绝找限位"), "否决文案点名「拒绝找限位」");
      check(has(ecatcmd::home_lim_refusal(false, false, false), "拒绝找限位"),
            "读不到那一条也点名「拒绝找限位」");
   }

   /* ---- 找限位: 发起前的分支预告 -------------------------------- */
   caseBegin("找限位预告: 说清这一趟走 a) 还是 b), 且方向与快慢都对得上");
   {
      /* 四句话两两不同 —— 四格必须可分辨。都在说"正在回零"就等于没说 */
      const char *txt[4] = {
         ecatcmd::home_lim_branch_text(18, false),
         ecatcmd::home_lim_branch_text(18, true),
         ecatcmd::home_lim_branch_text(17, false),
         ecatcmd::home_lim_branch_text(17, true),
      };
      for (int i = 0; i < 4; i++)
         for (int j = i + 1; j < 4; j++)
            check(std::string(txt[i]) != std::string(txt[j]), "四格预告两两不同");

      /* 逐格对手册: 方向 + 快慢 + 走哪条分支, 三样缺一不可 */
      check(has(txt[0], "a)") && has(txt[0], "正向高速"), "18(a): a) + 正向高速");
      check(has(txt[1], "b)") && has(txt[1], "反向低速"), "18(b): b) + 反向低速");
      check(has(txt[2], "a)") && has(txt[2], "反向高速"), "17(a): a) + 反向高速");
      check(has(txt[3], "b)") && has(txt[3], "正向低速"), "17(b): b) + 正向低速");

      /* 预告里的**方向与快慢分开说** —— 光说"反向"不够: b) 是"反向**低速**退开",
       * 而 a) 的第二段也是反向低速, 操作员要靠"低速"认出现在是退开那一段 */
      for (int i = 0; i < 4; i++)
      {
         check(has(txt[i], "高速") || has(txt[i], "低速"), "每格都写了快慢");
         check(has(txt[i], "释放点"),
               "每格都写明落点 = 开关的释放点 —— 那就是这趟结束之后坐标 0 的位置, "
               "不写清楚, 操作员会以为 0 在开关的中心上");
      }

      /* 预告的方向与真值表**同源**: 谁要是改了 home_method_first_dir 而没改文案,
       * 这一条会响。这是这组断言里最值钱的一条 —— 两处说法不一致比说错更难查 */
      const int ms[4] = { 18, 18, 17, 17 };
      const bool tas[4] = { false, true, false, true };
      for (int i = 0; i < 4; i++)
      {
         const std::string d = ecatcmd::home_method_first_dir(ms[i], tas[i]);
         check(std::string(txt[i]).find(d) != std::string::npos,
               "预告里的首段方向与 home_method_first_dir 一致");
      }

      /* 找原点那两格不走这条预告 (doHome 只对 17/18 调它), 但函数得是**全的** ——
       * 调用点写错时不许打出半句话 */
      check(has(ecatcmd::home_lim_branch_text(24, false), "找原点"),
            "24 落到兜底那句上, 而且不是半句话");
   }

   /* ---- 返回速度派生 -------------------------------------------- */
   caseBegin("回零: 6099h:02 = 6099h:01 / 4（下限 1）");
   {
      checkEq(ecatcmd::home_vel_slow(2000), 500, "2000 → 500");
      checkEq(ecatcmd::home_vel_slow(1000), 250, "1000 → 250");
      checkEq(ecatcmd::home_vel_slow(4),      1, "4 → 1");
      checkEq(ecatcmd::home_vel_slow(3),      1, "3 → 整除到 0, 由下限救回 1");
      checkEq(ecatcmd::home_vel_slow(0),      1, "0 → 1（写 0 是什么语义手册没写, 而"
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
      checkEq(ecatcmd::home_vel_clamp(0),         HMI_HOME_VEL_MIN, "0 → 下限");
      checkEq(ecatcmd::home_vel_clamp(99),        HMI_HOME_VEL_MIN, "99 → 下限");
      /* **比上限高一点点**, 不写一个具体的数 —— 上一个版本这里写的是 2001, 而 2001
       * 在上限从 2000 提到 100000 之后就成了一个**合法值**, 这条断言会从"验夹取"
       * 变成"验上限还是 2000"。夹取测的是边界关系, 不是某一个数 */
      checkEq(ecatcmd::home_vel_clamp((int32_t)HMI_HOME_VEL_MAX + 1), HMI_HOME_VEL_MAX,
              "MAX + 1 → 上限");
      checkEq(ecatcmd::home_vel_clamp(2147483647), HMI_HOME_VEL_MAX, "INT32_MAX → 上限");
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
   caseBegin("回零: 609Ah 由速度派生 —— 斜坡时间 0.1 s, 加速度封顶");
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
      check(ramp, "下限..50000 全域: 斜坡时间恒为 0.1 s (acc == v * 10)");

      /* 封顶: 一个很高的速度**不许**换来一个比机器自己配的还硬的加速度 */
      checkEq(ecatcmd::home_accel_for(50001), HMI_HOME_ACC_MAX, "刚过 50000 → 封顶");
      checkEq(ecatcmd::home_accel_for(HMI_HOME_VEL_MAX), HMI_HOME_ACC_MAX,
              "上限速度 → 还是封顶 (斜坡变长到 0.2 s, 而不是加速度翻倍)");
      /* 没有那个除法守卫的话 v * 10 会回绕, 于是"很大的速度"算出"很小的加速度" */
      checkEq(ecatcmd::home_accel_for(4294967295u), HMI_HOME_ACC_MAX,
              "UINT32_MAX → 封顶, 不回绕");

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
   caseBegin("回零闸: 分支, 以及分支的顺序");
   {
      check(ecatcmd::home_refusal(true, true, true, false, false) == nullptr,
            "全清 → 放行 (nullptr)");

      check(has(ecatcmd::home_refusal(false, true, true, false, false), "总线"),
            "没连上 → 说总线");
      check(has(ecatcmd::home_refusal(false, true, true, true, false), "总线"),
            "没连上 + 有故障 → 还是先说总线: 那时连状态字都没有, 说故障是在猜");

      check(has(ecatcmd::home_refusal(true, false, true, false, false), "位置"),
            "一笔 6064h 都没取到 → 说位置未知");

      /* **顺序的要害**: 一个从没收到过的状态字里的 bit3 不是信息。这一条与
       * axis_needs_reset 编码的是同一条规矩 */
      const char *r = ecatcmd::home_refusal(true, true, false, true, false);
      check(has(r, "未知"), "丢帧 + 有故障 → 说「状态未知」");
      check(!has(r, "bit3"), "丢帧时不许拿一个没收到过的状态字里的 bit3 说事");

      r = ecatcmd::home_refusal(true, true, true, true, false);
      check(has(r, "bit3") && has(r, "故障复位"), "有故障 → 点名 bit3, 并指到「故障复位」");

      r = ecatcmd::home_refusal(true, true, true, false, true);
      check(has(r, "停止"), "还有轴在走 → 让人先按「停止」");
      check(has(r, "回零期间插补器是停的") || has(r, "停在半途"),
            "还要说明白为什么 —— 不然那句话看着像没道理的门槛");
   }

   /* ---- 收尾结局 ------------------------------------------------ */
   caseBegin("回零收尾: 结局由实测状态定, 不由返回码排列组合");
   {
      check(ecatcmd::home_end_state(false, false, true,  0) == ecatcmd::HOME_END_NEVER_STARTED,
            "没发起过就是没发起过");
      check(ecatcmd::home_end_state(false, true,  true, -1) == ecatcmd::HOME_END_NEVER_STARTED,
            "闸拦下时哪怕现场有故障, 也不是「这次回零把它搞坏了」");

      check(ecatcmd::home_end_state(true, true, false, 0) == ecatcmd::HOME_END_FAULTED,
            "bit3 还在 → 故障");
      /* 按 CiA402 这两条不该同时成立; 万一真同时读到, 该报的是**故障** —— 那才是要人
       * 动手的那一件事。反过来说成"保持中"就是漏掉一个真故障 */
      check(ecatcmd::home_end_state(true, true, true, 0) == ecatcmd::HOME_END_FAULTED,
            "故障与使能同时读到 → 报故障");

      check(ecatcmd::home_end_state(true, false, false, 0) == ecatcmd::HOME_END_STRANDED,
            "没使能也没故障 → 状态不明");
      /* 这一格是这道函数存在的**主要理由**: 轴可能确实带电, 但驱动器不按 CSP 解释
       * 607Ah, 而 interpolate() 每周期都在往 607Ah 里写 —— "带力矩停着"和
       * "带电但模式不对"是两件事, 说成 HOLDING 就是撒谎 */
      check(ecatcmd::home_end_state(true, false, true, -1) == ecatcmd::HOME_END_STRANDED,
            "已使能但没切回 CSP → 不能说 HOLDING");
      check(ecatcmd::home_end_state(true, false, false, -1) == ecatcmd::HOME_END_STRANDED,
            "两样都不成立");

      check(ecatcmd::home_end_state(true, false, true, 0) == ecatcmd::HOME_END_HOLDING,
            "已使能 + 已是 CSP → 保持中 (用户要的那一档)");
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

      check(has(ecatcmd::home_cause_text(0), "到位"), "rc = 0 → 到位");
      check(has(ecatcmd::home_cause_text(1), "停止"), "rc = 1 点名「停止」");
      /* **被「停止」中止不是失败。** 那是人让它停的, 说成失败会让人去找一个不存在
       * 的毛病 (这条路径本来就是本功能的半个需求) */
      check(!has(ecatcmd::home_cause_text(1), "失败"), "被「停止」中止不许说成失败");
      check(has(ecatcmd::home_cause_text(-1), "方向"), "真失败时给换方向的建议");
      check(has(ecatcmd::home_cause_text(-1), "硬顶"),
            "失败建议里写明别硬顶 —— 撞着开关还硬回, 才是真会伤机器的做法");
      /* 失败那一格混着至少五种原因 (方式越界 / 参数写不进 / 驱动器没接受 HM 模式 /
       * 等 bit12 超时 / bit3 或 bit13)。**它们是同一句话就必须告诉人上哪去分开** ——
       * 否则操作员只会照着"方向不对"反复换按钮, 而真正的原因是"驱动器压根没进 HM"。 */
      check(has(ecatcmd::home_cause_text(-1), "控制台"),
            "失败那一格必须点明具体原因在控制台里 (五种原因在这里是同一句话)");
      check(has(ecatcmd::home_cause_text(-1), "6061h"),
            "失败那一格点名 6061h —— 「驱动器没接受 HM 模式」是它最常见的那个原因");
   }

   /* ---- 6061h (实际运行模式) 的说人话 ---------------------------- */
   caseBegin("6061h: 数字说成人话（含「没读过」与「读失败」两件不同的事）");
   {
      /* 手册 §3.7 / 6060h 那张表给出的全部取值 */
      check(std::string(ecatcmd::mode_text(0)) == "未定义",       "0 未定义");
      check(has(ecatcmd::mode_text(1), "PP"),                    "1 位置模式");
      check(has(ecatcmd::mode_text(3), "PV"),                    "3 速度模式");
      check(has(ecatcmd::mode_text(6), "HM"),                    "6 回原点 —— 回零那一刻要看到的");
      check(has(ecatcmd::mode_text(8), "CSP"),                   "8 循环同步位置 —— 收尾后要看到的");

      /* 两个负数**必须分开**: HMI_MODE_DISP_UNREAD 是"工作线程还没读过",
       * em_get_mode() 读失败时返回的 -1 是"读了但读不到"。界面上它们要是同一句话,
       * 一次掉线就会被看成一个从没读过 6061h 的轴。 */
      check(std::string(ecatcmd::mode_text(HMI_MODE_DISP_UNREAD)) == "还没读过",
            "没读过");
      check(has(ecatcmd::mode_text(-1), "读失败"), "读失败");
      check(std::string(ecatcmd::mode_text(-1)) !=
               std::string(ecatcmd::mode_text(HMI_MODE_DISP_UNREAD)),
            "「没读过」与「读失败」不能是同一句话");

      /* 手册之外的模式号照实说不认识。**猜一个名字比说不知道坏得多** ——
       * 2 在别的厂家是 VL (速度模式), 本驱动器手册里没有它。 */
      check(has(ecatcmd::mode_text(2), "手册"),  "2 不在手册里 → 不许猜");
      check(has(ecatcmd::mode_text(9), "手册"),  "9 不在手册里 → 不许猜");
      check(has(ecatcmd::mode_text(-3), "手册"), "-3 不在手册里 → 不许猜");
   }

   /* ---- 6061h 那个哨兵值本身 ------------------------------------ */
   caseBegin("6061h: 哨兵值不与任何合法模式号相撞");
   {
      check(HMI_MODE_DISP_UNREAD < 0, "哨兵是负数");
      check(HMI_MODE_DISP_UNREAD != -1,
            "哨兵不许是 -1 —— 那是 em_get_mode() 读失败的返回值");
      check(!has(ecatcmd::mode_text(HMI_MODE_DISP_UNREAD), "读失败"),
            "「没读过」不许说成「读失败」");
   }

   /* ---- 603Fh (驱动器故障码) ------------------------------------ */
   /* 上面那个 has 收 const char*, 而 603Fh 这几个 helper 返回 QString (要拼轴号与码) */
   auto hasq = [](const QString &s, const char *w) {
      return s.contains(QString::fromUtf8(w));
   };

   caseBegin("603Fh: 手册那张表逐条 (过流与过压处置办法不搭界, 认错一个就是白查半天)");
   {
      /* 表在 docs/ykd2205pe_ci402.md §报警与指示灯 (与 1003h 低 16 位同源)。
       * **逐条钉住**, 不许写成"非空即通过": 报错码是这台设备上唯一能分开这几种故障的东西。 */
      check(hasq(ecatcmd::fault_code_text(0x0000), "无错误"), "0000 无错误");
      check(hasq(ecatcmd::fault_code_text(0xFF01), "过流"),   "FF01 过流");
      check(hasq(ecatcmd::fault_code_text(0xFF02), "过压"),   "FF02 过压");
      check(hasq(ecatcmd::fault_code_text(0xFF03), "欠压"),   "FF03 欠压");
      check(hasq(ecatcmd::fault_code_text(0xFF04), "动力线"), "FF04 动力线报警");
      check(hasq(ecatcmd::fault_code_text(0xFF06), "通讯"),   "FF06 通讯报警");
      check(hasq(ecatcmd::fault_code_text(0xFF08), "传感器"), "FF08 传感器告警");

      /* 十六进制也要在: 操作员拿它去对驱动器面板, 而"过压"两个字对不了面板 */
      check(hasq(ecatcmd::fault_code_text(0xFF02), "FF02"), "码本身照实写出来");
      check(hasq(ecatcmd::fault_code_text(0x0000), "0000"), "0 要写成 0000, 不是 0");
      check(!hasq(ecatcmd::fault_code_text(0xFF02), "FF01"), "相邻的码不许抄串");

      /* 手册之外的码照实报十六进制。**猜一个名字比说不知道坏得多** */
      check(hasq(ecatcmd::fault_code_text(0x1234), "手册"), "1234 不在手册里 → 不许猜");
      check(hasq(ecatcmd::fault_code_text(0x1234), "1234"), "但码要照实写出来");

      /* 每一个手册里的码都要有一句"下一步查哪儿" —— 这张表少一条, 红横幅上就少半句,
       * 而那半句正是操作员唯一能照着做的东西 */
      const uint16_t known[7] = { 0x0000, 0xFF01, 0xFF02, 0xFF03, 0xFF04, 0xFF06, 0xFF08 };

      for (int k = 0; k < 7; k++)
         check(ecatcmd::fault_code_action(known[k]) != nullptr,
               "手册里的每个码都有处置办法");

      /* 反过来: 手册之外的码**不给**建议 —— 没有依据的建议比没有建议坏 */
      check(ecatcmd::fault_code_action(0x1234) == nullptr, "手册之外的码不编处置办法");
   }

   caseBegin("603Fh: 哨兵值 —— 「还没读到」「读不到」「0x0000 无错误」是三件事");
   {
      check(HMI_FAULT_CODE_UNREAD < 0, "哨兵是负数");
      check(HMI_FAULT_CODE_UNREAD != HMI_FAULT_CODE_FAIL, "两个哨兵不是一个值");
      check(HMI_FAULT_CODE_UNREAD != 0x0000 && HMI_FAULT_CODE_FAIL != 0x0000,
            "哨兵不许是 0 —— 0x0000 是「无错误」这个真实读数");

      const QString u = ecatcmd::fault_code_text(HMI_FAULT_CODE_UNREAD);
      const QString f = ecatcmd::fault_code_text(HMI_FAULT_CODE_FAIL);
      const QString z = ecatcmd::fault_code_text(0x0000);

      check(hasq(u, "还没读到"), "没读到的说法");
      check(hasq(f, "读不到"),   "读不到的说法");
      /* 与 6061h 那个坑同一个: 两句话要是一样, 一次掉线就会被看成一个从没读过 603Fh 的轴 */
      check(u != f,      "「还没读到」与「读不到」不能是同一句话");
      check(u != z && f != z, "「没读到」不许说成「无错误」");
      check(!hasq(f, "无错误"), "读不到不等于没故障 —— bit3 还立着");
   }

   caseBegin("603Fh: 拼进「轴X 603Fh = ……」之后仍然通顺");
   {
      /* 这几句是拼出来的: "轴X 603Fh = " + fault_code_text()。值那一段要是自带索引
       * (写成"正在读 603Fh…"), 整句就成了 "轴X 603Fh = 正在读 603Fh…" ——
       * 所以拼完那一句里 "603Fh" 只能出现一次。 */
      auto countOf = [](const QString &s) {
         int n = 0;
         for (int at = s.indexOf(QStringLiteral("603Fh")); at >= 0;
              at = s.indexOf(QStringLiteral("603Fh"), at + 1))
            n++;
         return n;
      };

      const int codes[3] = { 0xFF02, HMI_FAULT_CODE_UNREAD, HMI_FAULT_CODE_FAIL };

      for (int k = 0; k < 3; k++)
         checkEq(countOf(ecatcmd::fault_axis_text(0, codes[k])), 1,
                 "603Fh 在整句里只出现一次");

      check(hasq(ecatcmd::fault_axis_text(0, 0xFF02), "轴X"), "轴号说成人话");
      check(hasq(ecatcmd::fault_axis_text(1, 0xFF02), "轴Y"), "另一根也对");
   }

   caseBegin("603Fh: 横幅上每一根报故障的轴各自的码都要说到");
   {
      BusTelem t;
      t.connected = true;
      t.in_op     = true;
      t.naxis     = 2;
      for (int i = 0; i < 2; i++)
      {
         t.ax[i].valid     = true;
         t.ax[i].mirror_ok = true;
      }

      /* 没故障: 这一句不许自己编出个码来 */
      check(ecatcmd::faulted_axes_text(t).isEmpty(), "没故障 → 不提码");
      check(!hasq(ecatcmd::fault_banner_text(t), "603Fh"), "没故障 → 横幅里也不提 603Fh");

      /* 只有轴Y 报故障: 码只许说那一根 —— 说成两根会把操作员支到健康的那根上 */
      t.ax[1].fault      = true;
      t.ax[1].fault_code = 0xFF01;
      check(hasq(ecatcmd::faulted_axes_text(t), "轴Y"), "带上了轴Y");
      check(hasq(ecatcmd::faulted_axes_text(t), "过流"), "带上了码的意思");
      check(!hasq(ecatcmd::faulted_axes_text(t), "轴X"), "不许把没故障的轴X 也一起说");

      /* 码还没到 (UNREAD) 与读不到 (FAIL): 两拍都照实说, 都不许编 */
      t.ax[1].fault_code = HMI_FAULT_CODE_UNREAD;
      check(hasq(ecatcmd::fault_banner_text(t), "还没读到"), "第一拍: 照实说还没读到");
      t.ax[1].fault_code = HMI_FAULT_CODE_FAIL;
      check(hasq(ecatcmd::fault_banner_text(t), "读不到"), "读不到也照实说");

      t.ax[1].fault_code = 0xFF01;

      const QString b = ecatcmd::fault_banner_text(t);
      check(hasq(b, "6041h bit3"), "横幅说清是哪个位");
      check(hasq(b, "0xFF01"),    "横幅给出码");
      check(hasq(b, "过流"),      "横幅给出码的意思");
      check(hasq(b, "机械"),      "横幅给出下一步查哪儿 —— 处置办法随码走");

      /* 两根一起报: 各自的码都要在, 不能只留后一根 (那正是"故障码"这件事最容易被写丢的地方) */
      t.ax[0].fault      = true;
      t.ax[0].fault_code = 0xFF03;

      const QString both = ecatcmd::faulted_axes_text(t);
      check(hasq(both, "轴X") && hasq(both, "轴Y"), "两根都在");
      check(hasq(both, "欠压") && hasq(both, "过流"), "两根各自的码都在");
   }

   /* ---- 控制器那道闸 -------------------------------------------- */
   caseBegin("回零中不起扫 (armRun), 回零结束立刻能起扫");
   {
      Rig r;
      r.ctrl.setParams(Rig::smallParams());
      QString err;

      r.bus.setHoming(true);
      check(!r.startScan(QDir::tempPath() + "/hm1.csv", &err), "回零中 → 拒绝起扫");
      check(err.contains(QStringLiteral("回零")), "理由说的是回零", err.toStdString());

      /* 这道闸是"等一下", 不是"这份数据坏了" —— 回零做完必须马上能接着扫。
       * 只会拒绝的闸跟没写一样 (同 preflight 那条的规矩) */
      r.bus.setHoming(false);
      check(r.startScan(QDir::tempPath() + "/hm2.csv", &err), "回零结束 → 放行",
            err.toStdString());
      r.ctrl.abort(QString());
   }
}

/* ---------------------------------------------------------------- 三个模拟源 */

/* 「读一次」按钮四个源都能点, 这里钉的正是那条按钮的闸门所依赖的一件事: 一次请求恰好回一次
 * (readingReady 或 readingFailed, 不多不少)。某个源改成回两次 (或一次都不回), 出错的是扫描的
 * CSV 里悄悄少一个点或者错一个点, 不报任何错。
 * 回话全是 QTimer::singleShot 投递的, 每个请求自己起一个 QEventLoop: 复用同一个的话, 上一次
 * 那个超时定时器会在下一次 exec() 里提前把它按停。 */

/* 一次请求的回话 */
struct Reply
{
   int     ready  = 0;
   int     failed = 0;
   double  watts  = 0.0;
   QString err;
};

/* 发一个请求, 把事件循环转到有回话 (或超时) 为止。
 * connect 的 context 必须传 &loop: 不传的话宿主是信号发送方, lambda 里按引用捕获的 loop 早就
 * 析构了, 上一次留下的那个 lambda 会被叫起来, 碰的正是那个已经没了的 QEventLoop。 */
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

   /* 已经回了就不再进循环: 有的源是同步回话的 (没打开时直接 emit readingFailed), 那时上面两个
    * lambda 已经跑过; 而 loop.quit() 在 exec() 之前调是没有用的, 会白等到超时。 */
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
            "closed → exactly one failure, no reading", shut.err.toStdString());
      check(!man.isOpen(), "and it stays closed");

      QString e;
      check(man.open(&e), "open()", e.toStdString());

      const Reply r = ask(&man);
      checkEq(r.ready, 1, "open → exactly one reading");
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

   /* 这条不是在验某个源的行为, 是在验那条约定本身 —— 也就是界面上那个按钮为什么必须在扫描
    * 期间禁用: 两个请求撞在一起时, 源会老老实实回两次, 谁也不知道哪个数属于哪一次。真机那条
    * (Ophir) 更狠: 它按时间戳只认严格更新的采样, 于是其中一边白等到超时。 */
   caseBegin("meter: 两个未决请求撞在一起 → 源回两次, 所以闸门必须由调用方把");
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

      checkEq(n, 2, "two overlapping requests → two readings, unresolvable by the caller");
   }

   /* 这就是那个闸门要挡的东西 —— 而闸门在界面上 (ScanWindow::refresh 里那条 setEnabled),
    * 那层要 Qt Widgets, 本文件按约定不链。所以这条约定靠上面这条测试说明为什么必须挡。 */
}

/* ---------------------------------------------------------------- 真机功率计 */

/* PD300R + Juno+ 这条路 (见 ophircom.h 顶部)。这条腿不插表头也能跑 —— 验的是上半截: COM 对象
 * 注册了没有、绕开注册表的 typelib 加载走不走得通、没插设备时会不会干净地报错 (不是崩, 也不是
 * 卡住)。下半截 (真读到功率) 只在表头插着时跑。这台机器没装 StarLab -> SKIP, 不是 FAIL。 */
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

   /* GetVersion 这一条是整条路的关键证据: 这台机器上 IDispatch::GetIDsOfNames / GetTypeInfo /
    * Invoke 全返回 0x8002801D (注册表里 typelib 的版本号是字面量 "a.a"), 而 oPhirCom 绕开注册表,
    * 直接从 dll 资源里 LoadTypeLibEx 再走 ITypeInfo::Invoke —— 拿到数证明的是那套绕法成立。 */
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

/* ---------------------------------------------------------------- 连续读数 */

/* 独立功率计窗口那个「连续读数」的台架: 一个 MeterLog + 一个 FakeMeter, 时钟手拨。
 * 顺序与 Rig::stepOnce 一致 —— 先让采集器看一眼表 (决定发不发), 再放回包 (等价于"硬件"回话)。 */
struct MeterRig
{
   FakeMeter meter;
   MeterLog  log;
   int64_t   now = 5000;

   explicit MeterRig(int latency_ms = 60)
   {
      meter.setLatency(latency_ms);
      meter.open(nullptr);
      log.tick(now);              /* 先把钟对齐, 再挂源 (与 Rig 构造同一个理由) */
      log.setSource(&meter);
   }

   void step(int dt = 20)
   {
      meter.setNow(now);
      log.tick(now);
      meter.pump(now);
      now += dt;
   }

   void run(int64_t ms, int dt = 20)
   {
      const int64_t stop = now + ms;
      while (now < stop)
         step(dt);
   }
};

static void test_meterlog()
{
   caseBegin("meterlog: 到点才发 —— 一个间隔一个请求, 不等就是不发");
   {
      MeterRig r;              /* 往返 60 */
      QString e;
      r.log.setInterval(200);
      check(r.log.start(200, &e), "start()", e.toStdString());
      checkEq(r.log.intervalMs(), 200, "interval kept");

      /* 按下去那一刻就发第一个 (不空等一个间隔), 但**下一拍不许再发** */
      r.step(60);
      checkEq(r.meter.requests(), 1, "第一个请求发出去了");
      checkEq(r.meter.readings(), 0, "回话还没到 (往返 60)");
      check(r.log.pending(), "有一个未决请求在飞");

      r.step(60);
      checkEq(r.meter.readings(), 1, "回话到了");
      checkEq(r.log.count(), 1, "记下第一笔");

      /* 回话驱动排下一次: 两笔之间的间隔 = 间隔 + 往返 (再算上钟的粒度 60)。
       * **绝不是每拍一笔** —— 那正是一个未决请求的约束下不能做的事 */
      r.run(3000);
      const int n = r.log.count();
      check(n >= 8, "采到了一串");

      int too_close = 0, too_far = 0;
      for (int i = 1; i < n; i++)
      {
         const int64_t d = r.log.samples()[i].ms - r.log.samples()[i - 1].ms;
         if (d < 200)
            too_close++;
         if (d > 200 + 2 * 60)
            too_far++;
      }
      checkEq(too_close, 0, "没有哪两笔挤得比间隔还近");
      checkEq(too_far, 0, "也没有哪两笔隔得超出 间隔 + 往返 + 一拍钟");
      checkEq(r.meter.overlaps(), 0, "从不有两个未决请求同时压在一个源上");
   }

   caseBegin("meterlog: 未决期间再拨多少拍也不发第二个 (往返比间隔长也一样)");
   {
      /* 往返 500 > 间隔 20: 固定节拍的做法会在这里堆出一串请求 */
      MeterRig r(500);
      QString e;
      r.log.setInterval(20);
      r.log.start(20, &e);

      r.run(2000, 20);
      check(r.log.count() >= 3, "还是采到了数 (节奏里有往返时间, 这是诚实的记法)");
      checkEq(r.meter.overlaps(), 0, "没有因为间隔短就堆请求");
      check(r.log.count() <= 5, "2000ms / (500+20) 大约就是这么多笔, 不是 100 笔");
   }

   caseBegin("meterlog: 换源 —— 旧源迟到的回话被丢掉, 缓冲清空");
   {
      MeterRig r(500);
      QString e;
      r.log.setInterval(200);
      check(r.log.start(200, &e), "start()", e.toStdString());
      r.step(20);                                 /* 发出第一个, 500ms 后才回 */

      checkEq(r.meter.requests(), 1, "旧源那儿有一个在飞");
      const int64_t handed_over_at = r.now;

      FakeMeter other;                            /* 新源, 快得多 */
      other.setLatency(1);
      other.open(nullptr);

      r.log.setSource(&other);
      checkEq(r.log.count(), 0, "缓冲清空 (一条曲线只画一个源)");
      check(!r.log.pending(), "那一个未决请求作废了");

      /* 旧源那一份现在才回来。**必须被丢掉**: 它是另一个东西采的数 */
      r.meter.pump(handed_over_at + 1000);
      checkEq(r.meter.readings(), 1, "旧源确实回了一份 (所以下面这一条才有意义)");
      checkEq(r.log.count(), 0, "旧源迟到的回话被丢掉 (sender() 不是当前源)");

      /* 新源照常喂数。这一步得自己拨 —— 台架里 pump 的是老那一个 */
      for (int k = 0; k < 20; k++)
      {
         other.setNow(r.now);
         r.log.tick(r.now);
         other.pump(r.now);
         r.now += 20;
      }
      check(r.log.count() > 0, "新源在喂数");
      check(r.log.source() == &other, "当前源就是新的那一个");
   }

   caseBegin("meterlog: 环形缓冲 —— 到容量丢最旧的那个");
   {
      MeterRig r(1);
      QString e;
      r.log.setInterval(MeterLog::kMinIntervalMs);
      r.log.start(MeterLog::kMinIntervalMs, &e);
      check(!r.log.full(), "刚打开的时候没满");

      /* 记下每一笔的 ms, 于是"第一个该丢的是谁"是算出来的, 不是猜的 */
      QVector<int64_t> seen;
      QObject::connect(&r.log, &MeterLog::sampleAdded, [&] {
         if (!r.log.samples().isEmpty())
            seen.append(r.log.samples().last().ms);
      });

      const int want = MeterLog::kCapacity + 50;
      int guard = 0;
      while (seen.size() < want && guard++ < want * 4)
         r.step(30);

      check(seen.size() >= want, "确实采够了那么多个 (采 20050 个, 缓冲只有 20000)");
      checkEq(r.log.count(), MeterLog::kCapacity, "缓冲停在容量上, 不是无限涨");
      /* 满了这件事**必须能问出来**: 屏幕上那条曲线在丢数的时候看着照旧很健康 */
      check(r.log.full(), "full() 说得出'现在满了, 再采就要丢最旧的'");
      checkEq((long long)r.log.samples().first().ms, (long long)seen[50],
              "留在最前面的正是第 51 笔 —— 丢的只能是最旧的");
      checkEq((long long)r.log.samples().last().ms, (long long)seen.last(),
              "最后一笔就是刚采到的那个");
   }

   caseBegin("meterlog: 平均 —— 一笔采样要 N 个读数, 少一个都不算数");
   {
      MeterRig r(20);
      QString e;

      checkEq(r.log.average(), 1, "缺省是每次都要 (1)");
      r.log.setAverage(4);
      checkEq(r.log.average(), 4, "setAverage 记住了");
      r.log.setAverage(0);
      checkEq(r.log.average(), 1, "0 夹到 1");
      r.log.setAverage(100000);
      checkEq(r.log.average(), MeterLog::kMaxAverage, "太大夹到 kMaxAverage");
      r.log.setAverage(4);

      r.log.setInterval(200);
      check(r.log.start(200, &e), "start()", e.toStdString());

      /* 四个**不同的**数: 求平均与"只留最后一个"在同一个数上看不出区别 */
      const double v[4] = { 1.0, 2.0, 3.0, 4.0 };
      for (int i = 0; i < 4; i++)
      {
         r.step();                       /* 这一拍把第 i+1 个子读数发出去 */
         r.meter.setValue(v[i]);         /* 它回来的就是这个数 */
         r.step();                       /* 回话到, 累加进手上这一批 */
      }

      checkEq(r.meter.requests(), 4, "一笔采样发了 4 个请求");
      checkEq(r.log.count(), 1, "4 个读数只记成一笔");
      checkEq(r.log.stats().n, 1, "这一笔是 ok 的");
      checkNear(r.log.stats().last, 2.5, "记的是那 4 个的平均 (1+2+3+4)/4");

      /* N 次里有一次没读回来 -> **这一笔作废** (与扫描那个点读不到时同一个口径:
       * 拿半边的数求平均是编出来的, 而它在曲线上和别的点长得一模一样)。
       * 两条都得验: 第一个子读数就失败, 与攒到一半才失败 */
      MeterRig f(20);
      f.log.setAverage(4);
      f.log.setInterval(200);
      f.log.start(200, &e);
      checkEq(f.log.count(), 0, "刚开始一笔都没有");
      f.meter.failNext();                  /* 第 1 个子读数就让它失败 */
      f.run(100);
      checkEq(f.log.count(), 1, "失败也算一笔 (ok=false), 不是不记");
      checkEq(f.log.stats().n, 0, "没读回来的不算进统计");
      check(!f.log.samples().last().ok, "这一笔是 ok=false");

      MeterRig g(20);
      g.log.setAverage(4);
      g.log.setInterval(200);
      g.log.start(200, &e);
      g.step();                            /* 发第 1 个子读数 */
      g.step();                            /* 回来了, 攒进 1 个 (还差 3 个) */
      checkEq(g.log.count(), 0, "没凑够 N 个, 一笔都不记");
      g.meter.failNext();                  /* 第 2 个让它失败 */
      g.step();                            /* 发第 2 个 */
      g.step();                            /* 失败回来了 */
      checkEq(g.log.count(), 1, "攒到一半失败 → 这一笔作废 (ok=false)");
      checkEq(g.log.stats().n, 0, "攒着的那半份一起丢掉, 没有混进统计");
      check(!g.log.samples().last().ok, "这一笔是 ok=false");
      checkEq(g.meter.overlaps(), 0, "全程没有重叠请求");
   }

   caseBegin("meterlog: 文件头那几行 —— 是谁采的 / 什么单位 / 什么配置");
   {
      /* 一个"单位认不出来"的源: 真机判不出来时就是空 (powermeter.h 的 unit()) */
      struct Unitless : FakeMeter
      {
         QString unit() const override { return QString(); }
         QStringList configLines() const override
         {
            return QStringList{} << QStringLiteral("meter_mode=Unknown")
                                 << QStringLiteral("meter_wavelength=1064");
         }
      };

      Unitless src;
      src.open(nullptr);

      const QStringList ml = meterMetaLines(&src);
      checkEq((long long)ml.size(), 4, "来源 + 单位 + 它自己报的那两行");
      check(ml[0] == QStringLiteral("meter_source=fake"), "来源那个键", ml[0].toStdString());
      check(ml[1] == QStringLiteral("meter_unit=unknown"),
            "认不出来就写 unknown —— 不许替它写一个 W", ml[1].toStdString());
      check(unitLabel(&src) == QStringLiteral("单位不明"), "界面上那一句",
            unitLabel(&src).toStdString());
      check(unitLabel(nullptr) == QStringLiteral("单位不明"), "没有源也是这一句");
      check((long long)meterMetaLines(nullptr).size() == 0, "没有源就一行都不写");

      /* 缺省单位是 W: 三个模拟源按定义就是 (CSV 那一列本来就叫 watts) */
      FakeMeter plain;
      check(meterMetaLines(&plain).at(1) == QStringLiteral("meter_unit=W"),
            "模拟源照旧是 W", meterMetaLines(&plain).at(1).toStdString());
      check(unitLabel(&plain) == QStringLiteral("W"), "unitLabel 就是那个字");

      QTemporaryDir dir;
      check(dir.isValid(), "temp dir");
      const QString csv = dir.filePath(QStringLiteral("meta.csv"));

      MeterRig r(20);
      r.log.setMeta(QStringList{} << QStringLiteral("meter_source=fake")
                                 << QStringLiteral("meter_unit=W")
                                 << QStringLiteral("meter_interval_ms=200")
                                 << QStringLiteral("meter_avg=1"));
      QString err;
      check(r.log.beginRecord(csv, &err), "beginRecord", err.toStdString());
      r.log.start(200, &err);
      r.run(600);
      r.log.stop();
      r.log.endRecord();

      QFile f(csv);
      check(f.open(QIODevice::ReadOnly | QIODevice::Text), "read it back");
      const QString text = QString::fromUtf8(f.readAll());
      f.close();

      const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
      check(lines.first() == QStringLiteral("# meter_source=fake"),
            "第一行就是 meta (在表头之前)", lines.first().toStdString());
      check(text.contains(QStringLiteral("# meter_avg=1\n")), "每一行都带 `# `");
      checkEq((long long)lines.size(), 5 + r.log.count(), "行数 = 4 行 meta + 表头 + 样本");
      check(lines.at(4) == QStringLiteral("unix_ms,elapsed_ms,watts,ok"),
            "列表头照旧逐字没动, 单位改记在上面那几行里", lines.at(4).toStdString());

      /* 导出那份**也要**带: 它是个独立文件, 换个地方打开时上面那些字一个都不能少 */
      const QString dump = dir.filePath(QStringLiteral("meta_dump.csv"));
      check(r.log.saveBuffer(dump, &err), "saveBuffer", err.toStdString());
      QFile d(dump);
      check(d.open(QIODevice::ReadOnly | QIODevice::Text), "read the dump");
      check(QString::fromUtf8(d.readAll()).contains(QStringLiteral("# meter_unit=W\n")),
            "导出那份头上也有");
   }

   caseBegin("meterlog: 统计 —— ok 的那些才算, 标准差是样本标准差");
   {
      MeterRig r;
      /* 直接喂已知数 (跟随模式那条路), 统计是纯算术, 不掺时序 */
      for (double v : {1.0, 2.0, 3.0, 4.0})
         r.log.addFollowSample(1000 + (int64_t)v, v);

      const MeterLog::Stats s = r.log.stats();
      checkEq(s.n, 4, "n");
      checkNear(s.min, 1.0, "min");
      checkNear(s.max, 4.0, "max");
      checkNear(s.last, 4.0, "last");
      checkNear(s.mean, 2.5, "mean");
      /* 1,2,3,4 -> 样本方差 = (2.25+0.25+0.25+2.25)/3 = 5/3 */
      checkNear(s.sd, std::sqrt(5.0 / 3.0), "sample sd (除以 n-1)");

      /* 一个 ok=false 不许进统计, 也不许把 last 改掉 */
      MeterLog::Sample bad;
      bad.ms = 9999;
      bad.ok = false;
      r.log.addFollowSample(bad.ms, 0.0);      /* addFollowSample 一律算 ok, 这里另走超时那条路 */
      const MeterLog::Stats s2 = r.log.stats();
      checkEq(s2.n, 5, "跟随点全都算 ok (addFollowSample 的语义)");

      /* 单笔的 sd 是 0, 不是 NaN —— NaN 会一路糊到界面上 */
      MeterLog empty;
      checkNear(empty.stats().sd, 0.0, "没有样本时 sd = 0");
      checkEq(empty.stats().n, 0, "没有样本时 n = 0");
   }

   caseBegin("meterlog: 看门狗 —— 超时记一笔 ok=false, 并且停在那儿等");
   {
      /* 往返长过一个数量级: 永远回不来 */
      MeterRig r(60000);
      r.log.setInterval(200);

      QString err;
      int     failed_n = 0;
      QObject::connect(&r.log, &MeterLog::failed, [&](const QString &) { failed_n++; });

      r.log.start(200, &err);
      r.run(4000);        /* 跨过 kTimeoutMs */

      checkEq(r.log.count(), 1, "一笔: 那一个超时的空档");
      check(!r.log.samples().isEmpty() && !r.log.samples().first().ok,
            "那一笔记成 ok=false (与扫描 CSV 的 ok 列同口径)");
      checkEq(failed_n, 1, "报了一次 —— 不是每拍都喊");
      check(r.log.timedOut(), "状态是'卡住了'");
      checkEq(r.meter.requests(), 1, "没有再发第二个请求: 那一个还在源手上");
      checkEq(r.meter.overlaps(), 0, "所以也不会有两个未决请求同时压着");

      r.run(4000);
      checkEq(r.log.count(), 1, "还是那一笔: 采集真的停着, 不是继续往前冲");
      checkEq(r.meter.requests(), 1, "仍然没有第二个请求 (跑了 8 s 也没有)");

      /* 迟到的那一份回来了: 收下它, 并且重新走起来 */
      r.meter.setLatency(1);
      r.meter.setNow(r.now);
      r.meter.pump(r.now + 100000);
      checkEq(r.log.count(), 2, "迟到的回话收下了 —— 那是个真读数");
      check(r.log.samples().last().ok, "记成 ok");
      check(!r.log.timedOut(), "卡住的状态解开了");
      checkEq(r.meter.overlaps(), 0, "全程没有两个未决请求同时存在");

      r.run(1000);
      check(r.log.count() > 2, "之后照常续采");

      /* 「停止」是另一条出路: 卡住时按停止, 再开始就能重新发 */
      MeterRig w(60000);
      w.log.setInterval(200);
      w.log.start(200, &err);
      w.run(4000);
      check(w.log.timedOut(), "第二个台架也卡住了");
      w.log.stop();
      check(!w.log.timedOut() && !w.log.pending(), "「停止」解开了那个未决请求");
      w.meter.setLatency(1);
      w.log.start(200, &err);
      w.meter.setNow(w.now);
      w.run(600);
      check(w.log.count() > 1, "重新「开始」之后又能采了");
   }

   caseBegin("meterlog: setHold —— 停发, 放开之后从当时重排 (不补采欠下的)");
   {
      MeterRig r(20);
      r.log.setInterval(200);
      QString e;
      r.log.start(200, &e);

      r.run(600);
      const int before = r.log.count();
      /* 数"发出去几个"而不是"收回来几个": 让位那一刻可能正好有一个在飞, 它的回话
       * 到了也不算违规 —— 违规的是**又发**一个 */
      const int req_before = r.meter.requests();
      check(before >= 2, "先采到几个数");

      r.log.setHold(true);
      check(r.log.running(), "hold 期间 running() 照旧是 true (还在采, 只是让位)");
      check(!r.log.issuing(), "issuing() 才是'真的在发' —— 扫描那条闸看的是它");
      r.run(2000);
      checkEq(r.meter.requests(), req_before, "hold 期间一个请求都不发");
      check(r.log.count() <= before + 1, "计数也不再涨 (最多是让位那一刻已经在飞的那一个)");

      const int held_ms = r.log.count();
      r.log.setHold(false);
      r.step();
      checkEq(r.log.count(), held_ms, "放开之后第一拍就发, 不是又空等一个间隔");

      r.run(2000);
      /* 不补采: 2000ms 的 hold 里"欠下"的十个间隔, 放开之后只按正常节奏走 */
      check(r.log.count() - held_ms <= 12,
            "欠下的那些没有被补采回来 (补出来的是编的)");
      checkEq(r.meter.overlaps(), 0, "全程没有重叠请求");
   }

   caseBegin("meterlog: CSV —— 表头逐字固定, 行数与样本数一致, 追加不覆盖");
   {
      QTemporaryDir dir;
      check(dir.isValid(), "temp dir");
      const QString csv = dir.filePath(QStringLiteral("m.csv"));

      /* 行格式逐字比 —— 列宽与顺序是这个文件对外的全部约定 */
      MeterLog::Sample s;
      s.ms = 1234;
      s.watts = 1.5;
      s.ok = true;
      check(MeterLog::csvHeaderLine() == QStringLiteral("unix_ms,elapsed_ms,watts,ok\n"),
            "表头逐字固定");
      check(MeterLog::csvRowLine(s, 1000) == QStringLiteral("1234,234,1.5,1\n"),
            "一行 = unix_ms,elapsed_ms,watts,ok", MeterLog::csvRowLine(s, 1000).toStdString());

      MeterLog::Sample bad;
      bad.ms = 1234;
      bad.ok = false;
      check(MeterLog::csvRowLine(bad, 1000) == QStringLiteral("1234,234,,0\n"),
            "没读到的那些 watts 列是空的, ok=0 (不是 0 W)",
            MeterLog::csvRowLine(bad, 1000).toStdString());

      MeterRig r(20);
      r.log.setInterval(200);
      QString err;
      check(r.log.beginRecord(csv, &err), "beginRecord", err.toStdString());
      check(r.log.recording(), "recording()");

      r.log.start(200, &err);
      r.run(1200);
      const int n1 = r.log.count();
      check(n1 >= 3, "采到几个数");
      checkEq(r.log.written(), n1, "每一笔都落盘了");
      r.log.stop();
      r.log.endRecord();
      check(!r.log.recording(), "endRecord 之后不再写");

      {
         QFile f(csv);
         check(f.open(QIODevice::ReadOnly | QIODevice::Text), "read it back");
         const QString text = QString::fromUtf8(f.readAll());
         const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
         checkEq(lines.size(), n1 + 1, "行数 = 表头 + 样本数");
         check(lines.first() == QStringLiteral("unix_ms,elapsed_ms,watts,ok"),
               "第一行是表头", lines.first().toStdString());
         /* 表头只许出现一次 */
         check(text.count(QStringLiteral("unix_ms")) == 1, "表头只有一个");
      }

      /* 同一个路径再按一次「开始」: **接着写**, 不覆盖 (覆盖是没法撤销的) */
      check(r.log.beginRecord(csv, &err), "beginRecord again on the same path", err.toStdString());
      r.log.start(200, &err);
      r.run(600);
      r.log.stop();
      r.log.endRecord();

      {
         QFile f(csv);
         check(f.open(QIODevice::ReadOnly | QIODevice::Text), "read it back again");
         const QString text = QString::fromUtf8(f.readAll());
         const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
         check(text.count(QStringLiteral("unix_ms")) == 1, "第二段没有再写一个表头");
         checkEq(lines.size(), n1 + 1 + r.log.count() - n1, "第二段的数接着写在同一份后面");
         check(lines.size() > n1 + 1, "文件确实变长了, 而不是被截断重来");
      }

      /* 写不进去: 父目录是个文件 -> false + 原因, 且不抛 */
      const QString blocker = dir.filePath(QStringLiteral("blocker"));
      {
         QFile b(blocker);
         check(b.open(QIODevice::WriteOnly), "make a file to block the path");
         b.write("x");
      }
      QString why;
      check(!r.log.beginRecord(blocker + QStringLiteral("/no.csv"), &why),
            "路径写不进去 → beginRecord 返回 false");
      check(!why.isEmpty(), "并且给了一句原因", why.toStdString());
      check(!r.log.recording(), "失败之后没有半开的文件");

      /* saveBuffer: 整份导出, 不动正在记录的那份文件 */
      check(r.log.beginRecord(csv, &err), "record into csv again");
      const QString dump = dir.filePath(QStringLiteral("dump.csv"));
      check(r.log.saveBuffer(dump, &err), "saveBuffer", err.toStdString());
      check(r.log.recording(), "saveBuffer 没有把正在记录的那份关掉");

      QFile d(dump);
      check(d.open(QIODevice::ReadOnly | QIODevice::Text), "read the dump");
      const QStringList dl = QString::fromUtf8(d.readAll()).split(QLatin1Char('\n'),
                                                                Qt::SkipEmptyParts);
      checkEq(dl.size(), r.log.count() + 1, "导出 = 表头 + 缓冲里全部的点");
   }

   caseBegin("meterlog: 源没打开 / 没给文件名 —— 都是 false + 原因, 不是半开的状态");
   {
      MeterLog log;
      FakeMeter shut;                    /* 没 open() */

      log.setSource(&shut);
      QString err;
      check(!log.start(200, &err), "源没打开 → start 拒绝");
      check(!err.isEmpty(), "给了原因", err.toStdString());
      check(!log.running(), "没有半开着");

      /* 一个请求都没发出去 —— 拒绝必须是"什么都没干", 不是"发了一半才发现" */
      checkEq(shut.readings(), 0, "被拒绝时一个请求都没发");

      check(!log.beginRecord(QString(), &err), "空路径 → false");
      check(!err.isEmpty(), "也给了原因", err.toStdString());
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
   test_editgate();
   test_advprefs();
   test_faultreset();
   test_limitsw();
   test_homing();
   test_meter_sources();
   test_meter_meta();
   test_meterlog();
   test_ophir();

   std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
   return g_fail == 0 ? 0 : 1;
}
