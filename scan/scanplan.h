/*
 * scan/scanplan.h —— 扫描的**纯逻辑**: 参数 / 校验 / 网格 / 蛇形点列 / 单位换算 / CSV 格式
 *
 * **这个文件刻意不 include 任何 Qt**, 只用 std 与 int32_t。理由:
 * 它是整个程序里唯一有真逻辑的部分(网格边界、蛇形顺序、续扫的兼容性判定), 而
 * "这一点的坐标到底是多少"错了是**静默错**——扫描照跑、数据照出, 只是整片区域偏了。
 * 所以它要能脱离 Qt、脱离总线、脱离硬件单独编出来验 (见 scan/selftest.cpp)。
 *
 * 它不碰总线, 也不碰文件 I/O: 进出全是 std::string / 结构体。落盘与读盘在 scanlog 里。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scan {

/* ---------------------------------------------------------------- 参数 */

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
   /* ---- 几何 (默认: 27×27 单位, 分辨率 0.5, 1 单位 = 1 圈 = 50000 脉冲) ---- */
   double area_x_unit = 27.0;
   double area_y_unit = 27.0;
   double res_unit    = 0.5;
   double pulses_per_unit = 50000.0;

   /* ---- 运动与采样 ---- */
   uint32_t speed_pul_s        = 20000;   /* 会被 EcatThread 夹在 1000..100000 */
   int      dwell_ms           = 200;     /* 到点稳定后再停这么久才采样 */
   int      settle_ms          = 100;     /* 到位判据要连续成立这么久 */
   /* >1 = 到点后连采几次取平均。代价是每点多 n 倍**读数时间** (真机串口一次几百 ms 时
    * 这项很贵), 不是塞在停留期里白拿 —— 停留期是"等机械稳定", 两件事不该混。 */
   int      samples_per_point  = 1;
   int      meter_timeout_ms   = 2000;

   bool     serpentine    = true;         /* 蛇形(逐行往返); false = 每行同向 */
   bool     start_positive = true;        /* 第一行的 X 往 +X 还是 -X 走 */

   int32_t  range_pul = 0;                /* 0 = 按区域自动算, 见 autoRangePul() */
};

/* ---------------------------------------------------------------- 校验 */

/*
 * 网格点数的**硬上限**。
 *
 * 它不只用来劝退多打的一个零 —— 它是**一道资源闸**: 建网格要 nx*ny 个 Point、
 * 热力图是一张 nx*ny 的 QImage、预览折线是 nx*ny 个 lineTo。区域 500 单位配上
 * 分辨率 0.001 就是 2.5e11 个点, 那是几百 GB 和一次必然的卡死。
 *
 * **上限只在这里写一遍**: validate() 报的是它, ScanController::rebuildPlan() 拦的也是它。
 * 从前 validate 里写死 200000, 而**建网格那段根本不看它** —— "点数太多"只是一句提示,
 * 拦截发生在内存分配失败之后 (现象: 参数一改到某些值, 界面就再也回不来了)。
 */
constexpr long long kMaxPlanPoints = 200000;

/*
 * 参数体检。**返回空串 = 通过**, 否则是一句给操作员看的中文。
 * 不做"自动修正" —— 参数是操作员填的, 悄悄改成别的值比拒绝更糟。
 */
std::string validate(const Params &p);

/* ---------------------------------------------------------------- 网格 */

/* 一根轴上的点数: floor(area/res) + 1。闭区间两端都取。 */
int  axisCount(double area_unit, double res_unit);

/*
 * 一根轴上的坐标。**居中**: span = (n-1)*res 可能略小于 area (area/res 除不尽时),
 * 于是网格永远落在区域内 —— 宁可少扫一点也不扫到框外去。
 */
std::vector<double> axisCoords(double area_unit, double res_unit);

/* 完整点列, 已经按扫描顺序排好。 */
std::vector<Point> buildPlan(const Params &p);

/* ---------------------------------------------------------------- 换算 */

/* 单位 <-> 脉冲。用 llround, 不做截断 —— 截断会让 27/2*50000 少一个脉冲。 */
int32_t pulseOf(double unit, double pulses_per_unit);
double  unitOf(int32_t pul,  double pulses_per_unit);

/*
 * 软量程 (脉冲)。区域半宽 + 1 单位余量, 于是**扫描区之外还能手动走一点**便于对位。
 * 这是给 EcatThread::postRange() 的值。
 */
int32_t autoRangePul(const Params &p);

/*
 * 到位容差 (脉冲)。**由步长推出来, 不暴露给操作员** ——
 * 固定 50 pul 在 25000 pul 的步长下是 0.2%(合适), 但有人把步长调到 500 pul 时
 * 就变成"允许偏 10% 个格子"(不合适)。所以按步长取 1/20, 并夹在 [50, 步长/4]。
 */
int32_t posTolPul(const Params &p);

/* 自环: 扫描是否会走出量程之外 (会被 EcatThread 夹掉, 于是永远扫不到边)。 */
bool fitsRange(const Params &p, std::string *why);

/*
 * 几何是否一致 (区域 X/Y、分辨率、每单位脉冲数)。
 *
 * 这四项一改, "网格索引 → 坐标" 的映射就变了 —— 于是 CSV 里的 (ix,iy) 与现在的点列
 * 说的不是同一个地方。续扫与单点重测都靠它把关; 速度、停留、采样次数这些**不影响几何**的
 * 随便改, 不该拦。
 */
bool sameGeom(const Params &a, const Params &b);

/* ---------------------------------------------------------------- 预估 */

/*
 * 按**参数**估一个每点耗时(ms)。只用于在按开始之前把"这一趟多长"摆在操作员眼前。
 * 它是线性估计, **实际一定更长** —— 每次移动的进近段都要减速。
 */
int64_t estimatePerPointMs(const Params &p);

/* ---------------------------------------------------------------- CSV */

/*
 * CSV 的**表头与行格式**。表头两行 `#` 注释带全部参数, 靠它做续扫的兼容性判定。
 *
 * 列名一律 ASCII, 且数值用 C locale 格式化 —— 中文列名在中文 Windows 上会被 Excel
 * 按 GBK 打开成乱码, 而某些区域设置会把小数点变成逗号、把整列解析搞崩。
 */
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

/*
 * 解析一个 CSV: 认表头里的几何参数, 认每一行的 (index, ix, iy)。
 * 用于**断点续扫** —— CSV 本身就是进度文件, 不另存状态。
 *
 * done[iy*nx+ix] 置位 = 这一点已经有数据; max_index 用于算"扫到哪了"。
 * 几何参数与当前 p 不一致时返回一句差别说明 (而不是一句"不兼容"), 由调用方决定是拒绝还是问人。
 * 返回空串 = 兼容; 非空 = 差异说明。**解析失败 (文件损坏) 也走这里**。
 */
std::string csvParseForResume(const std::string &text, const Params &p,
                              std::vector<char> *done, int *max_index,
                              std::string *started_iso, int *zero_epoch);

/*
 * 再把**数值**读回网格, 用于续扫之后重画热力图。
 *
 * 没有这一步的话, 续扫打开的局面是: 前半场的数据在文件里、在图上却是空的 ——
 * 而这张图正是这个程序存在的理由。同一个文件里已经有了全部数据, 没理由不读回来。
 *
 * 一格多行时 (重测) **取最后一行**, 与热力图的"取该格最后一次"是同一条规则。
 * 列的位置从列名行按名字找, 不写死下标 —— 以后加列不会悄悄错位。
 */
void csvLoadGrid(const std::string &text, const Params &p,
                 std::vector<char> *have, std::vector<double> *watts);

}   /* namespace scan */
