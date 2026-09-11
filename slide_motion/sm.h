/*
 * sm.h - slide_motion 共享类型、常量与原形
 *
 * slide_motion = 滑台带动作验收验证 (在 slide_verify 的只读导入验证之上再进一步)。
 *   S0 身份与总线门禁 (只读)
 *   S1 参数基线与漂移 (只读, 外部基线文件)
 *   S2 OP 循环 + DC        (本版未实现, 只留阶段位置)
 *   S3 使能状态机          (写 6040h/6060h, 通电但不移动)
 *   S4 微动 + 反馈闭环     (写 6040h/607Ah/6081h/6083h/6084h, 真的动)
 *   S5 收尾/安全停机       (无条件执行, 含所有错误路径)
 *
 * 安全边界 (改代码前务必先读这一段):
 *   1. 全工程只有 sm_guard.c 里出现 ecx_SDOwrite。其余文件一律通过
 *      sm_wr_*() / sm_set_cw() 写, 这两个函数在未授权时直接拒绝。
 *      审阅"什么会动"只需看 sm_guard.c 一个文件。
 *   2. 默认不动: 不给 --allow-motion 就全程只读; 给了还要再给 --allow-jog 才移动。
 *   3. 上限只能收紧不能放宽, 放宽必须同时给 --force-caps 且非交互确认。
 *   4. 任何异常路径 (含 Ctrl-C / 故障 / 丢站 / 超时) 都必须走 sm_guard_teardown(),
 *      让 6040h 回到失能态。
 */

#ifndef SM_H
#define SM_H

#include <stdint.h>
#include <stddef.h>

#include "soem/soem.h"

/* ======================================================================
 * 退出码契约
 * ====================================================================== */
#define SM_EXIT_OK          0  /* 所选阶段全部 PASS */
#define SM_EXIT_USAGE       1  /* 用法错误 / 初始化失败 / 基线文件打不开或格式错 */
#define SM_EXIT_NO_SLAVE    2  /* 空总线 */
#define SM_EXIT_FAIL        3  /* 存在 FAIL */
#define SM_EXIT_WARN        4  /* 无 FAIL 但含 WARN */
#define SM_EXIT_NO_YKD      5  /* 有从站但一台 YKD 都没有 */
#define SM_EXIT_REFUSED     6  /* 安全护栏拒绝: 未授权 / Fault / 限位 / 越界 / 身份门不过。未写任何东西 */
#define SM_EXIT_ABORTED     7  /* 动作已开始但异常中止 (看门狗/Ctrl-C/故障/超时/失速), 已安全停机 */
#define SM_EXIT_BASELINE    8  /* 基线文件错误或 [meta] 与现场设备不匹配 */
#define SM_EXIT_NO_ENABLE   9  /* 使能状态机未达 Operation enabled (带 SDO abort 诊断) */
#define SM_EXIT_NOT_DISABLED 10 /* 收尾写了 6040h=0 但 6041h 仍报 Operation enabled:
                                   电机可能仍然带电 —— 最严重的一类失败, 覆盖其它码 */

/* ======================================================================
 * 判定码
 * ====================================================================== */
#define SM_V_PASS 0
#define SM_V_WARN 1
#define SM_V_FAIL 2
#define SM_V_SKIP 3
#define SM_V_INFO 4

const char *sm_verdict_str(int v);

/* ======================================================================
 * 受支持的滑台驱动器识别集合 (YKD2205PE, 与 slide_verify.c 保持一致)
 * ====================================================================== */
#define SM_YKD_VENDOR_ID 0x0994UL

/* ======================================================================
 * 受支持的 AL 状态与 SDO 超时
 *
 * 注意: SOEM 的 EC_TIMEOUTRXM = 700000us = 700ms。用它做运动期的 SDO 超时,
 * 意味着 Ctrl-C / 故障后最长 700ms 才能发出停机写 —— 这个反应窗口太长。
 * 运动期一律改用 SM_SDO_TMO_MOTION (200ms), 使最坏反应时间约 250ms。
 * ====================================================================== */
#define SM_STATE_PRE_OP   EC_STATE_PRE_OP
#define SM_STATE_SAFE_OP  EC_STATE_SAFE_OP

#define SM_SDO_TMO_IDLE     EC_TIMEOUTRXM   /* 700ms, 非运动期读参数用 */
#define SM_SDO_TMO_MOTION   200             /* 200ms, 运动期一切读写用 */

/* ======================================================================
 * 硬上限 (CLI 只能收紧; 放宽需 --force-caps)
 * ====================================================================== */
#define SM_JOG_PULSES_DEF     200
#define SM_JOG_PULSES_MAX     2000
#define SM_JOG_VEL_DEF        500
#define SM_JOG_VEL_MAX        2000
#define SM_JOG_ACC_DEF        5000
#define SM_JOG_ACC_MAX        20000
#define SM_MOVE_TMO_DEF       3000
#define SM_MOVE_TMO_MAX       15000
#define SM_REPEATS_DEF        1
#define SM_REPEATS_MAX        10
/*
 * 心跳看门狗必须 > 单次轮询的最坏耗时 (3 次 SDO 读 × SM_SDO_TMO_MOTION = 600ms),
 * 否则正常的慢轮询会被自己误判成掉线。这个不等式由 sm_guard_check_caps() 强制。
 */
#define SM_WATCHDOG_DEF       1000
#define SM_WATCHDOG_MIN       700
#define SM_WATCHDOG_MAX       3000
#define SM_TOTAL_MOTION_MAX   15000   /* 全部动作累计墙钟上限 ms */

/*
 * --force-caps 能放宽到的**绝对**天花板。存在的理由: sm_guard_clamp_i32 的
 * 语义是"用户可以主动放宽", 但 -INT32_MIN 这类值在 int32 里取绝对值会溢出回
 * 负数, 骗过比较后原样传下去, 结果是 2^31 脉冲的相对运动 —— 那不是"放宽",
 * 是事故。100000 脉冲已是硬上限 (2000) 的 50 倍, 足够长的调试行程用, 又远
 * 远够不着"一条指令撞穿机械死挡"的量级。
 */
#define SM_CAP_ABS_MAX        100000L

/* 容差上限 (脉冲)。容差决定 A1/A6 断言是否还有意义, 所以它也必须受上限约束:
   把容差放到比行程还大, 等于把"动得对不对"的检查整条删掉。 */
#define SM_TOL_PULSES_MAX     500

/* ======================================================================
 * 固定节拍与阈值
 * ====================================================================== */
#define SM_STEP_TMO_MS        500     /* 使能状态机单步超时 */
#define SM_ENABLE_TMO_MS      1000    /* 0x06 / 0x07 等待 */
#define SM_ENABLE_OP_TMO_MS   2000    /* 0x0F 等待 Operation enabled */
#define SM_ENABLE_HOLD_MS     500     /* 使能后保持观察 */
#define SM_TEARDOWN_TMO_MS    300     /* 收尾每步超时 */
#define SM_POLL_MS            5       /* 运动轮询节拍 */
#define SM_STALL_MS           500     /* 命令发出后多久位置仍无变化判失速 */
#define SM_SETTLE_MS          150     /* 位置静止多久视为动作结束 */
#define SM_POS_NOISE_PULSES   50      /* 未使能时位置读数噪声上限 */
#define SM_POS_ABS_MAX        1000000000L /* 位置读数合理性上限 */
#define SM_ORIGIN_GUARD       100000L     /* 相对微动前, |当前位置| 超过此值则拒绝 */
#define SM_NO_SOFTLIMIT_CAP   2000    /* 607Dh 不可读时的收紧微动上限 */

/* ======================================================================
 * CiA402 控制字 (6040h)
 * 6041h 低 4 位是非标准编码 0000/0001/0011/0111, 所以全程用位判断, 禁止数值比较。
 * ====================================================================== */
#define SM_CW_DISABLE_V   0x0000  /* bit0=0 bit1=0                -> Disable voltage */
#define SM_CW_QUICKSTOP   0x0002  /* bit1=1 bit0=0 bit2=0         -> Quick stop active */
#define SM_CW_SHUTDOWN    0x0006  /* bit1=1 bit2=1 bit0=0         -> Ready to switch on */
#define SM_CW_SWITCHON    0x0007  /* bit0=1 bit1=1 bit2=1         -> Switched on */
#define SM_CW_ENABLE_OP   0x000F  /* + bit3=1                     -> Operation enabled */
#define SM_CW_FAULT_RST   0x0080  /* bit7=1, 随后回 0x0000 */
#define SM_CW_PP_IDLE     0x006F  /* 0x0F | bit5(0x20 立即生效) | bit6(0x40 相对) */
#define SM_CW_PP_TRIGGER  0x007F  /* 0x6F | bit4(0x10 新设定点上升沿) */

/* ======================================================================
 * CiA402 状态字 (6041h) 位掩码
 * 只对文档明确列出的位置位断言; 其余位置 (bit4/bit7/bit9/bit11) 只记录不断言。
 * ====================================================================== */
#define SM_SW_RTSO       0x0001  /* bit0  Ready to switch on   */
#define SM_SW_SWITCHED   0x0002  /* bit1  Switched on          */
#define SM_SW_OP_ENABLED 0x0004  /* bit2  Operation enabled    */
#define SM_SW_FAULT      0x0008  /* bit3  Fault                */
#define SM_SW_VOLTAGE    0x0010  /* bit4  Voltage enabled (启发式, 文档未列) */
#define SM_SW_QUICKSTOP  0x0020  /* bit5  Quick stop (文档未列) */
#define SM_SW_WARNING    0x0080  /* bit7  Warning (文档未列)   */
#define SM_SW_TARGET     0x0400  /* bit10 Target reached       */
#define SM_SW_INTLIMIT   0x0800  /* bit11 Internal limit (文档未列) */
#define SM_SW_SETPT_ACK  0x1000  /* bit12 Set-point ack (PP)   */

/* ======================================================================
 * 对象索引
 * ====================================================================== */
#define SM_OID_CONTROLWORD 0x6040
#define SM_OID_STATUSWORD  0x6041
#define SM_OID_MODES       0x6060
#define SM_OID_MODES_DISP  0x6061
#define SM_OID_ACT_POS     0x6064
#define SM_OID_ACT_VEL     0x606C
#define SM_OID_TARGET_POS  0x607A
#define SM_OID_SOFTLIM     0x607D
#define SM_OID_HOMING_OFF  0x607C
#define SM_OID_PROF_VEL    0x6081
#define SM_OID_PROF_ACC    0x6083
#define SM_OID_PROF_DEC    0x6084
#define SM_OID_HOMING_MODE 0x6098
#define SM_OID_HOMING_VEL  0x6099
#define SM_OID_HOMING_ACC  0x609A
#define SM_OID_DIG_IN      0x60FD
#define SM_OID_DIG_OUT     0x60FE
#define SM_OID_TARGET_VEL  0x60FF

#define SM_MODE_PP 1  /* 轮廓位置模式 */

/* ======================================================================
 * SDO 读结果码
 * ====================================================================== */
#define SM_RD_OK       0
#define SM_RD_ABORT    1  /* 收到 SDO abort (对象不存在 / 状态不允许等) */
#define SM_RD_TIMEOUT  2  /* 无响应 / 邮箱超时 */

/* ======================================================================
 * 数据类型提示
 * 仅用于决定"符号性"; 实际字节宽度一律以驱动器 SDO 返回的 psize 为准
 * (手册没给的对象类型就不用猜, 靠驱动器自报, 免掉一整类读错问题)。
 * ====================================================================== */
#define SM_DT_U8  0
#define SM_DT_I8  1
#define SM_DT_U16 2
#define SM_DT_I16 3
#define SM_DT_U32 4
#define SM_DT_I32 5

int sm_dt_is_signed(int dt);

/* 比对方式 */
#define SM_CMP_NONE  0  /* 只读出来显示, 不比对 */
#define SM_CMP_EXACT 1  /* 必须完全相等 */
#define SM_CMP_PCT   2  /* 相对误差须 <= tol_pct */

/* ======================================================================
 * 基线项规格表 (表驱动: 加对象只改表)
 * ====================================================================== */
#define SM_SPEC_COUNT 48   /* 规格表容量上限, 留余量; 实际条数由 sm_specs() 给出 */

typedef struct
{
   uint16_t    index;
   uint8_t     sub;
   uint8_t     dt;         /* SM_DT_*, 仅决定符号性 */
   uint8_t     cmp;        /* SM_CMP_* */
   uint8_t     dangerous;  /* 1 = 这一项漂移则禁止 S3/S4 (限位功能/电子齿轮等) */
   uint32_t    tol_pct;    /* SM_CMP_PCT 用 */
   const char *label;      /* 中文名 */
} sm_spec_t;

const sm_spec_t *sm_specs(int *count);

/* ======================================================================
 * 基线文件
 * ====================================================================== */
#define SM_MAX_AXES       16   /* 基线支持的轴数 (= 总线位置 0..15) */
#define SM_BASE_PATH_MAX  512

typedef struct
{
   int     present;
   int     strict;   /* 行首 '!' = 必需项: 不匹配判 FAIL; 否则判 WARN */
   int64_t val;
} sm_base_slot_t;

typedef struct
{
   int  loaded;
   char path[SM_BASE_PATH_MAX];
   int  warn_count;   /* 未知键/未识别节 的个数 */

   /* [meta] */
   int      have_vendor;
   int      have_product;
   uint32_t vendor_id;
   uint32_t product_code;

   /* [motion] 默认值 (可被 CLI 覆盖) */
   int     have_jp, have_jv, have_ja, have_tol;
   int64_t jp, jv, ja, tol;

   /* [axis.N] 逐项 (下标 = sm_specs() 里的槽位) */
   sm_base_slot_t slot[SM_MAX_AXES][SM_SPEC_COUNT];
} sm_baseline_t;

/* ======================================================================
 * 每轴结果
 * ====================================================================== */
#define SM_TRACE_MAX 12

typedef struct
{
   uint32_t cw;        /* 写入的 6040h */
   uint16_t sw;        /* 读到的 6041h, 仅在 sw_valid=1 时有意义 */
   /*
    * sw 是不是一次**真实读数**。0 号值不是"没读到"的哨兵 —— 0x0000 在
    * CiA402 里是合法的 "Not ready to switch on"。一条没写出去的轨迹不应该
    * 看起来像"驱动器报了个状态", 报告与 CSV 在 sw_valid=0 时必须打 "----"。
    */
   char     sw_valid;
   uint32_t ms;        /* 相对本轴开始的毫秒 */
   int      step;      /* 步骤编号 */
} sm_trace_t;

typedef struct
{
   int  slave;    /* SOEM 从站序号 1-based */
   int  pos;      /* 总线位置 0-based */
   char is_ykd;
   char reached_state;  /* 是否达到要求的 AL 状态 */
   char fail;
   char warn;
   /*
    * 本次运行是否"接管"过这根轴 (通过前置检查、准备使能/微动)。
    * 收尾与急停**只**处理 engaged 的轴: 这保证了 --axis 选中的轴之外,
    * 别的轴不会被我们写一个 0x0007 / 0x0000 出去 —— 那根轴可能正被
    * 另一套程序控制, 我们从没检查过它的状态, 更不该把它拽停。
    * 换句话说: 我们只清理自己弄脏的东西。
    */
   char engaged;

   /* S1 参数快照 (供 teardown 恢复) */
   int      have_prof_vel, have_prof_acc, have_prof_dec, have_mode;
   uint32_t prof_vel_snap;
   uint32_t prof_acc_snap;
   uint32_t prof_dec_snap;
   int8_t   mode_snap;

   /* S3 使能轨迹 */
   int        trace_n;
   sm_trace_t trace[SM_TRACE_MAX];
   uint16_t   sw_initial;
   int        enable_ok;
   int32_t    abort_code;   /* 使能失败时最近的 SDO abort */

   /* S4 微动 */
   int     jog_done;
   int32_t p_start, p_fwd_end, p_rev_end;
   int32_t d_fwd, d_rev;
   int32_t peak_fwd, peak_rev;
   uint32_t ms_fwd, ms_rev;
   int     dir_violations;
   int32_t travel_err;      /* d_fwd 与命令值的差 */
   int32_t home_err;        /* p_rev_end 与 p_start 的差 */
} sm_axis_t;

/* ======================================================================
 * 中止原因
 * ====================================================================== */
#define SM_ABORT_NONE    0
#define SM_ABORT_CTRL_C  1
#define SM_ABORT_FAULT   2
#define SM_ABORT_TIMEOUT 3
#define SM_ABORT_STALL   4
#define SM_ABORT_IO      5
#define SM_ABORT_LOST    6
#define SM_ABORT_DEADLINE 7

const char *sm_abort_str(int r);

/* ======================================================================
 * 全局 SOEM 上下文 (定义在 sm_bus.c)
 * ====================================================================== */
extern ecx_contextt g_ctx;

/* ======================================================================
 * 安全护栏 (sm_guard.c)
 * ====================================================================== */
typedef struct
{
   int authorized_motion;  /* --allow-motion: 允许使能 (通电但不移动) */
   int authorized_jog;     /* --allow-jog: 允许移动 */
   int non_interactive;    /* --yes: 跳过交互确认 */
   int force_caps;         /* --force-caps: 允许突破硬上限 */
   int allow_foreign;      /* --allow-foreign: 允许总线上有非 YKD 从站 */
   int reset_fault;        /* --reset-fault */
   int requested_state;    /* SM_STATE_PRE_OP / SM_STATE_SAFE_OP */

   volatile long abort;    /* 非 0 = 请求中止 (信号处理器只置它, 不做 I/O) */
   int           abort_reason;

   uint32_t t_start_ms;    /* 本轴动作开始时刻 */
   uint32_t last_io_ms;    /* 最近一次成功的总线交互时刻 */
   uint32_t watchdog_ms;   /* 心跳看门狗阈值 */

   int  ds402_enabled;     /* 我们认为的使能状态 (写过 0x000F) */
   int  teardown_done;     /* 收尾幂等标志 */
   int  wrote_anything;    /* 是否已发生过任何写 */
   /*
    * 收尾写完 6040h=0 之后, 6041h 仍然报告 Operation enabled —— 也就是说
    * 电机可能还通着电。这是本次运行里最严重的一种失败, 必须让退出码和汇总
    * 都反映出来, 不能只打印一行提示就完事。
    */
   int  disable_unconfirmed;
   uint32_t total_motion_ms; /* 全部动作累计墙钟 ms */
} sm_guard_t;

extern sm_guard_t g_guard;

/* 授权与参数校验。返回 0 表示允许; 非 0 为退出码 (SM_EXIT_REFUSED 等) */
int sm_guard_check_authorization(int want_motion, int want_jog);
/* 上限收紧 (只能往下, 放宽需 --force-caps)。0 表示未指定 -> 用默认值 */
uint32_t sm_guard_clamp_u32(const char *name, uint32_t val,
                            uint32_t hard_max, uint32_t dflt);
int32_t sm_guard_clamp_i32(const char *name, int32_t val,
                           int32_t hard_max, int32_t dflt);
/* 校验看门狗与 SDO 超时的不等式关系。返回 0 = 合法 */
int sm_guard_check_caps(uint32_t watchdog_ms);

/* 交互确认。返回 1 = 确认, 0 = 拒绝 */
int sm_guard_confirm(const char *ifname, int nslaves, int n_jog_axes,
                     int32_t jog_pulses, uint32_t jog_vel, uint32_t jog_acc,
                     uint32_t move_tmo_ms, uint32_t watchdog_ms);
void sm_guard_install_ctrl_handler(void);
void sm_guard_abort(int reason);

/* 写入口 —— 全工程唯一的 ecx_SDOwrite 调用点。未授权时返回 -1 且不写。 */
int sm_wr_raw(int slave, uint16_t index, uint8_t sub, int size, const void *p,
              const char *why);
int sm_wr_u8(int slave, uint16_t index, uint8_t sub, uint8_t v, const char *why);
int sm_wr_u16(int slave, uint16_t index, uint8_t sub, uint16_t v, const char *why);
int sm_wr_u32(int slave, uint16_t index, uint8_t sub, uint32_t v, const char *why);
int sm_wr_i32(int slave, uint16_t index, uint8_t sub, int32_t v, const char *why);
/* 写控制字 (6040h), 自动记录轨迹与 ds402_enabled 状态 */
int sm_set_cw(sm_axis_t *ax, uint16_t cw, const char *why);
/*
 * 把刚读到的 6041h 回填进最近一条轨迹记录 (轨迹表用)。
 * valid=0 表示这次压根没读到: 不写 sw, 该条记录保持 sw_valid=0,
 * 报告与 CSV 会打 "----" 而不是拿 0x0000 冒充一个驱动器状态。
 */
void sm_trace_fill_sw(sm_axis_t *ax, uint16_t sw, int valid);

/*
 * 急停 (尽力而为) 与收尾 (幂等, 含所有错误路径)。
 * 这两个函数**绕过授权检查**直接写 —— 否则 Ctrl-C 之后反而停不下来。
 * 它们只写"去使能"方向的 6040h, 永远不会使能。
 */
void sm_guard_emergency_stop(sm_axis_t *axes, int nslaves);
void sm_guard_teardown(sm_axis_t *axes, int nslaves);

/* 看门狗: 每次成功总线交互后调用 */
void sm_guard_feed(void);
/*
 * dead-man 检查: 距上一次成功的总线交互已超过 watchdog_ms。
 * 掉线不会自己举手 —— 这份 SOEM 里 slavelist[].islost 从不被置位, 从站
 * 静静地不来, 表现为 SDO 一直超时。所以"多久没有成功交互"才是真正能察觉
 * 掉线的量。返回非 0 = 已超时, 调用者应当中止。
 */
int sm_guard_watchdog_expired(void);
/* 距上一次成功总线交互的毫秒数 (失联日志用) */
uint32_t sm_guard_io_idle_ms(void);
/* 返回非 0 表示应中止 (只看标志, 不做 I/O) */
int sm_guard_should_abort(void);

/* 打印审计用的写入横幅 */
void sm_guard_banner(const char *ifname, int nslaves);

/* ======================================================================
 * 总线层 (sm_bus.c)
 * ====================================================================== */
/* 毫秒时钟 (单调) */
uint32_t sm_now_ms(void);

void sm_console_utf8(void);
void sm_print_adapters(void);
int  sm_is_ykd(uint32_t eep_man, uint32_t eep_id);

/* SDO 读 (≤4 字节)。命中 abort 时 *abort_code 返回中止码。
 * 必须每次先清 ctx.ecaterror 粘滞位, 否则上一次失败会污染下一次判定。 */
int sm_rd_raw(int slave, uint16_t index, uint8_t sub, int timeout,
              uint8_t *buf, int *size, int32_t *abort_code);
int sm_rd_u16(int slave, uint16_t index, uint8_t sub, uint16_t *v, int timeout,
              int32_t *abort_code);
int sm_rd_i8(int slave, uint16_t index, uint8_t sub, int8_t *v, int timeout,
             int32_t *abort_code);
int sm_rd_i32(int slave, uint16_t index, uint8_t sub, int32_t *v, int timeout,
              int32_t *abort_code);

/* 把驱动器自报的字节按 dt 的符号性解读为 int64 */
int64_t sm_bytes_to_i64(const uint8_t *buf, int size, int dt);

/* 原始寄存器读 (注意首参是 port, 不是 context) */
int sm_reg_read16(int slave, uint16_t ado, uint16_t *v);
int sm_reg_read8(int slave, uint16_t ado, uint8_t *v);

/* 诊断计数 (ECT_REG_RXERR 等), 失败返回 -1 */
int sm_diag_counters(int slave, uint32_t *rxerr, uint32_t *frxerr,
                     uint32_t *pe_cnt, uint32_t *ll_cnt);

/* 把已确认的 YKD 从站置入要求的 AL 状态; 成功返回 0 */
int sm_enter_state(int requested_state, int verbose);

/* ======================================================================
 * SDO 事务日志 (实现见 sm_bus.c)
 *
 * 为什么要它: 读路径以前完全静默, 于是 S3 失败时日志只剩一句 6041h=----,
 * 把"写 6040h 没被接受"和"读 6041h 没拿到回音"合并成了一个现象。而
 * SM_RD_TIMEOUT 本身是个合并错误码 (wkc<=0 且 ecaterror 为假, 至少覆盖
 * 邮箱帧没发出去 / 发出去了没等到回信 / 邮箱缓冲耗尽 三种情况)。
 *
 * 归并: **只对读**。连续若干笔"结果签名"相同的读折叠成一行, 签名 =
 * (slave, index, sub, rc, abort) —— S4 的 jog_leg 是没有 sleep 的忙轮询,
 * 每秒上千笔, 不折叠会淹掉日志。签名一变就落一行, 所以日志呈现的是
 * "驱动器应答的变化", 而不是"我们轮询了多少次"。
 * 写**不归并**: 写频率低, 且"写的是哪个控制字"正是要看的东西 ——
 * 把 0x0006/0x0007/0x000F 折叠成一行 "×3" 等于把使能序列抹掉。
 * ====================================================================== */
/* 运动窗口开关。关的时候会先把未收尾的一行打出来。 */
void sm_xfer_set_active(int on);
/* 记一笔 SDO 事务。size/bytes 是驱动器自报的字节数与原始字节;
 * 读失败时传 size=0, bytes=NULL —— 不能拿清过零的缓冲冒充一个读数。 */
void sm_xfer_note(char dir, int slave, uint16_t index, uint8_t sub,
                  int rc, int32_t abort, int wkc, int ecerr, int size,
                  const uint8_t *bytes, uint32_t ms);
/* 把当前未收尾的 run 打出来。结论(PASS/FAIL)前必须调一次, 免得证据迟到。 */
void sm_xfer_flush(void);

/* 打印 SOEM 错误栈 (排空), 返回排空的条数。用于运动期捕获 EMCY/SDO 错误。 */
int sm_drain_errors(int slave, int print_emcy);
/* 消费 ctx.ecaterror 并取回首个匹配的 abort 码 (不清栈) */
int32_t sm_take_abort(int slave, uint16_t index, uint8_t sub);

/* ======================================================================
 * 基线 (sm_baseline.c)
 * ====================================================================== */
/* 解析基线文件。返回 0 = 成功; SM_EXIT_BASELINE = 内容错误; SM_EXIT_USAGE = 打不开 */
int sm_base_load(const char *path, sm_baseline_t *b);
/* 把现场实测值导出为基线文件。返回 0 = 成功 */
int sm_base_dump(const char *path, const sm_axis_t *axes, int nslaves);
/* 无基线时打印当前实测值表 (只读, 不做判定) */
void sm_base_show(const sm_axis_t *axes, int nslaves);
/* S1: 逐轴比对并打印。更新 ax->fail / ax->warn。
 * 返回 0 = 完成。dangerous 漂移会置 *dangerous_drift = 1 */
int sm_base_compare(const sm_baseline_t *b, sm_axis_t *ax, int nslaves,
                    int *dangerous_drift);
/* 从基线取某个 spec 槽的期望值 (按 index/sub 找) */
const sm_base_slot_t *sm_base_lookup(const sm_baseline_t *b, int pos,
                                     uint16_t index, uint8_t sub);

/* ======================================================================
 * 运动层 (sm_motion.c)
 * ====================================================================== */
/* 只读快照 6081h/6083h/6084h/6060h 到 ax->*_snap (收尾恢复用, 预演也要用) */
void sm_snapshot_pp(sm_axis_t *ax);
/* S3: 使能状态机。返回 SM_V_PASS / SM_V_WARN / SM_V_FAIL / SM_V_SKIP */
int sm_stage_enable(sm_axis_t *ax, uint32_t enable_hold_ms);
/* S4: 微动 + 反馈闭环。返回 SM_V_* */
int sm_stage_jog(sm_axis_t *ax, int32_t delta, uint32_t vel, uint32_t acc,
                 uint32_t dec, uint32_t move_tmo_ms, int repeats,
                 int32_t tol_pulses);
/* 预演: 只打印将要写什么, 一个字节都不写 */
void sm_stage_dry_run(const sm_axis_t *axes, int nslaves, int32_t delta,
                      uint32_t vel, uint32_t acc, uint32_t dec,
                      uint32_t move_tmo_ms, int repeats);
/* 前置检查 (在第一次写之前, 全程只读)。返回 0 = 允许动作, 非 0 = 退出码 */
int sm_preflight(sm_axis_t *ax, int nslaves, int32_t delta);

#endif /* SM_H */
