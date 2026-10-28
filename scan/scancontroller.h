/*
 * scan/scancontroller.h —— 扫描状态机: 只活在 GUI 线程里 (不碰总线, 不用加锁), 节拍由外部
 * 30Hz QTimer 喂 tick(now_ms), 时间一律用单调钟 (QElapsedTimer)。
 * 状态 Idle/Moving/Dwelling/Reading → 下一个点 / Done; Paused / Aborted 可从任意状态进入。
 * 连续行进 (2026-10-29, docs/scan_sweep.md §54) 那一路是 Moving → Traversing → 下一行,
 * 行内不停车, 每格只取经过中心最近的一笔采样。
 * 自动中止的条件每个 tick 都查 (含暂停中), 见 healthProblem() 与 externalWantChanged()。
 */
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "busview.h"
#include "limitguard.h"
#include "powermeter.h"
#include "scanarrive.h"
#include "scanlog.h"
#include "scanplan.h"

namespace scan {

/* 撞限位不在这里判定: 读 AxisTelem::limit_active 就够了, 它由 ecatcmd::limit_hit()
 * 一处算出 (6041h bit11, 见 ecatworker.h 的 LimitRule)。中止只由它决定,
 * 开关本身压没压着是另一回事 (看 60FDh 的 dig_home/dig_pos/dig_neg)。 */

/* 下发一个目标之后, 最多等它出现在遥测里多久 (ms)。见 externalWantChanged() */
#define WANT_CONFIRM_MS   200

class ScanController : public QObject
{
   Q_OBJECT

public:
   enum class State
   {
      Idle,      /* 没在跑; 参数可改 */
      Loaded,    /* 已装载一份续扫 (上半场在画布上、句柄开着、要补的点算好了), 但**一步没走**
                  * (2026-09-29)。**不算 running** —— 它属于"没在扫描循环里"那一族 (与 Idle
                  * 同类), 理由见 running() 上面那段注释 */
      Moving,    /* 已下发目标, 等两轴到位且稳 (含 settle 窗口) */
      Dwelling,  /* 到了, 停 dwell_ms */
      Reading,
      /* 连续行进 (2026-10-29, §54): 行末目标已下发, 整行不停车地走; 每格取经过中心
       * 最近的一笔采样。行首/行末各停在中心等一笔 (见 beginTraverse/issueTraverseTarget) */
      Traversing,
      Paused,    /* 冻住, 保持力矩; 继续时会重新走完当前点 */
      Aborted,
      Done       /* 全部点跑完 */
   };
   Q_ENUM(State)

   /* bus 与 meter 不归它管, 由窗口负责, 且必须活过本对象。
    *
    * **meter 只用来问"打开了没"与读表头那几行设备信息 (meterMetaLines)**。2026-09-29 起
    * 本类**不再向它发请求**: 一个点的值 = 该点读取期间从连续读数那条流里到齐的采样的平均,
    * 由 feedMeterSample() 送进来。持 const 指针就是为了让 requestReading() 在编译期不可达
    * —— 功率计"同时只有一个未决请求"那条约束 (powermeter.h) 现在只剩 MeterLog 一个发起方。 */
   ScanController(BusView *bus, PowerMeter *meter, QObject *parent = nullptr);

   /* 存下来并重算网格, 不做校验 (输入中间态一定不合法); 关口在 start() */
   void setParams(const Params &p);
   const Params &params() const { return m_p; }

   /* validate() 的话; 空 = 参数本身站得住 (不一定意味着"这一趟跑得起来") */
   QString paramsError() const;

   /* "这一趟跑得起来"的那一句: paramsError() + 连续行进的速度闸 (continuousRunError)。
    * **起扫 / armRun / 界面那个「开始扫描」用它**; 打开 CSV、对齐几何、存 ini 只认
    * paramsError() —— 它们不跑扫描 (理由见 scanplan.h 里 continuousRunError 的注释)。 */
   QString runError() const;

   /* 重算网格与预计时长。几何没变就不动 —— 不清掉刚采完的结果 */
   void rebuildPlan();

   int     gridNx() const { return m_nx; }
   int     gridNy() const { return m_ny; }
   int     totalPoints() const { return (int)m_plan.size(); }
   int64_t estimateTotalMs() const;

   State state() const { return m_st; }
   bool  running() const;              /* Moving/Dwelling/Reading/Traversing/Paused */
   bool  loaded() const { return m_st == State::Loaded; }   /* 打开一份 CSV, 停着等操作员 */

   /* 这一轮是不是连续行进 (armRun 时按 onFly(m_run_p) 定死, 一轮里不变)。
    * 界面拿它判「每点采样」亮不亮 —— 连续行进时那个框改了也没用 (每格只取一笔)。
    * **名字不能叫 onFly()**: 类成员会遮住命名空间里的 scan::onFly(p), armRun 里那句
    * `onFly(m_run_p)` 就成了找不到重载的编译错。 */
   bool  onTheFly() const { return m_onfly; }
   /* 本轮是「随机 (可重复)」: 永远到不了 Done, 只有「暂停」「中止」能停。
    * 界面拿它决定进度那几行怎么写 (pendingPoints 在那一支里会一轮一轮往回跳, 报出来是假话) */
   bool  endless() const { return m_endless; }
   QString stateText() const;          /* 状态栏那一行中文 */

   /* 正在走的点在 m_plan 里的下标; 不在跑时 -1 */
   int index() const { return m_cur; }
   const Point *currentPoint() const;  /* 不在跑时 nullptr */
   const Point *pointAt(int k) const;  /* k 是 m_plan 下标 */

   /* 已经有数据的格子数 (含采失败的点 —— 它们也算走过) */
   int completedPoints() const;
   int pendingPoints() const { return m_order.empty() ? 0 : (int)m_order.size() - m_ord_i; }
   int64_t elapsedMs() const { return m_run_ms >= 0 ? m_now_ms - m_run_ms : 0; }

   /* 本状态已经待了多久 (界面用来算"已用 / 预计剩余") */
   int64_t stateMs() const { return m_state_ms >= 0 ? m_now_ms - m_state_ms : 0; }

   /* 正在 Moving 里开稳定窗口 = 机械已经进容差了, 在等 settle_ms。
    * **2026-10-29 起界面这一路恒推 settle 0**, 于是它恒为假 —— 判据本身留着
    * (ArrivalJudge 仍有稳定窗口能力), 只是没有地方把它调大于 0 了。 */
   bool settlingNow() const;

   bool  cellDone(int ix, int iy) const;
   bool  cellHasValue(int ix, int iy) const;
   double cellValue(int ix, int iy) const;   /* 该格**最后一次**采到的功率 */
   const std::vector<char>   &doneMask()  const { return m_done; }
   const std::vector<double> &valueGrid() const { return m_watts; }

   /* 已采数据的功率范围 (热力图色标用), 空/失败的点不算数; false = 一点都没采到 */
   bool wattsRange(double *lo, double *hi) const;

   /* 开始新一轮: 新建 CSV (父目录自动建)。过不了 Preflight 就返回 false 并说明 */
   bool start(const QString &csv_path, QString *err);

   /* 回到"新建"模式 (2026-09-30, docs/scan_sweep.md §49): 关掉当前那份文件的写出句柄、
    * 丢掉点列与三个结果数组、回 Idle —— 于是参数栏解锁、画布清空, 下一次 start() 写一个
    * **新文件**。运行中拒绝 (界面那边「新建 CSV」也是灰的, 这一句是第二道)。
    *
    * 与 abort() 的区别不是"做多做少", 是**对谁**: abortInternal 的第三个分支 (中止一轮)
    * **刻意不关文件** —— 中止之后常要单点重测, 而重测就是往同一个文件追加; 而"新建"恰恰
    * 要把那份文件放掉。 */
   bool newFile(QString *err);

   /* 断点续扫, 拆成两步 (2026-09-29: 打开 CSV 之后**不直接开跑**, 先把数据读进来)。
    *
    * loadResume: 读一个已有 CSV, 校验几何一致 → 只补没采过的点 → 继续追加同一个文件,
    * 然后**停在 State::Loaded** —— 上半场已经画在画布上、要补的点已经算好、句柄开着,
    * 而滑台一步没走。zero_epoch 不一致时 (中间重连过、零点可能变过) 返回 false,
    * why 里要求操作员确认 (照旧)。
    * **采完的文件也装得进来** (2026-09-30): 那时 m_order 为空、pendingPoints() == 0, 能做的是
    * 单点重测而不是补点 —— 见 retest() 与 beginLoadedRun() 那两道判据。
    * **打开态下也调得进来, 那就是"换一份"** (2026-09-30, §49.9): 从前这里有一句
    * `if (loaded()) return fail(...)`, 理由是这次调用的后半段是破坏性的、而新旧句柄之间没有
    * 退路。现在它成功时换掉旧的那一份 (画布整张重铺 —— 新文件里没有的格子变回未采),
    * **失败时一个字节都不动**: 可能失败的那些步全排在换句柄之前, 而换句柄那一步自己带一条
    * 回滚 (新的没开成 → 把旧的装回去)。
    *
    * beginLoadedRun: 操作员按下「继续」才开始走。开工前把**装载那一刻的两条前提重查一遍**
    * (几何还是那一份吗 / 零点世代还是那一个吗) —— 这两件事都能在"装载好、还没按继续"那段
    * 时间里变掉, 而 m_order 里那些 (ix, iy) 绑的就是它们。拦下时返回 false 且**留在装载态**
    * (接好线再按一次就行), err 里是那句红横幅。 */
   bool loadResume(const QString &csv_path, bool accept_zero_epoch_change,
                   QString *err, QString *why);
   bool beginLoadedRun(QString *err);

   /* 「输出路径框里那个名字已经作废了」的一次性标志 (2026-09-30, docs/scan_sweep.md §49.5)。
    *
    * 一轮结束 / 中止 / 放弃打开 / 新建 之后,**当前那份文件已经不再是要写的目标**, 而窗口那个
    * 路径框还写着它的路径 —— 按「开始扫描」就是 beginNew 的 Truncate, 静默清空一份(可能是
    * 别人的)文件。所以这几条路各置一次这个标志, 由窗口在 refresh() 里取走换成新的时间戳名。
    *
    * 置起的地方一共四处, 一处一句 m_stale_csv_name = true:
    *   advance() (整轮 / 单点重测跑完) / abortInternal 的"中止一轮"那一支 / resetRunState()
    *   (它管"放弃打开"与 newFile() 两条)。
    * **唯独"打开成功"那一步不置** —— 那一步之后要写的正是刚打开的那份文件, 换名就等于
    * 把"接着往下补"变成"另起一份", 两半数据分家。自检钉的就是这三条。
    *
    * 放在控制器里而不是让窗口比状态跳变: 它进得了 SCAN_COMMON_SRC, 自检钉得住。取走即清 ——
    * 名字只该换一次, 而 applyCsvDefaultName() 每次调用都会取一个新时刻。 */
   bool takeStaleCsvName();

   /* 单点重测: 走回 (ix,iy) 再采一次, 追加一行 flags=retest。
    * 要求已经有一轮在跑 (否则没有文件可追加), 且几何参数没被改过 */
   bool retest(int ix, int iy, QString *err);

   void pause();
   void resumeRun();
   void abort(const QString &why);

   /* 外部驱动: 每 tick 喂一次单调钟的毫秒数。**这也是自动中止的唯一检查点** */
   void tick(int64_t now_ms);

public slots:
   /* 连续读数那条流到齐了一笔 (接 MeterLog::sampled)。**只在两种状态下算数**:
    *   Reading    —— 逐点停: 累加到平均, 凑够 samples_per_point 就收尾;
    *   Traversing —— 连续行进: 只留"到达位置离当前格中心最近"的那一笔, 一格只有一笔。
    * 移动/停留/暂停期间到齐的采样属于曲线上那条流, 不属于任何一个点。
    *
    * ok = false 的那一笔**不算进平均也不当场判死**: 它只是"这一次没要回来", 期限到了自然
    * 会收尾成 ok=false 的那一行 (与 MeterLog::setAverage 同一个口径 —— 拿缺了一笔的数求
    * 平均是编出来的, 而它在 CSV 里和别的行长得一模一样)。连续行进同理: 那一格没有候选,
    * 收尾时写 ok=false。
    *
    * 时刻 ms 收下不用 (当前点用的是 tick 那条单调钟)。Qt 的 connect 只能丢**尾部**参数,
    * 而 ok 是最后一个, 所以只能整个收下来。 */
   void feedMeterSample(int64_t ms, double watts, bool ok);

   /* CSV 写失败/文件路径, 给状态栏显示 */
   QString csvPath() const { return m_log.path(); }
   int     csvLines() const { return m_log.written(); }

   /* 零点世代: 零点真被搬过一次就 +1。**由工作线程计, 窗口从遥测同步** (只许往前), 这里只收。
    * 只进 CSV 表头, 续扫时对不上就要求操作员确认。
    * **连接不再推它** (2026-09-22 改): scan 现在跨重连沿用零点, 连接本身不动零点; 真搬零点的
    * 是回零 / 「设为区域中心」/ 第一次取零点 / 沿用不了只好重取, 那四处都在工作线程里。 */
   void setZeroEpoch(int epoch) { m_zero_epoch = epoch; }
   int  zeroEpoch() const { return m_zero_epoch; }

   /* 限位记录 (2026-09-29)。**界面是主, 变一次推一次** —— 与 setZeroEpoch 同一条路
    * (记录活在 ScanWindow 里, 这里只收一份用来判"这一趟的网格有没有越过线")。
    * 只在 armRun 里用一次: 越了就**拦在起点**, 不是走到一半才停 —— 走一半停会留下
    * 半张图, 而那个缺口还得靠人看出来。
    * 数组是**两根轴**, 下标 0 = X / 1 = Y (与 armRun 那句"正好两根轴"同一条)。 */
   void setLimitLines(const limitguard::LimitAxis *ax);   /* nullptr = 当作一条都没有 */

signals:
   void stateChanged();
   void pointLogged(int ix, int iy, bool ok);
   void runFinished(bool complete);
   /* 自动中止: 界面必须弹**红色横幅**, 不是一闪而过的提示 */
   void autoAborted(const QString &why);

private:
   bool armRun(QString *err);           /* Preflight + 进第一个点 */
   void prepareRunPlan();               /* 开扫前按这次的走法重排点列 (随机那两种还会洗牌) */
   void startPoint(int plan_index);
   void beginReading();
   void finishPoint(bool ok, const std::string &flags);
   void advance();                      /* 当前点收尾 → 下一个点 / Done */

   /* ---- 连续行进 (2026-10-29, docs/scan_sweep.md §54) ---- */
   void beginTraverse();                /* Moving 到位 → 进这一段 (行首先停在中心等一笔) */
   void issueTraverseTarget();          /* 行首那笔到手 → 下发段尾目标, 整行不停车 */
   /* 位置往前走就把当前格收掉、槽位 +1 (判据是"离下一格中心更近" = 过了中点)。
    * 会写 CSV —— 失败时内部已 abort, 调用方接着 return 就行 */
   void syncOnFlyCell(const BusTelem &t);
   bool finishOnFlyCell(int cell, bool have);   /* false = 写失败, 已 abort */
   void advanceLeg();                   /* 一段走完 → 下一段 / 整轮结束 */
   /* advance() 与 advanceLeg() 共用的收尾: 清点列、清行段、置 stale、Done、runFinished */
   void finishRun();
   /* finishPoint / finishOnFlyCell 共用的一段: 补 retest 后缀、写文件 (失败即 abort)、
    * 更新三个结果数组、发 pointLogged。**返回 false = 已经中止, 调用方必须当场 return** */
   bool logAndMark(Row &r);

   void enter(State s);
   void stopMotion();                   /* postStop + 丢掉未决读数 */
   void abortInternal(const QString &why, bool automatic);
   /* 三个结果数组归零, **网格尺寸保持原样** (cellDone 是按 m_nx 索引进来的, 不能 clear)。
    * 只有"放弃装载"与"新建"那两条路要它 */
   void clearResults();

   /* 把"当前这一轮"整个收掉: 关句柄 / 清点列 / 清结果网格 / 置那个路径框作废标志。
    * 两个调用方: abortInternal 的装载那一支 ("放弃这次打开") 与 newFile()。
    * **它不碰状态** —— enter(State::Idle) 由调用方自己来。 */
   void resetRunState();

   /* 每 tick 的安全检查。返回空串 = 健康; 非空 = 原因 (调用方据此自动中止) */
   QString healthProblem(const BusTelem &t);
   /* 有人从旁路改了目标 (扫描期间不允许) */
   bool externalWantChanged(const BusTelem &t, int axis, int32_t *seen);

   BusView          *m_bus   = nullptr;
   const PowerMeter *m_meter = nullptr;   /* 只读: 问开没开 + 表头那几行, 见构造函数 */

   Params  m_p;
   Params  m_plan_p;             /* 当前网格是照哪份参数建的 —— 见 rebuildPlan() */
   Params  m_run_p;              /* 本轮开始时的一份拷贝 —— 盘中改过参数时要能查出来 */
   int     m_nx = 0, m_ny = 0;
   std::vector<Point> m_plan;

   State   m_st = State::Idle;
   bool    m_is_retest = false;  /* 本轮是单点重测: 走完这一个点就 Done */
   /* 本轮是「随机 (可重复)」(见 modeEndless)。m_order 走完不判 Done, 而是把点列重洗一遍
    * 接着走 —— 于是这一轮只有 pause()/abort() 能结束。start()/loadResume() 里连同 m_order
    * 的构造一起定下来, advance() 里用它 (那是它唯一的分支点)。 */
   bool    m_endless   = false;
   /* 洗点列用的种子。**每轮换一个** (start/loadResume 里从 QRandomGenerator 取),
    * 每次洗牌自增 —— 于是同一轮里每洗一遍都是另一个顺序, 而自检能靠一个固定值钉住结果 */
   uint32_t m_seed = 0;
   std::vector<int>   m_order;   /* m_plan 的下标序列 (续扫时已滤掉采过的) */
   size_t  m_ord_i = 0;
   int     m_cur   = -1;         /* m_plan 下标; -1 = 不在点上 */

   /* ---- 连续行进 (2026-10-29, §54)。m_onfly 为假时下面那些一个都不参与 ---- */
   bool    m_onfly = false;      /* armRun 里按 onFly(m_run_p) 定死, 一轮里不变 */
   /* 行段: 每一段 = 同一行里**待扫**格子的 m_plan 下标, 按扫描顺序 (从右往左那一行也是
    * 这个顺序)。续扫留下的空洞格之间隔着已采格, 仍然算同一段 —— 中间那些格不停 */
   std::vector<std::vector<int>> m_legs;
   size_t  m_leg_i = 0;          /* 当前段 */
   size_t  m_leg_slot = 0;       /* 段内正在采的格子槽位 */
   size_t  m_leg_slot0 = 0;      /* 暂停续走从哪个槽位开始 */
   bool    m_of_started = false; /* 行首那笔已到, 段尾目标已下发 */
   bool    m_of_at_end = false;  /* 段尾到位这件事已经处理过 (别每拍重入) */
   bool    m_of_stop_wait = false; /* 段尾已到位, 在等"停稳之后到齐"的那一笔 */
   int64_t m_flush_deadline_ms = 0; /* 行首/行末等一笔的期限; 0 = 不在等 */

   /* 当前格的一笔候选采样: 只留离格中心最近的那一笔 (连续行进每格只出一行) */
   bool     m_of_have = false;
   double   m_of_watts = 0.0;
   int32_t  m_of_pos[EM_MAX_AXES] = {0, 0};
   int64_t  m_of_dist = 0;       /* |pos_x - 格中心 x|, 脉冲 */
   int64_t  m_of_unix_ms = 0;
   std::string m_of_time;        /* 与 m_of_unix_ms 同一次 QDateTime */

   ArrivalJudge m_judge[EM_MAX_AXES];
   int      m_nsamp = 0;         /* 这一个点已经到齐的采样数 (那个"半份"就在这里) */
   double   m_acc   = 0.0;
   int32_t  m_spread[EM_MAX_AXES] = {0, 0};
   int32_t  m_issued[EM_MAX_AXES] = {0, 0};   /* 最后下发的目标 (显示坐标) */
   int64_t  m_issued_ms[EM_MAX_AXES] = {0, 0};
   /* 已经看到 EcatThread 把目标登上了 (下发与 publish 相隔一个 2ms 周期) */
   bool     m_want_ok[EM_MAX_AXES] = {false, false};

   std::vector<char>   m_done;   /* 走过 (含采失败) */
   std::vector<char>   m_have;   /* 采到了数 */
   std::vector<double> m_watts;  /* 该格最后一次的功率 */

   ScanLog m_log;
   bool    m_stale_csv_name = false;   /* 见 takeStaleCsvName() */

   int64_t m_now_ms      = 0;    /* 最近一次 tick 的单调钟 */
   int64_t m_state_ms    = -1;   /* 进入当前状态的时刻 */
   int64_t m_run_ms      = -1;   /* 本轮第一次下发的时刻 (CSV 的 elapsed_ms 基准) */
   int64_t m_deadline_ms = 0;    /* Dwelling 截止 / Reading 的收尾期限 (见 m_read_budget_ms) */
   /* Reading 允许待多久 (= m_read_budget_ms, 见 beginReading)。看着像重复, 其实是为了那句
    * 超时文案: 它要报的是**这次给的预算**, 而 m_deadline_ms 一旦被 tick 比较过就没法反推
    * (而且报预算比报"等了多久"有用 —— 前者是参数定的, 后者还含着 tick 的相位) */
   int64_t m_read_budget_ms = 0;
   int64_t m_move_deadline_ms = 0;  /* 走到位的最后期限 —— 卡住/被夹住时的兜底 */

   int     m_bad_wkc = 0;
   /* CSV 表头里记的零点世代; 续扫时对不上 = 中途重连过 */
   int     m_zero_epoch = -1;
   /* 装载那一刻的零点世代 (loadResume 记下, beginLoadedRun 重查)。与上面那个分开:
    * 那个是"文件里写的世代 vs 现在", 这个是"装载时 vs 现在" —— 危险的是装载与启动之间
    * 零点动了, 与那份 CSV 写没写世代无关, 所以这一比不依赖文件 (§45.5) */
   int     m_load_epoch = -1;

   /* 限位记录: 只由 setLimitLines 换, 只在 armRun 里读 (理由见上面那个 setter)。
    * has_pos 全为 false 时 limitPlanWhy 一律给空串 —— 于是"没记过"这条路
    * 与"这道闸不在"是同一个行为, 不需要另开一个开关。 */
   limitguard::LimitAxis m_lim[2];
};

}   /* namespace scan */
