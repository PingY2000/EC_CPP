/*
 * scan/scanplan.h —— 扫描的纯逻辑: 参数 / 校验 / 网格 / 蛇形点列 / 单位换算 / CSV 格式。
 * 刻意不 include 任何 Qt (只用 std 与 int32_t), 不碰总线也不碰文件 I/O (落盘读盘在
 * scanlog 里), 要能单独编出来验: 坐标算错是静默错 —— 扫描照跑、数据照出, 整片区域偏了。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scan {

/* 一个网格点, 三种表示都带着 —— 显示用单位, 下发给驱动器用脉冲, 续扫认索引。 */
struct Point
{
   int     ix = 0;          /* X 网格索引 0..nx-1 */
   int     iy = 0;          /* Y 网格索引 0..ny-1 */
   double  x_unit = 0.0;
   double  y_unit = 0.0;
   int32_t x_pul  = 0;
   int32_t y_pul  = 0;
};

struct Params
{
   double area_x_unit = 27.0;
   double area_y_unit = 27.0;
   double res_unit    = 0.5;
   double pulses_per_unit = 50000.0;

   uint32_t speed_pul_s        = 20000;   /* 会被 EcatThread 夹在 1000..100000 */
   int      dwell_ms           = 200;     /* 到点稳定后再停这么久才采样 */
   int      settle_ms          = 100;     /* 到位判据要连续成立这么久 */
   /* >1 = 到点后连采几次取平均; 每点多 n 倍读数时间, 不在停留期内 */
   int      samples_per_point  = 1;
   int      meter_timeout_ms   = 2000;

   bool     serpentine    = true;         /* 蛇形(逐行往返); false = 每行同向 */
   bool     start_positive = true;        /* 第一行的 X 往 +X 还是 -X 走 */

   int32_t  range_pul = 0;                /* 0 = 按区域自动算, 见 autoRangePul() */
};

/* 网格点数的硬上限, 也是一道资源闸: 建网格要 nx*ny 个 Point, 热力图是一张 nx*ny 的 QImage。
 * validate() 报的是它, rebuildPlan() 拦的也是它。 */
constexpr long long kMaxPlanPoints = 200000;

/* 参数体检。返回空串 = 通过, 否则是一句给操作员看的中文。不做自动修正。 */
std::string validate(const Params &p);

/* 一根轴上的点数: floor(area/res) + 1。闭区间两端都取。 */
int  axisCount(double area_unit, double res_unit);

/* 一根轴上的坐标, 居中: span = (n-1)*res 可能略小于 area, 于是网格落在区域内 */
std::vector<double> axisCoords(double area_unit, double res_unit);

/* 完整点列, 已经按扫描顺序排好。 */
std::vector<Point> buildPlan(const Params &p);

/* 单位 <-> 脉冲。用 llround, 不做截断 —— 截断会让 27/2*50000 少一个脉冲。 */
int32_t pulseOf(double unit, double pulses_per_unit);
double  unitOf(int32_t pul,  double pulses_per_unit);

/* 画布视野的半宽, 单位与 Params 的长度量**同一种** (mm)。
 *
 * 它同时是软量程的**下限**(见 autoRangePul), 这是 2026-09-22 特意加上的一条耦合:
 * 面板上看得见的地方必须点得到。原来只有"区域半宽 + 1 单位"那一条下限, 于是一个 3 mm 的
 * 区域只放行 ±2.5 mm 的手动定位, 而画布仍然画到 ±16 —— Shift+左键点面板边缘会被
 * EcatThread::setTarget / interpolate 悄悄夹回来, 界面上完全看不出"我要的是 16, 实际只走到 2.5"。
 *
 * 16 而不是 15: 标尺画到 ±15, 多出的 1 单位是边距, 而画布本来就**接受** |x| ≤ 16 的点击
 * (超出才丢)。取 16 才真的没有够不到的一圈。
 *
 * **只有这一份**: mapcanvas.cpp 换算像素、判点击都读它, 不许再抄一个数 —— 两个数"必须一致"
 * 正是这个文件开头 VEL_MIN/VEL_MAX 那段已经在防的坑。 */
constexpr double kCanvasHalfUnits = 16.0;

/* 软量程 (脉冲) = max(区域半宽 + 1 单位余量, kCanvasHalfUnits); 给 EcatThread::postRange() 用 */
int32_t autoRangePul(const Params &p);

/* 到位容差 (脉冲) = 步长/20, 夹在 [50, 步长/4]; 由步长推出, 不暴露给操作员 */
int32_t posTolPul(const Params &p);

/* 自环: 扫描是否会走出量程之外 (会被 EcatThread 夹掉, 于是永远扫不到边)。
 *
 * **2026-09-22 起在界面这条路上它恒为真**: currentParams() 总是把 range_pul 设成
 * autoRangePul(), 而后者按构造就大于网格最远点。留着它是因为它仍是"有人显式塞了一个
 * range_pul"时的唯一那道闸 —— 自检里就有这样两处 (显式设 100000 / setRange(1000)),
 * 那是现在唯一走得到假分支的路径。别删。 */
bool fitsRange(const Params &p, std::string *why);

/* 几何是否一致 (区域 X/Y、分辨率、每单位脉冲数)。这四项一改, CSV 里的 (ix,iy) 与现在的
 * 点列就不是同一个地方了; 速度/停留/采样次数不影响几何。 */
bool sameGeom(const Params &a, const Params &b);

/* 按参数估一个每点耗时 (ms), 用于开始之前报出总时长。线性估计, 实际一定更长。 */
int64_t estimatePerPointMs(const Params &p);

/* CSV 表头与行格式。表头两行 `#` 注释带全部参数, 续扫靠它做兼容性判定; 列名一律 ASCII,
 * 数值用 C locale 格式化 (中文列名会被 Excel 按 GBK 打开成乱码)。 */
std::string csvMetaLines(const Params &p, const std::string &started_iso, int zero_epoch);
std::string csvColumnHeader();

struct Row
{
   int      index   = 0;
   Point    pt;
   double   watts   = 0.0;
   bool     ok      = false;      /* false = 这一点没采到 (超时/失败), watts 无意义 */
   std::string flags;             /* ok=false 时写原因; 重测写 "retest" */
   int32_t  pos_x_pul = 0;        /* 采样那一刻的实测位置, 采不到也要记 */
   int32_t  pos_y_pul = 0;
   int32_t  spread_x_pul = 0;     /* 稳定窗口内 pos 的极差 —— 振没振, 在这儿看得见 */
   int32_t  spread_y_pul = 0;
   int64_t  unix_ms  = 0;
   int64_t  elapsed_ms = 0;       /* 自本轮扫描开始 */
};

std::string csvRowLine(const Row &r);

/* 解析 CSV: 认表头里的几何参数与每行的 (index, ix, iy), 用于断点续扫; done[iy*nx+ix]
 * 置位 = 这一点已有数据。返回空串 = 兼容; 非空 = 差异说明 (几何不一致或文件损坏都走这里)。 */
std::string csvParseForResume(const std::string &text, const Params &p,
                              std::vector<char> *done, int *max_index,
                              std::string *started_iso, int *zero_epoch);

/* 把数值读回网格, 用于续扫之后重画热力图。一格多行时取最后一行;
 * 列的位置从列名行按名字找, 不写死下标。 */
void csvLoadGrid(const std::string &text, const Params &p,
                 std::vector<char> *have, std::vector<double> *watts);

}   /* namespace scan */
