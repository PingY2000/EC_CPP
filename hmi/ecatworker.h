/* hmi/ecatworker.h —— EtherCAT 工作线程
 * 全程序只有这个线程碰 em_bus_t 与总线相关的 motor_api; GUI 线程不调 motor_api, 也不
 * include SOEM。坐标: 驱动器坐标 = 6064h 原始值; 显示坐标 = 驱动器坐标 - 零点 origin。 */
#pragma once

#include <QMutex>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>

#include "ec_motor.h"

/* 工作范围默认值: ±500000 脉冲 (50000 pul/圈 => ±10 圈, 2400h 实测 = 50000)。
 * 真正的量程是 m_range, 可经 postRange() 改 —— scan/ 会让它跟着扫描区域走。 */
#define HMI_RANGE       500000

#define HMI_VEL_MIN       1000   /* pul/s, 约 0.02 圈/秒 */
#define HMI_VEL_MAX     100000   /* pul/s, 约 2 圈/秒 */
#define HMI_VEL_DEF      20000

#define HMI_CYCLE_US      2000   /* 过程数据周期 (µs), 与 motor_test 缺省一致, 不上 DC */
#define HMI_LOOP_MS          2   /* 循环里的让步节拍 */
#define HMI_STOP_MS        300   /* 进近段留出的刹停时间: 减速度 = v / 0.3s */

/* 6099h:01 找原点速度的上下限。
 * 缺省 50000 = 驱动器 6099h:01 的实测值 (2400h 实测一圈 50000 脉冲), 约 1 圈/秒;
 * MIN 100 是第一次在陌生机器上试方向该用的速度。界面 setRange 与工作线程的夹取都读它。 */
#define HMI_HOME_VEL_MIN     100
#define HMI_HOME_VEL_MAX    HMI_VEL_MAX    /* = 100000 pul/s, 约 2 圈/秒 */
#define HMI_HOME_VEL_DEF    50000          /* 驱动器自己 6099h:01 的实测值, 约 1 圈/秒 */

/* 609Ah 回零加减速度的上限 = 6083h/6084h 的实测值 (500000)。
 * 不写死一个加速度, 写死一个斜坡时间 —— 见 ecatcmd::home_accel_for()。 */
#define HMI_HOME_ACC_MAX   500000u

/* 等 6041h bit12 (Homing attained) 的上限, 同时是"一次回零最多能找多远"的上限:
 * 速度 × 这个时间 = 最远距离 (缺省 50000 pul/s => 1500000 脉冲 = 30 圈)。 */
#define HMI_HOME_TMO_MS    30000

/* AxisTelem::mode_disp 里"工作线程还没为这根轴读过 6061h"的哨兵值。
 * 不能拿 -1 兼这个语义 (em_get_mode 读失败也返回 -1), 也不能借 0 (合法模式号: 未定义)。 */
#define HMI_MODE_DISP_UNREAD  (-2)

/* 一根轴的一帧快照。全部是显示坐标。 */
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
    * dig_known 单独一个字段: 60FDh 多半不在生效 TxPDO 里, 读不到时那三个 bool 也全是
    * false, 而"三个都没压住"在界面上是个看起来完全正常的结论 —— 两者必须分得开。 */
   bool     dig_known = false;   /* em_dig_in_known(): 映射里有**且**收到过完整帧 */
   bool     dig_home  = false;   /* bit2 原点开关 */
   bool     dig_pos   = false;   /* bit1 正限位 */
   bool     dig_neg   = false;   /* bit0 负限位 */

   /* 撞限位 —— **会不会中止扫描**。在 publish() 里由 ecatcmd::limit_hit() 一处算出 */
   bool     limit_active = false;

   /* 驱动器自报的实际运行模式 6061h (em_get_mode), 或 HMI_MODE_DISP_UNREAD / -1 (读失败)。
    * 手册 §3.7 把「6061h 读回 6」当作 HM 的前提。它不是每周期刷新的: 6061h 不在 TxPDO 里,
    * 只能 SDO 读, 而 publish() 只读过程数据镜像 —— 由工作线程在连接 / 使能 / 回零收尾时读。 */
   int      mode_disp = HMI_MODE_DISP_UNREAD;
};

struct BusTelem
{
   bool     connected = false;
   bool     in_op     = false;
   /* 正在做连接/收尾 (SDO、状态机迁移, 会阻塞几秒); 不要用"点过连接"来推 */
   bool     busy      = false;
   bool     fault     = false;
   /* 正在做故障复位 (逐轴阻塞, 每轴最多 1 秒)。复位不可中断, 界面只能把它按住不动 */
   bool     resetting = false;
   /* 正在回零 (逐轴阻塞, 最长 HMI_HOME_TMO_MS + 收尾)。界面据此把四个回零按钮按住、
    * 把「停止」换成立即中止。与 resetting 同一个坑: 必须由 doHome() 加锁直写一次,
    * 再由 publish() 从 m_homing 拷一份, 两处都要。 */
   bool     homing    = false;
   int      homing_axis   = -1;   /* -1 = 没在回零 */
   int      homing_method = 0;    /* 6098h 的方式号 (24/29/18/17), 只为显示给人看 */
   int      naxis     = 0;
   int      wkc       = 0;
   int      expected_wkc = 0;
   /* 当前生效的量程 (脉冲)。默认 HMI_RANGE; scan/ 会经 postRange() 改 */
   int32_t  range     = HMI_RANGE;
   QString  note;                /* 最后一条给操作员看的话 */

   /* 「上位机侧取反」当前是否真的生效。**总线级**: 接线方式是整台机器的性质。
    * 放进电文是因为措辞必须说当前真正生效的那一种, 说错了会把人支到错的地方去。 */
   bool     di_invert = false;

   AxisTelem ax[EM_MAX_AXES];
};

/* 命令层的纯判据。刻意 inline 放在头文件: scan_selftest 只编 selftest.cpp +
 * SCAN_COMMON_SRC, 不编 ecatworker.cpp —— 写在 .cpp 里就永远验不到。 */
namespace ecatcmd
{

/* 这一根轴该不该做故障复位 —— 全部安全性压在这一行上。
 * em_fault_reset() 先写 6040h = 0x0000 (卸力) 打十帧再抬 bit7, 对一根没有故障的轴做这件事
 * 会真的卸力 (竖直滑台会掉下来)。复位对象由 6041h bit3 决定; mirror_ok = false 跳过不猜。 */
inline bool axis_needs_reset(bool valid, bool mirror_ok, bool fault)
{
   return valid && mirror_ok && fault;
}

/* 从一份遥测里挑出该复位的轴 (返回条数, out 里是轴号); 返回 0 = 一个字节都不该写 */
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

/* 撞限位 —— **只此一处定义**, 控制器 / 参数栏 / 画布都读它算出来的那一个字段。
 * 今天这条 = 6041h bit11 单独判定。手册 V2.4 (6041h 表): bit11 = "硬件限位信号有效时置 1",
 * 它是电平不是闩锁, 与 60FDh 的 bit0/bit1 同源。打开下面这个开关 = 再加 "且 60FDh 说压着"。 */
#define kRefineLimitWithDigIn 0

/* 三条判据 —— 哪一条生效由一条纯函数 (limit_rule_for) 决定, 不散在调用点。
 * 写成 enum 而不是几个 bool 参数: 判据"开没开"由多处开关各拼一次时, 会出现界面上看不出来的组合。 */
enum LimitRule
{
   LIMIT_RULE_BIT11   = 0,   /* 6041h bit11 单独判定 —— 出厂默认, 也是所有退路 */
   LIMIT_RULE_REFINED = 1,   /* bit11 **且** 60FDh 说正/负限位压着 (原点不算) */
   LIMIT_RULE_INVERT  = 2    /* 输入已反相(NPN): 只看反相后的两个限位开关, 未知则中止 */
};

/* 哪条判据生效。**只此一处**。反转开着 -> INVERT: 2300h 配反的机器上 bit11 恒为 1,
 * 它携带的信息量是零, 保护只能由反相之后那两个真实开关承担。 */
inline LimitRule limit_rule_for(bool di_invert)
{
   if (di_invert)
      return LIMIT_RULE_INVERT;
   return (kRefineLimitWithDigIn != 0) ? LIMIT_RULE_REFINED : LIMIT_RULE_BIT11;
}

/* 判据本体。规则用参数传 —— 三条分支在同一次构建里全部可测 (scan/selftest.cpp) */
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
      /* 反转生效, bit11 **完全不参与** (这台机器上 2300h = 0 而传感器是 NPN, bit11 恒为 1,
       * 拿它做判据的耦合是隐患)。保护改由反相之后的两个限位开关承担。
       * 读不到 60FDh -> **一律中止**: 这三条判据里只有这一条没有别的判据可退。 */
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

/* 把 60FDh 那三位说成一句人话, 供「拒绝启扫」与「自动中止」的文案用。
 * 未知时说"状态未知", 不是"都没压住"。返回 UTF-8 常量, 调用方自己包成 QString。 */
inline const char *limit_switch_text(bool dig_known, bool dig_pos, bool dig_neg,
                                     bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "60FDh 不在生效映射里, 开关状态**无从得知** —— 而「高级选项」里的"
           "「上位机侧取反」开着, 反相拿到的是一堆 0, 等于一条判据都没有"
         : "60FDh 不在生效映射里, 三个开关的状态**无从得知**";

   /* 正负限位同时读成压着, 物理上不成立 —— 现场是 X0~X3 接 NPN 传感器 (高电平表示
    * 未触发) 而 2300h 按常开配着, 于是"没触发"被读成"触发"。反转开着时这一条含义整个
    * 变了: 此时 dig_pos/dig_neg 已是反相之后的值, 同时为真 = 两路都被拉到低电平。 */
   if (dig_pos && dig_neg)
      return di_invert
         ? "反相**之后**正限位与负限位仍然**同时**报压着 —— 极性已经不是原因了 "
           "(极性错只会让两边一起反相, 反相完就该松开)。这是两路输入**同时**被读到"
           "低电平: 查传感器供电、输出有没有被拉到 0V、两路是不是接串了"
         : "60FDh 说正限位与负限位开关**同时**都压着 —— 滑台不可能同时在两头, "
           "所以这一条多半不是真的。最可能的原因是 2300h (输入有效电平逻辑) "
           "与接线不符: NPN 传感器高电平表示**未**触发, 驱动器就得按常闭认 "
           "(2300h 里对应的位置 1), 按常开配 (0) 会把「没触发」读成「触发」, "
           "两个限位于是常年都报压着 (本程序里的「上位机侧取反」也能治同一个病, "
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

/* bit11 置起时该去做什么 (与"是什么状态"分开)。措辞里不许出现"已撞上"这种话 ——
 * 只知道 6041h 报了这位、60FDh 说了什么, 剩下的说成"多半/可能"。 */
inline const char *limit_hit_advice(bool dig_known, bool dig_pos, bool dig_neg,
                                    bool dig_home, bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "「上位机侧取反」开着, 而 60FDh 读不到。反转生效时 bit11 **不参与判定**, "
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
           "界面上的「上位机侧取反」也能立刻解开上位机这一侧, 但它**只治软件**: "
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

/* 「这一位是怎么回事」的开场白, 带一个 %1 = 轴号。反转开着时 bit11 不参与判定,
 * 开场白必须跟着判据换。 */
inline const char *limit_hit_headline(bool di_invert)
{
   return di_invert
      ? "轴%1 的限位判据成立 —— 「上位机侧取反」开着, 此刻的判据是**反相之后的**"
        "正/负限位开关, 与 6041h bit11 无关"
      : "轴%1 的 6041h bit11 置起 —— 手册对这一位的定义是「**硬件限位信号有效**」";
}

/* 回零 (驱动器自带的 HM 模式)。能判的放这里, 因为 scan_selftest 不编 ecatworker.cpp;
 * 真正碰总线的 doHome() 验不了, 它是硬件清单上的东西 (见 docs/scan_sweep.md §13)。 */

/* 轴的中文名。轴号是硬约定: 0 = X, 1 = Y (scan 只接受 naxis == 2)。 */
inline const char *axis_label(int i)
{
   return (i == 0) ? "轴X" : "轴Y";
}

/* 方向 -> 6098h 方式: 24 = 原点开关 (X0) 为原点、正向高速先找, 29 = 反向; 不用 35
 * (以当前位置为机械原点, 不是"去找")。"正/反"是**电机轴的正反向**, 与画布 +X/+Y 是否同向
 * 只有现场试一次才知道, 所以界面措辞不能写成"+X 方向"。
 * 这是"找原点"的两条; "以限位开关为原点"的两条是 home_lim_method_for, 见下。 */
inline int home_method_for(bool negative)
{
   return negative ? EM_HOME_MODE_ORIGIN_NEG : EM_HOME_MODE_ORIGIN_POS;
}

inline const char *home_dir_text(bool negative) { return negative ? "反向" : "正向"; }

/* ---- 「找限位」: 把限位开关本身当机械基准 (6098h = 18 正限位 / 17 负限位) ----
 * 与 24/29 的区别是**基准不同**: 24/29 找的是原点开关 X0, 17/18 找的是那一侧的限位开关。
 * 手册 V2.4 p46~p48 给 17/18 各写了两条分支 —— a) 启动时目标开关没压着 (先朝它高速去,
 * 碰到再退开), b) 启动时已经压着 (直接朝反方向低速退开)。**两条分支的终点都是开关的
 * 释放点**, 也就是刚退开一点的那个位置。 */
inline int home_lim_method_for(bool negative)
{
   return negative ? EM_HOME_MODE_LIMIT_NEG : EM_HOME_MODE_LIMIT_POS;
}

inline bool home_method_allowed(int m)
{
   return m == EM_HOME_MODE_ORIGIN_POS || m == EM_HOME_MODE_ORIGIN_NEG ||
          m == EM_HOME_MODE_LIMIT_POS  || m == EM_HOME_MODE_LIMIT_NEG;
}

inline bool home_method_is_limit(int m)
{
   return m == EM_HOME_MODE_LIMIT_POS || m == EM_HOME_MODE_LIMIT_NEG;
}

/* 方式号的简称。进日志 / 横幅 / 结论句 */
inline const char *home_method_short(int m)
{
   if (m == EM_HOME_MODE_LIMIT_POS) return "找正限位";
   if (m == EM_HOME_MODE_LIMIT_NEG) return "找负限位";
   return "找原点";
}

/* 目标那个**开关**的名字 (18 -> "正限位")。与 home_method_short 是两样东西: 那个是**动作**
 * 的名字 "找正限位", 差一个"找"字 —— 拿它 .mid(1) 切会看不见地依赖那个字的长度。 */
inline const char *home_lim_switch_name(int m)
{
   if (m == EM_HOME_MODE_LIMIT_POS) return "正限位";
   if (m == EM_HOME_MODE_LIMIT_NEG) return "负限位";
   return "限位开关";
}

/* 目标开关此刻压着没有 (只有 17/18 有"目标开关"; 24/29 的基准是原点开关, 不在这里判)。
 * "哪一位是目标"由 EM_HOME_LIM_TARGET_BIT 定义 (在 ec_motor.h 里, motor_test 也读同一份)。 */
inline bool home_lim_target_active(int m, bool dig_pos, bool dig_neg)
{
   const uint32_t mask = EM_HOME_LIM_TARGET_BIT(m);

   if (mask == 0u)
      return false;
   return (mask & EM_DI_POS_LIMIT) ? dig_pos : dig_neg;
}

/* 另外那一侧的限位开关 (与目标相对)。与 target 同时压着 = 读数里至少有一个不是真的 */
inline bool home_lim_other_active(int m, bool dig_pos, bool dig_neg)
{
   const uint32_t mask = EM_HOME_LIM_OTHER_BIT(m);

   if (mask == 0u)
      return false;
   return (mask & EM_DI_POS_LIMIT) ? dig_pos : dig_neg;
}

/* 这一趟回零的**首段方向**。tgt_active = "启动时目标那个开关已经压着" (手册的 b) 分支)。
 * 原来的写法是从"方式号等不等于 29"推方向 (scanwindow.cpp / ecatworker.cpp 各一处),
 * 那套对 24/29 够用, 对 17/18 会把 a) 说成 b) —— 而且 17 与 18 的首段方向本身就是反的。
 * 四个方式 × a/b 是一张真值表, 被 scan/selftest.cpp 钉死。 */
inline const char *home_method_first_dir(int m, bool tgt_active)
{
   switch (m)
   {
   case EM_HOME_MODE_ORIGIN_POS: return "正向";                       /* 原点开关, 正向高速先找 */
   case EM_HOME_MODE_ORIGIN_NEG: return "反向";
   case EM_HOME_MODE_LIMIT_POS:  return tgt_active ? "反向" : "正向";  /* 18: b) 反向低速退开 */
   case EM_HOME_MODE_LIMIT_NEG:  return tgt_active ? "正向" : "反向";  /* 17: b) 正向低速退开 */
   }
   return "?";
}

/* 找限位之前的第二道闸 (第一道是 home_refusal)。nullptr = 可以发起。
 * **必须在任何写动作之前** —— doHome() 的第一件事是 em_disable(), 它真的会撤掉保持力矩。
 * 传入的必须是**驱动器自己**那两位 (em_di_poslim / em_di_neglim, 即 2300h + 2310h 之后的
 * 结果), 不是界面反相后的值 —— 驱动器按它自己的读数决定怎么走。
 * 文案里的「拒绝」两个字是界面着红色的判据 (见 scanwindow.cpp 的 hint 排布)。 */
inline const char *home_lim_refusal(bool dig_known, bool tgt_active, bool other_active)
{
   if (!dig_known)
      return "60FDh 读不到 (不在生效 TxPDO 里, 或还没收到过完整帧) -> 分不出这一趟该走"
             "手册的 a) 还是 b) 分支, **拒绝找限位**。出路: 在「高级选项」里勾上"
             "「让 60FDh 进 TxPDO」再重新「连接」";
   if (tgt_active && other_active)
      return "正限位与负限位**同时**报有效 -> 滑台不可能同时在两头, 这一对读数里至少有一个"
             "不是真的, **拒绝找限位**。最可能的原因是 2300h (输入有效电平逻辑) 与接线"
             "不符: NPN 传感器高电平表示**未**触发, 驱动器就得按常闭认 (2300h 对应位置 1), "
             "按常开配 (0) 会把「没触发」读成「触发」。查 2300h 与 2310h~2312h、"
             "以及 X0~X3 的接线之后再试";
   return nullptr;
}

/* 发起前那一句预告: 这一趟走手册的 a) 还是 b)。**这是防止"看到它先往反方向走, 以为点错了"
 * 的唯一提示** —— b) 的首段方向与 a) 相反, 两句话里都必须写明方向与快慢。 */
inline const char *home_lim_branch_text(int m, bool tgt_active)
{
   if (m == EM_HOME_MODE_LIMIT_POS)
      return tgt_active
         ? "轴%1 找正限位 (方式 18): 正限位现在压着, 走手册 b) 分支 —— 先**反向低速**退开, "
           "遇到限位释放后停机 (落点 = 开关的释放点)"
         : "轴%1 找正限位 (方式 18): 正限位现在没压着, 走手册 a) 分支 —— 先**正向高速**"
           "去找它, 碰到后减速停止, 再反向低速退开, 停在开关的释放点";
   if (m == EM_HOME_MODE_LIMIT_NEG)
      return tgt_active
         ? "轴%1 找负限位 (方式 17): 负限位现在压着, 走手册 b) 分支 —— 先**正向低速**退开, "
           "遇到限位释放后停机 (落点 = 开关的释放点)"
         : "轴%1 找负限位 (方式 17): 负限位现在没压着, 走手册 a) 分支 —— 先**反向高速**"
           "去找它, 碰到后减速停止, 再正向低速退开, 停在开关的释放点";
   return "轴%1 找原点 (方式 %2)";
}

/* 6099h:02 由 6099h:01 派生: vel_slow = vel_fast / 4 (与 motor_test 收紧 --vel 同式);
 * 下限 1 —— 写 0 的语义手册没写, 而"返回速度是 0"绝不该是它的意思。 */
inline uint32_t home_vel_slow(uint32_t vel_fast)
{
   uint32_t s = vel_fast / 4u;
   return s ? s : 1u;
}

/* 609Ah 回零加减速: 不写死加速度, 写死**斜坡时间 0.1 秒** —— 驱动器实测的 6099h:01 =
 * 50000 与 609Ah = 500000 就是这个比值; 之上封顶于 HMI_HOME_ACC_MAX (= 6083h/6084h 那个
 * 500000)。传入的必须是已经夹过的 vel_fast, 那个除法守卫是防乘 10 回绕。 */
inline uint32_t home_accel_for(uint32_t vel_fast)
{
   if (vel_fast > HMI_HOME_ACC_MAX / 10u)
      return HMI_HOME_ACC_MAX;
   return vel_fast * 10u;
}

/* 第二道夹取; 第一道是界面上那个 spin box 的 setRange */
inline uint32_t home_vel_clamp(int32_t v)
{
   if (v < HMI_HOME_VEL_MIN) return HMI_HOME_VEL_MIN;
   if (v > HMI_HOME_VEL_MAX) return HMI_HOME_VEL_MAX;
   return (uint32_t)v;
}

/* 从 scan.ini 读回来的 6099h:01: <= 0 = 没记过 (用 HMI_HOME_VEL_DEF), 其余夹进 [MIN, MAX]。
 * 夹取放在这里而不是界面里: 被手改坏的 ini (写成 0 或 1e9) 不该让回零用一个没验过的速度,
 * 而回零是软件唯一兜不住的动作。 */
inline uint32_t home_vel_from_pref(int v)
{
   if (v <= 0)
      return HMI_HOME_VEL_DEF;
   return home_vel_clamp(v);
}

/* 回零之前那道闸: nullptr = 可以发起, 否则是一句给操作员看的话。
 * **必须在任何写动作之前** —— doHome() 的第一件事是 em_disable(), 它真的会撤掉保持力矩
 * (竖直轴当场会滑)。传入的必须是刚读到的状态; mirror_ok 排在 fault 前面。 */
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

/* 收尾之后把结果说成一句人话。**判据是收尾结束那一刻的实测状态**, 不是三个返回码的
 * 排列组合 —— 后者会把"失能返回非 0, 但其实已经卸力了"说成"可能仍带电"。 */
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

   /* 故障排在使能前面: 该报的是故障 —— 那才是要人动手的那一件事 */
   if (fault_now)
      return HOME_END_FAULTED;

   /* rc_mode != 0 = 没能确认切回 CSP, **这一格不能说成 HOLDING** —— 那时驱动器不按 CSP
    * 解释 607Ah, 而 interpolate() 每周期都在往 607Ah 里写。 */
   if (end_enabled && rc_mode == 0)
      return HOME_END_HOLDING;

   return HOME_END_STRANDED;
}

/* rc 用字面量比较: 这里刻意不 include ec_motor_internal.h, 所以 EM_R_OK / EM_R_STOP
 * 没有名字, 只有 0 / 1 / 其它。 */
inline const char *home_cause_text(int rc)
{
   if (rc == 0)
      return "到位";
   if (rc == 1)
      return "被「停止」中止";
   /* 这一格里至少混着四种原因: 方式越界 / 6098h~607Ch 写不进去 / 驱动器没接受 HM 模式
    * (6061h 没读回 6) / 等 bit12 超时 / bit3 或 bit13 报上来。它们要人做的事不一样。 */
   return "失败 —— 方向不对就换另一个方向按钮试试; 找不到原点开关, 就先手动把滑台挪到"
          "开关附近再回零 (别硬顶)。找限位还多两条: 一是走反 (手册 a)/b) 两条分支的首段"
          "方向相反, 发起前控制台会预告这一趟走哪条), 二是撞上限位之后驱动器自己按 2204h "
          "(超程停车方式) 停住 —— 手册没写 HM 期间它与 a)/b) 谁优先, 那种情况下 bit11 "
          "全程举着而位置一步没变。**具体是哪一步失败看控制台**: 回零方式超范围 / "
          "6098h~607Ch 写不进去 / 驱动器没接受 HM 模式 (6061h 没读回 6) / 等 bit12 超时 / "
          "6041h 报了 bit3 或 bit13 —— 这几种在这里是同一句话, 在控制台里是五行不同的字";
}

/* 6061h (实际运行模式) 的数字说成人话。手册 §3.7 只给了这几个值 (6060h 那张表:
 * 0 未定义 / 1 位置 / 3 速度 / 6 回原点 / 8 循环同步位置), 别的数字一律照实报"手册之外"。
 * 两个负数**必须先判**: UNREAD 是"还没读过", -1 是"读了但读不到" (SDO 不通)。 */
inline const char *mode_text(int m)
{
   switch (m)
   {
      case HMI_MODE_DISP_UNREAD: return "还没读过";
      case -1:                   return "读失败";
      case 0:                    return "未定义";
      case 1:                    return "PP 轮廓位置";
      case 3:                    return "PV 速度";
      case 6:                    return "HM 回零";
      case 8:                    return "CSP 位置同步";
      default:                   return "手册之外的模式号";
   }
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

class EcatThread : public QThread
{
   Q_OBJECT

public:
   explicit EcatThread(QObject *parent = nullptr);
   ~EcatThread() override;

   /* GUI 线程调用: 排队给工作线程执行, 不阻塞 */
   void postListAdapters();            /* 结果走 adaptersListed() 信号 */
   void postConnect(const QString &ifname);
   void postDisconnect();
   void postEnable();
   void postDisable();
   void postStop();                    /* 冻在当前位置, **保持使能** */
   void postZeroHere(int axis);        /* 把当前位置设为显示坐标 0 */
   void postCenter(int axis);          /* 走到显示坐标 0 */
   void postCenterAll();
   void postRange(int32_t range);      /* 改量程 (脉冲) */

   /* 清驱动器的故障位 (6040h bit7 上升沿)。**不带轴参数** —— 该复位哪根是驱动器的
    * 事实 (6041h bit3), 不是操作员的选择。 */
   void postFaultReset();

   /* 回零 —— 驱动器自带的 HM 模式。method 只收这四个 (用 home_method_for /
    * home_lim_method_for 算): 24/29 = 找原点 (原点开关 X0), 18/17 = 找限位 (以正/负限位
    * 开关为原点)。vel_fast: 6099h:01, 内部还夹一道。本程序里最长的阻塞命令。
    * ⚠️ 它会**先失能**: 6098h/6099h/609Ah/607Ch 只能在未使能时写, 竖直轴失去保持力矩。 */
   void postHome(int axis, int method, uint32_t vel_fast);

   /* 「停止」在回零期间用这一个 —— **立即**让 em_home 的轮询看见 (≤ 它的 2ms 轮询周期)。
    * 全程序唯一一处 GUI 线程直呼 motor_api; 队列救不了正在找原点的轴 (回零阻塞着
    * 工作线程, 一条 CMD_STOP 要等它自己退出来才轮到)。 */
   void requestMotionStop();

   /* ---- GUI 线程调用: 每周期都要用的两个量, 加锁直接写 ---- */
   void setTarget(int axis, int32_t want_disp);
   void setSpeed (int axis, uint32_t vel);

   /* ---- GUI 线程调用: 连接期参数 ----
    * 下一次 em_setup 要不要把 60FDh 追加进 TxPDO (RAM only)。**默认不开**: 本机生效的
    * 1A00h 只有 6041h/6064h/606Ch 三项, 60FDh 不在里面。改了不重连不生效。 */
   void setWantDigIn(bool on);
   bool wantDigIn() const;

   /* ---- GUI 线程调用: 连接期参数 ----
    * 「写驱动器 2300h」(输入有效电平逻辑 -> NPN 要的常闭): 连接时读各轴原值 -> 写 bit0~bit2
    * = 1 -> 收尾由 em_shutdown 写回原值。**默认关**: hmi.exe 那一侧没有界面解释这个动作,
    * 默认开着会去改驱动器参数。改在下次连接时生效。 */
   void setNpnWriteDrive(bool on);
   bool npnWriteDrive() const;

   /* ---- GUI 线程调用: 运行期参数 ----
    * 「上位机侧取反」: X0~X3 接的是 NPN 传感器 (高电平 = **未**触发)。下一帧 publish()
    * 生效, 并整个换掉限位判据 (bit11 不再参与)。这里的成员初值是 false —— hmi.exe 没有
    * 这个开关的界面, 它不该自作主张; scan 那边由 ScanWindow 按 scan.ini 推过来。 */
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

   /* 网卡清单 (names[i] 就是 postConnect 要的字符串) */
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
      int     method = 0;     /* CMD_HOME 用: 6098h 方式号 (24/29/18/17) */
      int32_t value  = 0;     /* CMD_RANGE 用; CMD_HOME 用它装 6099h:01 */
      QString text;
   };

   /* 「阻塞命令期间遥测不断流」。
    *
    * em_home / em_fault_reset / em_enable_all 这些函数**内部自己跑周期帧** (em__cycle),
    * 而 run() 那一圈的 publish() 要等它们返回才轮得到 —— 于是整个动作期间界面拿到的是
    * 冻住的那一份遥测: 回零最长 30 秒, 那 30 秒里使能灯、三个开关灯、位置一起停住不动。
    * 构造时给总线挂上 em_set_cycle_hook, 析构时摘掉 —— 那些帧里也发一次遥测。
    *
    * 挂的时机**只限会阻塞的那几条命令**: 平时那一圈自己每 2ms publish 一次, 挂着等于
    * 每帧白拷两遍 BusTelem (它里面有 QString)。
    *
    * 做成 RAII 而不是前后两句: 摘不到就等于一直挂着, 而这里中间全是 return。 */
   class BlockTick
   {
   public:
      explicit BlockTick(EcatThread *t);
      ~BlockTick();

      BlockTick(const BlockTick &)            = delete;
      BlockTick &operator=(const BlockTick &) = delete;

   private:
      static void tick(void *user, int wkc);   /* 在 em__cycle 收完一帧时被调 */
      EcatThread *m_t = nullptr;
   };

   /* 以下全部在工作线程里跑 */
   void drainCommands();
   void doListAdapters();
   void doConnect(const QString &ifname);
   void doConnectInner(const QString &ifname);   /* m_busy 由外面的壳一个人管 */
   void doEnable();
   void doFaultReset();
   void doHome(int axis, int method, uint32_t vel_fast);
   void doStop();
   /* 把该轴的实际运行模式 6061h 读一次存进 m_mode_disp[axis] (SDO 读)。
    * **只许在本来就阻塞、或本来就便宜的时刻调** —— publish() / interpolate() 不许调。 */
   void readModeDisp(int axis);
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
   /* 6061h 的**上次读到值**。构造里整体置 HMI_MODE_DISP_UNREAD —— 聚合初始化剩下的会
    * 被填 0, 而 0 是合法模式号 ("未定义")。 */
   int        m_mode_disp[EM_MAX_AXES];
   bool       m_fault_latched = false;

   /* 故障复位进行中 (同 m_busy, 见 BusTelem::resetting); 单独存一份是为了让
    * doFaultReset 自己能在收尾时清干净 */
   bool       m_resetting = false;

   /* 回零进行中 (同上, 见 BusTelem::homing) */
   bool       m_homing    = false;
   int        m_homing_axis   = -1;
   int        m_homing_method = 0;

   /* 当前量程 (脉冲)。postRange 在工作线程里改它, setTarget 在 GUI 线程里读它 (夹取用),
    * 所以用原子量, 不另加锁。默认 HMI_RANGE。 */
   std::atomic<int32_t> m_range{HMI_RANGE};

   std::atomic<bool> m_quit{false};
   bool       m_maybe_live = false;

   /* 连接期参数 (见 setWantDigIn / setNpnWriteDrive)。只在未连接时可改, 所以普通成员 + 一把锁就够 */
   bool       m_want_dig_in = false;
   bool       m_npn_write_drive = false;

   /* 运行期参数 (见 setDiInvert)。UI 线程随时可改、工作线程每帧读, 所以是原子量 */
   std::atomic<bool> m_di_invert{false};
};
