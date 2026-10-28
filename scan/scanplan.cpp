#include "scanplan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace scan {

const char *modeText(ScanMode m)
{
   switch (m)
   {
   case ScanMode::Serpentine:   return "逐行往返";
   case ScanMode::SameDir:      return "每行同向";
   case ScanMode::RandomOnce:   return "随机 (不重复)";
   case ScanMode::RandomRepeat: return "随机 (可重复)";
   }
   return "逐行往返";      /* 到不了: 上面四支盖满了 enum。兜底只为 -Wreturn-type */
}

ScanMode modeFromIndex(int i)
{
   if (i < 0 || i >= kScanModeCount)
      return ScanMode::Serpentine;
   return (ScanMode)i;
}

bool modeRandom(ScanMode m)
{
   return m == ScanMode::RandomOnce || m == ScanMode::RandomRepeat;
}

bool modeEndless(ScanMode m)
{
   return m == ScanMode::RandomRepeat;
}

bool onFly(const Params &p)
{
   /* 随机走法没有"行", 连续不了 (见头文件)。dwell 的下限是 0 (validate 允许),
    * 于是 0 这个值天然可以当开关用, 不另加字段 */
   return !modeRandom(p.mode) && p.dwell_ms == 0;
}

/* 同 hmi/ecatworker.h 的 HMI_VEL_MIN / HMI_VEL_MAX, 照抄一份以保持本文件不依赖 Qt。
 * 两边必须一致: EcatThread::setSpeed 会夹速度, 而估算是照原值算的。 */
static const uint32_t VEL_MIN = 1000;
static const uint32_t VEL_MAX = 100000;

static uint32_t clampVel(uint32_t v)
{
   if (v < VEL_MIN) return VEL_MIN;
   if (v > VEL_MAX) return VEL_MAX;
   return v;
}

int32_t pulseOf(double unit, double pulses_per_unit)
{
   return (int32_t)std::llround(unit * pulses_per_unit);
}

double unitOf(int32_t pul, double pulses_per_unit)
{
   if (pulses_per_unit <= 0.0)
      return 0.0;
   return (double)pul / pulses_per_unit;
}

double resX(const Params &p)
{
   if (p.nx < 2 || !(p.area_x_unit > 0.0) || !std::isfinite(p.area_x_unit))
      return 0.0;
   return p.area_x_unit / (double)(p.nx - 1);
}

double resY(const Params &p)
{
   if (p.ny < 2 || !(p.area_y_unit > 0.0) || !std::isfinite(p.area_y_unit))
      return 0.0;
   return p.area_y_unit / (double)(p.ny - 1);
}

int axisCount(double area_unit, double res_unit)
{
   if (!(area_unit > 0.0) || !(res_unit > 0.0))
      return 0;
   if (!std::isfinite(area_unit) || !std::isfinite(res_unit))
      return 0;

   const double q = area_unit / res_unit;
   if (q > 1.0e9)      /* 折算出来的点数早已超上限, 让它走"超出范围"那条路 */
      return 0;

   /* 容差 (2026-10-27): 旧 CSV / 旧 ini 里的 res 是 6 位小数的十进制字面量, 反解回 double
    * 可能差一两个 ulp (0.3/0.1 在 double 里是 2.9999999999999996)。不补这一下, floor 就少
    * 一个点, 于是"刚对齐完的旧文件"换台机器又对不上了。1e-9 相对量级, 只吃掉舍入误差。 */
   const double eps = 1e-9 * std::max(1.0, q);
   return (int)std::floor(q + eps) + 1;
}

std::vector<double> axisCoords(double area_unit, double res_unit)
{
   std::vector<double> out;
   int n = axisCount(area_unit, res_unit);
   if (n <= 0)
      return out;

   /* 居中: area/res 除不尽时 span < area, 网格整个落在区域内 (两端各留半格) */
   double span = (double)(n - 1) * res_unit;
   out.reserve((size_t)n);
   for (int i = 0; i < n; i++)
      out.push_back(-span / 2.0 + (double)i * res_unit);
   return out;
}

std::vector<double> axisCoordsByCount(double span_unit, int n)
{
   std::vector<double> out;
   if (n < 2 || !(span_unit > 0.0) || !std::isfinite(span_unit))
      return out;

   /* 两端都取 (2026-10-27 与用户确认的语义): 第 0 个点在 -span/2, 第 n-1 个在 +span/2。
    * 首尾**直接赋值**而不是靠 i*step 累加出来 —— 累乘的最后一个点可能差一个 ulp, 而自检
    * 钉的是"首尾正好落在区域边界上", 差一个 ulp 就会让那条断言时灵时不灵。 */
   const double step = span_unit / (double)(n - 1);
   const double half = span_unit / 2.0;

   out.reserve((size_t)n);
   for (int i = 0; i < n; i++)
      out.push_back(-half + step * (double)i);

   out.front() = -half;
   out.back()  =  half;
   return out;
}

std::vector<Point> buildPlan(const Params &p)
{
   std::vector<Point> out;

   /* 公开函数自己也要拦 (2026-10-27): 点数如今是入参, 一个没走 validate 的调用方
    * (自检、将来的别的入口) 塞进 1 点或一亿点都会在这里除零 / 吃掉几个 GB。 */
   if (p.nx < 2 || p.ny < 2)
      return out;
   if ((long long)p.nx * (long long)p.ny > kMaxPlanPoints)
      return out;

   std::vector<double> xs = axisCoordsByCount(p.area_x_unit, p.nx);
   std::vector<double> ys = axisCoordsByCount(p.area_y_unit, p.ny);
   if ((int)xs.size() != p.nx || (int)ys.size() != p.ny)
      return out;

   int nx = p.nx;
   int ny = p.ny;
   out.reserve((size_t)nx * (size_t)ny);

   for (int iy = 0; iy < ny; iy++)
   {
      /* 蛇形: 奇数行反向。别的走法都是每行从头走 —— 随机那两种在这里出的只是**基准网格**,
       * 顺序随即便被 shufflePlan 洗掉, 所以走哪一套都无所谓, 用"每行同向"这套省一个分支。 */
      bool fwd = true;
      if (p.mode == ScanMode::Serpentine)
         fwd = ((iy % 2) == 0);
      if (!p.start_positive)
         fwd = !fwd;

      for (int k = 0; k < nx; k++)
      {
         int ix = fwd ? k : (nx - 1 - k);

         Point pt;
         pt.ix = ix;
         pt.iy = iy;
         pt.x_unit = xs[(size_t)ix];
         pt.y_unit = ys[(size_t)iy];
         pt.x_pul  = pulseOf(pt.x_unit, p.pulses_per_unit);
         pt.y_pul  = pulseOf(pt.y_unit, p.pulses_per_unit);
         out.push_back(pt);
      }
   }

   return out;
}

void shufflePlan(std::vector<Point> *pts, uint32_t seed)
{
   if (pts == nullptr || pts->size() < 2)
      return;
   std::mt19937 rng(seed);
   std::shuffle(pts->begin(), pts->end(), rng);
}

int32_t autoRangePul(const Params &p)
{
   double half = std::max(p.area_x_unit, p.area_y_unit) / 2.0;
   /* 两条下限, 取大的那个:
    *   half + 1.0        —— 区域外留 1 单位余量, 便于在区域外手动对位 (原来唯一的那条);
    *   kCanvasHalfUnits  —— **画布半宽** (2026-09-22 加)。少了它, 在面板边缘点一下会被
    *                        setTarget / interpolate 夹回来, 而界面上完全看不出这件事。
    * ceil 保证结果 ≥ llround(kCanvasHalfUnits * 脉冲当量) —— 正是点击那一路传进去的数。 */
   double rng_u = std::max(half + 1.0, kCanvasHalfUnits);
   double pul   = rng_u * p.pulses_per_unit;
   if (pul < 1.0)
      return 1;
   if (pul > 2000000000.0)
      return 2000000000;
   return (int32_t)std::ceil(pul);
}

int32_t posTolPul(const Params &p)
{
   /* 一根标量容差要同时守住两根轴 (2026-10-27), 取**较小**的那个分辨率:
    * 容差比细轴的一个步长还大, 就等于"还没走到位也算到了"。 */
   double step = std::min(resX(p), resY(p)) * p.pulses_per_unit;
   if (step <= 0.0)
      return 50;

   double tol = step / 20.0;
   if (tol < 50.0)          tol = 50.0;       /* EM_POS_TOL_DEF, 也是噪声底 */
   if (tol > step / 4.0)    tol = step / 4.0; /* 再大就不叫"到位"了 */
   if (tol < 1.0)           tol = 1.0;
   return (int32_t)std::ceil(tol);
}

bool fitsRange(const Params &p, std::string *why)
{
   int32_t rng = (p.range_pul > 0) ? p.range_pul : autoRangePul(p);

   /* 网格上离原点最远的那个点, 两轴各算一次 (按点数生成的坐标: 首尾就在 ±区域/2) */
   std::vector<double> xs = axisCoordsByCount(p.area_x_unit, p.nx);
   std::vector<double> ys = axisCoordsByCount(p.area_y_unit, p.ny);
   if (xs.empty() || ys.empty())
      return true;      /* 网格本身为空, 由 validate 去报 */

   double far_x = std::max(std::fabs(xs.front()), std::fabs(xs.back())) * p.pulses_per_unit;
   double far_y = std::max(std::fabs(ys.front()), std::fabs(ys.back())) * p.pulses_per_unit;

   if (far_x > (double)rng || far_y > (double)rng)
   {
      if (why != nullptr)
      {
         char buf[320];
         std::snprintf(buf, sizeof(buf),
            "区域超出量程: 最远点 X=%lld / Y=%lld pul, 量程只有 ±%lld。"
            "超出部分会被静默夹掉, 那几条边采不到。"
            "请把区域改小。",
            (long long)pulseOf(std::max(std::fabs(xs.front()), std::fabs(xs.back())), p.pulses_per_unit),
            (long long)pulseOf(std::max(std::fabs(ys.front()), std::fabs(ys.back())), p.pulses_per_unit),
            (long long)rng);
         *why = buf;
      }
      return false;
   }
   return true;
}

std::string validate(const Params &p)
{
   if (!(p.area_x_unit > 0.0) || !(p.area_y_unit > 0.0))
      return "区域大小必须是正数";
   if (!std::isfinite(p.area_x_unit) || !std::isfinite(p.area_y_unit))
      return "区域大小必须是正数";
   if (!(p.pulses_per_unit > 0.0))
      return "「每 mm 脉冲数」必须是正数";

   /* 点数 (2026-10-27): 分辨率由它和区域推出, 所以这里判的是点数。
    * 下限 2 —— 1 点网格没有间距可言 (resX/resY 也除不了 n-1)。 */
   if (p.nx < kGeomCountMin || p.ny < kGeomCountMin)
      return "点数至少 2 个, 否则网格没有间距。请调大点数。";

   /* 上限常量在 scanplan.h (kMaxPlanPoints), rebuildPlan() 拦的是同一个。
    * 单轴超过 kGeomCountMax 时另一根轴至少 2 点, 总数必然超限 —— 一并在这里拦。 */
   long long total = (long long)p.nx * (long long)p.ny;
   if (p.nx > kGeomCountMax || p.ny > kGeomCountMax || total > kMaxPlanPoints)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "点数 %lld 超出上限 %lld。请减少点数 X 或点数 Y。",
                    total, kMaxPlanPoints);
      return buf;
   }

   /* 派生的分辨率下限: 再细一步就落到位置噪声底 (0.001 mm × 50000 = 50 pul)。
    * 点数量程本身已经挡住了绝大多数情况, 这一句守的是"区域被改得极小"的那种参数。 */
   if (resX(p) < kGeomResMin * (1.0 - 1e-9) || resY(p) < kGeomResMin * (1.0 - 1e-9))
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "分辨率小于下限 %g mm。请减少点数 X 或点数 Y。",
                    kGeomResMin);
      return buf;
   }

   if (p.dwell_ms < 0 || p.dwell_ms > 60000)
      return "单点停留时间应在 0..60000 ms";
   if (p.settle_ms < 0 || p.settle_ms > 10000)
      return "稳定窗口应在 0..10000 ms";
   if (p.samples_per_point < 1 || p.samples_per_point > 100)
      return "每点采样次数应在 1..100";
   if (p.meter_timeout_ms < 100 || p.meter_timeout_ms > 60000)
      return "取样源超时应在 100..60000 ms";
   /* 边界用常量拼, 不再硬写数 —— 它必须与 MeterLog 那个夹取、与界面上那个旋钮的量程
    * **同源**: prefsMergeParams 是"validate 一失败就整份 ini 都不听", 三处漂一格就会
    * 出现"控件夹得住、这里卡得住"的假拒绝 */
   if (p.meter_interval_ms < kMeterIntervalMinMs || p.meter_interval_ms > kMeterIntervalMaxMs)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "连续读数间隔应在 %d..%d ms 之间。",
                    kMeterIntervalMinMs, kMeterIntervalMaxMs);
      return buf;
   }

   if (p.speed_pul_s < VEL_MIN || p.speed_pul_s > VEL_MAX)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "速度应在 %u..%u pul/s 之间。",
                    VEL_MIN, VEL_MAX);
      return buf;
   }

   /* 连续行进那条闸**不在这儿** (2026-10-29): 它说的是"这一趟能不能连续走", 而 validate()
    * 的调用方里有一半不跑扫描 (打开 CSV / 存 ini)。见 continuousRunError 的注释。 */

   std::string why;
   if (!fitsRange(p, &why))
      return why;

   return std::string();
}

std::string continuousRunError(const Params &p)
{
   /* 只有连续行进那一趟才管这条; 逐点停 (停留 > 0, 或随机那两种) 一格在中心停着采,
    * 采样间距与格宽没有关系 */
   if (!onFly(p))
      return std::string();

   /* 连续行进时每格的值是"经过格中心时最近的一笔采样", 而采样是按「间隔」一笔笔到的 ——
    * 一格在中心附近停留多久由速度决定:
    * 采样间距 (脉冲) = 速度 × (间隔 + 往返) / 1000, 它必须 ≤ 格宽 (resX)。
    * 超过格宽时会有格子**一笔都采不到**, 而那些格子在 CSV 里与采到的行长得一样 (ok=0),
    * 起扫前拦掉比走完再解释便宜。
    *
    * `+ 50` 是往返的占位, 与 estimatePerPointMs 里那笔 `interval + 50` 同一个口径
    * (MeterLog 是"回包之后再排下一拍", 真实周期 = 间隔 + 往返)。 */
   const double spacing_mm = (double)p.speed_pul_s * (double)(p.meter_interval_ms + 50)
                           / 1000.0 / p.pulses_per_unit;
   if (spacing_mm > resX(p) + 1e-9)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "连续行进时采样间距 %.2f mm 超过分辨率 %.2f mm, "
                    "请降低扫描速度或调小「间隔」。",
                    spacing_mm, resX(p));
      return buf;
   }

   return std::string();
}

int64_t estimatePerPointMs(const Params &p)
{
   uint32_t vel = clampVel(p.speed_pul_s ? p.speed_pul_s : 1);

   /* 连续行进 (2026-10-29, §54): 一个"点"的长短不再是"一步 + 停 + 读", 而是**一行的平均**:
    * 一行 = 行内移动 + 一次换行 (蛇形) 或整行返程 (同向) + 行首/行末各等一笔采样。
    * 除回 nx 就是每点的估计。600 是行首加速 + 行末减速的余量; 2×(间隔+50) 是行首、行末
    * 各等一笔 (往返占位与下面那句同一个口径)。口径照旧"宁可高估"。 */
   if (onFly(p))
   {
      const double rx = resX(p) * p.pulses_per_unit;
      const double ry = resY(p) * p.pulses_per_unit;

      double row_pul = rx * (double)(p.nx - 1) + ry;
      if (p.mode == ScanMode::SameDir)
         row_pul += rx * (double)(p.nx - 1);      /* 每行同向: 还要整行走回去 */

      double row_ms = row_pul / (double)vel * 1000.0
                    + (double)p.settle_ms + 600.0
                    + 2.0 * (double)(p.meter_interval_ms + 50);

      double per = row_ms / (double)std::max(1, p.nx);
      if (per < 1.0)
         per = 1.0;
      return (int64_t)per;
   }

   /* "移动"这一项走多少距离, 看走法。
    *
    * 一行一行走的那两种: 每一步都只动一根轴、都走一个 res —— 是常数 (见 §4 那段"蛇形里
    * 每一步只动一根轴")。
    *
    * 随机那两种: 每一步都是**两个随机点之间的一跳**, 不再是 res。取区域里两个均匀随机点的
    * 平均距离 —— 边长 S 的正方形上那个数是 0.5214·S (闭式解), 长方形上没有这么简洁的式子,
    * 就照正方形那个系数取 max(区域X, 区域Y)。**拉长的区域上这一项偏大**, 那是故意的:
    * 本函数的口径一直是"宁可高估", 而随机那一跳还常常是两轴同时走的斜线 (进近减速比单轴
    * 走更久), 偏大方向正好。 */
   /* 行扫描: 每步只动一根轴, 走一个步长。两轴分辨率可能不同 (2026-10-27), 取**较大**的
    * 那个 —— 口径照旧"宁可高估"。 */
   double step_unit = std::max(resX(p), resY(p));
   if (modeRandom(p.mode))
      step_unit = 0.5214 * std::max(p.area_x_unit, p.area_y_unit);

   double step_pul = step_unit * p.pulses_per_unit;
   double move_ms = step_pul / (double)vel * 1000.0;

   /* 进近段要减速, 实际比 step/vel 长。按 30% 粗加一笔, 宁可高估 */
   double per = move_ms * 1.30 + (double)p.settle_ms + (double)p.dwell_ms;

   /* 采样本身。**2026-09-29 起这个点的值不是背靠背连要 N 次了**: 它是"读取时间段里到齐的
    * 连续采样的平均", 而连续采样按 meter_interval_ms 的节奏到齐 —— 于是一个采样最少要等
    * 一个间隔, 再加上自己那一次往返 (meterlog 的 `m_due_ms = m_now_ms + m_interval` 是
    * 间隔之后再排下一拍, 往返叠在里面; 这里那个 50 是那次往返的占位, 与"模拟源是 0,
    * 真机未知"同一条口径)。**不这么改的话「预计全程」会少报好几倍。**
    * 口径照旧是"宁可高估": 每点的第一笔采样其实平均只等半个间隔。 */
   per += (double)(p.meter_interval_ms + 50) * (double)std::max(1, p.samples_per_point);

   if (per < 1.0)
      per = 1.0;
   return (int64_t)per;
}

std::string csvColumnHeader()
{
   /* `time_local` 是 2026-09-30 加在人看得懂那一头的一列 (用户原话: 「在记录每个点的时候
    * 记录记录的时间」) —— unix_ms 与 elapsed_ms 都在, 但读文件的人要的是"这一点几点几分
    * 采的"。
    *
    * **只许追加在末尾**: csvLoadGrid 是**按列名找** ix/iy/watts/ok 的, 那套"找不到就退回
    * 默认下标 1/2/7/8"的兜底只在**没有列名行**的老文件上才用到 —— 末尾多一列, 它连一个字
    * 都不用改; csvParseForResume 更只看前三个字段。改这一行的时候别动前面任何一列的位置。 */
   return "index,ix,iy,x_unit,y_unit,x_pul,y_pul,watts,ok,flags,"
          "pos_x_pul,pos_y_pul,spread_x_pul,spread_y_pul,unix_ms,elapsed_ms,time_local";
}

std::string csvMetaLines(const Params &p, const std::string &started_iso, int zero_epoch)
{
   /* 开头这几行 `#` 注释带全部参数, 续扫靠它做兼容性判定: 决定物理网格的那几项一个都不能少。
    *
    * 2026-10-27 起几何的**权威字段是 nx/ny** (整数, 精确); res_x_unit / res_y_unit 只供人读,
    * 解析一律以点数为准。旧文件里那个单一的 res_unit 仍然认 (见 csvParseForResume 的折算),
    * 于是新旧两边的文件都能续扫。 */
   char buf[1024];
   std::string s = "# scan v1\n";
   s += "# area_x_unit=" + std::to_string(p.area_x_unit);
   s += " area_y_unit="   + std::to_string(p.area_y_unit);
   s += " nx="            + std::to_string(p.nx);
   s += " ny="            + std::to_string(p.ny);
   s += " pulses_per_unit=" + std::to_string(p.pulses_per_unit);
   s += "\n";
   s += "# res_x_unit=" + std::to_string(resX(p));
   s += " res_y_unit="  + std::to_string(resY(p));
   s += "\n";

   /* `mode=%d` 是 2026-10-11 从 `serp=%d` 改的 (0..3, 见 ScanMode)。**没有任何代码解析它**
    * —— 续扫只比几何那几项 (csvParseForResume), 所以这不是格式破坏, 只是这一栏记得更全:
    * 一行一行走和随机走, 出来的数据不是一回事, 读文件的人该看得出来 (docs/scan_sweep.md §8)。 */
   std::snprintf(buf, sizeof(buf),
                 "# speed_pul_s=%u dwell_ms=%d settle_ms=%d samples=%d mode=%d start_pos=%d\n",
                 (unsigned)(p.speed_pul_s ? p.speed_pul_s : 0),
                 p.dwell_ms, p.settle_ms, p.samples_per_point,
                 (int)p.mode, p.start_positive ? 1 : 0);
   s += buf;

   std::snprintf(buf, sizeof(buf),
                 "# range_pul=%lld pos_tol_pul=%lld zero_epoch=%d\n",
                 (long long)(p.range_pul > 0 ? p.range_pul : autoRangePul(p)),
                 (long long)posTolPul(p), zero_epoch);
   s += buf;

   s += "# started=" + started_iso + "\n";
   return s;
}

std::string csvRowLine(const Row &r)
{
   /* buf 的余量按最坏情况量过: flags 最长那条中文串在 UTF-8 下约 60 字节, 其余 15 个字段
    * 合起来约 180 字节, 再加 time_local 的 19 —— 整行不到 260, 512 有近两倍余量。
    * **加列的时候顺手看一眼这里** (这是最容易忘的一处)。 */
   char buf[512];

   /* 数值用 C locale, 不用 QLocale: 某些区域设置会把小数点变成逗号 */
   char watts[64];
   if (r.ok)
      std::snprintf(watts, sizeof(watts), "%.9g", r.watts);
   else
      watts[0] = '\0';          /* 采不到就留空: 0 是一个合法的读数 */

   std::snprintf(buf, sizeof(buf),
                 "%d,%d,%d,%.9g,%.9g,%d,%d,%s,%d,%s,%d,%d,%d,%d,%lld,%lld,%s\n",
                 r.index, r.pt.ix, r.pt.iy,
                 r.pt.x_unit, r.pt.y_unit, r.pt.x_pul, r.pt.y_pul,
                 watts, r.ok ? 1 : 0, r.flags.c_str(),
                 r.pos_x_pul, r.pos_y_pul, r.spread_x_pul, r.spread_y_pul,
                 (long long)r.unix_ms, (long long)r.elapsed_ms,
                 r.time_local.c_str());

   return std::string(buf);
}

/* 在 "# a=1 b=2" 这类行里找 key=value。找到写 *out 返回 true。 */
static bool metaGet(const std::string &line, const char *key, double *out)
{
   std::string pat = std::string(key) + "=";
   size_t pos = line.find(pat);
   if (pos == std::string::npos)
      return false;

   const char *v = line.c_str() + pos + pat.size();
   char *end = nullptr;
   double d = std::strtod(v, &end);
   if (end == v)
      return false;
   *out = d;
   return true;
}

/* 切一行 CSV。空字段保留 (flags 可以是空串)。 */
static std::vector<std::string> splitCsv(const std::string &line)
{
   std::vector<std::string> out;
   std::string cur;
   for (char c : line)
   {
      if (c == ',')
      {
         out.push_back(cur);
         cur.clear();
      }
      else
      {
         cur.push_back(c);
      }
   }
   out.push_back(cur);
   return out;
}

static bool nearly(double a, double b)
{
   double d = std::fabs(a - b);
   return d <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

std::string csvParseForResume(const std::string &text, const Params &p,
                              std::vector<char> *done, int *max_index,
                              std::string *started_iso, int *zero_epoch)
{
   const int nx = p.nx;
   const int ny = p.ny;
   const bool grid_ok = (nx >= 2 && ny >= 2 && (long long)nx * (long long)ny <= kMaxPlanPoints);

   if (done != nullptr)
      done->assign(grid_ok ? (size_t)nx * (size_t)ny : 0, 0);
   if (max_index != nullptr)
      *max_index = -1;
   if (started_iso != nullptr)
      started_iso->clear();
   if (zero_epoch != nullptr)
      *zero_epoch = -1;

   double a_x = -1, a_y = -1, r_u = -1, ppu = -1;
   double n_xd = -1, n_yd = -1;       /* 新表头的整数点数, 读成 double 过一遍 metaGet */
   bool have_geom = false;

   /* 索引越界先记下, 等几何差异判完再报 (2026-10-27): 几何不同时数组本来就不对,
    * 先报"第 N 行索引超出"会把人引到错误的排查方向上去 (见函数末尾)。 */
   bool   bad_index  = false;
   size_t bad_lineno = 0;
   int    bad_ix = 0, bad_iy = 0;

   std::string line;
   size_t pos = 0;
   bool seen_data = false;
   size_t lineno = 0;

   while (pos <= text.size())
   {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos)
         line = text.substr(pos);
      else
         line = text.substr(pos, nl - pos);

      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      lineno++;

      if (line.empty())
      {
         if (nl == std::string::npos) break;
         pos = nl + 1;
         continue;
      }

      if (line[0] == '#')
      {
         double v = 0;
         if (metaGet(line, "area_x_unit", &v))      { a_x = v; have_geom = true; }
         if (metaGet(line, "area_y_unit", &v))      { a_y = v; have_geom = true; }
         if (metaGet(line, "nx", &v))               { n_xd = v; have_geom = true; }
         if (metaGet(line, "ny", &v))               { n_yd = v; have_geom = true; }
         if (metaGet(line, "res_unit", &v))         { r_u = v; have_geom = true; }
         if (metaGet(line, "pulses_per_unit", &v))  { ppu = v; have_geom = true; }
         if (zero_epoch != nullptr && metaGet(line, "zero_epoch", &v))
            *zero_epoch = (int)v;

         size_t sp = line.find("started=");
         if (started_iso != nullptr && sp != std::string::npos)
            *started_iso = line.substr(sp + 8);

         if (nl == std::string::npos) break;
         pos = nl + 1;
         continue;
      }

      if (!seen_data && line.compare(0, 5, "index") == 0)
      {
         if (nl == std::string::npos) break;
         pos = nl + 1;
         continue;
      }

      seen_data = true;
      std::vector<std::string> f = splitCsv(line);

      if (f.size() < 3)
      {
         /* 残行 (写到一半掉电) 只在最后一行能容忍; 中间出现就是真损坏 */
         if (nl == std::string::npos)
            break;
         char buf[160];
         std::snprintf(buf, sizeof(buf), "CSV 第 %zu 行字段不足 (%zu 个), 文件已损坏。",
                       lineno, f.size());
         return std::string(buf);
      }

      int idx = std::atoi(f[0].c_str());
      int ix  = std::atoi(f[1].c_str());
      int iy  = std::atoi(f[2].c_str());

      if (ix < 0 || iy < 0 || ix >= nx || iy >= ny || !grid_ok)
      {
         if (!bad_index)
         {
            bad_index  = true;
            bad_lineno = lineno;
            bad_ix = ix;
            bad_iy = iy;
         }
      }
      else
      {
         if (done != nullptr)
            (*done)[(size_t)iy * (size_t)nx + (size_t)ix] = 1;
         if (max_index != nullptr && idx > *max_index)
            *max_index = idx;
      }

      if (nl == std::string::npos) break;
      pos = nl + 1;
   }

   if (!have_geom || !std::isfinite(a_x) || !std::isfinite(a_y) || !std::isfinite(ppu))
      return "CSV 中没有几何参数 (表头不完整), 无法确认与当前参数是否为同一片区域。";
   (void)seen_data;

   /* 文件里的网格到底是几个点 (2026-10-27):
    *   有 nx 也有 ny   → 直接用 (整数, 精确);
    *   只有一个        → 表头不完整;
    *   都没有, 有旧 res → 按旧公式折算 (axisCount)。 */
   const bool have_counts = (n_xd >= 0.0 && n_yd >= 0.0);
   const bool have_legacy = (r_u > 0.0);
   if (!have_counts && !have_legacy)
      return "CSV 中没有几何参数 (表头不完整), 无法确认与当前参数是否为同一片区域。";

   int f_nx = -1, f_ny = -1;
   if (have_counts)
   {
      auto toCount = [](double d) -> int
      {
         if (!std::isfinite(d))
            return -1;
         const double r = std::floor(d + 0.5);
         if (std::fabs(d - r) > 1e-6)
            return -1;
         if (r < (double)kGeomCountMin || r > (double)kGeomCountMax)
            return -1;
         return (int)r;
      };
      f_nx = toCount(n_xd);
      f_ny = toCount(n_yd);
      if (f_nx < 0 || f_ny < 0)
         return "CSV 的点数超出可设范围, 文件已损坏。请另建文件。";
   }
   else
   {
      f_nx = axisCount(a_x, r_u);
      f_ny = axisCount(a_y, r_u);
      if (f_nx < kGeomCountMin || f_ny < kGeomCountMin)
         return "CSV 的分辨率折算不出至少 2 个点, 文件已损坏。请另建文件。";
   }

   /* **兼容判据按点数比, 不按文件里那个 res 比** (2026-10-27): 旧文件 area/res 除不尽时,
    * 折算出的点数与它表头里写的 res 推不回同一个值 —— 拿 res 直接比, 就会出现"刚按 CSV 对
    * 齐完, 再续扫却被自己拒掉"。这里两边都换成"由区域与点数算出的分辨率", 于是只剩整数
    * 的一致性, 显示给人看的仍是 mm, 标签沿用旧名 (docs 与自检钉着"分辨率")。 */
   auto resOfCount = [](double area, int n) -> double
   {
      if (n < 2 || !(area > 0.0))
         return 0.0;
      return area / (double)(n - 1);
   };

   std::string diff;
   auto noteDiff = [&diff](const char *name, double was, double now)
   {
      if (nearly(was, now))
         return;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s %g → %g; ", name, was, now);
      diff += buf;
   };
   noteDiff("区域X(mm)", a_x, p.area_x_unit);
   noteDiff("区域Y(mm)", a_y, p.area_y_unit);
   noteDiff("分辨率X (mm)", resOfCount(a_x, f_nx), resX(p));
   noteDiff("分辨率Y (mm)", resOfCount(a_y, f_ny), resY(p));
   noteDiff("每 mm 脉冲数", ppu, p.pulses_per_unit);

   if (!diff.empty())
   {
      diff = "CSV 的几何参数与当前设置不一致: " + diff +
             "。几何不一致无法续扫, 请把参数改回或另建文件";
      return diff;
   }

   /* 几何一致了才轮到索引: 到这一步 (ix,iy) 还越界, 就是文件本身被改坏了 */
   if (bad_index)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "CSV 第 %zu 行的网格索引 (%d, %d) 超出 %d×%d, 文件已损坏。",
                    bad_lineno, bad_ix, bad_iy, nx, ny);
      return std::string(buf);
   }

   return std::string();
}

std::string csvAlignParams(const std::string &text, const Params &cur, Params *out)
{
   /* 逐 `#` 行取几何。**区域 X/Y、每 mm 脉冲数、以及点数 (或旧的单一 res) 缺一不可**:
    * 本程序写出来的表头永远俱全 (见 csvMetaLines), 少一项只可能是手改或截断的文件 ——
    * 那种文件按哪一项去对齐都是猜, 不如说"表头不完整"。
    *
    * 2026-10-27 起点数优先: 有新表头 (nx/ny) 就用它; 只有旧表头 (res_unit) 时按旧公式折算。
    * 两个新键只出现一个也算不完整 —— 不说清是哪一根轴的文件, 对齐了也是猜。 */
   bool   have_area_x = false, have_area_y = false, have_ppu = false;
   bool   have_nx = false, have_ny = false, have_res = false;
   double area_x = 0.0, area_y = 0.0, ppu = 0.0;
   double n_xd = 0.0, n_yd = 0.0, res_legacy = 0.0;

   std::string line;
   size_t pos = 0;
   while (pos <= text.size())
   {
      const size_t nl = text.find('\n', pos);
      line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
      if (!line.empty() && line.back() == '\r')
         line.pop_back();

      if (!line.empty() && line[0] != '#')
         break;      /* 表头全在开头那一串 `#` 行里, 列名行一出现就不用再看了 */

      double d = 0;
      if (metaGet(line, "area_x_unit", &d))     { have_area_x = true; area_x = d; }
      if (metaGet(line, "area_y_unit", &d))     { have_area_y = true; area_y = d; }
      if (metaGet(line, "nx", &d))              { have_nx = true; n_xd = d; }
      if (metaGet(line, "ny", &d))              { have_ny = true; n_yd = d; }
      if (metaGet(line, "res_unit", &d))        { have_res = true; res_legacy = d; }
      if (metaGet(line, "pulses_per_unit", &d)) { have_ppu = true; ppu = d; }

      if (nl == std::string::npos)
         break;
      pos = nl + 1;
   }

   const bool counts_full   = (have_nx && have_ny);
   const bool counts_partial = (have_nx != have_ny);
   if (!have_area_x || !have_area_y || !have_ppu || counts_partial
       || (!counts_full && !have_res))
      return "CSV 中没有几何参数 (表头不完整), 无法确认与当前参数是否为同一片区域。";

   /* 区域与每 mm 脉冲数的量程, 名前缀与 csvParseForResume 那句诊断**同源**
    * (docs/scan_sweep.md §24.1 钉着那几个名字): 同一个东西在两条路上不许有两个叫法。 */
   struct Bound
   {
      const char *name;
      double      lo, hi;
   };
   static const Bound kBound[3] = {
      { "区域X(mm)",   kGeomAreaMin, kGeomAreaMax },
      { "区域Y(mm)",   kGeomAreaMin, kGeomAreaMax },
      { "每 mm 脉冲数", kGeomPpuMin,  kGeomPpuMax  },
   };
   const double v[3] = { area_x, area_y, ppu };

   for (int i = 0; i < 3; i++)
   {
      if (v[i] >= kBound[i].lo && v[i] <= kBound[i].hi)
         continue;

      /* 说清是哪一项、读到的是多少: setValue 会把它**静默夹小**, 夹完之后续扫那句
       * "几何不一致"就没有人看得懂了。用 %g 不用定点 —— 0.0001 那种值按 3 位小数印
       * 出来是 "0.000", 恰好把要说的那个数藏掉了 */
      char buf[240];
      std::snprintf(buf, sizeof(buf),
                    "CSV 的%s = %g, 超出面板可设的范围, 无法按它对齐。请另建文件。",
                    kBound[i].name, v[i]);
      return std::string(buf);
   }

   /* 点数 (2026-10-27): 界面那个 QSpinBox 会把超范围的值**静默夹小**, 所以这里自己先判
    * 一次 2..kGeomCountMax —— 夹过的值写进面板, 屏幕上与盘上就成了两份。 */
   int n_x = 0, n_y = 0;
   if (counts_full)
   {
      auto checked = [](double d, const char *axis, int *out, std::string *err) -> bool
      {
         const double r = std::isfinite(d) ? std::floor(d + 0.5) : -1.0;
         if (!std::isfinite(d) || std::fabs(d - r) > 1e-6
             || r < (double)kGeomCountMin || r > (double)kGeomCountMax)
         {
            char buf[240];
            std::snprintf(buf, sizeof(buf),
                          "CSV 的%s %g, 超出面板可设的范围, 无法按它对齐。请另建文件。",
                          axis, d);
            *err = buf;
            return false;
         }
         *out = (int)r;
         return true;
      };
      std::string err;
      if (!checked(n_xd, "点数X", &n_x, &err) || !checked(n_yd, "点数Y", &n_y, &err))
         return err;
   }
   else
   {
      /* 旧表头: 按旧公式折算。除不尽时旧网格居中留白、新网格两端顶满, 逐点最多差半格 ——
       * 由点数定义出的新网格没有"半格"这个概念, 这是兼容旧文件必须付的代价 (见 .h)。 */
      n_x = axisCount(area_x, res_legacy);
      n_y = axisCount(area_y, res_legacy);
      if (n_x < kGeomCountMin || n_y < kGeomCountMin || n_x > kGeomCountMax || n_y > kGeomCountMax)
      {
         char buf[240];
         std::snprintf(buf, sizeof(buf),
                       "CSV 的 res_unit = %g, 折算不出可用的点数, 无法按它对齐。请另建文件。",
                       res_legacy);
         return std::string(buf);
      }
   }

   /* 只改这五项 (区域 X/Y、点数 X/Y、每 mm 脉冲数), 别的字段原样 (= cur 的) ——
    * 于是这一份整份交给 validate() 也是对的 */
   Params q = cur;
   q.area_x_unit     = area_x;
   q.area_y_unit     = area_y;
   q.nx              = n_x;
   q.ny              = n_y;
   q.pulses_per_unit = ppu;

   /* 量程跟着新几何重算, 与 currentParams() 同一条规矩 ("量程永远按区域自动算")。
    * ★ **不重算会误判**: range_pul 是从**旧**几何算出来的数, 而它比新区域还小的场合
    * (例如当前 3×3 mm = 800000 pul 的量程, CSV 是 40×40 mm = 1000000 pul) 下面那句
    * validate() 会拿旧量程去量新区域, 判它"超出量程" —— 一个完全正常的 CSV 会被拒。 */
   q.range_pul = autoRangePul(q);

   /* 手改坏的几何 (分辨率大于区域 / 点数超上限) 在这里拦掉。不拦的话面板会停在一份
    * validate() 不认的参数上 —— 那一份连 prefsSave 都不采纳 (prefsMergeParams),
    * 盘上屏幕上就成了两份; 而且接着 resume() 报的那句是说给"参数"听的, 不是给 CSV 听的。
    * 不把 validate() 那句话原样搬出去: 它自带一句「请调小分辨率」, 而这里改不了 CSV。 */
   if (!validate(q).empty())
   {
      char buf[280];
      std::snprintf(buf, sizeof(buf),
                    "CSV 的几何参数不合法: 区域 %g × %g mm, 点数 %d × %d。请另建文件。",
                    area_x, area_y, n_x, n_y);
      return std::string(buf);
   }

   if (out != nullptr)
      *out = q;
   return std::string();
}

void csvLoadGrid(const std::string &text, const Params &p,
                 std::vector<char> *have, std::vector<double> *watts)
{
   const int nx = p.nx;
   const int ny = p.ny;
   /* 同 buildPlan: 这个函数也是"谁都能调"的, 参数不合法时先挡一道再分配 */
   if (nx < 2 || ny < 2 || (long long)nx * (long long)ny > kMaxPlanPoints)
   {
      if (have != nullptr)  have->clear();
      if (watts != nullptr) watts->clear();
      return;
   }
   const size_t n = (size_t)nx * (size_t)ny;

   if (have != nullptr)  have->assign(n, 0);
   if (watts != nullptr) watts->assign(n, 0.0);

   /* 万一没有列名行 (手改过的文件), 退回 csvColumnHeader() 里的位置 */
   int  c_ix = 1, c_iy = 2, c_w = 7, c_ok = 8;
   bool cols_seen = false;

   std::string line;
   size_t pos = 0;

   while (pos <= text.size())
   {
      const size_t nl = text.find('\n', pos);
      line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
      if (!line.empty() && line.back() == '\r')
         line.pop_back();

      if (!line.empty() && line[0] != '#')
      {
         if (!cols_seen && line.compare(0, 5, "index") == 0)
         {
            const std::vector<std::string> h = splitCsv(line);
            for (size_t k = 0; k < h.size(); k++)
            {
               if (h[k] == "ix")    c_ix = (int)k;
               if (h[k] == "iy")    c_iy = (int)k;
               if (h[k] == "watts") c_w  = (int)k;
               if (h[k] == "ok")    c_ok = (int)k;
            }
            cols_seen = true;
         }
         else
         {
            const std::vector<std::string> f = splitCsv(line);
            const int need = std::max(std::max(c_ix, c_iy), std::max(c_w, c_ok));

            if ((int)f.size() > need)
            {
               const int ix = std::atoi(f[(size_t)c_ix].c_str());
               const int iy = std::atoi(f[(size_t)c_iy].c_str());
               if (ix >= 0 && iy >= 0 && ix < nx && iy < ny)
               {
                  const size_t cell = (size_t)iy * (size_t)nx + (size_t)ix;
                  const bool ok = (std::atoi(f[(size_t)c_ok].c_str()) != 0);

                  /* 覆盖不取第一次: 重测追加的那一行才是图上要的 */
                  if (have != nullptr)  (*have)[cell] = ok ? 1 : 0;
                  if (watts != nullptr) (*watts)[cell] = ok ? std::atof(f[(size_t)c_w].c_str()) : 0.0;
               }
            }
         }
      }

      if (nl == std::string::npos)
         break;
      pos = nl + 1;
   }
}

bool sameGeom(const Params &a, const Params &b)
{
   return nearly(a.area_x_unit, b.area_x_unit)
       && nearly(a.area_y_unit, b.area_y_unit)
       && a.nx == b.nx
       && a.ny == b.ny
       && nearly(a.pulses_per_unit, b.pulses_per_unit);
}

}   /* namespace scan */
