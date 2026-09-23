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

#define HMI_CYCLE_US      2000   /* 过程数据周期 (µs) —— 2 ms。不上 DC, 所以这个值自由
                                  * (不需要跟从站的 DC 周期对齐), 改它只用改这一个数 */
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

/* 等 6041h bit12 (Homing attained) 的上限, **同时是"一次回零最多能找多远"的上限**:
 * 速度 × 这个时间 = 最远距离。缺省 120 s 配缺省速度 50000 pul/s = 6000000 脉冲 ≈ 120 mm,
 * 是缺省扫描区域(27 mm)的 4.4 倍 —— 所以够不着开关时**先挪滑台**, 把超时调长等于把
 * 撞上去的行程一起调长。让滑台停下的始终是硬件限位开关, 这个数只管"找不到还不肯停"。
 *
 * 做成可改是因为它原来只对缺省速度成立: 速度下限 100 pul/s 时 30 s 只走 0.06 圈。
 * 单位一律用**秒** (ini 键 / 界面 / Cmd 都叫 *_s), 只在 doHome() 里乘一次 1000 ——
 * 每一个换算点都是一次写错数的机会。界面 setRange 与工作线程的夹取都读这三个宏。 */
#define HMI_HOME_TMO_MIN_S      5
#define HMI_HOME_TMO_MAX_S    600
#define HMI_HOME_TMO_DEF_S    120

/* AxisTelem::mode_disp 里"工作线程还没为这根轴读过 6061h"的哨兵值。
 * 不能拿 -1 兼这个语义 (em_get_mode 读失败也返回 -1), 也不能借 0 (合法模式号: 未定义)。 */
#define HMI_MODE_DISP_UNREAD  (-2)

/* AxisTelem::fault_code 的两个哨兵。与 6061h 那个坑同一个: **0x0000 是"无错误"这个真实
 * 读数**, 拿它兼"还没读"就会在界面上把"不知道"说成"没故障"。 */
#define HMI_FAULT_CODE_UNREAD (-2)   /* 还没读过 (故障沿刚起来, 那一刻还没轮到 SDO) */
#define HMI_FAULT_CODE_FAIL   (-1)   /* 读了, 但读不到 (SDO 没应答 / 驱动器不自答) */

/* 一根轴的一帧快照。全部是显示坐标。 */
struct AxisTelem
{
   bool     valid     = false;
   bool     mirror_ok = false;   /* **现在**还有完整帧 (否则下面的 sw/pos 是陈值) */
   bool     enabled   = false;   /* 6041h bit2 = 电机带电 */
   bool     fault     = false;   /* 6041h bit3 —— **只这一个**, 「故障复位」挑轴就靠它 */
   bool     at_target = false;   /* 插值目标已到 want */
   int32_t  pos       = 0;       /* 6064h - origin */
   int32_t  want      = 0;       /* 点击给出的目标 */
   int32_t  tgt       = 0;       /* 本周期真正下发的插值目标 */
   uint32_t vel       = HMI_VEL_DEF;
   uint16_t sw        = 0;
   uint32_t frames    = 0;
   /* 当前这段**连续**短帧有几帧 (收到一帧完整的就归零)。只在非 0 时值得显示:
    * 它是"链路正在丢帧"的即时量, 与 frames 那个只增的累计数不是一回事 */
   uint32_t bad_frames = 0;
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

   /* 驱动器故障码 603Fh。**6041h bit3 只说"有故障", 说不了是哪一个** —— 这个才是那件事:
    * 0x0000 无错误 / 0xFF01 过流 / 0xFF02 过压 / 0xFF03 欠压 / 0xFF04 动力线报警 /
    * 0xFF06 通讯报警 / 0xFF08 传感器告警 (手册 §报警)。
    *
    * 两个来源, 挑哪一个由 ecatcmd::err_code_from() **一处**决定:
    *   ① 603Fh 在生效 TxPDO 里 -> 每帧与 bit3 同帧到达 (em_require_err_code 默认就要求补);
    *   ② 不在映射里 -> 只能 SDO 读, 而那条路**故障沿之后才走**(publish() 不许做 SDO) ——
    *      三个取值: 那两个哨兵, 或 0..0xFFFF 的实测值。故障清掉后置回 UNREAD。
    * 只对**报故障的那一根**读。 */
   int      fault_code = HMI_FAULT_CODE_UNREAD;
   /* 上面的码是从**过程数据**里来的 (603Fh 在生效 TxPDO 里)。
    * 与 dig_known 同一个坑: "映射里没有, 退回 SDO 读" 与 "过程数据里读到 0x0000 无错误"
    * 必须分得开 —— 前者要靠一条 SDO (那条 SDO 期间主站一帧过程数据都不发), 后者不用。
    * ⚠️ 它问的是**静态**的"映射里有没有" (em_err_code_offset), **不是** em_err_code_known
    * —— 后者还要 mirror_ok, 拿它当闸门会使"发不发那条 SDO"正好在链路最差时打开。 */
   bool     err_code_mapped = false;
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
   /* 正在回零 (逐轴阻塞, 最长一次回零的超时 + 收尾; 超时可改, 见 HMI_HOME_TMO_*_S)。
    * 界面据此把四个回零按钮按住、把「停止」换成立即中止。与 resetting 同一个坑:
    * 必须由 doHome() 加锁直写一次, 再由 publish() 从 m_homing 拷一份, 两处都要。 */
   bool     homing    = false;
   int      homing_axis   = -1;   /* -1 = 没在回零 */
   int      homing_method = 0;    /* 6098h 的方式号 (24/29/18/17), 只为显示给人看 */
   int      naxis     = 0;
   int      wkc       = 0;
   int      expected_wkc = 0;
   /* 当前生效的量程 (脉冲)。默认 HMI_RANGE; scan/ 会经 postRange() 改 */
   int32_t  range     = HMI_RANGE;

   /* ---- (B) 上位机自己看到的通讯健康 ----
    * bad_wkc_run: **连续**多少帧 WKC 不足 (好帧归零)。由工作线程自己数, 不由界面数 ——
    *   scan/ 的 ScanController 早就有一份自己的 m_bad_wkc, 那是它的中止判据; 这一份是
    *   电文里的, 两个程序 (scan 与 hmi) 共用同一个数。
    * comm_bad: 上面那个数到了 HMI_BAD_WKC_LIMIT (ecatcmd::comm_bad_from 一处算出)。
    * max_gap_ms / gaps_over_ms: 本程序自己**最长多久没发出过一帧** / 超过 HMI_GAP_WARN_MS
    *   的次数。这是"是不是这台 PC 的锅"唯一可对账的数: 空闲时最大值只有几 ms ⇒ 与上位机
    *   无关 (查线缆/干扰/驱动器设置); 几百 ms ⇒ 网卡或电源管理在拖延。 */
   int      bad_wkc_run  = 0;
   bool     comm_bad     = false;
   int      max_gap_ms   = 0;
   int      gaps_over_ms = 0;
   /* 上面那个最大值里, **由本程序自己的命令造成的**那一份 (复位/使能/回零期间 SOEM 的
    * SDO 事务一帧都不发)。不分开说, 我们自己一次使能就会被读成"这台 PC 在卡"。 */
   int      max_gap_self_ms = 0;
   /* 自动重请求 OP 的窗口量: **本轮**坏帧期间的最长停顿 (好帧归零), 与 max_gap_ms 那个
    * 只增不减的会话最大值不是一回事 —— 见 EcatThread::m_gap_bad_ms。 */
   int      gap_bad_ms   = 0;
   /* 连续多少帧**按时**发出去了 (间隔 <= HMI_GAP_WARN_MS)。这是"上位机稳不稳"的唯一的数,
    * 与 WKC 无关 —— 见 ecatcmd::recover_verdict 与 HMI_RECOVER_ARM_FRAMES。 */
   int      gap_ok_run   = 0;
   /* 本会话自动恢复动过几次手 (0 = 没动过)。界面据此说"已经自己救过一次了" */
   int      recover_tries = 0;

   /* AL 状态与 AL 状态码 (只有 al_checked 为真时才作数)。
    * **只在异常时读**(ecatcmd::comm_bad_from 立起来那一下), 不在健康路径上每秒读一次 ——
    * 健康时判"够不够帧"靠每帧免费的 wkc, 正常运行时一个字节的额外流量都不发。 */
   int      al_state    = 0;
   int      al_code     = 0;
   bool     al_checked  = false;

   /* 零点世代: **零点真被搬过一次就 +1**, 由工作线程在写 m_origin[] 的那三处维护。
    * 界面从它同步 (只许往前, 见 origin_epoch_sync), 只进 CSV 表头。
    * 它在 m_origin[] 旁边而不是只在电文里 —— teardown() 会把整份电文清成默认值,
    * 清掉了界面就会漏发下一代, 而漏发那一侧正是危险的。 */
   int      origin_gen = 0;
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

/* 「点复位」这一刻该说什么。
 *
 * 为什么不能只看 axis_needs_reset: 那个判据为了不误卸力, 在 mirror_ok = false 时返回
 * false —— 拒绝写入是对的, 但它把两种完全不同的局面压成了同一个 `false`:
 *   · 6041h 是新鲜的, 只是 bit3 都是 0  -> 真的没轴需要复位;
 *   · 6041h 是**陈旧值** (帧不足)      -> **根本判不了有没有故障**。
 * 现场日志里第二种被说成了第一种:「无轴报故障 (6041h bit3 均为 0), 未写入驱动器」——
 * 而同一块屏幕上, 程序刚刚宣布过那个 6041h 是陈旧的。两句话互相打脸, 且错的那一句
 * 把人往"没事"的方向带。
 *
 * 三支的**次序**也是内容: RESET_UNKNOWN 必须盖过 RESET_NOFAULT 报出来。
 * 只有"新鲜且 bit3 立着"才落到 RESET_DO —— 判据本身一个字节都不放宽。 */
enum ResetGate
{
   RESET_DO,       /* 至少有轴该复位, 可以写 */
   RESET_NOFAULT,  /* 数据新鲜, 没有轴报故障 -> 确实不该写 */
   RESET_UNKNOWN   /* 数据陈旧, 判不了 -> 不该写, 而且要先说清"为什么判不了" */
};

inline ResetGate reset_gate(bool valid, bool mirror_ok, bool fault)
{
   if (axis_needs_reset(valid, mirror_ok, fault))
      return RESET_DO;
   /* 到这里就没轴该复位了, 剩下的只是"为什么": 数据不可信是唯一需要单独解释的理由 */
   return (valid && !mirror_ok) ? RESET_UNKNOWN : RESET_NOFAULT;
}

/* RESET_UNKNOWN 那一支说的一整句 (axes = 那几根轴的名字, 已用 "/" 连好)。
 * **不许说"bit3 均为 0"**: 那个读数是陈旧的, 说它等于把"判不了"说成"没有"。
 * 处置也必须是通讯向的 (「通讯」灯/红横幅/重连总线), 不许混进 A 家族那条
 * "查驱动器参数"的建议 —— 这里的问题不在驱动器。 */
inline QString reset_unknown_text(const QString &axes)
{
   return QStringLiteral(
      "%1 的过程数据帧不完整, 6041h 是陈旧值 —— 判不了有没有故障, 一个字节都没写。"
      "请先恢复通讯。").arg(axes);
}

/* RESET_NOFAULT 那一支: 真的没轴需要复位 (数据新鲜, bit3 都是 0)。
 * 后半句是"为什么不能拿它当万用清零" —— 复位的第一件事是 6040h = 0x0000 (卸力),
 * 对一根健康的轴做等于松开它的保持力矩 (竖直滑台会掉下来)。 */
inline QString reset_nofault_text()
{
   return QStringLiteral("无轴报故障 (6041h bit3 均为 0), 未写入驱动器。"
                         "故障复位会先卸力, 对未报故障的轴执行会松开其保持力矩。");
}

/* 使能的前置闸: 有轴的过程数据不完整 -> 这一次失败的原因**是通讯**。
 * 没有这一道, 它会落到那句「请查看控制台输出, 并确认 6041h 故障位与限位状态」——
 * 把人支去看驱动器故障位与限位开关, 而现场那一次的真因是过程数据从来没回来
 * (6041h 本身就是陈值, 它连"有没有故障"都答不了)。
 * 那句话**留着**给真的与驱动器状态有关的失败 (模式切换 / 故障位 / Remote / CW 阶梯)。
 * 措辞走 (B) 家族: 查的是线缆/网卡/上位机负载, 不许出现"驱动器参数"。 */
inline QString enable_stale_text(const QString &axes)
{
   return QStringLiteral(
      "%1 的过程数据帧不完整, 6041h 与 6064h 都是陈旧值 —— 状态未知, 拒绝使能。"
      "这次失败与驱动器的故障位、限位无关, 请先恢复通讯。")
         .arg(axes);
}

/* 驱动器**自报**有报警 —— 灯与横幅用这一个, 不是只看 bit3。
 *
 * 为什么必须多这一条: 通讯报警 (603Fh = 0xFF06) 不保证把 6041h bit3 立起来, 而故障灯
 * 只读 bit3 —— 那就正是现场报的那件事「隔一阵子就会驱动器通讯报警, 且上位机这里故障
 * 信号灯没有更新」: 驱动器面板上亮着灯, 界面上却什么都不亮。
 *
 * 判据就是一行 (bit3 **或** 一个非 0 的码), **做成纯函数而不是电文里再存一个 bool**:
 * 存一份字段就有"造遥测的人忘了设它"的那天, 而这里的两个输入本来就都在电文里。
 * 0x0000 是"无错误"这个真实读数, 那两个负数是"不知道", 都不算报警。
 *
 * **只给显示用**。要写驱动器的那条路 (axis_needs_reset -> em_fault_reset, 会真的写
 * 6040h = 0x0000 卸力) 仍按 bit3 —— 判据不跟着放宽。 */
inline bool axis_alarm(bool fault, int fault_code)
{
   return fault || fault_code > 0;
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
 * 它是电平不是闩锁, 与 60FDh 的 bit0/bit1 同源。打开下面这个开关 = 再加 "且 60FDh 说触发"。 */
#define kRefineLimitWithDigIn 0

/* 三条判据 —— 哪一条生效由一条纯函数 (limit_rule_for) 决定, 不散在调用点。
 * 写成 enum 而不是几个 bool 参数: 判据"开没开"由多处开关各拼一次时, 会出现界面上看不出来的组合。 */
enum LimitRule
{
   LIMIT_RULE_BIT11   = 0,   /* 6041h bit11 单独判定 —— 出厂默认, 也是所有退路 */
   LIMIT_RULE_REFINED = 1,   /* bit11 **且** 60FDh 说正/负限位触发 (原点不算) */
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
 * 未知时说"状态未知", 不是"都没触发"。
 * **只说状态, 不说成因** —— 成因与排查步骤在 docs/scan_messages.md, 这里多说一句就
 * 会在横幅上把那句"现在是什么状况"埋掉。返回 UTF-8 常量, 调用方自己包成 QString。 */
inline const char *limit_switch_text(bool dig_known, bool dig_pos, bool dig_neg,
                                     bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "60FDh 读不到, 而上位机侧取反已开启 —— 无可用限位判据"
         : "60FDh 读不到, 三个开关的状态无从得知";

   /* 正负限位同时为真, 物理上不成立 —— 成因见 docs/scan_messages.md。
    * 反转开着时这一条含义整个变了: 此时 dig_pos/dig_neg 已是反相之后的值。 */
   if (dig_pos && dig_neg)
      return di_invert
         ? "反相之后正限位与负限位同时触发"
         : "正限位与负限位同时触发";

   if (dig_pos)
      return di_invert ? "反相之后正限位触发" : "正限位触发";
   if (dig_neg)
      return di_invert ? "反相之后负限位触发" : "负限位触发";
   return di_invert ? "反相之后正/负限位都没触发"
                    : "正/负限位都没触发";
}

/* bit11 置起时该去做什么 (与"是什么状态"分开)。只给一条出路, 措辞里不许出现"已撞上"
 * 这种话 —— 只知道 6041h 报了这位、60FDh 说了什么, 剩下的说成"多半/可能"。
 * **成因与完整排查表在 docs/scan_messages.md**, 这里只留"下一步按哪个"。 */
inline const char *limit_hit_advice(bool dig_known, bool dig_pos, bool dig_neg,
                                    bool dig_home, bool di_invert)
{
   if (!dig_known)
      return di_invert
         ? "限位判据缺失, 扫描无法启动。请勾选「让 60FDh 进 TxPDO」并重新连接, "
           "或关闭上位机侧取反"
         : "请勾选「让 60FDh 进 TxPDO」并重新连接, 以区分原点与限位";

   if (dig_pos && dig_neg)
      return di_invert
         ? "请检查传感器供电与两路接线 (是否被拉到 0V / 是否接串)"
         : "请检查 2300h 输入有效电平逻辑与 X0~X3 接线; 或启用「上位机侧取反」"
           "(只作用于本程序)";

   if (dig_pos || dig_neg)
      return di_invert
         ? "读数可信, 请手动把滑台移离该限位"
         : "请手动把滑台移离触发的限位, 再查 607Dh:01/:02 软限位";

   if (dig_home)
      return "请手动把滑台移离原点开关";

   /* 607Dh 那一支: CiA402 一般定义里软限位是另一路可能的来源, 本机实测它是开着的
    * (不是 0/0), 所以多半不指向它。它的实测值当初由 CLI 的只读诊断打出, 那个程序
    * 2026-09-21 从仓库移除了 —— 要复核得自己用 em_rd_i32() 临时加一行。 */
   return di_invert
      ? "请重新连接总线"
      : "请检查 2310h~2312h 功能码与 607Dh:01/:02 软限位";
}

/* 「这一位是怎么回事」的开场白, 带一个 %1 = 轴号。反转开着时 bit11 不参与判定,
 * 开场白必须跟着判据换。
 * 「硬件限位信号有效」是 6041h bit11 的原始定义, 照抄 —— 它是电平, 不是"撞上了"。 */
inline const char *limit_hit_headline(bool di_invert)
{
   return di_invert
      ? "轴%1 的限位判据成立 —— 判据是反相之后的正/负限位开关, 与 6041h bit11 无关"
      : "轴%1 的 6041h bit11 置起 (硬件限位信号有效)";
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
 * 手册 V2.4 p46~p48 给 17/18 各写了两条分支 —— a) 启动时目标开关未触发 (先朝它高速去,
 * 碰到再退开), b) 启动时已经触发 (直接朝反方向低速退开)。**两条分支的终点都是开关的
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

/* 目标开关此刻触发与否 (只有 17/18 有"目标开关"; 24/29 的基准是原点开关, 不在这里判)。
 * "哪一位是目标"由 EM_HOME_LIM_TARGET_BIT 定义 (在 ec_motor.h 里; 现在只有界面侧读它 ——
 * 信号灯的着色与下面这条判据共用同一份)。 */
inline bool home_lim_target_active(int m, bool dig_pos, bool dig_neg)
{
   const uint32_t mask = EM_HOME_LIM_TARGET_BIT(m);

   if (mask == 0u)
      return false;
   return (mask & EM_DI_POS_LIMIT) ? dig_pos : dig_neg;
}

/* 另外那一侧的限位开关 (与目标相对)。与 target 同时触发 = 读数里至少有一个不是真的 */
inline bool home_lim_other_active(int m, bool dig_pos, bool dig_neg)
{
   const uint32_t mask = EM_HOME_LIM_OTHER_BIT(m);

   if (mask == 0u)
      return false;
   return (mask & EM_DI_POS_LIMIT) ? dig_pos : dig_neg;
}

/* 这一趟回零的**首段方向**。tgt_active = "启动时目标那个开关已经触发" (手册的 b) 分支)。
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
      return "60FDh 读不到 → 分不出这一趟走 a) 还是 b) 分支, 拒绝找限位。"
             "请勾选「让 60FDh 进 TxPDO」并重新连接";
   if (tgt_active && other_active)
      return "正限位与负限位同时报有效 → 这一对读数至少有一个不是真的, 拒绝找限位。"
             "请检查 2300h 输入有效电平逻辑与 X0~X3 接线";
   return nullptr;
}

/* 发起前那一句预告: 这一趟走手册的 a) 还是 b)。**这是防止"看到它先往反方向走, 以为点错了"
 * 的唯一提示** —— b) 的首段方向与 a) 相反, 两句话里都必须写明方向与快慢。 */
inline const char *home_lim_branch_text(int m, bool tgt_active)
{
   if (m == EM_HOME_MODE_LIMIT_POS)
      return tgt_active
         ? "轴%1 找正限位 (方式 18): 正限位已触发 → 走 b) 分支, 先反向低速退开, "
           "停在开关的释放点"
         : "轴%1 找正限位 (方式 18): 正限位未触发 → 走 a) 分支, 先正向高速寻找, "
           "碰到后减速停止再反向低速退开, 停在开关的释放点";
   if (m == EM_HOME_MODE_LIMIT_NEG)
      return tgt_active
         ? "轴%1 找负限位 (方式 17): 负限位已触发 → 走 b) 分支, 先正向低速退开, "
           "停在开关的释放点"
         : "轴%1 找负限位 (方式 17): 负限位未触发 → 走 a) 分支, 先反向高速寻找, "
           "碰到后减速停止再正向低速退开, 停在开关的释放点";
   return "轴%1 找原点 (方式 %2)";
}

/* 6099h:02 由 6099h:01 派生: vel_slow = vel_fast / 4 —— 这个 1/4 是本仓库定死的比例;
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

/* 回零超时 (s) 的第二道夹取; 第一道是界面上那个 spin box 的 setRange。
 * 与 home_vel_clamp 同一个理由: 界面显示的数就是线上发的数, 两处读同一组宏, 不许漂移。 */
inline int home_tmo_s_clamp(int v)
{
   if (v < HMI_HOME_TMO_MIN_S) return HMI_HOME_TMO_MIN_S;
   if (v > HMI_HOME_TMO_MAX_S) return HMI_HOME_TMO_MAX_S;
   return v;
}

/* 从 scan.ini 读回来的回零超时: <= 0 = 没记过 (用 HMI_HOME_TMO_DEF_S), 其余夹进 [MIN, MAX]。
 * 夹取放在这里而不是界面里 —— 与 home_vel_from_pref 同一条理由: 被手改坏的 ini
 * (写成 0 或 1e9) 不该让回零用一个没验过的超时, 而超时是回零唯一兜底的那条线。
 * 下界 ≥ 1 是硬要求: doHome() 那一次 "秒 × 1000" 不能得 0。 */
inline int home_tmo_s_from_pref(int v)
{
   if (v <= 0)
      return HMI_HOME_TMO_DEF_S;
   return home_tmo_s_clamp(v);
}

/* 沿用上一轮的零点安不安全 —— 只有滑台仍在**当前量程**内才许沿用。
 * 量程未知 (<= 0) 一律不沿用: 不知道就别赌。
 *
 * 为什么非有这条不可: doEnable() 的重新锚定写的是 pos - origin 且**不夹取**, 而
 * interpolate() 每周期把 m_tgt 夹进 ±range。两处一撞就是一次没人按过按钮的全速运动 ——
 * 滑台停在旧零点外 1200000 而量程 800000 时, 使能后驱动器会被命令从 origin+1200000
 * 走到 origin+800000。而"重连时滑台不在零点附近"正是零点保留要支持的正常用法。 */
inline bool origin_keep_ok(int32_t pos, int32_t origin, int32_t range)
{
   if (range <= 0)
      return false;
   const int64_t d = (int64_t)pos - (int64_t)origin;   /* 不许 int32 相减溢出 */
   return d <= (int64_t)range && d >= -(int64_t)range;
}

/* 界面的零点世代只能往前。
 * 工作线程是**唯一**知道零点真被搬过的那个, 所以世代从它同步过来; 但 teardown() 会把
 * 整份遥测清成默认值 —— 一个只会赋值的界面会在那一刻把世代倒回去, 而倒回去等于给下一次
 * 续扫发一张假的"世代对不上"红横幅。**漏发一代是危险的那一侧, 多发一代只是多问一次**,
 * 所以这里取大。 */
inline int origin_epoch_sync(int ui_epoch, int worker_gen)
{
   return (worker_gen > ui_epoch) ? worker_gen : ui_epoch;
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
      return "一笔完整的 6064h 都还没取到 → 位置未知, 拒绝回零";
   if (!mirror_ok)
      return "这一根没收到过完整帧 → 6041h 状态未知, 拒绝回零";
   if (fault)
      return "6041h bit3 = Fault, 请先「故障复位」";
   if (any_axis_moving)
      return "还有轴在走, 请先「停止」";
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
   /* 这一格里至少混着五种原因: 方式越界 / 6098h~607Ch 写不进去 / 驱动器没接受 HM 模式
    * (6061h 没读回 6) / 等 bit12 超时 / bit3 或 bit13 报上来 —— 它们要人做的事不一样,
    * 但在这里是同一句话: 先看控制台那五行分清是哪一步。
    * 完整的排查表 (方向不对 / 走反 / 撞限位后按 2204h 停住) 在 docs/scan_messages.md。 */
   return "失败, 请查看控制台输出的那五行 (6061h / bit12 / bit3 / bit13)";
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
         return "未发起 (前置判据未通过)";
      case HOME_END_FAULTED:
         return "该轴报故障 (6041h bit3), 停在未使能。请处理后「故障复位」";
      case HOME_END_STRANDED:
         return "收尾未确认「已使能 + CSP」, 电机可能仍带电。"
                "请查看控制台输出并断开驱动器动力电源";
      case HOME_END_HOLDING:
         return "已切回 CSP 并保持使能, 停在落点带保持力矩";
   }
   return "";
}

/* ---- 故障码 603Fh 的说人话 ----
 * 判据做成 inline 放这里, 与 mode_text 同一个理由: **scan_selftest 不编 ecatworker.cpp**,
 * 写在 .cpp 里就永远验不到。 */

/* 手册那张表 (ykd2205pe_ci402.md §报警与指示灯)。0x0000 是**驱动器自报的一个合法读数**
 * ("无错误"), 不是"没读到" —— 所以它不能兼哨兵值 (见下面那两个负数)。真机上"bit3 置起
 * 而码读回 0000"是可能的 (故障位上一轮没清), 那也是照实说 0000, 不替它改口。
 * 返回 nullptr = 手册之外 —— 那种值照实报十六进制, 不猜成"未知故障"。 */
inline const char *fault_code_meaning(uint16_t code)
{
   switch (code)
   {
      case 0x0000: return "无错误";
      case 0xFF01: return "过流";
      case 0xFF02: return "过压";
      case 0xFF03: return "欠压";
      case 0xFF04: return "动力线报警";
      case 0xFF06: return "通讯报警";
      case 0xFF08: return "传感器告警";
   }
   return nullptr;
}

/* 这个码该去查什么。**不是诊断结论**, 只是把"下一步看哪儿"指出来 —— 否则操作员看到
 * 0xFF02 只会想到"换驱动器"。手册那张表就这几个码, 处置办法彼此完全不搭界 (供电 / 机械 /
 * 接线 / 干扰), 所以值得各写一句。
 * 顺带一个可对账的地方: 手册 §报警与指示灯里 **ALM 灯的红闪次数就是这几个数字**
 * (1 过流 / 2 过压 / 3 欠压 / 6 通讯 / 8 传感器)。面板上数出来的次数与这里读到的码对得上,
 * 说明这条 SDO 读的确实是同一件事; 对不上就先别信这里。返回 nullptr = 手册之外。 */
inline const char *fault_code_action(uint16_t code)
{
   switch (code)
   {
      /* 0x0000 这一条是给"bit3 还立着但驱动器自报无错"那一拍用的 —— 它前面已经写了
       * "0000 (无错误)", 这里再说一遍就重复了, 所以只讲下一步 */
      case 0x0000: return "码是好的, 只是故障位还没清, 请「故障复位」";
      case 0xFF01: return "过流: 请查机械卡死 / 堵转与动力线 U V W";
      case 0xFF02: return "过压: 请查供电电压与减速设置";
      case 0xFF03: return "欠压: 请查供电与接触器";
      case 0xFF04: return "动力线报警: 请查动力线接线与电机相间 / 对地";
      case 0xFF06: return "通讯报警: 请查通讯线、干扰源与站号配置";
      case 0xFF08: return "传感器告警: 请查编码器接线与信号";
   }
   return nullptr;
}

/* 一个 fault_code 字段说成人话: "0xFF02 (过压)" / "0xFF02 (手册之外)" /
 * "还没读到 (正在读)" / "读不到 (SDO 没应答)"。
 *
 * **四句话都写成"接在 603Fh = 后面也通顺"的样子** —— 它要被拼进 "轴X 603Fh = ……"
 * (fault_axis_text), 也要单独用。写成"正在读 603Fh…"那种自带索引的说法, 拼起来就是
 * "轴X 603Fh = 正在读 603Fh…"。
 *
 * **两种"不知道"必须分开说**: 前者是"再等一帧就有", 后者是"这一趟读不到了"。 */
inline QString fault_code_text(int code)
{
   if (code == HMI_FAULT_CODE_UNREAD)
      return QStringLiteral("还没读到 (正在读)");
   if (code == HMI_FAULT_CODE_FAIL)
      return QStringLiteral("读不到 (SDO 没应答)");

   const uint16_t v   = (uint16_t)(code & 0xFFFF);
   const char    *mean = fault_code_meaning(v);
   const QString  hex  = QString::number(v, 16).toUpper()
                            .rightJustified(4, QLatin1Char('0'));

   return QStringLiteral("0x%1 (%2)")
             .arg(hex, QString::fromUtf8(mean != nullptr ? mean : "手册之外"));
}

/* "轴X 603Fh = 0xFF02 (过压)" —— 日志、横幅、自动中止那几句话都从这一句拼,
 * 免得几处各写一份 (两处说法不一致比说错更难查)。 */
inline QString fault_axis_text(int i, int code)
{
   return QStringLiteral("%1 603Fh = %2")
             .arg(QString::fromUtf8(axis_label(i)), fault_code_text(code));
}

inline QString fault_axis_text(const AxisTelem &a, int i)
{
   return fault_axis_text(i, a.fault_code);
}

/* 读到码那一刻写进日志/状态栏的**一整句**: "轴X 603Fh = 0xFF02 (过压) —— 过压: ……"。
 * 工作线程那边只有轴号与码 (还没有快照), 所以它有自己那个重载。 */
inline QString fault_code_line(int i, int code)
{
   QString s = fault_axis_text(i, code);

   if (code != HMI_FAULT_CODE_UNREAD && code != HMI_FAULT_CODE_FAIL)
   {
      const char *act = fault_code_action((uint16_t)(code & 0xFFFF));

      if (act != nullptr)
         s += QStringLiteral(" —— ") + QString::fromUtf8(act);
   }

   return s;
}

/* 哪几根轴报了报警、各自的码是多少, 拼成一句 (没有轴报警时是空的)。
 * 用于自动中止那句结论: 6041h 只说得清"有故障", 说清"是哪一个"要靠它。
 * **判据是 alarm 不是 fault**: 通讯报警 (0xFF06) 不保证把 bit3 立起来, 走 fault 的话
 * 那种报警在这里会一个字都不说。 */
inline QString faulted_axes_text(const BusTelem &t)
{
   QStringList parts;

   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
      if (t.ax[i].valid && t.ax[i].mirror_ok
          && axis_alarm(t.ax[i].fault, t.ax[i].fault_code))
         parts << fault_axis_text(t.ax[i], i);

   return parts.join(QStringLiteral(", "));
}

/* 故障横幅那一整句。**两拍都用它**: 第一拍码还没到 (那时 fault_code = UNREAD, 这一句说
 * "还没读到"), 第二拍码到了。同一条模板发两次 —— 界面上每次写的都是"眼下知道的那一份",
 * 而不是"我刚知道的那一点", 于是迟一拍的那次不会把先前那句顶掉半截。
 * 有真实码时把"下一步查哪儿"也带上 (fault_code_action): 红横幅是无人值守时唯一还在看的东西。
 *
 * 标题写"bit3 或 603Fh"而不是只写 bit3: alarm 把"只有码、bit3 没立"那一类也收进来了,
 * 只写 bit3 的话界面会指着一条根本没立的位说事。 */
inline QString fault_banner_text(const BusTelem &t)
{
   QString s = QStringLiteral("驱动器自报故障 (6041h bit3 或 603Fh), 目标已冻结。");

   const QString codes = faulted_axes_text(t);

   if (!codes.isEmpty())
      s += QStringLiteral("  ") + codes + QStringLiteral("。");

   /* 只对**真实读数**说处置办法: UNREAD/FAIL 那两拍本来就没东西可查。
    * **同一个码只说一遍**: 两根轴同时报同一个码时, 逐轴再说一次就是同一句话连着出现两遍
    * (2026-09-22 实机: 两根轴都是 "0x0000 (无错误)", 处置那句就重了两遍)。"是哪些轴"
    * 已经由上面那段逐轴说清了, 处置办法是按码给的, 不按轴给。 */
   QStringList act;

   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
   {
      const AxisTelem &a = t.ax[i];

      if (!a.valid || !a.mirror_ok || !axis_alarm(a.fault, a.fault_code))
         continue;
      if (a.fault_code == HMI_FAULT_CODE_UNREAD || a.fault_code == HMI_FAULT_CODE_FAIL)
         continue;

      const char *p = fault_code_action((uint16_t)(a.fault_code & 0xFFFF));

      if (p == nullptr)
         continue;

      const QString one = QString::fromUtf8(p);

      if (!act.contains(one))
         act << one;
   }

   if (!act.isEmpty())
      s += QStringLiteral("  ") + act.join(QStringLiteral("; ")) + QStringLiteral("。");

   return s;
}

/* ---- (B) 家族: 上位机**自己看到**的通讯健康 ----
 * 与上面那一族 (驱动器自报的 603Fh) 是**两件不同的事**, 措辞也刻意不共用:
 * 603Fh = 0xFF06 是"驱动器说它跟我断了", 这里的 comm_bad 是"我这边的帧不够了"。
 * 处置不一样 (前者查驱动器设置与干扰, 后者查网线/网卡/上位机负载), 所以不能说成同一句话。 */

/* 连续多少帧 WKC 不足就认总线出事。与 ScanController::healthProblem 那个 10 同源 ——
 * 单帧抖动不值得报, 30Hz 下 10 帧约 1/3 秒。 */
#define HMI_BAD_WKC_LIMIT 10

/* 单帧间隔超过它就算"卡了一下", 计数并记最大值。50ms 是量出来的量级:
 * 本机 2ms 圈期的实测抖动是几 ms 级, 而 SDO 事务造成的静默是 700ms 级 ——
 * 取 50 正好把"抖动"与"真卡住"分开。 */
#define HMI_GAP_WARN_MS     50

/* **进了 OP 之后**所有 SDO 读的超时 (微秒) —— 进 OP 那一刻由 doConnectInner 用
 * em_set_sdo_timeout 装上, 之后 6061h / 1C32h / 2217h / 603Fh 那几条诊断读全走它。
 *
 * 为什么不是 EC_TIMEOUTRXM (700ms): **SOEM 的 SDO 事务期间过程数据帧一帧都不发**
 * (ecx_SDOread -> ecx_mbxreceive, 只有邮箱轮询, 见 public 头 em_sdo_read 那段), 所以
 * 超时就是"总线被静默多久"。2026-09-22 实机量到一次 1638ms 的静默, 就是两根轴各超时
 * 700ms —— 而那一刻驱动器正因为收不到帧在报通讯报警, 于是上位机自己把报警按实了一次。
 * 正常应答 1~3ms 就到, 60ms 已经是它的二十来倍; 实际最长阻塞还要加邮箱发送的
 * EC_TIMEOUTTXM (20ms)。
 *
 * 光靠压超时不够 —— 发不发某条 SDO 本身另有判据 (见 serviceFaultCodeReads: 帧不健康时
 * 一条都不发)。 */
#define HMI_OP_SDO_TMO_US 60000

/* 上位机的帧够不够。四条判据, 边界逐条可测:
 * expected <= 0 = SOEM 还没算出期望值 (没进 OP / 没建映射) —— 不能拿它当"期望 0 帧";
 * limit <= 0 同理, 是个非法阈值而不是"永不报警"; wkc >= expected 就是好帧, 不管 bad_run 多少。 */
inline bool comm_bad_from(int wkc, int expected, int bad_run, int limit)
{
   if (expected <= 0 || limit <= 0)
      return false;
   if (wkc >= expected)
      return false;
   return bad_run >= limit;
}

/* 603Fh 那两条路的**唯一**一处挑选规则。两个来源:
 *   mapped = 603Fh **在生效 TxPDO 里** (静态判据: AxisTelem::err_code_mapped /
 *            em_err_code_offset) -> field_raw 就是那一帧的读数, 与 6041h bit3 **同帧**到达。
 *            含 0x0000 —— 那是"无错误"这个真实读数, 不许当成"没读到"。
 *            (读不到时那个 accessor 自己返回 UNREAD, 所以这一支照样说得出"还没读到"。)
 *   不在映射里时只剩 SDO 那一条路, 而那条路**只在 bit3 立起时才走**:
 *            sdo_fallback 是 HMI_FAULT_CODE_UNREAD / _FAIL / 或上一次读到的码。
 * bit3 = 0 且不在映射里 -> UNREAD。这一支不能把 sdo_fallback 原样放出去: 那多半是**上一次**
 * 故障留下的陈值, 显示出来就是"已经清掉的故障还在报"。
 * 返回值的语义与 AxisTelem::fault_code 一致: 真实读数 / UNREAD / FAIL。 */
inline int err_code_from(bool mapped, int field_raw, int sdo_fallback, bool bit3)
{
   if (mapped)
      return field_raw;
   if (bit3)
      return sdo_fallback;
   return HMI_FAULT_CODE_UNREAD;
}

/* AL 状态码说人话。表照 SOEM/src/ec_print.c 的 ec_ALstatuscodelist 手抄 ——
 * **selftest 不链 SOEM**, 用不了 ec_ALstatuscode2string, 所以判据这边必须自带一份。
 * 只抄这台机器真可能撞上的那几条; 认不出的回落到十六进制, 不猜成"未知错误"。
 * 返回 nullptr = 不在这一份表里。 */
inline const char *al_code_meaning(int alcode)
{
   switch (alcode)
   {
      case 0x0000: return "无错";
      case 0x0011: return "被要求的状态迁移不合法";
      case 0x0012: return "不认识被要求的状态";
      case 0x0017: return "同步管理器配置不合法";
      case 0x001A: return "同步错误";
      /* 这一条是「主站喂帧超时」在从站侧的对应物: 驱动器嫌主站发过程数据太慢/太少。
       * 与 603Fh = 0xFF06 说的是同一件事, 一个在从站侧报、一个在驱动器侧报 */
      case 0x001B: return "同步管理器看门狗 (主站喂帧超时)";
      case 0x001C: return "同步管理器类型不合法";
      case 0x001D: return "输出配置不合法";
      case 0x001E: return "输入配置不合法";
      case 0x001F: return "看门狗配置不合法";
      case 0x0020: return "从站需要冷启动";
      case 0x0021: return "从站需要回到 INIT";
      case 0x0022: return "从站需要回到 PREOP";
      case 0x0023: return "从站需要回到 SAFEOP";
      case 0x0024: return "输入映射不合法";
      case 0x0025: return "输出映射不合法";
      case 0x0028: return "不支持同步";
      case 0x002A: return "后台看门狗";
      case 0x0030: return "DC 同步配置不合法";
      case 0x0034: return "DC 同步超时";
      case 0x0036: return "DC SYNC0 周期不合法";
   }
   return nullptr;
}

/* AL 状态名 (EC_STATE_*, SOEM/soem/ethercattype.h) + 状态码, 合成一句给状态栏看。
 * state <= 0 = 还没读到, 直接说"没读到" —— 不把 0 说成 INIT (0 不是合法 AL 状态)。 */
inline QString al_code_text(int state, int alcode)
{
   if (state <= 0)
      return QStringLiteral("AL 状态没读到");

   const char *st = nullptr;

   switch (state)
   {
      case 0x01: st = "INIT";    break;
      case 0x02: st = "PRE-OP";  break;
      case 0x03: st = "BOOT";    break;
      case 0x04: st = "SAFE-OP"; break;
      case 0x08: st = "OP";      break;
   }

   QString head = QStringLiteral("AL %1")
                     .arg(st != nullptr ? QLatin1String(st)
                                        : QStringLiteral("0x%1").arg(state, 0, 16));
   /* 码 0 是"无错", 不必跟着报 —— 只想说状态时那一截不该出现 */
   if (alcode == 0)
      return head;

   const char  *mean = al_code_meaning(alcode);
   const QString hex  = QString::number((uint)(alcode & 0xFFFF), 16).toUpper()
                           .rightJustified(4, QLatin1Char('0'));

   return head + QStringLiteral(" / 0x%1 (%2)")
                    .arg(hex, QString::fromUtf8(mean != nullptr ? mean : "表外码"));
}

/* 「通讯」灯与那条不自动消失的红横幅说的一整句。**走 (B) 措辞** ——
 * 这条横幅讲的是"我这边的帧不够", 不能借 fault_code_action(0xFF06) 那句
 * ("查通讯线、干扰源与站号配置"): 那句话的前提是驱动器自报通讯报警, 是另一件事。
 * max_gap_ms/gaps_over_ms: 本程序自己最长多久没发出过一帧 (见 EcatThread 的帧间隔统计)。
 *   0 = 还没量到 / 量的是"没有超阈值"; > 0 时把最大值说出来 —— 它是"这台 PC 有没有份"
 *   唯一可对账的数: 几 ms = 与上位机无关, 几百 ms = 就是这台 PC。
 * self_gap_ms: 上面那个最大值里, **由本程序自己的命令造成的**那一份 (复位/使能/回零
 *   期间 SOEM 的 SDO 事务一帧都不发, 见 HMI_OP_SDO_TMO_US 那段)。**必须分开说** ——
 *   否则我们自己一次使能造成的静默会被读成"这台 PC 在卡", 把人支去查电源计划与网卡
 *   节能, 而那两样与它毫无关系。*/
inline QString comm_banner_text(int wkc, int expected, int bad_run,
                                int max_gap_ms, int gaps_over_ms, int self_gap_ms)
{
   QString s = QStringLiteral(
      "过程数据帧连续 %1 帧不足 (工作计数器 %2/%3), 位置与状态为陈旧值, 目标已冻结。")
      .arg(bad_run).arg(wkc).arg(expected);

   if (max_gap_ms > 0)
   {
      s += QStringLiteral("\n本程序最长 %1 ms 未发出帧 (超过 %2 ms 的 %3 次)。")
              .arg(max_gap_ms).arg(HMI_GAP_WARN_MS).arg(gaps_over_ms);

      /* 一句反证的话, 只在真的成立时才说: 全程最长的那一次就是我们自己造成的,
       * 那就**不能**把它算成"这台 PC 在卡"。 */
      if (self_gap_ms > 0 && self_gap_ms >= max_gap_ms)
         s += QStringLiteral("\n其中最长的那次是本程序自己的命令 (复位/使能/回零) "
                             "造成的, 不是外部卡顿。");
   }

   return s;
}

/* ---- 自动重请求 OP: 什么时候动手 ------------------------------------------------
 * 现场那一次的病根(上位机停顿 1.6s)按掉之后, 这一层是**兜底**: 停顿真发生了, 至少别
 * 留下"只能断开重连"这一个回程。 */

/* 本机"一直在按时发帧"跑够这么多帧才允许自动动手 (≈ 2s @ HMI_LOOP_MS=2ms)。
 *
 * **量的是上位机, 不是总线**: 数的是"这一帧发得及不及时"(相邻两帧的间隔 <= 50ms),
 * **不是**"WKC 够不够"。现场那一次从站一旦掉出 OP, WKC 就恒为 2/6 ——
 * 拿 WKC 当"停机停稳了"的判据, 那个数就永远涨不上去, 自动恢复永远不会动手。
 * 而这里要知道的恰恰是"本机还在不在卡": 卡住了才不能动手 (边卡边请求 OP 只会再被
 * SM 看门狗踢出来), 而本机不卡了就说明那一段停顿已经过去。
 *
 * 取 1000 而不是 HMI_BAD_WKC_LIMIT(10): 那个 10 是"该不该报给操作员"的判据, 短到
 * 1/3 秒; 自动**写驱动器 AL 寄存器**这件事要保守得多。 */
#define HMI_RECOVER_ARM_FRAMES    1000

/* 两次自动恢复之间的冷却。一轮恢复本身 1~3s, 冷却 30s 保证不会变成"每 2 秒拉一次 OP"
 * —— 那样从站在 OP 与 SAFE-OP 之间来回跳, 比停在 SAFE-OP 更糟。 */
#define HMI_RECOVER_COOLDOWN_MS   30000

/* 本会话自动动手的上限。到顶之后只报「该断开重连了」, 不再试 ——
 * 反复请求 OP 而每次都掉回来, 说明不是 AL 层能修的, 再试只是让日志变长。 */
#define HMI_RECOVER_MAX_TRIES     2

/* 自动恢复该不该动手。**顺序是内容**: 先报"这台 PC 自己在卡"这条最要紧的结论, 再报
 * "试满了"。两条同时成立时必须先说前者 —— 它指出的是根因, 而"试满"只是结果。
 *
 * 三个阈值参数各自非法时一律 RECOVER_NO (`<= 0`): 被改成 0 是个非法阈值, 不是"永不
 * 触发"; 其中 arm_frames = 0 尤其危险 —— 那等于"一帧都没按时发也动手"。
 * 与 comm_bad_from 同一套写法。 */
enum RecoverVerdict
{
   RECOVER_NO,       /* 不值得动手 (刚缓过来 / 还没缓够 / 刚动过手) */
   RECOVER_TRY,      /* 动手: 本机已经稳了一阵子, 从站还没跟回来 —— 这是本功能要修的那一次 */
   RECOVER_MASTER,   /* 别动手, 先查**这台 PC**: 眼下这一帧就迟到了, 本机还在卡 */
   RECOVER_GIVEUP    /* 本会话试满了 -> 只剩「重连总线」这一条路 */
};

/* cur_gap_ms: **刚刚**这一帧迟了多久 (不是会话最大值 —— 最大值一次停顿之后就再也下不来,
 *   拿它判就等于"从此永远认为本机在卡", 自动恢复再也动不了手)。
 * ok_run: 连续多少帧**按时**发出去了 (上位机的健康状况, 见 HMI_RECOVER_ARM_FRAMES)。
 * tries/max_tries: 本会话动过几次手 / 上限。 */
inline RecoverVerdict recover_verdict(int cur_gap_ms, int gap_limit_ms,
                                      int ok_run, int arm_frames,
                                      int tries, int max_tries)
{
   if (gap_limit_ms <= 0 || arm_frames <= 0 || max_tries <= 0)
      return RECOVER_NO;
   /* 本机**现在**就在卡 -> 先说这个, 别去动从站: 这一帧都迟到, 请求 OP 之后下一帧
    * 多半照样迟到, 从站只会被 SM 看门狗再踢一次。放在 tries 之前 —— 根因比结果要紧。 */
   if (cur_gap_ms > gap_limit_ms)
      return RECOVER_MASTER;
   if (tries >= max_tries)
      return RECOVER_GIVEUP;
   /* 本机按时发帧要**持续**够久才算那一段停顿真过去了。 */
   if (ok_run < arm_frames)
      return RECOVER_NO;
   return RECOVER_TRY;
}

/* 冷却是否已过 (cd = 现在 - 上次动手)。last_ms < 0 = 本会话还没动过手 -> 放行。
 * `cd <= 0` **不放行**: 那不是"冷却已过", 那是时钟回绕 / 初值 ——
 * 把它当成"过了"就是"每次调用都动手", 与冷却这件事正好相反。 */
inline bool recover_cooldown_ok(int cd_ms, int cooldown_ms)
{
   if (cooldown_ms <= 0)
      return false;
   if (cd_ms <= 0)
      return false;
   return cd_ms >= cooldown_ms;
}

/* 总线**确实坏着** —— 动手的第二个前置条件。与 recover_verdict 是**两个问题**:
 *   recover_verdict 问"本机够不够稳、还有没有机会" (上位机侧),
 *   这一条问"总线是不是真的需要救" (总线侧)。两个都成立才动手。
 *
 * **少了这一条会怎样 (2026-09-23 实跑)**: 一条**健康**的总线在连上约 2 秒
 * (HMI_RECOVER_ARM_FRAMES) 之后就被点着 —— 白阻塞界面 1~3 s、白烧一次尝试次数 (上限只有 2),
 * 而且那一趟的 BlockTick 会把 m_gap_self_want 置起来污染帧间隔的归因。现场日志里第一次
 * 就是这样烧掉的: 它自己紧接着报「所有从站的 AL 都在 OP —— 没有一台需要恢复」,
 * 而**触发前那句**却说"从站仍不在 OP" —— 同一段日志里两句互相打脸, 而那句从没被检查过。
 *
 * 判据用 bad_wkc_run (连续多少帧 WKC 不足), **不是** mirror_ok: 从站一旦掉出 OP, WKC 就
 * 恒为 2/6, 那个数会一直涨上去 —— 正是要的"持续坏着"。arm_frames <= 0 一律不放行
 * (与 recover_verdict 同一条: 非法阈值不是"永不触发", 而 arm_frames=0 等于"一帧没坏也动手")。 */
inline bool recover_bus_broken(int bad_wkc_run, int arm_frames)
{
   return arm_frames > 0 && bad_wkc_run >= arm_frames;
}

/* 一趟自动恢复的结局 (由 EcatThread 填, 措辞由下面的 recover_done_text 出)。
 * 这里的字段**照抄 em_recover_t**, 不另立一套判断 —— 库说不该救、救不了, 这里就照着说。 */
struct RecoverReport
{
   int  need      = 0;      /* AL 不在 OP 的从站数 (0 = 病不在 AL 层) */
   int  ok        = 0;      /* 回到 OP 的 */
   int  fail      = 0;      /* 试过没回来的 */
   int  gone      = 0;      /* 没答话 (掉线/掉电) */
   int  nofit     = 0;      /* 停在 INIT/BOOT (配置丢了) */
   int  mixed     = 0;      /* 被"有轴仍在 OP 且带力矩"那道闸拦下, 一个字节没写 */
   /* em_recover_op **在读到任何从站状态之前就拒绝了** (总线没开 / 没建映射 / 没进过 OP),
    * 原因已经在控制台。**与"没救全"必须分得开**: 那一种 rc 也是 -1, 但它有 need/fail 可报,
    * 而且处置完全不同 (一个只能重连, 一个要人上去接手)。判据是"库什么数都没填"。 */
   bool refused   = false;
   bool stop      = false;  /* 中途收到停止请求 */
   int  max_gap_ms = 0;     /* 触发那一次的最长停顿, 原样说出来 */
};

/* 结局那句话。**分支顺序也是内容**: 先说不该动手的那几种 (它们的原因比结果要紧),
 * 再说结果。任何一支都不许出现"已恢复正常"这种盖过从站状态的结论 ——
 * 恢复只管过程数据, 各轴一律停在**未使能**, 那是要人接着做的第一步。 */
inline QString recover_done_text(const RecoverReport &r)
{
   if (r.stop)
      return QStringLiteral(
         "自动重请求 OP 已中止 (收到停止请求)。已动过的轴不会自动撤回, "
         "6040h 已被压成 Disable voltage, 请人工使能。");

   if (r.gone > 0 || r.nofit > 0)
      return QStringLiteral(
         "自动重请求 OP 没有执行, 一个字节都没写: %1 台从站无任何应答 (掉线/掉电), "
         "%2 台停在 INIT/BOOT (配置已丢)。这些不是 AL 层能修的, "
         "请检查线缆与驱动器供电后再重连总线。")
            .arg(r.gone).arg(r.nofit);

   if (r.mixed)
      return QStringLiteral(
         "自动重请求 OP 没有执行, 一个字节都没写: 有轴仍在 OP 且 6041h bit2 = 1 "
         "(可能正带保持力矩), 同时有轴已掉出 OP。这种局面不自动处理, "
         "请先人工确认机械安全, 再重连总线 (断开会先卸力)。");

   /* 库**在读到任何从站状态之前**就拒绝了 —— 这一支什么都不知道, 所以**一句关于从站的话
    * 都不许说** (尤其不许说"所有从站的 AL 都在 OP"—— 它根本没读过 AL)。
    * 判据见 RecoverReport::refused: 库什么数都没填。 */
   if (r.refused)
      return QStringLiteral(
         "自动重请求 OP 没有执行 (前置条件不满足, 原因已写在控制台)。"
         "总线此刻的从站状态未知, 请重连总线。");

   if (r.need == 0)
      return QStringLiteral(
         "自动重请求 OP 没有动手: 所有从站的 AL 状态都在 OP —— 这次停顿不在 AL 层, "
         "在这台 PC 或线缆上。最长 %1 ms 未发帧。请检查这台 PC 的电源计划与网卡节能。")
            .arg(r.max_gap_ms);

   /* 救回来了 —— 也必须把"各轴未使能"说出来。 */
   if (r.fail == 0 && r.ok > 0)
      return QStringLiteral(
         "自动重请求 OP 成功: %1 台从站已回到 OP, 过程数据交换恢复 (位置与状态重新更新)。"
         "各轴停在未使能 (6040h 已压成 Disable voltage), 不会自己带电 —— "
         "要出力请重新「使能」; 有故障请先「故障复位」。").arg(r.ok);

   return QStringLiteral(
      "自动重请求 OP 没有救全: 本来不在 OP 的 %1 台里回来了 %2 台, 还有 %3 台没回来。"
      "总线现在一半在 OP、一半不在, 位置与状态都不可信, "
      "不要在这个状态下继续走轴。请重连总线。")
         .arg(r.need).arg(r.ok).arg(r.fail);
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
    * tmo_s: 等 6041h bit12 的上限 (秒), 内部还夹一道。
    * ⚠️ 它会**先失能**: 6098h/6099h/609Ah/607Ch 只能在未使能时写, 竖直轴失去保持力矩。
    *
    * tmo_s **不给缺省实参** —— 本程序只有一个调用点, 逼每个将来的调用方都把话说出来。 */
   void postHome(int axis, int method, uint32_t vel_fast, int tmo_s);

   /* 断开重连时沿不沿用上一份零点。**默认 false, 也就是本类自己的老行为**: 连接那一刻的
    * 位置就是零点。`scan/` 在构造之后调一次 true。
    *
    * 这是两个程序**产品上的差别**, 不是同一件事的两种实现: scan 的操作模型里零点该跨重连
    * 连续 (同一个显示坐标必须还是同一个物理位置), hmi 的是"连接即零点"。做成成员而不是给
    * hmi 也改, 是因为 hmi 那一侧的界面文案与操作习惯全是围着后者写的。
    * 沿用还有一道硬条件 (见 ecatcmd::origin_keep_ok): 滑台必须仍在当前量程内。 */
   void setKeepOrigin(bool on);

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
      int     tmo_s  = 0;     /* CMD_HOME 用: 等 6041h bit12 的上限 (s) */
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
   void doHome(int axis, int method, uint32_t vel_fast, int tmo_s);
   void doStop();
   /* 把该轴的实际运行模式 6061h 读一次存进 m_mode_disp[axis] (SDO 读)。
    * **只许在本来就阻塞、或本来就便宜的时刻调** —— publish() / interpolate() 不许调。 */
   void readModeDisp(int axis);

   /* 把报故障那几根的 603Fh 读出来 (SDO 读, 每根一趟只读一次)。
    * 故障沿是 publish() 发现的, 而 publish() 不许做 SDO —— 于是它只挂个牌子, 由这里在
    * **下一圈的圈顶**做掉 (2ms 之后)。见 publish() 里 m_fault_read_want 那一段。 */
   void serviceFaultCodeReads();

   /* 兜底: 停顿过去了、从站没跟回来, 就自己把过程数据救回来 (em_recover_op)。
    * **只能从 run() 的主循环调** (publish(wkc) 的下一行) —— 见 run() 里那一段注释。 */
   void serviceAutoRecover(qint64 now_ms);
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

   /* ---- 零点跨重连保留 (只有 scan/ 会打开, 见 setKeepOrigin) ----
    * 下面四个**故意不被 teardown() 清**: m_origin[] 留在内存里, 重连时才有东西可沿用。
    * (断开→连接用的是同一个 EcatThread 对象, 所以这是免费的 —— 位置从来不是"对象没了"
    * 才丢的, 是 tryInitOrigin() 每次覆盖掉。) */
   bool       m_keep_origin  = false;   /* scan/ 打开; 默认 false = 老行为 */
   bool       m_origin_kept  = false;   /* m_origin[] 里那份零点还在不在 */
   int        m_origin_naxis = 0;       /* 存下那份零点时总线报了几根轴 */
   int        m_origin_gen   = 0;       /* 见 BusTelem::origin_gen */

   int32_t    m_origin[EM_MAX_AXES] = {0};
   int32_t    m_tgt   [EM_MAX_AXES] = {0};
   /* 6061h 的**上次读到值**。构造里整体置 HMI_MODE_DISP_UNREAD —— 聚合初始化剩下的会
    * 被填 0, 而 0 是合法模式号 ("未定义")。 */
   int        m_mode_disp[EM_MAX_AXES];
   bool       m_fault_latched = false;

   /* 603Fh 的**上次读到值** (HMI_FAULT_CODE_UNREAD / _FAIL / 实测值), 与"这一趟还欠它一次
    * 读吗"的牌子。同 m_mode_disp: 构造里整体置 UNREAD —— 聚合初始化剩下的会被填 0,
    * 而 0 是"无错误"这个真实读数。 */
   int        m_fault_code[EM_MAX_AXES];
   bool       m_fault_read_want[EM_MAX_AXES] = {};

   /* 故障复位进行中 (同 m_busy, 见 BusTelem::resetting); 单独存一份是为了让
    * doFaultReset 自己能在收尾时清干净 */
   bool       m_resetting = false;

   /* 回零进行中 (同上, 见 BusTelem::homing) */
   bool       m_homing    = false;
   int        m_homing_axis   = -1;
   int        m_homing_method = 0;

   /* ---- 帧间隔统计 (见 BusTelem::max_gap_ms) ----
    * 每次 em_service() 前后各取一次 clk, 相邻两次的间隔就是"多久没发帧"。
    * 不加任何计时器: 那对 elapsed() 本来就在算 dt。
    * m_svc_prev_ms < 0 = 这一趟还没发过帧 (连接那一拍), 不能拿它当一个间隔 */
   qint64     m_svc_prev_ms = -1;
   int        m_max_gap_ms  = 0;
   int        m_gaps_over   = 0;

   /* 上面那个最长间隔里, **由本程序自己的命令造成的**那一份 (复位/使能/回零期间 SOEM 的
    * SDO 事务期间一帧都不发)。见 comm_banner_text 的 self_gap_ms 与 recover_verdict ——
    * 不分开的话, 我们自己一次使能就会被读成"这台 PC 在卡"。 */
   int        m_max_gap_self_ms = 0;
   /* "下一个测到的帧间隔该算成**本程序自己的命令**造成的" —— BlockTick 构造时置起,
    * 由下一次测量消费掉 (那种命令干完就测, 中间不会插别的测量)。
    * **不在 BlockTick 析构时清**: 命令跑完那一刻正是间隔被测到的那一刻, 析构先清掉就什么都
    * 归不到自己头上了 —— 这正是要防的那个错。只用来给归因, 不参与任何安全判据。 */
   bool       m_gap_self_want = false;

   /* ---- 自动重请求 OP (见 ecatcmd::recover_verdict) ----
    * m_gap_bad_ms: 触发判据用的窗口量 —— **与本轮坏帧同生共死**, 好帧归零。
    *   不能拿 m_max_gap_ms 顶替: 那是个只增不减的会话最大值, 于是第一次停顿之后就再也
    *   回不到"停顿已经过去"这个状态, 自动恢复会**永远不动手** —— 症状与"没写这个功能"一样。
    * m_recover_last_ms: 上次动手的 clk; < 0 = 本会话还没动过手 (冷却放行)。
    * m_recover_tries: 本会话动手次数 (到 HMI_RECOVER_MAX_TRIES 就只报「重连总线」)。
    * m_recover_said: 上一次报过的结局话 (用来避免同一句话每 2ms 刷一次)。 */
   int        m_gap_bad_ms      = 0;
   /* 刚刚那一帧迟了多久 / 连续多少帧按时发出去了 (上位机的健康状况)。
    * **不是** m_max_gap_ms: 那是个会话最大值, 一次停顿之后就再也下不来。 */
   int        m_last_gap_ms     = 0;
   int        m_gap_ok_run      = 0;
   int        m_recover_last_ms = -1;
   int        m_recover_tries   = 0;
   QString    m_recover_said;    /* 上次由自动恢复报出去的那一句 (防同一句反复弹) */
   int        m_recover_warn_ms = -1;  /* 上次报"别动手"那句的时刻 (与动手冷却分开) */

   /* 连续不足帧数。**工作线程自己数**: scan/ 的 ScanController 有一份自己的中止判据,
    * 这一份是要进电文给两个程序共用的 (见 BusTelem::bad_wkc_run) */
   int        m_bad_wkc_run = 0;
   /* **连续**多少帧是好的 (好帧 ++, 坏帧归零)。自动恢复的"停机停稳了"判据就是它
    * (见 HMI_RECOVER_ARM_FRAMES) —— 与 m_bad_wkc_run 是一对, 一个数坏一个数好。 */
   int        m_good_wkc_run = 0;
   bool       m_comm_bad    = false;

   /* 最近一次读到的 AL 状态 (只在 comm_bad 的**上升沿**读一次, 见 publish())。
    * 存成员而不是只在那一圈的电文里: 用户真正遇到的是**闩锁**的报警 —— 报警过去之后
    * 那一次读到的 AL 码正是最该留在屏幕上的东西。连接时清掉。 */
   int        m_al_state   = 0;
   int        m_al_code    = 0;
   bool       m_al_checked = false;

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
