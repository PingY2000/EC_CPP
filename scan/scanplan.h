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

/* 走法 (2026-10-11)。**这是"这一趟怎么走"的唯一定义处** —— 界面那个「扫描方式」下拉、
 * 点列的排法、预览画不画、一轮会不会自己结束, 全从这里推。
 *
 * 前两种是按行走的**几何式**走法 (每步只动一根轴, 时间是常数); 后两种是随机抽点,
 * 每一步都是一个随机跳, 所以时间估算那一项另算 (见 estimatePerPointMs)。
 * 数值即界面下拉的下标, 也是 ini 与 CSV 表头里记的那个数 —— **别改这几个数**。 */
enum class ScanMode
{
   Serpentine   = 0,   /* 逐行往返 (蛇形, 缺省) */
   SameDir      = 1,   /* 每行同向 */
   RandomOnce   = 2,   /* 随机 (不重复): 整片网格洗成一个随机顺序走一遍, 走完即止 */
   RandomRepeat = 3,   /* 随机 (可重复): 走完一遍再洗一遍接着走, 永远不结束 */
};

/* 四个下拉项文案的**唯一来源** (界面循环建项, 自检钉着这几个字) */
constexpr int kScanModeCount = 4;
const char *modeText(ScanMode m);

/* 下标 (界面下拉 / ini 里那个数) → 模式。越界一律回落 Serpentine, 不做别的解释 */
ScanMode modeFromIndex(int i);

/* 两种随机共用的一条: 开扫时要重排点列, 画布上不画预览折线 */
bool modeRandom(ScanMode m);

/* 只有「随机 (可重复)」为真: 这一轮永远到不了 Done, 只有暂停 / 中止能停 */
bool modeEndless(ScanMode m);

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
   /* 连续读数那一路的**采样间隔**。2026-09-29 起一个点的值就是"读取时间段里到齐的采样的
    * 平均", 而采样是按这个间隔到齐的 —— 于是它既是"一个点要多久"的时间尺度 (见
    * estimatePerPointMs 与 ScanController::beginReading 的读取预算), 也是扫描节奏本身。
    * **界面那个「间隔」旋钮是它唯一的来源** (scanwindow 的 currentParams), 这里没有第二个
    * 写点 —— 同一个数两份实现正是本文件开头 VEL_MIN/VEL_MAX 那段在防的坑。 */
   int      meter_interval_ms  = 200;

   ScanMode mode          = ScanMode::Serpentine;   /* 这一趟怎么走 (见上面那个 enum) */
   bool     start_positive = true;        /* 第一行的 X 往 +X 还是 -X 走; 只对前两种走法有意义 */

   int32_t  range_pul = 0;                /* 0 = 按区域自动算, 见 autoRangePul() */
};

/* 网格点数的硬上限, 也是一道资源闸: 建网格要 nx*ny 个 Point, 热力图是一张 nx*ny 的 QImage。
 * validate() 报的是它, rebuildPlan() 拦的也是它。 */
constexpr long long kMaxPlanPoints = 200000;

/* 参数栏那四个几何输入框的可设范围 (2026-09-29)。**界面按这四个数 setRange, 判据也按它们判**
 * (见 csvAlignParams) —— 两处各写一份的话, "CSV 里的值设得进面板吗"这件事迟早跟界面对不上。
 *
 * 为什么要有这条判据: QDoubleSpinBox::setValue 会把超范围的值**静默夹进来** (600 变成 500),
 * 于是"照 CSV 对齐"会悄悄对齐成另一个值, 而续扫那边一句"几何不一致"说得人不明不白。 */
constexpr double kGeomAreaMin = 0.1,   kGeomAreaMax = 500.0;     /* mm */
constexpr double kGeomResMin  = 0.001, kGeomResMax  = 50.0;      /* mm */
constexpr double kGeomPpuMin  = 100.0, kGeomPpuMax  = 1000000.0; /* pul/mm */

/* Params::meter_interval_ms 的量程。**与 MeterLog::kMinIntervalMs / kMaxIntervalMs 必须
 * 逐字一致** (那一头夹的是连续读数真的用的间隔, 这一头夹的是同一件事的估计与预算)。
 * 本文件刻意不依赖 Qt (见开头 VEL_MIN 那段), 所以只能照抄一份 —— 但这次**钉住了**:
 * scan/selftest.cpp 里有一条 checkEq 比对它与 MeterLog 那两个常量 (VEL_MIN 那两个至今
 * 只有一句注释, 是没钉的)。 */
constexpr int kMeterIntervalMinMs = 20;
constexpr int kMeterIntervalMaxMs = 60000;

/* 参数体检。返回空串 = 通过, 否则是一句给操作员看的中文。不做自动修正。 */
std::string validate(const Params &p);

/* 一根轴上的点数: floor(area/res) + 1。闭区间两端都取。 */
int  axisCount(double area_unit, double res_unit);

/* 一根轴上的坐标, 居中: span = (n-1)*res 可能略小于 area, 于是网格落在区域内 */
std::vector<double> axisCoords(double area_unit, double res_unit);

/* 完整点列, 已经按扫描顺序排好。随机那两种在这里出的是**基准网格** (每行同向那个顺序),
 * 真正的随机顺序由调用方再洗一次 —— 见 shufflePlan() 与 ScanController::start()。 */
std::vector<Point> buildPlan(const Params &p);

/* 把点列洗成随机顺序 (随机那两种走法用)。
 *
 * seed **由调用方给**而不是在这里现抽: 自检要能钉住"同一个 seed 给出同一份点列",
 * 而界面每次开扫给一个新的。行模式不调用它 —— 它们的顺序是算出来的, 不是抽出来的。 */
void shufflePlan(std::vector<Point> *pts, uint32_t seed);

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

/* 按参数估一个每点耗时 (ms), 用于开始之前报出总时长。线性估计, 实际一定更长。
 * 随机那两种走法每一步是一个随机跳, "移动"那一项按**平均跳距**估, 不按分辨率 (见 .cpp)。 */
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
   /* 人能看的那一列 (2026-09-30): `yyyy-MM-ddTHH:mm:ss`, 与表头 `# started=` 同一个格式,
    * **本机本地时间、不带时区后缀** (所以它不能当 UTC 用 —— 与 `# started=` 同一个性质)。
    *
    * 它**由调用方填**, 不在这里从 unix_ms 推: 本文件是纯 C++ (不含 Qt), 推本地时间得走
    * localtime_r, 而那与 controllers 那边 isoNow() 用的 QDateTime 不是同一套规则 —— 两边
    * 一旦对不上, 这一列与 `# started=` 会**差着时区而没人看得出来**。同一个来源才安全。
    * 空串照写 (那一格是个空字段), 列数保持齐整。 */
   std::string time_local;
};

std::string csvRowLine(const Row &r);

/* 解析 CSV: 认表头里的几何参数与每行的 (index, ix, iy), 用于断点续扫; done[iy*nx+ix]
 * 置位 = 这一点已有数据。返回空串 = 兼容; 非空 = 差异说明 (几何不一致或文件损坏都走这里)。 */
std::string csvParseForResume(const std::string &text, const Params &p,
                              std::vector<char> *done, int *max_index,
                              std::string *started_iso, int *zero_epoch);

/* 续扫前: 该不该把面板对齐到这份 CSV 的几何 (2026-09-29)。
 *
 * 返回空串 = 可以, *out 就是拿去设进那四个控件的那一份 (**在 cur 的基础上改那四项几何**,
 * 别的字段一个都不动 —— 所以它整份交给 validate() 也是对的)。唯一的例外是 range_pul:
 * 它本来就是"由区域算出来的"数 (见 autoRangePul), 这里跟着新几何重算一遍 —— 不重算的话
 * validate() 会拿旧量程去量新区域, 一个正常的 CSV 会被误判成"几何不合法";
 * 非空 = 一句给操作员看的理由, 调用方**一个数都不许设进面板**。
 *
 * 与 csvParseForResume 分工不同, 两边都不动对方那一个: 那一个的活是"比出差异"(它照旧一个字
 * 不改, 现在退居兜底 —— 手改过的 CSV、以及任何不走界面的调用方), 这一个的活是"把值取出来,
 * 并且先确认它真的设得进面板" (范围见上面 kGeom*; 手改坏的几何过不了 validate() 也在这一关
 * 拦掉 —— 不拦的话面板会停在一份连 prefsSave 都不采纳的参数上, 盘上屏幕上就分成两份了)。 */
std::string csvAlignParams(const std::string &text, const Params &cur, Params *out);

/* 把数值读回网格, 用于续扫之后重画热力图。一格多行时取最后一行;
 * 列的位置从列名行按名字找, 不写死下标。 */
void csvLoadGrid(const std::string &text, const Params &p,
                 std::vector<char> *have, std::vector<double> *watts);

}   /* namespace scan */
