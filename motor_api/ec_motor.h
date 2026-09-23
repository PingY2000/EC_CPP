/*
 * ec_motor.h - 多轴 CiA402 电机调用接口 (位置同步 CSP / 速度 PV / 回零 HM)
 *
 * 6060h: CSP = 8 (每周期下发目标位置) / PV = 3 (每周期下发目标速度) / HM = 6
 * (6098h 方式 + 6040h bit4 上升沿启动)。多轴 = 一根网卡、一个进程、一个周期帧喂所有轴。
 * 目标值必须每周期刷新, 故 6040h/607Ah/60FFh 进 RxPDO, 6041h/6064h/606Ch 进 TxPDO
 * (SOEM 的 SDO 超时 700ms, 远大于同步周期)。
 *
 * 本驱动器没有 CSV (6060h = 9): 手册 6502h = 0x00A5 = PP + PV + HM + CSP, 速度模式只能是 PV。
 *
 * 偏移只许现场实读: 一律实读 1C12h/1C13h/1600h/1A00h 推字节偏移, 推不出就拒绝 (返回 -1);
 * 缺失的映射项只追加不替换。写入口唯一: 本目录只有 ec_motor.c 出现 ecx_SDOwrite, 会写的
 * 只有 6060h、6098h/6099h/609Ah/607Ch、经镜像写的 6040h、1C12h/1C13h/1600h/1A00h (仅 RAM),
 * 以及 2300h 的 bit0~bit2 (需 em_allow_param_write, 收尾由 em_shutdown 还原原值);
 * 从不写 2102h (EEPROM) / 607Dh 软限位 / 2400h/2408h/2409h。
 * 2300h 会不会被驱动器自己落 EEPROM 手册没写 -> 对外文案不许说"断电即回"。
 *
 * 安全: em_enable 之后电机通电、有保持力矩, CSP/PV/回零都会真的移动滑台。收尾必须无条件走
 * em_shutdown(); *motor_maybe_live = 1 表示写了失能但 6041h 仍报 Operation enabled。
 */
#ifndef EC_MOTOR_H
#define EC_MOTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 退出码 (0/1/2/3/4/5/6/10, 另加 7 = 回零失败) */
#define EM_EXIT_OK           0  /* 全部阶段 PASS */
#define EM_EXIT_USAGE        1  /* 用法错误 / 初始化失败 / 网卡打不开 */
#define EM_EXIT_NO_SLAVE     2  /* 空总线 / 无目标轴 / 目标不是 YKD */
#define EM_EXIT_FAIL         3  /* 存在 FAIL */
#define EM_EXIT_REFUSED      4  /* 安全护栏拒绝: 未授权 / 身份门不过 / 偏移证不出 / 映射缺项且不许改写 */
#define EM_EXIT_NO_OP        5  /* 进不去 SAFE_OP 或 OP */
#define EM_EXIT_NO_PDO       6  /* 过程数据未落地 (WKC 持续偏短) */
#define EM_EXIT_HOMING       7  /* 回零未达位 / 回零出错 */
#define EM_EXIT_NO_DISABLE  10  /* 收尾写了 6040h=0 但 6041h 仍报 Operation enabled: 电机可能仍带电 */

/* 运行模式 (6060h) */
#define EM_MODE_PP   1  /* 轮廓位置 (本接口不实现) */
#define EM_MODE_PV   3  /* 速度模式 (本机唯一可用的速度模式) */
#define EM_MODE_HM   6  /* 回零 */
#define EM_MODE_CSP  8  /* 位置同步模式 */

/* 容量与默认值 */
#define EM_MAX_AXES     8      /* 本接口最多同时驱动的轴数 */
#define EM_MAP_MAX      8      /* 一个 PDO 映射对象最多几项 (本驱动器 1~5 项, 留余量) */
#define EM_IOMAP_MAX 4096      /* 过程数据镜像缓冲 */

#define EM_POS_TOL_DEF      50    /* CSP 到位容差默认值 (pul) */
#define EM_MAX_DELTA_DEF  50000   /* 单次运动位移上限默认值 (pul) */
/* 轮廓加减速度兜底值 (pul/s²); 本机 6083h/6084h 实读值就是 500000, 读不到驱动器自己的值时才用 */
#define EM_RAMP_ACC_DEF 500000u
#define EM_RAMP_DEC_DEF 500000u
#define EM_POLL_MS           2    /* 周期轮询节拍让步 */
#define EM_STEP_TMO_MS    1000    /* 使能状态机单步超时 */
#define EM_SDO_TMO_MOTION  200    /* 运动期的 SDO 超时 (非运动期用 EC_TIMEOUTRXM) */

/* em_recover_op 的两个预算。**与 EM_EXIT_* 分开**: 那几个是进程退出码, 这两个不是。
 * 单级 1s 与 em_shutdown 降回 PRE_OP 同量级; 整趟 3s 是"工作线程最长被占多久"的上界,
 * 调用方那句"正在自动重请求 OP"背后就是这个数。 */
#define EM_RECOVER_TMO_MS        1000
#define EM_RECOVER_TOTAL_TMO_MS  3000

/* 连续多少个短帧就把 mirror_ok 降回 0 (收到一帧完整的就归零)。取 20:
 * 按本机实测 ~3ms/圈约 60ms —— 单帧抖动不会误判, 而链路真断了 60ms 内就认账。
 * 与 ScanController 那边"WKC 连续不足 10 帧"同一量级 (30Hz ≈ 1/3 秒)。 */
#define EM_SHORT_FRAMES_LIMIT 20

/* 603Fh 没有读数时的返回值 (映射里没有, 或还没收到过完整帧)。
 * 与 0x0000 必须分得开 —— 0 是驱动器在说"我没有故障"。 */
#define EM_ERR_CODE_UNREAD (-1)

/* YKD2205PE 身份集合 */
#define EM_YKD_VENDOR_ID 0x0994UL
#define EM_YKD_PRODUCT_1 0x2000UL
#define EM_YKD_PRODUCT_2 0x3000UL

/* CiA402 对象索引 —— 只列本接口真正会碰的 */
#define EM_OID_ERROR_CODE    0x603F
#define EM_OID_CONTROLWORD   0x6040
#define EM_OID_STATUSWORD    0x6041
#define EM_OID_MODES         0x6060  /* 运行模式控制 */
#define EM_OID_MODES_DISP    0x6061  /* 运行模式显示 (读回确认驱动器接受了) */
#define EM_OID_ACT_POS       0x6064
#define EM_OID_ACT_VEL       0x606C
#define EM_OID_TARGET_POS    0x607A
#define EM_OID_HOMING_OFF    0x607C
#define EM_OID_SOFTLIM       0x607D
#define EM_OID_PROF_VEL      0x6081  /* 轮廓速度 (PP 用; 本接口不做 PP) */
#define EM_OID_PROF_ACC      0x6083  /* 轮廓加速度 (PV 的斜坡靠它) */
#define EM_OID_PROF_DEC      0x6084  /* 轮廓减速度 */
#define EM_OID_GEAR_ENABLE   0x2201  /* 0 细分有效 / 1 电子齿轮有效 —— 决定行程当量 */
#define EM_OID_SUBDIV        0x2400  /* 细分: 电机一圈脉冲数, 本机实测 50000 */
#define EM_OID_HOMING_MODE   0x6098
#define EM_OID_HOMING_VEL    0x6099  /* :01 找原点速度 / :02 返回速度 */
#define EM_OID_HOMING_ACC    0x609A
#define EM_OID_DIG_IN        0x60FD
#define EM_OID_TARGET_VEL    0x60FF
#define EM_OID_HOMING_AUX    0x2214  /* 回零辅助: 决定回零后 6064h 显示什么 */
#define EM_OID_DI_LOGIC      0x2300  /* 输入端子有效电平逻辑 (U16 RW, 手册 V2.4 p84) */

/* 6098h 回零方式号 —— 本仓库用到的那四个。手册 V2.4 p46~p48 那张表里还有别的,
 * em_home() 自己只查 [1,14] ∪ [17,30] ∪ {33,34,35}; 这四个是**上位机放行**的那一组。
 *   24/29 = 找**原点开关** (X0), 一正一反;
 *   18/17 = 找**限位开关** (手册叫"找限位"): 18 = 以正限位为原点, 17 = 以负限位为原点。
 * 后两个各带 a)/b) 两条分支 (启动时那个开关没压着 / 已经压着), 落点都是**开关的释放点**。 */
#define EM_HOME_MODE_ORIGIN_POS  24
#define EM_HOME_MODE_ORIGIN_NEG  29
#define EM_HOME_MODE_LIMIT_POS   18
#define EM_HOME_MODE_LIMIT_NEG   17

/* 找限位时"目标那个开关"与"另外那一侧"分别对应 60FDh 的哪一位 —— **只此一处**。
 * 现在的使用者只有界面侧 (hmi/ecatworker.h 的 home_lim_target_active / _other, 判 a)/b)
 * 分支与那道否决)。2026-09-21 之前 motor_test 的发起前预检也读同一份 —— 那个程序已从仓库
 * 移除, 但这个宏留着: 判据只能有一份, 两边各写一份就会各错一份, 而错了正好是"方向反了",
 * 现场看着就是"按找正限位它朝负限位冲"。
 * 非 17/18 返回 0 (没有目标开关) —— 调用方照 0 处理。 */
#define EM_HOME_LIM_TARGET_BIT(m) \
   (((m) == EM_HOME_MODE_LIMIT_POS) ? EM_DI_POS_LIMIT \
    : (((m) == EM_HOME_MODE_LIMIT_NEG) ? EM_DI_NEG_LIMIT : 0u))

#define EM_HOME_LIM_OTHER_BIT(m) \
   (((m) == EM_HOME_MODE_LIMIT_POS) ? EM_DI_NEG_LIMIT \
    : (((m) == EM_HOME_MODE_LIMIT_NEG) ? EM_DI_POS_LIMIT : 0u))

/* PDO 配置对象 (CiA301 通信区) */
#define EM_OID_RXPDO_ASSIGN  0x1C12
#define EM_OID_TXPDO_ASSIGN  0x1C13
/* 只是分配为空时的兜底值; 生效的 PDO 一律实读 1C12h/1C13h (真机上 1C12h 指的是 1601h) */
#define EM_OID_RXPDO0        0x1600
#define EM_OID_TXPDO0        0x1A00

/* 数字输入 60FDh —— 三个开关。位定义见 docs/ykd2205pe_ci402.md。
 * 本机接线 (2310h~2312h 实测 = 1/2/3): X0 原点 -> bit2, X1 正限位 -> bit1, X2 负限位 -> bit0。
 * 三位是经 2300h 电平反转 + 2310h 功能映射之后的结果 (手册 V2.4 p84), 不必再取反。
 */
#define EM_DI_NEG_LIMIT  0x00000001u  /* bit0 负限位 */
#define EM_DI_POS_LIMIT  0x00000002u  /* bit1 正限位 */
#define EM_DI_HOME       0x00000004u  /* bit2 原点开关 */
#define EM_DI_X3         0x00000008u  /* bit3 X3 (本机未接线, 2313h = 0) */

/* 2300h 输入端子有效电平逻辑: bit0~bit2 = X0~X2, 每位 0 = 常开 / 1 = 常闭 (手册 V2.4 p84)。
 * 现场是 NPN 传感器 (高电平 = 未触发), 而本机 2300h = 0 (常开 = 高电平算触发) —— 两者正好
 * 反着, 于是"没触发"被读成"触发": 60FDh 的 bit1/bit2 同时置起、6041h bit11 恒为 1、
 * 2204h = 0 (超程停车 = 停止) 把两个方向都挡死 -> 回零进得去但一动不动。
 * 置 0x0007 才是 NPN 该有的极性 (驱动器自己的限位保护也跟着一起对)。
 * 高几位 (bit3 以上) 不属于本掩码, 一旦某台机器 2300h 里写了别的位, 比对时不该管它。 */
#define EM_DI_LOGIC_MASK     0x0007u
#define EM_DI_LOGIC_NPN      0x0007u

/* 三位已经等于想要的值就不用写 —— 写 = 改驱动器, 能不写就不写, 也不留还原负担 */
#define EM_DI_LOGIC_EQ(cur, want) \
   ((((uint32_t)(cur) ^ (uint32_t)(want)) & EM_DI_LOGIC_MASK) == 0u)

/* 控制字 6040h —— 推送过的值只有下面这几个, 不试探厂商私有控制字 */
#define EM_CW_DISABLE_V  0x0000  /* Disable voltage */
#define EM_CW_SHUTDOWN   0x0006  /* Shutdown */
#define EM_CW_SWITCHON   0x0007  /* Switch on (也是 Disable operation) */
#define EM_CW_ENABLE_OP  0x000F  /* Enable operation —— 电机从这里开始带电 */
#define EM_CW_HOMING_GO  0x001F  /* 0x000F | bit4: HM 模式启动回零 (要 0->1 上升沿) */
#define EM_CW_FAULT_RST  0x0080  /* Fault reset (bit7 上升沿) */

/* 状态字 6041h (手册 V2.4 p11: bit5/bit6 是有定义的) */
#define EM_SW_RTSO        0x0001  /* bit0  Ready to switch on */
#define EM_SW_SWITCHED    0x0002  /* bit1  Switched on */
#define EM_SW_OP_ENABLED  0x0004  /* bit2  Operation enabled (**=1 就是电机带电**) */
#define EM_SW_FAULT       0x0008  /* bit3  Fault */
#define EM_SW_VOLTAGE     0x0010  /* bit4  Voltage enabled (上电后该位置 1) */
#define EM_SW_QUICKSTOP   0x0020  /* bit5  Quick stop (0 = 正在快速停止) */
#define EM_SW_SOD         0x0040  /* bit6  Switch on disabled */
#define EM_SW_WARNING     0x0080  /* bit7  Warning */
#define EM_SW_REMOTE      0x0200  /* bit9  Remote (**0 = 控制字不可操作**) */
#define EM_SW_TARGET      0x0400  /* bit10 按模式: PP/PV/HM = Target reached */
#define EM_SW_INTLIMIT    0x0800  /* bit11 Internal limit active (硬件限位有效) */
#define EM_SW_BIT12       0x1000  /* bit12 按模式 —— 见下面四个别名 */
#define EM_SW_BIT13       0x2000  /* bit13 按模式 */

/* bit12/bit13 的按模式别名 (同一根线, 含义随 6060h 变) */
#define EM_SW_PP_SETPT_ACK  EM_SW_BIT12  /* PP: Set-point acknowledge */
#define EM_SW_PV_SPEED_ZERO EM_SW_BIT12  /* PV: Speed (1 = 速度为 0) */
#define EM_SW_HM_ATTAINED   EM_SW_BIT12  /* HM: Homing attained */
#define EM_SW_PP_FOLLOW_ERR EM_SW_BIT13  /* PP: Following error */
#define EM_SW_HM_ERROR      EM_SW_BIT13  /* HM: Homing error */

/* 状态字判读 —— 集中在 ec_motor.c, 调用方不要自己写位运算 */

typedef enum
{
   EM_ST_NOT_READY = 0,          /* Not ready to switch on */
   EM_ST_SWITCH_ON_DISABLED,     /* Switch on disabled */
   EM_ST_READY_TO_SWITCH_ON,     /* Ready to switch on (电机释放) */
   EM_ST_SWITCHED_ON,            /* Switched on */
   EM_ST_OPERATION_ENABLED,      /* Operation enabled (**电机带电**) */
   EM_ST_QUICK_STOP_ACTIVE,      /* Quick stop active */
   EM_ST_FAULT_REACTION_ACTIVE,  /* Fault reaction active */
   EM_ST_FAULT,                  /* Fault */
   EM_ST_UNKNOWN
} em_sw_state_t;

em_sw_state_t em_sw_state(uint16_t sw);          /* 按 bit6/5/3/2/1/0 的 8 态表 */
const char   *em_sw_state_str(uint16_t sw);      /* 中文名 */
const char   *em_sw_describe(uint16_t sw);       /* 带按模式位的更详细一行 */

/* 「未使能且无故障」的判据 = (sw & 0x000C) == 0 (bit2 未使能, bit3 无故障); 不要写成
 * (sw & 0x000F) == 0 —— 上电自检完成后驱动器合法地停在低 4 位 = 0001 (Ready to switch on)。 */
int em_sw_disabled_no_fault(uint16_t sw);

/* bit9 = 0 时无法对控制字进行操作 (实测 0x0210/0x0231 的 bit9 都是 1) */
int em_sw_remote_ok(uint16_t sw);

typedef struct em_bus  em_bus_t;   /* 不透明: 内部持有 ecx_contextt 与过程数据镜像 */
typedef struct em_axis em_axis_t;  /* 不透明: 一根轴 */

/* 选轴配置 */
typedef struct
{
   int     bus_pos;   /* 0-based 总线位置 (= 线序 - 1, 与 SOEM 的 slave 序号差 1) */
   int32_t pos_tol;   /* CSP 到位容差 (pul); <= 0 取 EM_POS_TOL_DEF */
} em_axis_cfg_t;

/* 从站信息 (只读快照) */
typedef struct
{
   int      pos;          /* 0-based 总线位置 */
   int      slave;        /* 1-based 从站序号 */
   uint16_t configadr;
   uint16_t aliasadr;     /* 拨码站号 (0 = 拨码为 0 或未启用) */
   uint32_t eep_man;
   uint32_t eep_id;
   uint32_t eep_rev;
   uint16_t state;        /* AL 状态字 (含错误位 0x10) */
   uint16_t alstatuscode;
   uint32_t Obytes;
   uint32_t Ibytes;
   int      is_ykd;
} em_slave_info_t;

/* 回零参数 (对应 6098h / 6099h:01 / 6099h:02 / 609Ah / 607Ch) */
typedef struct
{
   int      method;      /* 6098h 回零方式。建议 24: 原点开关(X0)为原点, 正向先找 */
   uint32_t vel_fast;    /* 6099h:01 找原点速度 (pul/s) */
   uint32_t vel_slow;    /* 6099h:02 找到原点后返回速度 (pul/s), 越小精度越高 */
   uint32_t acc;         /* 609Ah 回零加减速度 (pul/s^2) */
   int32_t  offset;      /* 607Ch 原点偏移 / 原点补偿 (pul) */
} em_home_cfg_t;

/* 填一组保守的默认回零参数 (方式 24, 速度很低 —— 首次回零务必慢) */
void em_home_cfg_default(em_home_cfg_t *c);

/* 停止请求 —— Ctrl-C 的处理器只能调 em_request_stop() (只置标志位, 不做任何 I/O) */
void em_request_stop(void);
int  em_stop_requested(void);
void em_clear_stop(void);

em_bus_t *em_bus_new(void);
void      em_bus_free(em_bus_t *bus);

/* 打开网卡 + ecx_config_init。返回从站数; <0 = 失败 (原因已打印) */
int em_open(em_bus_t *bus, const char *ifname);

/* 打开失败时列出可用网卡 */
void em_print_adapters(void);

/* 网卡清单, 供界面填下拉框。name 就是 em_open() 要的那个字符串 (Windows Npcap 形如
 * `\Device\NPF_{GUID}`), 128 字节与 SOEM 的 ec_adaptert 同宽。
 * 返回填进 out 的条数 (0 = 一块网卡都没有); max <= 0 时只数不填。 */
typedef struct
{
   char name[128];
   char desc[128];
} em_adapter_t;

int em_list_adapters(em_adapter_t *out, int max);

/* 把控制台输出代码页设成 UTF-8, 应在 main() 第一句调它 (幂等); 模块内部也会惰性调一次,
 * 但那之前的汉字输出会是乱码 (源码 UTF-8, Windows 控制台默认 GBK)。 */
void em_console_init(void);

/* 毫秒睡眠 —— 调用方写自己的过程数据循环时要用它做节拍 (空转会吃满一个核) */
void em_sleep_ms(int ms);

int em_slave_count(const em_bus_t *bus);
int em_slave_info(const em_bus_t *bus, int slave, em_slave_info_t *out);

/* 选轴 + 补 PDO 映射 + 建过程数据 + 逐轴证明偏移 + 上 SAFE_OP。返回 0 成功 / -1 拒绝或失败。
 * allow_remap = 0: 需要的对象已经全在映射里就继续; 缺任何一项就拒绝, 一个字节都不写。
 * allow_remap = 1: 允许往 1C12h/1C13h/1600h/1A00h 追加缺失项 (仅 RAM), 写前仍会证明偏移。 */
int em_setup(em_bus_t *bus, const em_axis_cfg_t *cfg, int naxis, int allow_remap);

int        em_axis_count(const em_bus_t *bus);
em_axis_t *em_axis(em_bus_t *bus, int i);              /* i = 0 .. em_axis_count()-1 */
em_axis_t *em_axis_by_pos(em_bus_t *bus, int bus_pos); /* 按 0-based 总线位置找 */

int em_expected_wkc(const em_bus_t *bus);

/* 配置 DC 并请求所有轴进 OPERATIONAL。返回 0 / -1 */
int em_enter_op(em_bus_t *bus, int use_dc, uint32_t cycle_us);

/* ---- 自动重请求 OP: 怎么答复 ---------------------------------------------------- */
/* 四个计数用于组织给操作员的话; 两张位图用于**只对被动过的那几根轴**收尾
 * (位 i = 轴 i; EM_MAX_AXES = 8, 装得下 uint32_t)。 */
typedef struct
{
   int      need;      /* AL 不在 OP 的从站数。**0 = 没有一台需要恢复 -> 病不在 AL 层** */
   int      ok;        /* 重请求之后回到 OP 的从站数 */
   int      fail;      /* 试过但没回来 */
   int      gone;      /* AL 读回来是 0 (没答话: 掉线 / 掉电) */
   int      nofit;     /* 停在 INIT/BOOT (配置已丢) —— 这一种只有"断开->重连" */
   int      mixed;     /* 被"有轴仍在 OP 且带力矩"那道闸拦下, **一个 AL 写都没发** */
   uint32_t was_out;   /* 动手之前 轴 i 的 AL 不在 OP (调用方据此重映射目标) */
   uint32_t still_out; /* 走完这一趟 轴 i 的 AL 仍不在 OP */
} em_recover_t;

/* 自动重请求 OP —— 把**过程数据交换**救回来。**不是**自动重新给力矩。
 *
 * 明确不做的事 (写在这里, 因为它比"做了什么"更容易被后人改坏):
 *   · 不重新给力矩: 不调 em_enable / em_arm / em_set_mode。掉出 OP 的轴在请求 OP 之前
 *     其 RxPDO[6040h] 被压成 Disable voltage, OP 一恢复驱动器看到的是 switch-on-disabled;
 *   · 不清驱动器故障: 一个 SDO 都不做、不写 6040h bit7 —— 故障去 em_fault_reset(), 那条
 *     路有人工闸 (且 0xFF01 过流那类靠反复复位硬顶会顶坏硬件);
 *   · 不碰仍在 OP 的从站: 它可能正带保持力矩, 压它的 6040h 会真的卸力 (竖直轴会掉下来)。
 *     所以"逐台判断"不是啰嗦, 是安全 —— 这里没有"整组一起处理"的代码路径;
 *   · 不重建 PDO 映射: 从站回到 PRE-OP/INIT 之后的映射对不对不上一律交"断开->重连"。
 *
 * 触发时机由调用方决定 (本函数只管"怎么救")。返回 0 = 跑完一趟 (成败看 *out) /
 * -1 = 拒绝或没有救全 / EM_R_STOP = 中途收到停止请求。**out 是递增填的**: 任何一条提前
 * 返回带出去的数都不会谎报。out == NULL 一律拒绝 —— "几台需要恢复"本身就是结论的一半。 */
int em_recover_op(em_bus_t *bus, em_recover_t *out);

/* 收发一帧并更新各轴镜像。返回 wkc; -1 = 致命错误 (调用者应停止)。镜像只在整帧完整时
 * 更新 (wkc >= 期望 WKC): 短帧时镜像是陈值或半个帧。 */
int em_service(em_bus_t *bus);

/* ---- 每周期回调 (可选) ----
 * 每一帧 (em_service / 各阻塞函数内部自己跑的那些帧) 收完、镜像更新完就调一次 fn(user, wkc)。
 * 用途只有一个: **阻塞命令期间调用方的循环还在不在**。em_home / em_fault_reset /
 * em_enable_all / em_wait_sw 这些函数内部自己跑周期帧, 调用方那一刻正卡在它们里面, 于是
 * 调用方自己那一圈"每帧更新一次界面"的活整整停掉一个动作那么久 (回零能跑满 30 秒)。
 * 挂上回调, 那些帧里也能把界面喂一次。
 *
 * 三条约定:
 *   · **在调用者的线程里同步跑**, 每帧一次 —— 里面别做重活, 更不许回调本 API 的阻塞函数
 *     (em_home 等), 也不许再收发帧;
 *   · **只能读**: 回调能改的是调用方自己那份快照, 不许写输出镜像 (否则会插进一段
 *     没人预期的过程数据, 回零/复位那类时序就变了);
 *   · fn = NULL 注销。bus 为 NULL 时什么都不做 (便于写在收尾路径上)。
 * 不挂 = 行为与本函数不存在时一模一样 —— 本仓库只有 scan / hmi 用这个库, 它们挂在
 * 阻塞命令期间 (见 EcatThread::BlockTick); 不挂在任何地方都等价于这段代码不存在。 */
typedef void (*em_cycle_fn)(void *user, int wkc);
void em_set_cycle_hook(em_bus_t *bus, em_cycle_fn fn, void *user);

/* 轴的 AL 状态字实读 (含错误位 0x10) */
uint16_t em_al_state(em_bus_t *bus, int slave);

/* 版本字符串 —— 打印在横幅里, 便于事后对日志 */
const char *em_version(void);

/* 详细程度: 0 = 只报错与拒绝 (库的默认), 1 = 报告每个周期/每步的进展 */
void em_set_verbose(em_bus_t *bus, int on);

/* 收尾 —— 无条件执行, 正常跑完 / FAIL / 前置条件失败 / Ctrl-C 都走这里。
 * restore_mapping = 1: 把跑之前快照下来的 PDO 映射写回去。
 * motor_maybe_live (可空): 置 1 = 未能确认全部轴失能, 电机可能仍带电。 */
void em_shutdown(em_bus_t *bus, int restore_mapping, int *motor_maybe_live);

/* 只读诊断 —— 这里全是读, 没有裸写函数 */

/* 读一个对象, *size 传入传出 (必须先用缓冲大小初始化)。返回 0 / -1
 *
 * ⚠️ **一条 SDO 事务期间, 过程数据帧一帧都不发** (2026-09-22 实测确认, 这不是"让圈期
 * 变长", 是**把圈停掉**)。SOEM 的 SDO 走邮箱: ecx_SDOread -> ecx_mbxreceive
 * (SOEM/src/ec_coe.c:117 -> SOEM/src/ec_main.c:1600), 那里只有邮箱状态轮询
 * (ecx_readmbxstatus / FPRD) 与 osal_usleep, **没有任何 processdata 调用**。
 * 也就是说: 在 OP 里做一条 SDO, 等于对驱动器说"我这一段时间不喂你了" —— 而驱动器
 * 的 SM 看门狗照样在数 (AL 状态码 0x001B Sync manager watchdog 就是它踢出来的)。
 *
 * 因此这个函数只适合两种地方: ① 还没进 OP 的连接/配置期 (那时没有过程数据可静默,
 * 上面那件事也就不成立); ② 已经用 em_set_sdo_timeout() 把超时压短了的时期。
 * 周期循环里"顺手读一个监视量"是最危险的一种用法 —— 越是驱动器不正常的时候,
 * 越读不到, 静默就越长。 */
int em_sdo_read(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                void *p, int *size);

/* 把本连接**所有** SDO 读的超时改成 tmo_us (微秒; <= 0 被忽略)。作用范围包含
 * em_rd_u8/u16/u32/i8/i32/any —— 它们最后都走 em_sdo_read。
 *
 * 为什么需要它: 超时 = "这次读最多把过程数据静默多久" (见上), 而 OP 期间的过程数据正是
 * 驱动器看门狗在盯的东西。默认 EC_TIMEOUTRXM (700ms) 是配置期的值; **进了 OP 就该压短**,
 * 按"正常应答的几倍"取 (实测正常应答 1~3ms, 60ms 已是二十来倍)。实际最长阻塞 ≈ 超时 +
 * 邮箱发送上限 EC_TIMEOUTTXM (20ms)。
 *
 * 什么时候调: 进 OP 之后、任何一次读之前。事后不必调回去 —— 一次连接一个 bus,
 * 关掉就没了, 下一次 em_bus_new 又是默认值。
 * 失败时驱动器那条迟到的应答不会被下一次读当成自己的结果: ecx_SDOread 发请求前会把
 * 邮箱里的残包丢掉, 收下之后还会比对 Index。 */
void em_set_sdo_timeout(em_bus_t *bus, int tmo_us);

int em_rd_u8 (em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint8_t  *v);
int em_rd_i8 (em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int8_t   *v);
int em_rd_u16(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint16_t *v);
int em_rd_u32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint32_t *v);

/* 只读诊断: 按驱动器自报的宽度读一个不超过 4 字节的对象, 宽度经 *size_bytes 带出。
 * 与 em_rd_u16/em_rd_u32 不同 —— 那几个要求宽度相符, 不符就失败。实机上 2201h/2400h/
 * 2408h/2409h 自报 2 字节, 6502h/6064h/607Dh 是 4 字节。值按小端拼进 *val。返回 0 / -1 */
int em_rd_any(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
              uint32_t *val, int *size_bytes);
int em_rd_i32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int32_t  *v);

/* 打印 1C12h/1C13h/1600h/1A00h 的实读值、SM2/SM3 长度, 以及每个字段推出的字节偏移 */
void em_dump_pdo(em_bus_t *bus, int slave);

int         em_axis_slave(const em_axis_t *ax);   /* 1-based 从站序号 */
int         em_axis_buspos(const em_axis_t *ax);  /* 0-based 总线位置 */
int         em_axis_index(const em_axis_t *ax);   /* 0 .. n-1 */
const char *em_axis_label(const em_axis_t *ax);   /* 形如 "轴0(从站1)" */

/* 单次运动位移上限 (pul)。em_csp_move_* 与 em_pv_run_for 都会拒绝超过它的请求。
 * 默认 EM_MAX_DELTA_DEF; 没有"传 0 表示不限"这种写法。 */
int  em_axis_set_move_limit(em_axis_t *ax, uint32_t max_delta_pul);
uint32_t em_axis_move_limit(const em_axis_t *ax);

/* 设定运行模式 (写 6060h, 再读 6061h 确认驱动器认了)。已使能时拒绝 —— CiA402 规定模式
 * 只能在未使能时改。
 * 传输方式由 setup 时实读决定: 6060h 在生效 RxPDO 里就走过程数据 (写输出镜像 + 打帧),
 * 不在里面才走 SDO —— 在生效 RxPDO 里的对象是主站拥有的, SDO 写会被下一帧撤销。
 * 走哪条路可用 em_modes_via_pdo() 查。 */
int em_set_mode(em_axis_t *ax, int mode);

/* 运行模式是不是经过程数据驱动的 (即 6060h 在生效的 RxPDO 里) */
int em_modes_via_pdo(const em_axis_t *ax);
int em_modes_offset(const em_axis_t *ax);   /* 6060h 的字节偏移, -1 = 不在映射里 */

/* 轮廓加减速度 6083h / 6084h (pul/s²) —— PV 的斜坡由它们决定。两者和 6060h 一样在生效的
 * RxPDO 里 (本机 1601h 第 6/7 项, 输出镜像 +15 / +19), 不写就是下发 0, 而 6083h = 0 意味着
 * 斜坡永远起不来 (606Ch 恒为 0, 6064h 不动)。缺省取自驱动器实读值; 传 0 不拒绝, 只打 WARN。 */
void     em_set_ramp(em_axis_t *ax, uint32_t acc, uint32_t dec);
uint32_t em_ramp_acc(const em_axis_t *ax);
uint32_t em_ramp_dec(const em_axis_t *ax);
int      em_ramp_offset(const em_axis_t *ax);  /* 6083h 的字节偏移, -1 = 不在映射里 */

/* 读 6061h 实际生效的模式 */
int em_get_mode(em_axis_t *ax);

/* 把该模式的目标值先钉在"原地不动"上: CSP -> 607Ah = 当前实际位置 (镜像不可信时走 SDO 兜底读);
 * PV -> 60FFh = 0; HM -> 无目标值 (回零靠 6040h bit4 启动)。
 * 必须在使能之前做: 607Ah/60FFh 在镜像里且每周期都在发, 若使能那一刻它还停在 0 (或残留值),
 * 驱动器会在使能生效的同一帧朝那个值冲过去。em_enable() 自己会先调一次; 已使能时拒绝。 */
int em_arm(em_axis_t *ax);

/* 使能状态机: 0x0000 -> 0x0006 -> 0x0007 -> 0x000F, 每步断言 6041h 跟上了; 内部先调 em_arm()。
 * 返回 0 / -1 (已打印哪一步、期望与实测) / 1 (停止请求) */
int em_enable(em_axis_t *ax);
int em_disable(em_axis_t *ax);
int em_fault_reset(em_axis_t *ax);   /* 6040h bit7 上升沿 */

int em_enable_all(em_bus_t *bus);
int em_disable_all(em_bus_t *bus);

/* 最新一帧过程数据里的读数。先用 em_mirror_ok() 判断可不可信: 短帧时镜像不更新,
 * 返回的是上一次完整帧的值 (或 0, 一帧都没完整过时)。 */
uint16_t em_sw (const em_axis_t *ax);   /* 6041h */
int32_t  em_pos(const em_axis_t *ax);   /* 6064h */
int32_t  em_vel(const em_axis_t *ax);   /* 606Ch */

/* mirror_ok = "**现在**还有完整帧": 收到一帧完整的就置 1, 连续短帧到 EM_SHORT_FRAMES_LIMIT
 * 就降回 0 (2026-09-22 之前它只置不清, 于是所有 !mirror_ok 判据都是死代码 ——
 * 链路断了位置冻住, 界面照样显示"正常")。 */
int      em_mirror_ok(const em_axis_t *ax);
uint32_t em_mirror_frames(const em_axis_t *ax);  /* 收到过多少个完整帧 (累计, 只增) */
uint32_t em_bad_frames(const em_axis_t *ax);     /* 当前这段连续短帧有几帧; 0 = 刚收到整帧 */

/* ---- 60FDh 三个开关 (原点 / 正限位 / 负限位) ----
 * 先问 em_dig_in_known(), 再问下面三个: 60FDh 不在生效 TxPDO 里时 (本机 1A00h 只有
 * 6041h/6064h/606Ch) 下面三个一律返回 0, 而 0 看起来就像"三个都没压住"(短帧时同理) ——
 * 不知道和"都没压住"必须分得开, 返回值本身分不开。 */
int      em_dig_in_known(const em_axis_t *ax);  /* 映射里有且收到过完整帧 */
int      em_di_home  (const em_axis_t *ax);     /* bit2 原点开关 */
int      em_di_poslim(const em_axis_t *ax);     /* bit1 正限位 */
int      em_di_neglim(const em_axis_t *ax);     /* bit0 负限位 */
uint32_t em_dig_in_raw(const em_axis_t *ax);    /* 原始 60FDh (调试用) */
int      em_dig_in_offset(const em_axis_t *ax); /* 字节偏移; -1 = 不在生效映射里 */

/* 让下一次 em_setup 把 60FDh 追加进 TxPDO (RAM only, 收尾时还原)。默认不开。
 * 代价: SM3 从 10 字节变 14 字节; 只写 RAM, 崩在收尾之前会把改动留在驱动器里直到断电重启;
 * 需要 allow_remap 授权。连接期参数, 必须在 em_setup 之前调。 */
void em_require_dig_in(em_bus_t *bus, int on);

/* ---- 603Fh 驱动器故障码 ----
 * 6041h bit3 只说"有故障", 说不了是哪一个 (0xFF01 过流 / 0xFF02 过压 / 0xFF03 欠压 /
 * 0xFF04 动力线 / 0xFF06 通讯报警 / 0xFF08 传感器); 603Fh 才是那个"哪一个"。
 * 它在生效 TxPDO 里时与 bit3 **同帧到达** —— 不必等 SDO, 而那条 SDO 会把过程数据
 * 整个停掉最多 EC_TIMEOUTRXM = 700ms (见上面 em_sdo_read 那段说明)。
 * 先问 em_err_code_known(), 再问 em_err_code()。
 *
 * ⚠️ 两个问题别问错函数: "**映射里有吗**" 问 em_err_code_offset (静态, 连接期就定了);
 * em_err_code_known 还要求 mirror_ok —— 链路一坏它就变 0, 拿它当"映射里有吗"会使
 * "发不发那条 SDO"的闸门恰好在链路最差时打开 (2026-09-22 实机踩过: 1638ms 静默)。 */
int em_err_code_known(const em_axis_t *ax);  /* 映射里有且收到过完整帧 */
int em_err_code(const em_axis_t *ax);        /* 无读数时返回 EM_ERR_CODE_UNREAD */
int em_err_code_offset(const em_axis_t *ax); /* 字节偏移; -1 = 不在生效映射里 */

/* 让下一次 em_setup 把 603Fh 追加进 TxPDO (RAM only, 收尾时还原)。**默认就是开** ——
 * 它是一个只读监视量, 而"上位机对驱动器报警只有一扇窗"正是这一轮要修的病;
 * 再加一个默认关的开关等于把同一件事再关上一次。代价与 em_require_dig_in 相同
 * (SM3 长 2 字节, 只写 RAM, 崩在收尾之前会把改动留到断电重启, 需要 allow_remap 授权)。
 * 连接期参数, 必须在 em_setup 之前调。 */
void em_require_err_code(em_bus_t *bus, int on);

/* 授权改驱动器参数 (目前只有 2300h 输入端子有效电平逻辑)。默认不授权。
 * 与 em_setup 的 allow_remap 是**分开**的两道门: 改 PDO 映射是通信配置 (掉电即回),
 * 改参数是驱动器行为 —— 代价不同, 一次授权不该管两件事。连接期参数。 */
void em_allow_param_write(em_bus_t *bus, int on);

/* 把每根轴的 2300h bit0~bit2 写成 want & EM_DI_LOGIC_MASK 的极性
 * (want 只收 EM_DI_LOGIC_NPN = 0x0007 这一个意图, 见上面的常量块)。返回 0 / -1。
 * 需要 em_allow_param_write 授权, 否则一个字节都不写。
 * 三步: ① 先逐轴读原值 (宽度按驱动器自报的来), **任一根读不到就整体拒绝** ——
 *      没读到原值就写, 收尾无从还原; ② 已经等于 want 的轴跳过 (写 = 改驱动器);
 *      ③ 写后逐轴回读 memcmp 确认 (本仓库这份 SOEM 在加急路径上把 SDO abort 当写成功)。
 * bit3 以上的位原样保留。收尾由 em_shutdown 按快照还原。 */
int em_di_set_logic(em_bus_t *bus, uint16_t want);

/* 该轴生效的 PDO 映射对象索引 (setup 时实读 1C12h/1C13h 得到), setup 之前为 0;
 * 真机上 1C12h 指的是 1601h, 不是默认的 1600h。 */
uint16_t em_rx_pdo(const em_axis_t *ax);
uint16_t em_tx_pdo(const em_axis_t *ax);

/* 该从站现在的 AL 状态 (state) 与 AL 状态码 (alstatuscode)。0 = 成功 / -1 = 失败。
 * 内部走 ecx_readstate (BRD 广播读, 不走邮箱), **只在异常时调** —— 健康路径上判
 * "够不够帧"靠每帧免费的 wkc, 不必也不能每周期发这一次额外往返。
 * 典型值: state 8 = OP; alstatuscode 0x001B = Sync manager watchdog (主站喂帧超时),
 * 0x001E = 非法 SM 配置。判据表见 hmi/ecatworker.h 的 ecatcmd::al_code_text。 */
int em_al_status(em_bus_t *bus, int slave, int *state, int *alcode);

int em_is_enabled(const em_axis_t *ax);

/* 镜像不可信时的兜底: 直接 SDO 读 (慢, 但一定是最新的驱动器状态) */
int em_pos_sdo(em_axis_t *ax, int32_t *pos);
int em_vel_sdo(em_axis_t *ax, int32_t *vel);
int em_sw_sdo (em_axis_t *ax, uint16_t *sw);

/* 运动 —— 全部阻塞, 内部跑周期帧 (调用者不用再调 em_service)。每个都会: 检查 Ctrl-C ->
 * 检查模式/使能状态 -> 检查位移上限 -> 检查 Fault/限位 -> 推进轨迹并每周期下发 -> 判到位或超时。
 * 返回 0 = 成功; -1 = 失败 (原因已打印); 1 = 收到停止请求 (已安全停下) */

/* 某个模式现在能不能用 —— 取决于它的字段在不在该轴实读的映射里。补映射可能只补上一部分,
 * 那时不该整体拒绝: 一个 60FFh 补不进去不该让 CSP 连坐。缺的字段 setup 时逐个打 [WARN]。 */
int em_csp_available(const em_axis_t *ax);   /* 需要 6040h + 607Ah + 6064h */
int em_pv_available(const em_axis_t *ax);    /* 需要 6040h + 60FFh + 6041h */

/* 绝对 / 相对位置运动 (CSP)。vel 单位 pul/s */
int em_csp_move_abs(em_axis_t *ax, int32_t target, uint32_t vel, uint32_t tmo_ms);
int em_csp_move_rel(em_axis_t *ax, int32_t delta,  uint32_t vel, uint32_t tmo_ms);

/* 把 607Ah 目标位置写进输出镜像。纯镜像写: 不发帧、不做 SDO —— 写完要有人去打那一帧
 * (em_csp_move_multi() 内部每周期做的正是这一件事)。有意不检查"已使能、6061h == CSP":
 * 它是每周期都要调的路径, 上面不能有 SDO 往返, 由调用方保证。
 * 返回 0 = 已写进镜像 / -1 = 607Ah 不在本轴实读的映射里 */
int em_csp_set_target(em_axis_t *ax, int32_t target);

/* 多轴相对运动 (CSP) —— 起点由接口内部取: 每根轴的 607Ah 目标 = 调用这一刻该轴的实际位置
 * + delta[i], 全部轴的起点在同一帧上取再一起下发 (逐轴调 em_csp_move_rel() 起点会落不同帧)。
 * 取不到某根轴的实际位置就整体拒绝, 一个字节都不下发 (不拿 0 当当前位置)。 */
int em_csp_move_rel_multi(em_axis_t **axes, const int32_t *delta,
                          const uint32_t *vel, int n, uint32_t tmo_ms);

/* 多轴同周期下发: 一个周期内给每根轴写各自的目标位置, 然后发一帧喂所有轴。各轴按自己的 vel
 * 独立推进、独立判到位 (谁先到谁先停), 所有轴都到位才算成功。 */
int em_csp_move_multi(em_axis_t **axes, const int32_t *target, const uint32_t *vel,
                      int n, uint32_t tmo_ms);

/* 速度模式: 写到 vel 跑 hold_ms, 再写 0 停, 并断言 6041h bit12 (Speed = 0) */
int em_pv_run_for(em_axis_t *ax, int32_t vel, uint32_t hold_ms);
int em_pv_stop(em_axis_t *ax);

/* 多轴 PV 同周期下发: 每根轴跑自己的 vel 跑满 hold_ms, 然后逐轴写 0 并断言 6041h bit12。
 * 任一轴不合格 (缺 60FFh / vel 为 0 / 模式不是 PV / 有 Fault 或限位) 就整体拒绝, 一根都不动;
 * 运行中任一根出故障或撞限位则所有轴一起停。 */
int em_pv_run_multi(em_axis_t **axes, const int32_t *vel, int n, uint32_t hold_ms);

/* 回零 (HM)。顺序: 未使能时写 6098h/6099h/609Ah/607Ch (写后回读) -> 6060h = 6 -> 使能
 * -> 6040h 抬 bit4 (0x000F -> 0x001F) 启动 -> 等 6041h bit12 (Homing attained);
 * bit13 (Homing error) 一置就失败。不断言"回零后 6064h == 0" —— 2214h 决定那时显示什么。
 * 返回 0 / -1 / 1(停止请求) */
int em_home(em_axis_t *ax, const em_home_cfg_t *cfg, uint32_t tmo_ms);

#ifdef __cplusplus
}
#endif

#endif /* EC_MOTOR_H */
