#include "scanplan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace scan {

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

int axisCount(double area_unit, double res_unit)
{
   if (area_unit <= 0.0 || res_unit <= 0.0)
      return 0;
   return (int)std::floor(area_unit / res_unit) + 1;
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

std::vector<Point> buildPlan(const Params &p)
{
   std::vector<Point> out;

   std::vector<double> xs = axisCoords(p.area_x_unit, p.res_unit);
   std::vector<double> ys = axisCoords(p.area_y_unit, p.res_unit);
   if (xs.empty() || ys.empty())
      return out;

   int nx = (int)xs.size();
   int ny = (int)ys.size();
   out.reserve((size_t)nx * (size_t)ny);

   for (int iy = 0; iy < ny; iy++)
   {
      /* 蛇形: 奇数行反向。不蛇形就是每行都从头走。 */
      bool fwd = true;
      if (p.serpentine)
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
   double step = p.res_unit * p.pulses_per_unit;
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

   /* 网格上离原点最远的那个点, 两轴各算一次 */
   std::vector<double> xs = axisCoords(p.area_x_unit, p.res_unit);
   std::vector<double> ys = axisCoords(p.area_y_unit, p.res_unit);
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
   if (!(p.res_unit > 0.0))
      return "分辨率必须是正数";
   if (!(p.pulses_per_unit > 0.0))
      return "「每 mm 脉冲数」必须是正数";

   if (p.res_unit > p.area_x_unit || p.res_unit > p.area_y_unit)
      return "分辨率大于区域, 网格只剩一个点。请调小分辨率或放大区域。";

   int nx = axisCount(p.area_x_unit, p.res_unit);
   int ny = axisCount(p.area_y_unit, p.res_unit);
   if (nx < 1 || ny < 1)
      return "网格为空 (区域或分辨率不合法)";

   /* 上限常量在 scanplan.h (kMaxPlanPoints), rebuildPlan() 拦的是同一个 */
   long long total = (long long)nx * (long long)ny;
   if (total > kMaxPlanPoints)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "点数 %lld 超出上限 %lld。请确认分辨率是否少打一位。",
                    total, kMaxPlanPoints);
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

   if (p.speed_pul_s < VEL_MIN || p.speed_pul_s > VEL_MAX)
   {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "速度应在 %u..%u pul/s 之间。",
                    VEL_MIN, VEL_MAX);
      return buf;
   }

   std::string why;
   if (!fitsRange(p, &why))
      return why;

   return std::string();
}

int64_t estimatePerPointMs(const Params &p)
{
   double step_pul = p.res_unit * p.pulses_per_unit;
   uint32_t vel = clampVel(p.speed_pul_s ? p.speed_pul_s : 1);

   /* 蛇形里每一步都只动一根轴、都走一个 res —— 所以"移动"这一项是常数 */
   double move_ms = step_pul / (double)vel * 1000.0;

   /* 进近段要减速, 实际比 step/vel 长。按 30% 粗加一笔, 宁可高估 */
   double per = move_ms * 1.30 + (double)p.settle_ms + (double)p.dwell_ms;

   /* 采样本身: 模拟源是 0, 真机未知, 按 50ms/次占个位 */
   per += 50.0 * (double)std::max(1, p.samples_per_point);

   if (per < 1.0)
      per = 1.0;
   return (int64_t)per;
}

std::string csvColumnHeader()
{
   return "index,ix,iy,x_unit,y_unit,x_pul,y_pul,watts,ok,flags,"
          "pos_x_pul,pos_y_pul,spread_x_pul,spread_y_pul,unix_ms,elapsed_ms";
}

std::string csvMetaLines(const Params &p, const std::string &started_iso, int zero_epoch)
{
   /* 两行 `#` 注释带全部参数, 续扫靠它做兼容性判定: 决定物理网格的那四项一个都不能少 */
   char buf[1024];
   std::string s = "# scan v1\n";
   s += "# area_x_unit=" + std::to_string(p.area_x_unit);
   s += " area_y_unit="   + std::to_string(p.area_y_unit);
   s += " res_unit="      + std::to_string(p.res_unit);
   s += " pulses_per_unit=" + std::to_string(p.pulses_per_unit);
   s += "\n";

   std::snprintf(buf, sizeof(buf),
                 "# speed_pul_s=%u dwell_ms=%d settle_ms=%d samples=%d serp=%d start_pos=%d\n",
                 (unsigned)(p.speed_pul_s ? p.speed_pul_s : 0),
                 p.dwell_ms, p.settle_ms, p.samples_per_point,
                 p.serpentine ? 1 : 0, p.start_positive ? 1 : 0);
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
   char buf[512];

   /* 数值用 C locale, 不用 QLocale: 某些区域设置会把小数点变成逗号 */
   char watts[64];
   if (r.ok)
      std::snprintf(watts, sizeof(watts), "%.9g", r.watts);
   else
      watts[0] = '\0';          /* 采不到就留空: 0 是一个合法的读数 */

   std::snprintf(buf, sizeof(buf),
                 "%d,%d,%d,%.9g,%.9g,%d,%d,%s,%d,%s,%d,%d,%d,%d,%lld,%lld\n",
                 r.index, r.pt.ix, r.pt.iy,
                 r.pt.x_unit, r.pt.y_unit, r.pt.x_pul, r.pt.y_pul,
                 watts, r.ok ? 1 : 0, r.flags.c_str(),
                 r.pos_x_pul, r.pos_y_pul, r.spread_x_pul, r.spread_y_pul,
                 (long long)r.unix_ms, (long long)r.elapsed_ms);

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
   int nx = axisCount(p.area_x_unit, p.res_unit);
   int ny = axisCount(p.area_y_unit, p.res_unit);

   if (done != nullptr)
      done->assign((size_t)std::max(0, nx) * (size_t)std::max(0, ny), 0);
   if (max_index != nullptr)
      *max_index = -1;
   if (started_iso != nullptr)
      started_iso->clear();
   if (zero_epoch != nullptr)
      *zero_epoch = -1;

   double a_x = -1, a_y = -1, r_u = -1, ppu = -1;
   bool have_geom = false;

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

      if (ix < 0 || iy < 0 || ix >= nx || iy >= ny)
      {
         char buf[200];
         std::snprintf(buf, sizeof(buf),
                       "CSV 第 %zu 行的网格索引 (%d, %d) 超出 %d×%d, 几何参数可能填错。", lineno, ix, iy, nx, ny);
         return std::string(buf);
      }

      if (done != nullptr)
         (*done)[(size_t)iy * (size_t)nx + (size_t)ix] = 1;
      if (max_index != nullptr && idx > *max_index)
         *max_index = idx;

      if (nl == std::string::npos) break;
      pos = nl + 1;
   }

   if (!have_geom)
      return "CSV 中没有几何参数 (表头不完整), 无法确认与当前参数是否为同一片区域。";
   (void)seen_data;

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
   noteDiff("分辨率 (mm)", r_u, p.res_unit);
   noteDiff("每 mm 脉冲数", ppu, p.pulses_per_unit);

   if (!diff.empty())
   {
      diff = "CSV 的几何参数与当前设置不一致: " + diff +
             "。几何不一致无法续扫, 请把参数改回或另建文件";
   }
   return diff;
}

void csvLoadGrid(const std::string &text, const Params &p,
                 std::vector<char> *have, std::vector<double> *watts)
{
   const int nx = axisCount(p.area_x_unit, p.res_unit);
   const int ny = axisCount(p.area_y_unit, p.res_unit);
   const size_t n = (size_t)std::max(0, nx) * (size_t)std::max(0, ny);

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
       && nearly(a.res_unit,    b.res_unit)
       && nearly(a.pulses_per_unit, b.pulses_per_unit);
}

}   /* namespace scan */
