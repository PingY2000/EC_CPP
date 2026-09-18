/*
 * hmi/ecatworker.h —— EtherCAT 工作线程
 *
 * **整个程序里只有这个线程碰 em_bus_t 与总线相关的 motor_api。** GUI 线程一个 motor_api
 * 函数都不调, 也不 include SOEM 的任何头文件 —— 界面侧与总线侧之间只有下面这几个
 * post 系列 / set 系列 与 telemetry()。这样 "ecx_* 必须串行" 与 "网卡单进程独占" 两条约束是
 * **结构上**成立的, 不靠调用纪律。
 *
 * 例外只有两个, 都是**说得出理由**的:
 *
 *   · main() 里那句 em_console_init(): 只设控制台代码页, 不碰总线、不碰网卡, 而它的注释
 *     要求"main() 的第一句就调"(否则第一次打日志之前的汉字是乱码)。
 *
 *   · 「停止」在回零期间直呼的 em_request_stop() (见 requestMotionStop)。
 *
 * 第二个是本版新开的。它非开不可, 而它安全的理由是**结构性**的: em_request_stop() 只往
 * 一个 `static volatile sig_atomic_t` 里存 1 —— 不碰 em_bus_t、不碰网卡、不做任何 I/O,
 * motor_api 自己就是按"信号处理器里也能调"来设计它的。非直呼不可的原因同样结构性:
 * 回零是**阻塞在工作线程里**的 (em_home 自己泵帧、自己轮询), 而命令队列要等那条命令
 * 退出来才轮到下一条 —— 一条队列命令救不了正在找原点的轴, 它得等 30 秒超时。
 *
 * 除这两个之外, 界面侧一个 motor_api 函数都不调。**这条边界是这块代码可审计的全部依据,
 * 所以宁可把它写宽并写明理由, 也不能默默违反。**
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

/* ---------------------------------------------------------------- 回零 (HM) */

/* 6099h:01 找原点速度的上下限。
 *
 * **上限就是这台程序里别的运动的同一顶天花板** (HMI_VEL_MAX)。原先写 2000 是照
 * em_home_cfg_default() 里那个"刻意保守"的值抄的 —— 但它跟这台机器自己的配置差得
 * 很远: 驱动器里**实测**的 6099h:01 = **50000**, 6083h/6084h = **500000**
 * (见 docs/ykd2205pe_ci402.md)。更要紧的是这一条: 「手动速度」+ 点画布**本来就能**
 * 以 100000 pul/s 朝同一个开关走, 所以把回零单独压到 2000 并不构成一道真实的保护,
 * 只是让这个功能没法用 (500 pul/s = 0.01 圈/秒, 30 秒走出 0.3 圈)。
 *
 * 缺省**不自己编一个吉利的数**, 用驱动器自己配的那个 6099h:01 = 50000 (1 圈/秒)。
 * 50000 是 2400h 实测的"一圈多少脉冲"。
 *
 * MIN 留在 100: **第一次在陌生机器上试方向时, 那是唯一该用的速度** (见 scan 的
 * §13 硬件清单与确认弹窗里那句"别硬顶")。
 *
 * **这几个数只定义一次**: 界面那个输入框的 setRange 与工作线程的第二道夹取都读它,
 * 于是"界面上显示的数"与"线上发的数"不可能对不上 —— 本仓库的既有规矩 (见
 * EcatThread::setTarget 的注释: 越界的目标不该只靠界面拦)。 */
#define HMI_HOME_VEL_MIN     100
#define HMI_HOME_VEL_MAX    HMI_VEL_MAX    /* = 100000 pul/s, 约 2 圈/秒 */
#define HMI_HOME_VEL_DEF    50000          /* 驱动器自己 6099h:01 的实测值, 约 1 圈/秒 */

/* 609Ah 回零加减速度的上限 = 驱动器自己那个数 (6083h/6084h 也是它, 实测 500000)。
 * 用法见下面 ecatcmd::home_accel_for() —— **不写死一个加速度, 写死一个斜坡时间**。 */
#define HMI_HOME_ACC_MAX   500000u

/* 等 6041h bit12 (Homing attained) 的上限。
 *
 * **它同时是一条"能找多远"的上限**: 速度 × 这个时间 = 一次回零最多走过的距离。
 * 按缺省速度 50000 pul/s 算 = 1500000 脉冲 = 30 圈, 而缺省扫描区域是 27 单位
 * (27 圈) —— 也就是说**缺省状态下从区域一头找到另一头也够**。
 *
 * 速度调低时能找的范围跟着缩小 (2000 pul/s 只能找 1.2 圈)。那时界面上的建议是
 * "先手动把滑台挪到开关附近", 而不是"把速度调快" —— 这条不限速的选择是刻意的:
 * 真正该防的是"找不到还不肯停", 那由这个超时兜着。 */
#define HMI_HOME_TMO_MS    30000

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
   /* 正在回零 (逐轴阻塞, 最长 HMI_HOME_TMO_MS + 收尾)。界面据此把四个回零按钮按住不动、
    * 把「失能」「设为区域中心」「开始扫描」灰掉, 以及 —— 最要紧的 —— 把「停止」
    * **换成立即中止**。见 ScanWindow::onStopClicked。
    *
    * 与 resetting 是**同一个坑的两份**: 这个字段必须由 doHome() 加锁直写一次 (阻塞期间
    * publish() 跑不到, 那份每周期拷贝到不了界面), 再由 publish() 从 m_homing 拷一份 ——
    * 两处都要, 理由与 publish() 里那段注释一字不差。 */
   bool     homing    = false;
   int      homing_axis   = -1;   /* -1 = 没在回零 */
   int      homing_method = 0;    /* 6098h 的方式号 (24/29), 只为显示给人看 */
   int      naxis     = 0;
   int      wkc       = 0;
   int      expected_wkc = 0;
   /* 当前生效的量程 (脉冲)。默认 HMI_RANGE; scan/ 会经 postRange() 改成跟它的区域匹配,
    * 界面靠它画量程、也靠它判断"我要的目标会不会被夹" */
   int32_t  range     = HMI_RANGE;
   QString  note;                /* 最后一条给操作员看的话 */

   /* 「输入电平反转 (NPN)」当前是否真的生效 —— 见 EcatThread::setDiInvert 与
    * ecatcmd::limit_rule_for。**总线级**, 因为接线方式是整台机器的性质 (X0~X3 接的是
    * 同一种传感器), 不是某一根轴的事。
    *
    * 为什么放进电文, 而不是只让界面记着自己那个勾: 措辞 (limit_switch_text /
    * limit_hit_advice) 必须说**当时真正生效**的那一种。界面以为勾上了而实际没生效,
    * 或者反过来, 都会让那几句话把人支到错的地方去 —— 而它们正是操作员唯一的线索。 */
   bool     di_invert = false;

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
 * 先钉住这一位是什么: 手册说它是「**硬件限位信号有效时置 1**」
 * --------------------------------------------------------------------------
 * ykd 手册 V2.4 (6041h 表) 的原话就是这九个字。也就是说 bit11 **不是**一个独立的
 * "撞过没有"的判断, 它报的是**限位信号当前的 level** —— 与 60FDh 的 bit0/bit1 同源
 * (都经 2300h 电平逻辑 + 2310h~2312h 功能映射), 只是各走各的路出来。
 * 两个由此而来的后果, 下面每一句措辞都得守着:
 *   · 它是**电平不是闩锁**: 压着是 1, 松开回 0。回零时它本来就该是 1
 *     (回零就是朝开关走, 见 motor_api/ec_motor_motion.c 里那句"仍在找原点");
 *     把"它置起"当成需要清除的故障去处理, 是把它认错了。
 *   · 它**不是与限位开关无关的第二道保险** —— 它就是那路信号。所以"bit11 置起而限位
 *     没压着"不是"另一层保护起了作用", 而是两个视图**不一致**, 该去查为什么
 *     (最可能: 2310h~2312h 功能码没配对, 于是 60FDh 那两位恒 0)。
 *
 * --------------------------------------------------------------------------
 * 关于下面那个开关: 若真机上"只压原点开关"也让 bit11 置起, 扫描经过原点就会**误中止**
 * --------------------------------------------------------------------------
 * 那时的修法是**把判定改细, 不是把 bit11 删掉**。改细之后:
 *
 *     撞限位 = bit11 置起 **且** 60FDh 说正限位或负限位压着   (原点不算)
 *     60FDh 未知 (不在映射里 / 丢帧) -> **退回只看 bit11** (即今天这条, 一个字没弱化)
 *
 * 它**不减保护**的理由: 区域算错 = 往限位那边发指令 -> 限位信号有效 -> bit11 与
 * 60FDh 同时为真 -> 照样中止; 而只经过原点开关时正/负限位都松开 -> 不中止, 那本来
 * 也不该中止。
 *
 * 但要说清它**加了什么**: 既然两者同源, 那个 `&&` 在功能码配对的情况下**几乎是重复
 * 的** —— 它真正买到的是一个**一致性检查**, 以及"2310h 没配对时 60FDh 那两位恒 0"
 * 这一种情形下**主动降级**的机会。**这也正是它现在不打开的原因**: 那一情形下
 * dig_known = true 而改细的判据永不成立, 保护会**静悄悄地没了** —— 比不打开更坏。
 * 所以**先按真机实测结果决定** (docs/scan_sweep.md §13 那两条待验证)。
 *
 * 打开 = 把 0 改成 1。两条分支的测试都已经写好了 —— 这个开关必须是一次**有证据**的改动,
 * 不是一次摸索。
 */
#define kRefineLimitWithDigIn 0

/*
 * ==========================================================================
 * 三条判据 —— **哪一条生效由一条纯函数 (limit_rule_for) 决定, 不散在调用点**
 * ==========================================================================
 *
 * 写成 enum 而不是几个 bool 参数, 是因为下面第二条分支的教训: 一条判据"开没开"这件事
 * 一旦由多个开关各自拼出来, 就会出现"勾了某个框, 而判定走的是另一条路"这种
 * **界面上看不出来**的组合。三条各是一个完整语义, 选哪条只有 limit_rule_for 一处说了算。
 */
enum LimitRule
{
   LIMIT_RULE_BIT11   = 0,   /* 6041h bit11 单独判定 —— 出厂默认, 也是所有退路 */
   LIMIT_RULE_REFINED = 1,   /* bit11 **且** 60FDh 说正/负限位压着 (原点不算) */
   LIMIT_RULE_INVERT  = 2    /* 输入已反相(NPN): 只看反相后的两个限位开关, 未知则中止 */
};

/*
 * 哪条判据生效。**只此一处**。
 *
 *   · 反转开着 -> LIMIT_RULE_INVERT。理由见下面 limit_hit_rule 里那条分支:
 *     2300h 配反的机器上 bit11 恒为 1, 它携带的信息量是零, 保护只能改由反相之后的
 *     那两个真实开关承担。
 *   · 否则看 kRefineLimitWithDigIn 那个编译期开关 (理由见上面 limit_hit_rule 那一大段)。
 */
inline LimitRule limit_rule_for(bool di_invert)
{
   if (di_invert)
      return LIMIT_RULE_INVERT;
   return (kRefineLimitWithDigIn != 0) ? LIMIT_RULE_REFINED : LIMIT_RULE_BIT11;
}

/* 判据本体。规则**用参数传** —— 于是三条分支在同一次构建里全部可测 (scan/selftest.cpp),
 * 而不是"要改一个 #define 重编一次才能验另一条"。安全关键的逻辑必须能一次测完。 */
inline bool limit_hit_rule(uint16_t sw, bool dig_known, bool dig_pos, bool dig_neg,
                           LimitRule rule)
{
   const bool bit11 = (sw & EM_SW_INTLIMIT) != 0;

   switch (rule)
   {
   case LIMIT_RULE_BIT11:
      return bit11;

   case LIMIT_RULE_REFINED:
      /* 未知就退回 bit11 单独判定 —— 这一条是"不弱化"的保证 */
      if (!dig_known)
         return bit11;
      return bit11 && (dig_pos || dig_neg);

   case LIMIT_RULE_INVERT:
      /*
       * 反转生效。**bit11 完全不参与** —— 这与上面两条的写法不同, 是故意的:
       *
       *   这台机器 (2026-09-18 实测 2300h = 0 而传感器是 NPN) 上 bit11 **恒为 1**,
       *   它已经不是"限位有没有压着"的判据了。要是还按 `bit11 && (...)` 写,
       *   在 bit11 恒 1 时它恰好退化成 `(...)` 而在这一点上看着也对 —— 但只要哪天
       *   有人把 2300h 改对了而反转忘了关, bit11 一变 0 就会把整条判据**恒置为 false**,
       *   保护静悄悄地全没。不参与, 就没有这个耦合。
       *
       * 保护改由**反相之后的两个限位开关**承担。那正是"反转"这个动作的完整语义:
       * 把限位判定从"驱动器的意见"换成"开关的真实状态"。
       *
       * 读不到 60FDh -> **一律中止**。这三条判据里只有这一条把"未知"判成"中止",
       * 因为此时**没有别的判据可退** (bit11 已经不用了, 开关又读不到 —— 退无可退,
       * 那就不动)。至于它会不会在扫描中误触发: 不会。dig_known 里的 mirror_ok 是
       * **闩锁**的 (em_dig_in_known = off_dig_in >= 0 && mirror_ok, 而 mirror_ok 只在
       * 轴初始化时清零, 收到过一帧完整帧之后一直是 1) —— 所以这一支只可能在连接之后、
       * 第一帧完整过程数据到达**之前**命中, 而那时候本来就该什么都不让干。
       */
      if (!dig_known)
         return true;
      return dig_pos || dig_neg;
   }

   return bit11;   /* 不可达; 留着是为了 -Wswitch 之外也能有一个确定返回值 */
}

inline bool limit_hit(uint16_t sw, bool dig_known, bool dig_pos, bool dig_neg,
                      bool di_invert)
{
   return limit_hit_rule(sw, dig_known, dig_pos, dig_neg, limit_rule_for(di_invert));
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
inline const char *limit_switch_text(bool dig_known, bool dig_pos, bool dig_neg,
                                     bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "60FDh 不在生效映射里, 开关状态**无从得知** —— 而「输入电平反转」开着, "
           "反相拿到的是一堆 0, 等于一条判据都没有"
         : "60FDh 不在生效映射里, 三个开关的状态**无从得知**";

   /*
    * 正负限位**同时**读成压着, 这一条物理上不成立 —— 滑台不可能同时在两头。
    * 真机上 2026-09-18 出现过, 现场是 X0~X3 接的是 **NPN 传感器 (高电平表示未触发)**,
    * 而 2300h (输入有效电平逻辑) 按常开配着 —— 于是"没触发"被读成"触发",
    * 两个限位常年都是压着的。
    *
    * 为什么这句话必须写在**这里**: 原来的文案是一句"两个都压着"就完了, 而它长得和
    * "真撞在限位上"一模一样, 操作员根本看不出这是个配置问题 —— 现场的反应正是
    * 反复去手动走离限位, 而那个限位压根不存在。这种"现象相同、成因相反"的地方,
    * 措辞必须把人往正确的方向推 (与下面 neither-pressed 那一条同一个道理)。
    *
    * ---- 反转开着时这条现象的含义**整个变了** (2026-09-18 之后) ----
    *
    * 反转开着, 上面这个 dig_pos/dig_neg 已经是**反相之后**的值。两路同时为真意味着
    * 60FDh 的 bit1/bit0 同时为 0, 也就是驱动器在两个输入端子上都读到**低电平**。
    * 而 NPN 传感器低电平 = **已触发** —— 所以这不是极性配置问题 (极性错只会让两边
    * 一起反相, 反相完就正常了), 而是两路真的都被拉到了低: 掉电 / 输出对 0V 短路 /
    * 两路接串。往 2300h 上找是找不到的, 所以两种语境必须是两句不同的话。
    */
   if (dig_pos && dig_neg)
      return di_invert
         ? "反相**之后**正限位与负限位仍然**同时**报压着 —— 极性已经不是原因了 "
           "(极性错只会让两边一起反相, 反相完就该松开)。这是两路输入**同时**被读到"
           "低电平: 查传感器供电、输出有没有被拉到 0V、两路是不是接串了"
         : "60FDh 说正限位与负限位开关**同时**都压着 —— 滑台不可能同时在两头, "
           "所以这一条多半不是真的。最可能的原因是 2300h (输入有效电平逻辑) "
           "与接线不符: NPN 传感器高电平表示**未**触发, 驱动器就得按常闭认 "
           "(2300h 里对应的位置 1), 按常开配 (0) 会把「没触发」读成「触发」, "
           "两个限位于是常年都报压着 (本程序里的「输入电平反转」也能治同一个病, "
           "但它只治上位机这一侧, 驱动器的 bit11 与它自己的限位保护不受影响)";

   if (dig_pos)
      return di_invert ? "反相之后 60FDh 说正限位开关压着"
                       : "60FDh 说正限位开关压着";
   if (dig_neg)
      return di_invert ? "反相之后 60FDh 说负限位开关压着"
                       : "60FDh 说负限位开关压着";
   return di_invert ? "反相之后 60FDh 说正/负限位开关都没压着"
                    : "60FDh 说正/负限位开关都没压着 (**与 bit11 不一致** —— 两者本该是"
                      "同一路限位信号的两个视图; 但若 2310h~2312h 功能码没配对, 这两位"
                      "恒为 0, 那它就不足以推翻 bit11)";
}

/*
 * bit11 置起时**该去做什么**。和上面那句分开, 因为"是什么状态"与"接下来查哪儿"
 * 是两回事, 而这两件事的答案会随 60FDh 那一行变。
 *
 * 为什么要它: 原来三个调用点各写各的, 而且写的都是同一句"先手动把它走离限位" ——
 * 那句话只在"真有个限位压着"时成立。其余几种情况下它是**把人支到错的地方去**:
 * 没有限位可走离, 而人会在那儿反复试。这里合到一处, 与 limit_switch_text 同一个道理。
 *
 * 措辞里**不许出现"已撞上"这种话** —— bit11 是 CiA402 的 "internal limit active",
 * 驱动器拿它表示什么本机只实测过一部分。我们**知道的**只有 6041h 报了这位、60FDh 说了
 * 什么; 剩下的说成"多半/可能"。把可能讲成就是, 操作员会去处理一个不存在的问题。
 */
inline const char *limit_hit_advice(bool dig_known, bool dig_pos, bool dig_neg,
                                    bool dig_home, bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "「输入电平反转」开着, 而 60FDh 读不到。反转生效时 bit11 **不参与判定**, "
           "所以此时一条判据都没有 —— 限位信号一律按「有效」中止, 扫描**永远开不了**。"
           "两条路挑一条: "
           "把 60FDh 弄进映射 (勾「让 60FDh 进 TxPDO」再重新「连接」, 或用厂家上位机改一次 "
           "1A00h), 或者把这个反转关掉、退回只看 bit11"
         : "读不到 60FDh, 所以**分不清是原点还是限位**。让 60FDh 可读就能分清: "
           "勾上「让 60FDh 进 TxPDO」再重新「连接」(或用厂家上位机改一次 1A00h) —— "
           "这一步只读不写, 零代价";

   if (dig_pos && dig_neg)
      return di_invert
         ? "反相之后两个限位仍然同时压着, 所以**不是极性的事** (见上一条)。查传感器供电"
           "与接线 —— 两路被同时读到低电平, 多半是没上电、输出被拉到 0V、或两路接串了"
         : "先别去走离限位 —— 正负限位**同时**压着, 那个限位不存在。查 2300h 的输入"
           "有效电平逻辑与 X0~X3 接线是否一致 (NPN 传感器高电平表示未触发, 该按常闭认), "
           "再顺手看一眼 607Dh:01/:02 软限位是不是 0/0 —— 这两个都只读, 不改任何东西。"
           "界面上的「输入电平反转」也能立刻解开上位机这一侧, 但它**只治软件**: "
           "驱动器自己的 bit11 与限位保护不受影响, 所以能改 2300h 还是先改它";

   if (dig_pos || dig_neg)
      return di_invert
         ? "反相之后的读数是可信的 (反转开着时判定用的就是它), 所以**真的有个开关压着** —— "
           "先手动把它走离。走离之后灯要是还亮着, 那就是那一路的接线或传感器本身的问题"
         : "先手动把它走离压着的那个开关。若确认没有开关压着而这一位仍然置起, "
           "那就不是接线问题 —— 查 607Dh:01/:02 软限位是不是 0/0";

   if (dig_home)
      return "**正/负限位都没压着, 压着的是原点开关。** 滑台要是正停在原点附近, 这一位"
             "多半就是这么来的 —— 但「任何开关压着就置起」这件事本机还没实测过, "
             "所以先按「它真的会让扫描中止」对待";

   return di_invert
      ? "反相之后正/负限位都没压着 —— 而反转开着时 bit11 **不参与判定**, 按理这一位"
        "不该让扫描停下来。走到这里说明命中的是「60FDh 未知」那一支 (连接之后还没收到过"
        "完整的过程数据帧), 先把连接重做一次"
      : "正/负限位都没压着, 而 bit11 置起了 —— 按手册, 这一位报的就是**硬件限位信号**"
        "有效, 而那两位是同一路信号经 2300h + 2310h 之后的结果, 所以这是个**自相矛盾**的"
        "读数。先查 2310h~2312h 的功能码对不对 (60FDh 说没压着, 会不会正是因为功能码被"
        "改成 0 了), 再查 2300h 与接线。607Dh 软限位是 CiA402 一般定义里的另一路可能"
        "来源, 本机实测它是开着的 (不是 0/0), 所以多半不是它; `motor_test` 不带参数跑"
        "一次会把 607Dh:01/:02 与 6064h 一起打出来";
}

/*
 * 「这一位是怎么回事」的开场白, 带一个 %1 = 轴号。拒绝启扫与自动中止都用它。
 *
 * 为什么要它: 这两处从前各写各的"6041h bit11 置起", 而**反转开着时那句是错的** ——
 * LIMIT_RULE_INVERT 那一支根本不看 bit11, 此时 limit_active 为真与 bit11 毫无关系。
 * 让操作员去看一个决定不了任何事的位, 是最费时间的那种误导。判据换了, 开场白就得跟着换。
 */
inline const char *limit_hit_headline(bool di_invert)
{
   return di_invert
      ? "轴%1 的限位判据成立 —— 「输入电平反转」开着, 此刻的判据是**反相之后的**"
        "正/负限位开关, 与 6041h bit11 无关"
      : "轴%1 的 6041h bit11 置起 —— 手册对这一位的定义是「**硬件限位信号有效**」";
}

/*
 * 为什么开场白说的是"硬件限位信号有效", **不是**"驱动器认为它撞在硬件限位上":
 *
 * ykd 手册 V2.4 (6041h 表) 对这一位的原话就是 **"硬件限位信号有效时置 1"** ——
 * 它报的是**同一路限位信号的当前电平** (与 60FDh 的 bit0/bit1 同源, 都是经 2300h
 * 电平逻辑与 2310h~2312h 功能映射之后的结果), 不是驱动器的"判断", 更不是"撞上了"。
 *
 * 这个区别不是抠字眼, 它有两个实际后果:
 *   · **它是电平, 不是闩锁。** 压着就是 1, 松开就回 0 —— 所以 2026-09-18 那台机器
 *     上它恒为 1 恰恰说明**限位信号一直是"有效"的**, 与我们量到的 60FDh bit0/bit1
 *     同时为 1 完全自洽。要是把它当成一个"曾经撞过"的锁存故障, 现场就会去清故障、
 *     去重新使能 —— 全是白费。
 *   · **找原点时它本来就该是 1。** motor_api/ec_motor_motion.c 的回零流程里那句
 *     `"[..] bit11 硬件限位有效, 仍在找原点 ..."` 就是这个意思: 回零是**朝开关走**,
 *     走到就是有效。拿"bit11 置起"当异常去处理, 回零就没法做了。
 *
 * 所以措辞必须落在**信号**上。至于"这一位该不该挡住扫描", 那是策略问题, 由
 * limit_hit_rule 那三条判据回答 —— 两件事分开。
 */

/* ==========================================================================
 * 回零 (驱动器自带的 HM 模式)
 * ==========================================================================
 *
 * 和上面那一段同一个理由放在头文件里: scan_selftest 编 busview.h (它 include 本文件)
 * 却**不编 ecatworker.cpp** —— 凡是能判的放这里, 才验得到。真正碰总线的那个
 * doHome() 验不了, 它是硬件清单上的东西 (见 docs/scan_sweep.md §13)。
 */

/* 轴的中文名。轴号是硬约定: 0 = X, 1 = Y (scan 只接受 naxis == 2)。 */
inline const char *axis_label(int i)
{
   return (i == 0) ? "轴X" : "轴Y";
}

/* ---- 方向 -> 6098h 方式 --------------------------------------------------
 *
 * 24 = 原点开关 (X0) 为原点, **正向**高速先找
 * 29 = 原点开关 (X0) 为原点, **反向**高速先找
 * (docs/ykd2205pe_ci402.md 的回零方式表)
 *
 * **不用 35**: 它是"以当前位置为机械原点", 不是"去找"。按钮上写的是"找原点",
 * 按下去却把当前位置认作原点 —— 那是另一个动作, 该有另一个按钮。
 *
 * 另外要说清一句: 这里的"正/反"是**电机轴的正反向**, 与画布上 +X/+Y 是不是同一个
 * 方向, 只有现场试一次才知道 (em_home_cfg_default 那段注释里"方向不对就换"说的就是
 * 这个)。所以界面措辞不能写成"+X 方向"。 */
inline int home_method_for(bool negative) { return negative ? 29 : 24; }

inline const char *home_dir_text(bool negative) { return negative ? "反向" : "正向"; }

/* ---- 6099h:02 由 6099h:01 派生 ------------------------------------------
 * 与 motor_test.c 收紧 --vel 时用的那个式子一样: vel_slow = vel_fast / 4。
 * 下限 1: 写 0 的语义手册没写, 而"返回速度是 0"绝不该是它的意思。 */
inline uint32_t home_vel_slow(uint32_t vel_fast)
{
   uint32_t s = vel_fast / 4u;
   return s ? s : 1u;
}

/* ---- 609Ah 回零加减速 ----------------------------------------------------
 *
 * **不写死一个加速度, 写死一个斜坡时间。**
 *
 * 原先写死 5000: 配上 2000 pul/s 是 0.4 秒斜坡, 看着没问题; 可是把速度放开到
 * 100000 就成了 **20 秒**斜坡 —— 一次回零整个花在加速上, 操作员把速度调上去
 * 却看不出任何区别。这是"两个数各自看都对、配在一起就错"的典型, 所以速度与
 * 加速度**必须一起定**, 不能各写各的。
 *
 * 斜坡时间取 **0.1 秒**, 这不是编的 —— 驱动器自己那一对实测值就是
 *     6099h:01 = 50000 与 609Ah = 500000   =>   50000 / 500000 = 0.1 秒
 * 于是这条函数在 50000 及以下**原样复现机器自己的斜坡**, 而在那之上封顶于
 * HMI_HOME_ACC_MAX (= 器件自己那个 500000, 6083h/6084h 也是它): 100000 pul/s 时
 * 斜坡变长到 0.2 秒, 而不是加速度继续翻倍 —— **这个程序里没有哪一处该比机器
 * 自己配的加速度更硬**。
 *
 * 传入的必须是**已经夹过**的 vel_fast (见 home_vel_clamp), 它才 <= HMI_VEL_MAX。
 * 那个除法守卫是给"万一没夹"留的: 直接乘 10 会在 429496729 以上回绕, 于是一个
 * 很大的速度会算出一个很小的加速度 —— 正是最难看出来的一种错。 */
inline uint32_t home_accel_for(uint32_t vel_fast)
{
   if (vel_fast > HMI_HOME_ACC_MAX / 10u)
      return HMI_HOME_ACC_MAX;
   return vel_fast * 10u;
}

/* ---- 第二道夹取 ----------------------------------------------------------
 * 第一道是界面上那个 spin box 的 setRange。同 EcatThread::setTarget 的规矩:
 * "界面也会夹一次, 这里是第二道" —— 越界的速度不该只靠界面拦。 */
inline uint32_t home_vel_clamp(int32_t v)
{
   if (v < HMI_HOME_VEL_MIN) return HMI_HOME_VEL_MIN;
   if (v > HMI_HOME_VEL_MAX) return HMI_HOME_VEL_MAX;
   return (uint32_t)v;
}

/* ---- 回零之前那道闸 ------------------------------------------------------
 * 返回 nullptr = 可以发起; 否则是一句给操作员看的话 (调用方自己加"轴X: ")。
 *
 * **这道闸必须在任何写动作之前** —— 与 axis_needs_reset 是同一条先例, 而这里更硬:
 * doHome() 要做的第一件事是 em_disable(), 它**真的会撤掉保持力矩** (竖直轴当场会滑)。
 * 等 em_home() 自己去发现"不该做", 力矩已经撤了。
 *
 * 好消息是 em_home() 自己没有这个毛病 —— 它的四条拒绝 (已使能 / 无完整帧 / 有故障 /
 * 方式越界) 全都排在第一次写之前。危险的是我们外围那圈收尾。
 *
 * 传入的必须是**刚读到的**状态, 不是上一轮发布的遥测快照 (同 doFaultReset 的规矩)。
 *
 * 判据顺序是有讲究的:
 *   mirror_ok 排在 fault **前面** —— 一个从来没收到过的状态字里的 bit3 不是信息,
 *   说"它有故障"是在猜。同 axis_needs_reset 编码的那条规矩。
 *   any_axis_moving 放最后 —— 前几条"根本不能做", 这条"现在不能做"。 */
inline const char *home_refusal(bool bus_ready, bool origin_ready, bool mirror_ok,
                                bool fault, bool any_axis_moving)
{
   if (!bus_ready)
      return "还没连上总线 (或没进 OP)";
   if (!origin_ready)
      return "一笔完整的 6064h 都还没取到 -> 位置未知, 拒绝回零";
   if (!mirror_ok)
      return "这一根没收到过完整帧 -> 6041h 状态未知, 拒绝回零";
   if (fault)
      return "6041h bit3 = Fault —— 先点「故障复位」";
   if (any_axis_moving)
      return "还有轴在走 —— 回零期间插补器是停的, 那根轴会停在半途。"
             "先点「停止」让所有轴都停稳";
   return nullptr;
}

/* ---- 收尾之后, 把结果说成一句人话 ----------------------------------------
 *
 * 收尾那三步 (失能 -> 切回 CSP -> 重新使能) 各有各的失败方式, 组合起来有好几种情形,
 * 有的要人立刻去断动力电源, 有的只要重新点一下「使能」。这段判断留在 .cpp 里就永远
 * 验不到, 所以搬进来。
 *
 * **判据是收尾结束那一刻的实测状态, 不是三个返回码的排列组合。** 后者会把
 * "失能返回非 0, 但其实已经卸力了" (bit3 故障时的常见情形) 说成"可能仍带电" ——
 * 那句话会让人去拉总闸, 而真正该做的是点「故障复位」。
 */
enum HomeEnd
{
   HOME_END_NEVER_STARTED = 0,  /* 闸就拦下了, 一个字节都没写 */
   HOME_END_FAULTED,            /* 6041h bit3 还在 -> 先「故障复位」 */
   HOME_END_STRANDED,           /* 没故障, 但没能走到"已使能 + 已是 CSP" -> 状态不明 */
   HOME_END_HOLDING             /* 已使能 + 已是 CSP -> 停在落点, 有保持力矩 */
};

inline HomeEnd home_end_state(bool started, bool fault_now, bool end_enabled, int rc_mode)
{
   if (!started)
      return HOME_END_NEVER_STARTED;

   /* 故障排在使能前面: 按 CiA402 "bit3 置起"与"bit2 带电"不该同时成立, 而万一真同时
    * 读到, 该报的是故障 —— 那才是要人动手的那一件事。 */
   if (fault_now)
      return HOME_END_FAULTED;

   /* rc_mode != 0 = 没能确认切回 CSP。**这一格不能说成 HOLDING** —— 那时轴可能确实
    * 带电, 但驱动器不按 CSP 解释 607Ah, 而 interpolate() 每周期都在往 607Ah 里写。
    * "带力矩停着"和"带电但模式不对"是两件事, 混起来就是一句谎。 */
   if (end_enabled && rc_mode == 0)
      return HOME_END_HOLDING;

   return HOME_END_STRANDED;
}

/* rc 用字面量比较: ecatworker.cpp 刻意不 include ec_motor_internal.h (见 doFaultReset
 * 那段注释), 所以 EM_R_OK / EM_R_STOP 在这里没有名字, 只有 0 / 1 / 其它。 */
inline const char *home_cause_text(int rc)
{
   if (rc == 0)
      return "到位";
   if (rc == 1)
      return "被「停止」中止";
   return "失败 —— 方向不对就换另一个方向按钮试试; 找不到原点开关, "
          "就先手动把滑台挪到开关附近再回零 (别硬顶)";
}

inline const char *home_end_text(HomeEnd e)
{
   switch (e)
   {
      case HOME_END_NEVER_STARTED:
         return "没有发起 (闸拦下了, 一个字都没写)";
      case HOME_END_FAULTED:
         return "该轴报了故障 (6041h bit3), 现在停在未使能 —— 处理完现场后点「故障复位」";
      case HOME_END_STRANDED:
         return "收尾没能确认到「已使能 + CSP」 —— **电机可能仍带电**, "
                "看控制台里是哪一步失败; 拿不准就断开驱动器的动力电源";
      case HOME_END_HOLDING:
         return "已切回 CSP 并保持使能, 停在落点带保持力矩";
   }
   return "";
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

   /* 回零 —— 驱动器自带的正/反向找原点。axis: 0/1; method: 只收 24/29
    * (用 ecatcmd::home_method_for 算, 别自己写数); vel_fast: 6099h:01, 内部还夹一道。
    *
    * **这是本程序里最长的一条阻塞命令**: 工作线程会在里面待满整个回零过程
    * (最长 HMI_HOME_TMO_MS + 收尾那几秒)。期间遥测靠 doHome 里那两次加锁直写活着。
    *
    * **结束状态**: 不论到位、被中止还是失败, 收尾都会把它带回
    * "CSP + 已使能 + 停在落点 + 显示坐标已把落点当做 0"。所以调用方**必须**
    * 同时把零点世代 +1 (见 ScanWindow::onHomeClicked) —— 那件事 GUI 做,
    * 因为只有它知道"派发过了", 而工作线程的闸可能根本没让它跑。
    *
    * ⚠️ 它会**先失能**: 6098h/6099h/609Ah/607Ch 只能在未使能时写。竖直轴会在这段
    * 时间里失去保持力矩。这件事躲不掉, 只能每次都告诉操作员。 */
   void postHome(int axis, int method, uint32_t vel_fast);

   /* 「停止」在回零期间用这一个 —— **立即**让 em_home 的轮询看见 (≤ 它的 2ms 轮询周期),
    * 由它撤掉 6040h bit4 把轴停住。
    *
    * 这是全程序**唯一**一处 GUI 线程直呼 motor_api (见文件顶部那条边界的第二个例外)。
    * 安全的理由是结构性的: em_request_stop() 只往一个 `static volatile sig_atomic_t`
    * 里存 1 —— 不碰 em_bus_t、不碰网卡、不做 I/O。
    *
    * **为什么不能走 postStop()**: 队列是在工作线程顶部排空的, 而回零正阻塞着那个线程
    * —— 一条 CMD_STOP 要等回零自己退出来才轮到。那条路救不了正在找原点的轴。
    *
    * 空闲时调它也**无害**: motor_api 那个标志是粘住的, 但 drainCommands() 每出队一条
    * 命令都会 em_clear_stop() 一次 (见那里的注释), 所以它活不过下一条命令的开头。 */
   void requestMotionStop();

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

   /* ---- GUI 线程调用: 运行期参数 ----
    *
    * 「输入电平反转 (NPN)」: X0~X3 接的是 NPN 传感器 (高电平 = **未**触发), 而驱动器的
    * 2300h (输入有效电平逻辑) 按常开配着时, 60FDh 那三位是**整排反相**的 ——
    * 打开这个开关, 把原点/正限位/负限位三个一起翻回来。
    *
    * **默认关, 而且不持久化** (与 setWantDigIn 同一个先例, 理由见 scan/scanprefs.h):
    * 反转是"这台机器的线是这么接的"的一条断言, 而断言错了的后果不是"灯显示不对",
    * 是**保护反过来** (真压着限位时它说没压着)。这种东西必须每次由人当面确认,
    * 不能从一个 ini 里悄悄继承 —— 换台机器, 或者哪天 2300h 被改对了, 继承下来的
    * 那个"开"就是一个已经静悄悄失效的保护。
    *
    * 与 setWantDigIn 的另一个区别: 它**是运行期参数**, 随时可改, 下一帧 publish()
    * 就生效 (所以用原子量, 不走命令队列)。打开它的完整语义见 ecatcmd::limit_rule_for:
    * **同时**把限位判据换掉 (bit11 不再参与) —— 这两件事是一件, 不是一个勾加一个开关。
    */
   void setDiInvert(bool on);
   bool diInvert() const;

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
      CMD_STOP, CMD_ZERO, CMD_CENTER, CMD_RANGE, CMD_FAULT_RESET, CMD_HOME
   };
   struct Cmd
   {
      CmdType type   = CMD_STOP;
      int     axis   = -1;
      int     method = 0;     /* CMD_HOME 用: 6098h 方式号 (24/29) */
      int32_t value  = 0;     /* CMD_RANGE 用; CMD_HOME 用它装 6099h:01 */
      QString text;
   };

   /* 以下全部在工作线程里跑 */
   void drainCommands();
   void doListAdapters();
   void doConnect(const QString &ifname);
   void doConnectInner(const QString &ifname);   /* 见 .cpp: m_busy 由外面的壳一个人管 */
   void doEnable();
   void doFaultReset();
   void doHome(int axis, int method, uint32_t vel_fast);
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

   /* 回零进行中 (同上, 见 BusTelem::homing) */
   bool       m_homing    = false;
   int        m_homing_axis   = -1;
   int        m_homing_method = 0;

   /* 当前量程 (脉冲)。**不是普通成员**: postRange 在工作线程里改它, 而 setTarget 在
    * GUI 线程里读它 (夹取用), interpolate 又在工作线程里读 —— 所以用原子量, 不另加锁。
    * 默认 HMI_RANGE, 于是 hmi 自己的行为一个字都不变。 */
   std::atomic<int32_t> m_range{HMI_RANGE};

   std::atomic<bool> m_quit{false};
   bool       m_maybe_live = false;

   /* 连接期参数 (见 setWantDigIn)。**只在未连接时可改**, 所以普通成员 + 一把锁就够 */
   bool       m_want_dig_in = false;

   /* 运行期参数 (见 setDiInvert)。**UI 线程随时可改、工作线程每帧读**, 所以是原子量 ——
    * 它不值得为了一条 bool 去走 m_cmds 那条命令队列 (那还要等一个周期才生效) */
   std::atomic<bool> m_di_invert{false};
};
