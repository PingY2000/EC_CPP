/*
 * hmi/ecatworker.h —— EtherCAT 工作线程
 *
 * **整个程序里只有这个线程碰 em_bus_t 与总线相关的 motor_api。** GUI 线程一个 motor_api
 * 函数都不调, 也不 include SOEM 的任何头文件 —— 界面侧与总线侧之间只有下面这几个
 * post 系列 / set 系列 与 telemetry()。这样 "ecx_* 必须串行" 与 "网卡单进程独占" 两条约束是
 * **结构上**成立的, 不靠调用纪律。
 *
 * 唯一的例外是 main() 里那句 em_console_init(): 它只设控制台代码页, 不碰总线、不碰网卡,
 * 而它的注释要求"main() 的第一句就调"(否则第一次打日志之前的汉字是乱码)。
 *
 * 坐标有两套, 不要混:
 *   驱动器坐标 = 6064h 的原始值 (掉电清零, 每次上电从驱动器自己的 0 开始)
 *   显示坐标   = 驱动器坐标 - 软件零点 origin。界面上那个"中间 0"就是它。
 * 对外 (postZeroHere / setTarget / telemetry) 一律用**显示坐标**。
 */
#pragma once

#include <QMutex>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>

#include "ec_motor.h"

/* ---------------------------------------------------------------- 参数 */

/* 工作范围**默认值**: ±500000 脉冲。50000 pul/圈 => ±10 圈 (2400h 实测 = 50000)。
 *
 * 它只是缺省 —— 真正的量程是 EcatThread::m_range, 可以经 postRange() 改。
 * 加这一层是为了 scan/ (蛇形扫描采集): 它的区域默认 27 单位 = ±675000 脉冲,
 * 比这里大。而量程在 interpolate() 里是**夹取**用的, 差一点就会把区域边缘悄悄削掉 ——
 * 那种 bug 不报错, 只是永远扫不到边, 所以必须让它跟着区域参数走。
 *
 * hmi 自己从不调 postRange, 于是它拿到的永远是下面这个值, 行为与从前完全一致。 */
#define HMI_RANGE       500000

#define HMI_VEL_MIN       1000   /* pul/s, 约 0.02 圈/秒 */
#define HMI_VEL_MAX     100000   /* pul/s, 约 2 圈/秒 */
#define HMI_VEL_DEF      20000

#define HMI_CYCLE_US      2000   /* 过程数据周期 (µs), 与 motor_test 缺省一致, 不上 DC */
#define HMI_LOOP_MS          2   /* 循环里的让步节拍 */
#define HMI_STOP_MS        300   /* 进近段留出的刹停时间: 减速度 = v / 0.3s */

/* ---------------------------------------------------------------- 遥测 */

/* 一根轴的一帧快照。全部是**显示坐标**。 */
struct AxisTelem
{
   bool     valid     = false;
   bool     mirror_ok = false;   /* 收到过完整帧 (否则下面的 sw/pos 是陈值) */
   bool     enabled   = false;   /* 6041h bit2 = 电机带电 */
   bool     fault     = false;   /* 6041h bit3 */
   bool     at_target = false;   /* 插值目标已到 want */
   int32_t  pos       = 0;       /* 6064h - origin */
   int32_t  want      = 0;       /* 点击给出的目标 */
   int32_t  tgt       = 0;       /* 本周期真正下发的插值目标 */
   uint32_t vel       = HMI_VEL_DEF;
   uint16_t sw        = 0;
   uint32_t frames    = 0;
   QString  state;               /* em_sw_state_str(sw) 的中文名 */

   /* ---- 60FDh 三个开关 (原点 / 正限位 / 负限位) ----
    *
    * dig_known 是**单独一个字段**, 不是"三个 bool 都 false 就代表都没压住":
    * 60FDh 多半不在生效 TxPDO 里, 读不到时那三个 bool 也全是 false, 而"三个都没压住"
    * 在界面上是个**看起来完全正常**的结论。不知道和"都没压住"必须分得开。 */
   bool     dig_known = false;   /* em_dig_in_known(): 映射里有**且**收到过完整帧 */
   bool     dig_home  = false;   /* bit2 原点开关 */
   bool     dig_pos   = false;   /* bit1 正限位 */
   bool     dig_neg   = false;   /* bit0 负限位 */

   /* 撞限位 —— **会不会中止扫描**。在 publish() 里由 ecatcmd::limit_hit() 一处算出。
    *
    * 以前这个判断在三个地方各写一遍 (ScanController::healthProblem / ScanWindow::
    * refreshAxisSignals / MapCanvas::drawMarkers), 而「分开算就会有一天两边说的不一样」
    * 正是这块界面最不该有的那种 bug。 */
   bool     limit_active = false;
};

struct BusTelem
{
   bool     connected = false;
   bool     in_op     = false;
   /* 正在做连接/收尾 (SDO、状态机迁移, 会阻塞几秒)。界面靠它决定按钮形态 ——
    * **不要用"点过连接"来推**, 那会和实际的线程状态错开 */
   bool     busy      = false;
   bool     fault     = false;
   /* 正在做故障复位 (逐轴阻塞, 每轴最多 1 秒)。界面据此把按钮按住不动 ——
    * 复位**不可中断** (em_fault_reset 里那个 em_stop_requested 只有 CLI 才装),
    * 所以只能让它看起来被占住, 不能给一个按了没用的「取消」 */
   bool     resetting = false;
   int      naxis     = 0;
   int      wkc       = 0;
   int      expected_wkc = 0;
   /* 当前生效的量程 (脉冲)。默认 HMI_RANGE; scan/ 会经 postRange() 改成跟它的区域匹配,
    * 界面靠它画量程、也靠它判断"我要的目标会不会被夹" */
   int32_t  range     = HMI_RANGE;
   QString  note;                /* 最后一条给操作员看的话 */
   AxisTelem ax[EM_MAX_AXES];
};

/* ---------------------------------------------------------------- 命令层的纯判据
 *
 * **刻意放在头文件里 (inline), 不放 .cpp。** 理由是可测性:
 * scan/CMakeLists.txt 的 scan_selftest 只编 selftest.cpp + SCAN_COMMON_SRC,
 * **不编 ecatworker.cpp** —— 逻辑写在 .cpp 里就等于永远不会被自检碰到,
 * 而下面这几条恰好是最不能错的那几条。
 */
namespace ecatcmd
{

/*
 * 这一根轴该不该做故障复位。
 *
 * **全部安全性就压在这一行上。** 因为 em_fault_reset() 的动作顺序是:
 *   先写 6040h = 0x0000 (Disable voltage = **卸力**) 打十帧, 再抬 bit7
 *   (bit7 是**上升沿**触发, 不先把 0 压下去就构不成沿)
 * —— 见 motor_api/ec_motor_motion.c 的 em_fault_reset()。
 *
 * 这个顺序本身是对的, 但它对**一根没有故障的轴**做这件事时, 那一轴会**真的卸力**:
 * 竖直滑台会掉下来。而 em_fault_reset() 是**到函数末尾**才报告"本来就没有故障"的 ——
 * 那时候十帧卸力已经打出去了。**所以这道闸必须在调用之前。**
 *
 * 复位的对象由 6041h bit3 决定, 不由操作员点了哪个按钮决定。
 * mirror_ok = false 的轴**跳过、不猜**: 不知道它有没有故障, 就不碰它。
 */
inline bool axis_needs_reset(bool valid, bool mirror_ok, bool fault)
{
   return valid && mirror_ok && fault;
}

/* 从一份遥测里挑出该复位的轴 (返回条数, out 里是轴号)。
 * **返回 0 就表示一个字节都不该写。** 供 selftest 与界面弹窗使用。 */
inline int pick_faulted_axes(const BusTelem &t, int *out, int max)
{
   int n = 0;

   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
   {
      if (!axis_needs_reset(t.ax[i].valid, t.ax[i].mirror_ok, t.ax[i].fault))
         continue;
      if (out != nullptr && n < max)
         out[n] = i;
      n++;
   }
   return n;
}

/*
 * ==========================================================================
 * 撞限位 —— **只此一处定义**。控制器 / 参数栏 / 画布都读它算出来的那一个字段。
 * ==========================================================================
 *
 * 今天这条 = 6041h bit11 (Internal limit active) 单独判定。**本轮一个比特都没改。**
 *
 * --------------------------------------------------------------------------
 * 关于下面那个开关: 若真机上"只压原点开关"也让 bit11 置起, 扫描经过原点就会**误中止**
 * --------------------------------------------------------------------------
 * 那时的修法是**把判定改细, 不是把 bit11 删掉** —— bit11 是"一头撞上去"的最后一道闸,
 * 区域算错时正是靠它。改细之后:
 *
 *     撞限位 = bit11 置起 **且** 60FDh 说正限位或负限位压着   (原点不算)
 *     60FDh 未知 (不在映射里 / 丢帧) -> **退回只看 bit11** (即今天这条, 一个字没弱化)
 *
 * 三条理由说明它不弱化保护:
 *   · 区域算错 = 往限位那边发指令 -> 60FDh 那一位**一定**压着 -> 照样中止;
 *   · 经过原点开关 = 正/负限位都松开 -> 不中止, 而它本来也不该中止;
 *   · 换一台没有 60FDh 的驱动器 -> dig_known = false -> 自动退回旧判据。
 *
 * **为什么现在不打开**: 有一类配置会让它变弱 —— 2310h~2312h 的功能码被改成 0 (未定义) 时,
 * 60FDh 那三位恒为 0, 而 bit11 照样会因限位置起; 此时 dig_known = true 而改细的判据
 * 永不成立, 保护就没了。所以**先按真机实测结果决定** (docs/scan_sweep.md §13 那条待验证)。
 *
 * 打开 = 把 0 改成 1。两条分支的测试都已经写好了 —— 这个开关必须是一次**有证据**的改动,
 * 不是一次摸索。
 */
#define kRefineLimitWithDigIn 0

/* 精判据。规则本身用参数传, 于是**两条分支在同一次构建里都可测** (见 scan/selftest.cpp) */
inline bool limit_hit_rule(uint16_t sw, bool dig_known, bool dig_pos, bool dig_neg,
                           bool refine)
{
   const bool bit11 = (sw & EM_SW_INTLIMIT) != 0;

   if (!refine)
      return bit11;

   /* 改细之后: 未知就退回 bit11 单独判定 —— 这一条是"不弱化"的保证 */
   if (!dig_known)
      return bit11;

   return bit11 && (dig_pos || dig_neg);
}

inline bool limit_hit(uint16_t sw, bool dig_known, bool dig_pos, bool dig_neg)
{
   return limit_hit_rule(sw, dig_known, dig_pos, dig_neg,
                         kRefineLimitWithDigIn != 0);
}

/*
 * 把 60FDh 那三位说成一句人话, 供「拒绝启扫」与「自动中止」的文案用。
 *
 * 为什么要它: bit11 置起时操作员第一件要知道的事就是**是不是真的有个开关压着** ——
 * 那是 §6 清单里第 2、3 条那两个待验证问题的现场答案。未知时说的是
 * "状态未知", **不是"都没压住"**: 后者是个看起来完全正常的结论, 而这里恰恰不该给。
 *
 * 返回 UTF-8 常量, 不返回 QString —— 这个头文件里不带 QObject 那套, 调用方自己包。
 */
inline const char *limit_switch_text(bool dig_known, bool dig_pos, bool dig_neg)
{
   if (!dig_known)
      return "60FDh 不在生效映射里, 三个开关的状态**无从得知**";
   if (dig_pos && dig_neg)
      return "60FDh 说正限位与负限位开关都压着";
   if (dig_pos)
      return "60FDh 说正限位开关压着";
   if (dig_neg)
      return "60FDh 说负限位开关压着";
   return "60FDh 说正/负限位开关都没压着 (bit11 另有来源)";
}

}   /* namespace ecatcmd */

/* ---------------------------------------------------------------- 线程 */

class EcatThread : public QThread
{
   Q_OBJECT

public:
   explicit EcatThread(QObject *parent = nullptr);
   ~EcatThread() override;

   /* ---- GUI 线程调用: 生命周期。排队给工作线程执行, 不阻塞 ---- */
   void postListAdapters();            /* 结果走 adaptersListed() 信号 */
   void postConnect(const QString &ifname);
   void postDisconnect();
   void postEnable();
   void postDisable();
   void postStop();                    /* 冻在当前位置, **保持使能** */
   void postZeroHere(int axis);        /* 把当前位置设为显示坐标 0 */
   void postCenter(int axis);          /* 走到显示坐标 0 */
   void postCenterAll();
   void postRange(int32_t range);      /* 改量程 (脉冲)。见 ecatworker.cpp 的 doRange */

   /* 清驱动器的故障位 (6040h bit7 上升沿)。
    * **不带轴参数** —— 该复位哪根是驱动器的事实 (6041h bit3), 不是操作员的选择。
    * 界面侧的闸在 ScanWindow::onFaultResetClicked(); 这里的闸见 doFaultReset()。 */
   void postFaultReset();

   /* ---- GUI 线程调用: 每周期都要用的两个量, 加锁直接写 ---- */
   void setTarget(int axis, int32_t want_disp);
   void setSpeed (int axis, uint32_t vel);

   /* ---- GUI 线程调用: 连接期参数 ----
    *
    * 下一次 em_setup 要不要把 60FDh 追加进 TxPDO (RAM only)。**默认不开**。
    * 为什么要它: 本机生效的 1A00h 只有 6041h/6064h/606Ch 三项, 60FDh 不在里面 ——
    * 三个限位开关的灯就永远是灰的。要么用厂家上位机改一次驱动器的映射 (推荐, 零代码),
    * 要么打开这个开关让主站在连接时补一次 (代价见 em_require_dig_in 的说明)。
    *
    * **改了不重连不生效**, 所以界面只在未连接时让它可改。
    */
   void setWantDigIn(bool on);
   bool wantDigIn() const;

   /* ---- 遥测 ---- */
   BusTelem telemetry() const;

   /* 收尾时未能确认失能 (**CLI 的退出码 10 就是它**) —— 必须在界面上弹模态告警 */
   bool maybeLive() const { return m_maybe_live; }

   /* 请求工作线程退出并做收尾 (关网卡)。调用方随后 wait() */
   void requestQuit();

signals:
   /* 给操作员看的一次性消息: 连接失败 / 使能失败 / 故障。别拿它做逐周期刷新 */
   void notify(const QString &text);

   /* 网卡清单 (names[i] 就是 postConnect 要的字符串)。走这里而不是让界面自己调
    * em_list_adapters: 界面侧要**一个 motor_api 函数都不调**, 那条边界才守得住 */
   void adaptersListed(const QStringList &names, const QStringList &descs);

protected:
   void run() override;

private:
   enum CmdType
   {
      CMD_LIST, CMD_CONNECT, CMD_DISCONNECT, CMD_ENABLE, CMD_DISABLE,
      CMD_STOP, CMD_ZERO, CMD_CENTER, CMD_RANGE, CMD_FAULT_RESET
   };
   struct Cmd
   {
      CmdType type  = CMD_STOP;
      int     axis  = -1;
      int32_t value = 0;      /* CMD_RANGE 用 */
      QString text;
   };

   /* 以下全部在工作线程里跑 */
   void drainCommands();
   void doListAdapters();
   void doConnect(const QString &ifname);
   void doConnectInner(const QString &ifname);   /* 见 .cpp: m_busy 由外面的壳一个人管 */
   void doEnable();
   void doFaultReset();
   void doStop();
   void doZero(int axis);
   void doCenter(int axis);
   void doRange(int32_t range);
   void tryInitOrigin();
   void interpolate(uint32_t dt_ms);
   void publish(int wkc);
   void teardown();

   void note(const QString &s);   /* 记进遥测 + 发给界面 */

   mutable QMutex m_mtx;                 /* 保护 m_cmds / m_want / m_vel / m_telem */
   QQueue<Cmd>    m_cmds;
   int32_t        m_want[EM_MAX_AXES] = {0};
   uint32_t       m_vel [EM_MAX_AXES] = {0};
   BusTelem       m_telem;
   QString        m_note;

   /* 以下只有工作线程碰, 不需要锁 */
   em_bus_t  *m_bus   = nullptr;
   em_axis_t *m_ax[EM_MAX_AXES] = {nullptr};
   int        m_naxis = 0;
   bool       m_in_op = false;
   bool       m_busy  = false;      /* 见 BusTelem::busy */
   bool       m_origin_ready = false;
   int32_t    m_origin[EM_MAX_AXES] = {0};
   int32_t    m_tgt   [EM_MAX_AXES] = {0};
   bool       m_fault_latched = false;

   /* 故障复位进行中。工作线程写、界面读 —— 和 m_busy 一样走 m_telem 那份拷贝,
    * 但这里单独存一份是为了让 doFaultReset 自己能在收尾时清干净 */
   bool       m_resetting = false;

   /* 当前量程 (脉冲)。**不是普通成员**: postRange 在工作线程里改它, 而 setTarget 在
    * GUI 线程里读它 (夹取用), interpolate 又在工作线程里读 —— 所以用原子量, 不另加锁。
    * 默认 HMI_RANGE, 于是 hmi 自己的行为一个字都不变。 */
   std::atomic<int32_t> m_range{HMI_RANGE};

   std::atomic<bool> m_quit{false};
   bool       m_maybe_live = false;

   /* 连接期参数 (见 setWantDigIn)。**只在未连接时可改**, 所以普通成员 + 一把锁就够 */
   bool       m_want_dig_in = false;
};
