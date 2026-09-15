/*
 * ec_motor.h - 多轴 CiA402 电机调用接口 (位置同步 CSP / 速度 PV / 回零 HM)
 *
 * ============================================================================
 * 这是什么
 * ============================================================================
 * 在 test2.c 验证过的过程数据通路上, 加一层"能真正驱动电机"的接口:
 *   - **位置同步模式 (CSP, 6060h = 8)** —— 每个周期下发目标位置, 由本接口在内部
 *     按实测 dt 做线性插值, 而不是把目标一次跳到位。
 *   - **速度模式 (PV, 6060h = 3)** —— 每个周期下发目标速度。
 *   - **回零 (HM, 6060h = 6)** —— 6098h 方式 + 6040h bit4 上升沿启动。
 *   - **多轴**: 一根网卡、一个进程、**一个周期帧喂所有轴**。这是"同时调用两个
 *     驱动器"成立的地方 —— 不是开两个线程各发各的帧。
 *
 * ============================================================================
 * 为什么必须走过程数据 (PDO) 而不是 SDO
 * ============================================================================
 * slide_motion 走 SDO + PP 模式, 那是因为 PP 的速度规划器在驱动器内部, 一个目标
 * 位置下一遍就行。CSP/PV 不是: 目标值**必须每周期刷新**。而 SOEM 的 SDO 超时
 * (EC_TIMEOUTRXM) 是 700ms, 一个同步周期是 2ms —— SDO 做不到, 差三个数量级。
 * 所以 6040h/607Ah/60FFh 必须进 RxPDO, 6041h/6064h/606Ch 必须进 TxPDO。
 *
 * ============================================================================
 * 这台驱动器**没有 CSV (6060h = 9)**
 * ============================================================================
 * 手册 6502h = 0x00A5 = bit0(PP) + bit2(PV) + bit5(HM) + bit7(CSP), 正好是 6060h
 * 接受的 1/3/6/8 四种模式, 自洽。ESI 声明的 0x01DD 虽然含 CSV 位, 但那份声明连
 * HM 都不包含, 与"能跑回零"矛盾 —— 以 6060h 是否接受该值为准。
 * 所以本接口的"速度模式"**只能是 PV**。别再去找 CSV 了。
 *
 * ============================================================================
 * 偏移只许现场读, 不许假设 —— 本文件里唯一不许妥协的一条
 * ============================================================================
 * 同一台设备的出厂映射, 手册(V2.4 附录 3)、ESI 声明、ESI 字典默认值给了**三个**
 * 互不相同的答案, 而真机上量到的又是**第四个** (输出 23 字节)。**23 是奇数, 与上面
 * 任何一份组合都对不上, 至今无法解释。** 所以:
 *   - 一律实读 1C12h / 1C13h / 1600h / 1A00h, 从实读的表推偏移;
 *   - **推不出字节偏移就拒绝** (返回 -1), 绝不拿"大概是 0 吧"去写过程数据 ——
 *     偏移算错等于把控制字字节写进一个别的字段, 万一那是 607Ah(目标位置),
 *     就在毫不知情的情况下下了一个位置指令。
 *   - 缺失的映射项**只追加, 不替换**: 已有的项原样保留, 且写之前先把追加后的整张
 *     表证明一遍(每个需要的字段都字节对齐), 证不出就不写。
 *
 * ============================================================================
 * 唯一的写入口
 * ============================================================================
 * **本目录里只有 ec_motor.c 出现 ecx_SDOwrite** (与 slide_motion/ 里"只有
 * sm_guard.c 写"同一条不变量)。而且头文件**不导出任何裸写函数** —— 所有写都是
 * 语义化的: em_set_mode / em_enable / em_home / ... 。想审计"这接口会写什么",
 * 只需要读 ec_motor.c 里那几个函数。
 *
 * 会写的对象只有: 6060h(模式)、6098h/6099h/609Ah/607Ch(回零参数)、
 * 6040h 只经过程数据镜像写、1C12h/1C13h/1600h/1A00h(PDO 映射, 仅 RAM)。
 * **从不写 2102h (EEPROM)**, 不改软限位 607Dh, 不改 2400h/2408h/2409h 任何当量参数。
 *
 * ============================================================================
 * 将来抽 ecat_core 时的迁移路径
 * ============================================================================
 * 本模块的总线初始化 / SDO 读写 / 映射读改那一层, 与 sm_bus.c / sm_guard.c 是并列
 * 的重复实现 —— 这是本次**明确接受**的代价 (见 docs/qt_hmi_layout.md §14)。
 * 做那个规划的 P0 时: 把 ec_motor.c 里的 ecx_init / ecx_config_init /
 * ecx_config_map_group / SDO 读写 / 映射读改换成 ecat_core 的实现,
 * **ec_motor_motion.c 那一层 (使能状态机 / CSP 轨迹 / PV / 回零 / 多轴调度) 可原样保留。**
 *
 * ============================================================================
 * 安全边界 (调用者必须知道)
 * ============================================================================
 * 使能 (em_enable) 之后**电机通电、有保持力矩**; CSP/PV/回零都会**真的移动滑台**;
 * 回零会**撞向限位开关或原点开关**。真实运行时: 人在设备旁, 手放在物理急停上。
 * 收尾必须无条件走 em_shutdown(); 它会把 *motor_maybe_live 置 1 来表示
 * "写了失能但回读 6041h 仍报 Operation enabled, 电机可能还带电" —— 见到就断电确认。
 */
#ifndef EC_MOTOR_H
#define EC_MOTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * 退出码 —— 沿用 sm_pdo.c 的口径 (0/1/2/3/4/5/6/10), 另加 7 = 回零失败
 * ====================================================================== */
#define EM_EXIT_OK           0  /* 全部阶段 PASS */
#define EM_EXIT_USAGE        1  /* 用法错误 / 初始化失败 / 网卡打不开 */
#define EM_EXIT_NO_SLAVE     2  /* 空总线 / 无目标轴 / 目标不是 YKD */
#define EM_EXIT_FAIL         3  /* 存在 FAIL */
#define EM_EXIT_REFUSED      4  /* 安全护栏拒绝: 未授权 / 身份门不过 / 偏移证不出 /
                                 * 映射缺项且不许改写。未写任何东西 */
#define EM_EXIT_NO_OP        5  /* 进不去 SAFE_OP 或 OP */
#define EM_EXIT_NO_PDO       6  /* 过程数据未落地 (WKC 持续偏短) */
#define EM_EXIT_HOMING       7  /* 回零未达位 / 回零出错 */
#define EM_EXIT_NO_DISABLE  10  /* 收尾写了 6040h=0 但 6041h 仍报 Operation enabled:
                                 * **电机可能仍带电** —— 覆盖其它所有码 */

/* ======================================================================
 * 运行模式 (6060h)
 * ====================================================================== */
#define EM_MODE_PP   1  /* 轮廓位置 (本接口不实现, 那是 slide_motion 的活) */
#define EM_MODE_PV   3  /* 速度模式 —— 本机可用的唯一速度模式 (没有 CSV) */
#define EM_MODE_HM   6  /* 回零 */
#define EM_MODE_CSP  8  /* 位置同步模式 */

/* ======================================================================
 * 容量与默认值
 * ====================================================================== */
#define EM_MAX_AXES     8      /* 本接口最多同时驱动的轴数 */
#define EM_MAP_MAX      8      /* 一个 PDO 映射对象最多几项 (本驱动器 1~5 项, 留余量) */
#define EM_IOMAP_MAX 4096      /* 过程数据镜像缓冲 (2 轴放大的映射也远远够) */

#define EM_POS_TOL_DEF      50    /* CSP 到位容差默认值 (pul) */
#define EM_MAX_DELTA_DEF  50000   /* 单次运动位移上限默认值 (pul) —— 见 ec_motor.h 的说明 */
/*
 * 轮廓加减速度的兜底值 (pul/s²)。本机 6083h/6084h 的实读值就是 500000, 也是
 * slide_motion 用成功过的那个值 —— 但**只在读不到驱动器自己的值时才用它**,
 * 正常路径是采信驱动器自己的配置。为什么必须有一个兜底: 见 em_setup 里那段
 * "6083h 实读 0" 的说明 —— 0 是主站自己下发进去的, 拿它当"驱动器的值"会自己骗自己。
 */
#define EM_RAMP_ACC_DEF 500000u
#define EM_RAMP_DEC_DEF 500000u
#define EM_POLL_MS           2    /* 周期轮询节拍让步 */
#define EM_STEP_TMO_MS    1000    /* 使能状态机单步超时 */
#define EM_SDO_TMO_MOTION  200    /* 运动期的 SDO 超时 (非运动期用 EC_TIMEOUTRXM) */

/* YKD2205PE 身份集合 (与 slide_verify / sm_bus.c 一致) */
#define EM_YKD_VENDOR_ID 0x0994UL
#define EM_YKD_PRODUCT_1 0x2000UL
#define EM_YKD_PRODUCT_2 0x3000UL

/* ======================================================================
 * CiA402 对象索引 —— 只列本接口真正会碰的
 * ====================================================================== */
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
#define EM_OID_PROF_VEL      0x6081  /* 轮廓速度 (PP 用; 本接口不做 PP, 见 em_arm 的 PP 分支) */
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

/* PDO 配置对象 (CiA301 通信区) */
#define EM_OID_RXPDO_ASSIGN  0x1C12
#define EM_OID_TXPDO_ASSIGN  0x1C13
/*
 * 这两个只是**分配为空时的兜底值**, 不是"生效的 PDO"。
 * 哪个 PDO 生效一律实读 1C12h / 1C13h —— 真机上 1C12h 指的是 **1601h**:
 * 照着这里写死 1600h 去补映射, 改的是一张没生效的表, 没有任何报错。
 * 生效索引在 em_setup 之后用 em_rx_pdo() / em_tx_pdo() 读。
 */
#define EM_OID_RXPDO0        0x1600
#define EM_OID_TXPDO0        0x1A00

/* ======================================================================
 * 控制字 6040h —— 推送过的值只有下面这几个, 不试探厂商私有控制字
 * ====================================================================== */
#define EM_CW_DISABLE_V  0x0000  /* Disable voltage */
#define EM_CW_SHUTDOWN   0x0006  /* Shutdown */
#define EM_CW_SWITCHON   0x0007  /* Switch on (也是 Disable operation) */
#define EM_CW_ENABLE_OP  0x000F  /* Enable operation —— 电机从这里开始带电 */
#define EM_CW_HOMING_GO  0x001F  /* 0x000F | bit4: HM 模式启动回零 (要 0->1 上升沿) */
#define EM_CW_FAULT_RST  0x0080  /* Fault reset (bit7 上升沿) */

/* ======================================================================
 * 状态字 6041h (V2.4 p11 —— 手册**定义了** bit5/bit6, 别信"未定义 bit6"那句旧话)
 * ====================================================================== */
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

/* ======================================================================
 * 状态字判读 —— 集中在 ec_motor.c, 不许在调用方散落写位运算
 * ====================================================================== */

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

/*
 * 「未使能且无故障」的**正确判据** = (sw & 0x000C) == 0 (bit2 未使能, bit3 无故障)。
 *
 * **不要写成 (sw & 0x000F) == 0。** 上电自检完成后驱动器合法地停在低 4 位 = 0001
 * (Ready to switch on, **电机释放**), 那时低四位不是 0 —— 把"必须读到 0000"写成硬断言,
 * 在一台已经上电自检完成的驱动器上必然失败。那是期望值不对, 不是通信故障。
 * 论证与实测见 docs/ykd2205pe_ci402.md 「状态字 6041h」。
 */
int em_sw_disabled_no_fault(uint16_t sw);

/* bit9 = 0 时无法对控制字进行操作 (实测 0x0210/0x0231 的 bit9 都是 1, 即 Remote 有效) */
int em_sw_remote_ok(uint16_t sw);

/* ======================================================================
 * 类型
 * ====================================================================== */

typedef struct em_bus  em_bus_t;   /* 不透明: 内部持有 ecx_contextt 与过程数据镜像 */
typedef struct em_axis em_axis_t;  /* 不透明: 一根轴 */

/* 选轴配置 */
typedef struct
{
   int     bus_pos;   /* 0-based 总线位置 (与 aliasinfo / pysoem --pos 一致) */
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

/* ======================================================================
 * 停止请求 —— Ctrl-C 的处理器只能调 em_request_stop() (它只置一个标志位,
 * 不做任何 I/O: 在信号上下文里发 SDO 是未定义行为)
 * ====================================================================== */
void em_request_stop(void);
int  em_stop_requested(void);
void em_clear_stop(void);

/* ======================================================================
 * 生命周期
 * ====================================================================== */

em_bus_t *em_bus_new(void);
void      em_bus_free(em_bus_t *bus);

/* 打开网卡 + ecx_config_init。返回从站数; <0 = 失败 (原因已打印) */
int em_open(em_bus_t *bus, const char *ifname);

/* 打开失败时列出可用网卡 (照 test2.c 的 print_adapters) */
void em_print_adapters(void);

/*
 * 网卡清单。**给界面用的那一份** —— em_print_adapters() 只往 stdout 打印, 而 GUI 要把
 * 它们填进下拉框, 所以同一份遍历必须能返回数组。
 *
 * name 就是 em_open() 要的那个字符串 (Windows Npcap 形如 `\Device\NPF_{GUID}`)。
 * 128 字节与 SOEM 的 ec_adaptert 同宽, 不截断。
 *
 * 返回填进 out 的条数 (0 = 一块网卡都没有, 通常是 Npcap 没装或被独占);
 * max <= 0 时只数不填。
 */
typedef struct
{
   char name[128];
   char desc[128];
} em_adapter_t;

int em_list_adapters(em_adapter_t *out, int max);

/*
 * 把控制台输出代码页设成 UTF-8。**调用方应当在 main() 的第一句调它。**
 *
 * 本模块内部会在打日志时惰性调用一次, 但那只救得了**那之后**的输出。源码是 UTF-8,
 * Windows 控制台默认是 GBK, 所以"在第一次打日志之前"打印的汉字会全是乱码 ——
 * 实测把「多轴」打成「澶氳酱」。横幅恰好就在那个区间里, 所以别等它自己初始化。
 * 幂等, 多调无妨。
 */
void em_console_init(void);

/*
 * 毫秒睡眠。对外提供是因为**调用方写自己的过程数据循环时要用它**: 过程数据要持续
 * 打, 而"持续打"就得有个节拍 —— 光用空转会把一个核吃满, 而且那样打出去的帧间隔
 * 由 CPU 速度决定, 不是我们可以复现的。
 */
void em_sleep_ms(int ms);

int em_slave_count(const em_bus_t *bus);
int em_slave_info(const em_bus_t *bus, int slave, em_slave_info_t *out);

/*
 * 选轴 + 补 PDO 映射 + 建过程数据 + 逐轴证明偏移 + 上 SAFE_OP。
 *
 * allow_remap = 0: 需要的对象**已经全在**映射里就照常继续; 缺任何一项就拒绝 (返回 -1),
 *                  一个字节都不写 —— 这是"只读跑一遍看现状"的用法。
 * allow_remap = 1: 允许往 1C12h/1C13h/1600h/1A00h **追加**缺失项 (仅 RAM)。
 *                  仍然会在写之前证明偏移; 证不出就拒绝, 不写。
 *
 * 返回 0 成功 / -1 拒绝或失败 (原因已打印)。
 */
int em_setup(em_bus_t *bus, const em_axis_cfg_t *cfg, int naxis, int allow_remap);

int        em_axis_count(const em_bus_t *bus);
em_axis_t *em_axis(em_bus_t *bus, int i);              /* i = 0 .. em_axis_count()-1 */
em_axis_t *em_axis_by_pos(em_bus_t *bus, int bus_pos); /* 按 0-based 总线位置找 */

int em_expected_wkc(const em_bus_t *bus);

/* 配置 DC 并请求所有轴进 OPERATIONAL。返回 0 / -1 */
int em_enter_op(em_bus_t *bus, int use_dc, uint32_t cycle_us);

/*
 * 收发一帧并更新各轴镜像。返回 wkc; 返回 -1 = 致命错误 (调用者应停止)。
 *
 * 镜像**只在整帧完整时更新** (wkc >= 期望 WKC): 短帧时输入镜像是陈值或半个帧,
 * 拿它去断言等于拿"我们不知道"冒充"驱动器报了个状态"。
 */
int em_service(em_bus_t *bus);

/* 轴的 AL 状态字实读 (含错误位 0x10) */
uint16_t em_al_state(em_bus_t *bus, int slave);

/* 版本字符串 —— 打印在横幅里, 便于事后对日志 */
const char *em_version(void);

/* 详细程度: 0 = 只报错与拒绝 (库的默认), 1 = 报告每个周期/每步的进展 */
void em_set_verbose(em_bus_t *bus, int on);

/*
 * 收尾 —— **无条件执行**, 正常跑完 / 某步 FAIL / 前置条件失败 / Ctrl-C 都走这里。
 *
 * restore_mapping = 1: 把跑之前快照下来的 PDO 映射写回去 (默认该这么做;
 *                      docs/qt_hmi_layout.md §14.2 "不持久化 PDO 映射")。
 * motor_maybe_live  (可空): 置 1 = 未能确认全部轴失能, **电机可能仍带电**。
 */
void em_shutdown(em_bus_t *bus, int restore_mapping, int *motor_maybe_live);

/* ======================================================================
 * 只读诊断 —— 没有裸写函数, 这里全是读
 * ====================================================================== */

/* 读一个对象, *size 传入传出 (必须先用缓冲大小初始化)。返回 0 / -1 */
int em_sdo_read(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                void *p, int *size);

int em_rd_u8 (em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint8_t  *v);
int em_rd_i8 (em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int8_t   *v);
int em_rd_u16(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint16_t *v);
int em_rd_u32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint32_t *v);

/*
 * 只读诊断: 按驱动器**自报的宽度**读一个不超过 4 字节的对象, 并把宽度一起带出来。
 *
 * 与 em_rd_u16 / em_rd_u32 的区别是刻意的。那几个的语义是"我知道它应该几字节, 不符
 * 就算失败" —— 对要拿去判断的字段, 这是对的: 宽度不足时高位补零会解出一个**看起来
 * 完全正常**的值, 那是"我们不知道"冒充"读到了"。
 *
 * 本函数是给"我只想看看里面是什么"用的。实机上 2201h / 2400h / 2408h / 2409h 自报
 * **2 字节**, 而 6502h / 6064h / 607Dh 是 4 字节 —— 假设 4 字节会让这一排诊断值一个
 * 都打不出来, 那比打出来更没用。值按小端拼进 *val (不足 4 字节的高位补 0), 调用方
 * 配合 *size_bytes 自己判断该按几字节解释。
 *
 * 返回 0 = 成功 / -1 = 失败
 */
int em_rd_any(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
              uint32_t *val, int *size_bytes);
int em_rd_i32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int32_t  *v);

/*
 * 打印 1C12h / 1C13h / 1600h / 1A00h 的实读值、SM2/SM3 长度, 以及本接口需要的
 * 每个字段**从实读的表推出来的**字节偏移。
 *
 * 这是本模块最有价值的一次输出: 它把"这台设备现在到底映射了什么"变成可读的事实,
 * 从而回答"要不要写映射"这个问题 —— 而不是照文档假设。
 */
void em_dump_pdo(em_bus_t *bus, int slave);

/* ======================================================================
 * 轴
 * ====================================================================== */

int         em_axis_slave(const em_axis_t *ax);   /* 1-based 从站序号 */
int         em_axis_buspos(const em_axis_t *ax);  /* 0-based 总线位置 */
int         em_axis_index(const em_axis_t *ax);   /* 0 .. n-1 */
const char *em_axis_label(const em_axis_t *ax);   /* 形如 "轴0(从站1)" */

/*
 * 单次运动位移上限 (pul)。em_csp_move_* 与 em_pv_run_for 都会拒绝超过它的请求。
 * 默认 EM_MAX_DELTA_DEF。**放宽它必须是有意的动作**, 所以没有"传 0 表示不限"这种写法。
 */
int  em_axis_set_move_limit(em_axis_t *ax, uint32_t max_delta_pul);
uint32_t em_axis_move_limit(const em_axis_t *ax);

/*
 * 设定运行模式 (写 6060h, 再读 6061h 确认驱动器**认了**)。
 * **已使能时拒绝**: CiA402 规定模式只能在未使能时改。
 *
 * 传输方式由 setup 时实读决定 —— **6060h 在生效 RxPDO 里就走过程数据** (写输出镜像
 * + 打帧等 6061h 跟上), 不在里面才走 SDO。理由是通用的: 对象只要在生效的 RxPDO 里,
 * 它就是主站拥有的, 对它做 SDO 写会被下一帧撤销。真机上 6060h 正是这种情况 ——
 * 曾经 SDO 写进去 6060h=8, 6061h 却一直读回 0, 因为主站每周期都在下发 6060h=0。
 * 走哪条路可用 em_modes_via_pdo() 查。
 */
int em_set_mode(em_axis_t *ax, int mode);

/* 运行模式是不是经过程数据驱动的 (即 6060h 在生效的 RxPDO 里) */
int em_modes_via_pdo(const em_axis_t *ax);
int em_modes_offset(const em_axis_t *ax);   /* 6060h 的字节偏移, -1 = 不在映射里 */

/*
 * 轮廓加减速度 6083h / 6084h (pul/s²) —— PV 的斜坡由它们决定。
 *
 * **为什么接口必须管这两个对象**: 它们和 6060h 一样在生效的 RxPDO 里 (本机 1601h 的
 * 第 6/7 项, 输出镜像 +15 / +19), 所以主站每周期都在下发它们。不写就是**下发 0**,
 * 而 6083h = 0 意味着斜坡永远起不来 —— 现象极隐蔽: 驱动器收下了速度指令
 * (6041h bit12 "Speed=0" 因此清零), 606Ch 却恒为 0, 6064h 一个计数不动。
 * 断言"驱动器接受了指令"的那套检查全都会通过。
 *
 * 缺省取自驱动器自己的实读值 (读不到或读到 0 才落到 EM_RAMP_*_DEF, 并在 em_setup
 * 里说明); 传 0 不拒绝, 但会打一条 WARN —— 有些驱动器把 0 解释成"瞬时", 本机不是。
 */
void     em_set_ramp(em_axis_t *ax, uint32_t acc, uint32_t dec);
uint32_t em_ramp_acc(const em_axis_t *ax);
uint32_t em_ramp_dec(const em_axis_t *ax);
int      em_ramp_offset(const em_axis_t *ax);  /* 6083h 的字节偏移, -1 = 不在映射里 */

/* 读 6061h 实际生效的模式 */
int em_get_mode(em_axis_t *ax);

/*
 * 把该模式的**目标值先钉在"原地不动"**上:
 *   CSP -> 607Ah = 当前实际位置 (镜像不可信时走 SDO 兜底读)
 *   PV  -> 60FFh = 0
 *   HM  -> 无目标值 (回零靠 6040h bit4 启动), 不做任何事
 * 模式为 PP 或其它本接口不实现的模式时什么也不写, 只说明一句。
 *
 * **为什么必须在使能之前做**: CSP 下驱动器每个周期都跟着 607Ah 走, PV 下跟着 60FFh
 * 走。607Ah/60FFh 就在过程数据镜像里, 而且**每周期都在发** —— 若使能那一刻它还停在
 * 出厂值 0 (或上一轮的残留值), 驱动器会在使能生效的同一帧朝那个值冲过去。回零之前
 * 也是同理, 所以 em_enable() **自己会先调一次本函数**, 不让这条路依赖调用者记得。
 * 本函数单独对外是为了: 运动结束后想重新钉一个新的保持点, 或者调用者想显式确认
 * 钉在哪。
 *
 * 已使能时拒绝 (那正是它要防的时刻已经过去了)。返回 0 / -1
 */
int em_arm(em_axis_t *ax);

/* 使能状态机: 0x0000 -> 0x0006 -> 0x0007 -> 0x000F, 每步都断言 6041h 跟上了。
 * 内部先调 em_arm() 把目标值钉在原位。返回 0 = 成功 / -1 = 失败 (已打印是哪一步、
 * 期望什么、实测什么) / 1 = 收到停止请求 */
int em_enable(em_axis_t *ax);
int em_disable(em_axis_t *ax);
int em_fault_reset(em_axis_t *ax);   /* 6040h bit7 上升沿, 需要 --allow 类授权由调用方把关 */

int em_enable_all(em_bus_t *bus);
int em_disable_all(em_bus_t *bus);

/*
 * 最新一帧过程数据里的读数。**先用 em_mirror_ok() 判断可不可信**:
 * 短帧时镜像不更新, 这几个函数返回的是上一次完整帧的值 (或 0, 如果一帧都没完整过)。
 */
uint16_t em_sw (const em_axis_t *ax);   /* 6041h */
int32_t  em_pos(const em_axis_t *ax);   /* 6064h */
int32_t  em_vel(const em_axis_t *ax);   /* 606Ch */
int      em_mirror_ok(const em_axis_t *ax);
uint32_t em_mirror_frames(const em_axis_t *ax);  /* 收到过多少个完整帧 */

/*
 * 该轴**生效**的 PDO 映射对象索引 (setup 时实读 1C12h / 1C13h 得到), setup 之前为 0。
 * 偏移是按这两张表推出来的, 所以出问题时第一件事是把它们打出来 —— 真机上 1C12h
 * 指的是 1601h, 而不是通常默认的 1600h。
 */
uint16_t em_rx_pdo(const em_axis_t *ax);
uint16_t em_tx_pdo(const em_axis_t *ax);

int em_is_enabled(const em_axis_t *ax);

/* 镜像不可信时的兜底: 直接 SDO 读 (慢, 但一定是最新的驱动器状态) */
int em_pos_sdo(em_axis_t *ax, int32_t *pos);
int em_vel_sdo(em_axis_t *ax, int32_t *vel);
int em_sw_sdo (em_axis_t *ax, uint16_t *sw);

/* ======================================================================
 * 运动 —— 全部阻塞, 内部跑周期帧 (调用者自己不用再调 em_service)
 *
 * 每个都会: 检查 Ctrl-C -> 检查模式/使能状态 -> 检查位移上限 -> 检查 Fault/限位
 *           -> 推进轨迹并每周期下发 -> 判到位或超时。
 * 返回 0 = 成功; -1 = 失败 (原因已打印); 1 = 收到停止请求 (已安全停下)
 * ====================================================================== */

/*
 * 某个模式现在能不能用 —— 取决于它的字段在不在该轴实读的映射里。
 *
 * 为什么需要这个: 补映射可能只补上一部分(驱动器可能拒绝写 1600h, 或者某个字段
 * 与前一项位不对齐)。那时**不该整体拒绝**, 而该让能跑的模式照跑、跑不了的模式
 * 给出明确理由 —— 否则一个 60FFh 补不进去, CSP 也被连坐。setup 时会把缺的字段
 * 逐个打成 [WARN], 这两个函数让调用方能在安排测试阶段前问一句。
 */
int em_csp_available(const em_axis_t *ax);   /* 需要 6040h + 607Ah + 6064h */
int em_pv_available(const em_axis_t *ax);    /* 需要 6040h + 60FFh + 6041h */

/* 绝对 / 相对位置运动 (CSP)。vel 单位 pul/s */
int em_csp_move_abs(em_axis_t *ax, int32_t target, uint32_t vel, uint32_t tmo_ms);
int em_csp_move_rel(em_axis_t *ax, int32_t delta,  uint32_t vel, uint32_t tmo_ms);

/*
 * 把 607Ah 目标位置写进输出镜像。**纯镜像写: 不发帧、不做 SDO、不发任何东西。**
 *
 * 这是 CSP 的本来面目: 目标位置**每周期下发一次**, 驱动器跟着它走。em_csp_move_multi()
 * 内部每个周期做的也正是这一件事, 本函数只是把那一步单独露出来 —— 谁要自己做轨迹
 * (比如界面上的"点哪去哪、随时改向"), 就用它配 em_service()。
 *
 * 注意"纯镜像写"意味着**它自己不产生任何周期**: 写完要有人去打那一帧。调用方还得
 * 自己保证"这一轴已使能、且 6061h == CSP" —— 本函数**有意不做这些检查**, 因为它是
 * 每周期都要调的路径, 那上面不能有 SDO 往返 (一次 700ms, 比周期大三个数量级)。
 *
 * 返回 0 = 已写进镜像 / -1 = 607Ah 不在本轴实读的映射里 (写不进去)
 */
int em_csp_set_target(em_axis_t *ax, int32_t target);

/*
 * 多轴相对运动 (CSP) —— **起点由接口内部取**。
 *
 * 每根轴的 607Ah 目标 = "调用这一刻该轴的实际位置" + delta[i]。关键在"这一刻":
 * 全部轴的起点在**同一个决定时刻、同一帧**上取, 然后一次下发。逐轴各调一次
 * em_csp_move_rel() 的话, 各轴的起点落在不同时刻、不同帧上 —— 对"两轴同时"这种
 * 用法, 那个差别正是要消除的东西。
 *
 * 取不到某根轴的实际位置就**整体拒绝**, 一个字节都不下发 (绝不拿 0 当当前位置)。
 */
int em_csp_move_rel_multi(em_axis_t **axes, const int32_t *delta,
                          const uint32_t *vel, int n, uint32_t tmo_ms);

/*
 * **多轴同周期下发。** 本接口的核心, 也是"同时调用两个驱动器"成立的地方:
 * 一个周期内给每根轴写各自的目标位置, 然后**发一帧喂所有轴**。
 * 各轴按自己的 vel 独立推进、独立判到位 —— 谁先到谁先停 (不是插补同起同停)。
 * 所有轴都到位才算成功。
 */
int em_csp_move_multi(em_axis_t **axes, const int32_t *target, const uint32_t *vel,
                      int n, uint32_t tmo_ms);

/* 速度模式: 写到 vel 跑 hold_ms, 再写 0 停, 并断言 6041h bit12 (Speed = 0) */
int em_pv_run_for(em_axis_t *ax, int32_t vel, uint32_t hold_ms);
int em_pv_stop(em_axis_t *ax);

/*
 * **多轴 PV 同周期下发。** 每根轴跑自己的 vel 跑满 hold_ms, 然后逐轴写 0 并断言
 * 6041h bit12。
 *
 * 任一轴不合格 (缺 60FFh / vel 为 0 / 模式不是 PV / 有 Fault 或限位) 就**整体拒绝**,
 * 一根都不动 —— 多轴下"跑到一半才发现第 2 根不行"意味着一根在动一根没动, 更难收场。
 * 运行中任一根出故障或撞限位, **所有轴一起停**, 不留别的轴还在转。
 */
int em_pv_run_multi(em_axis_t **axes, const int32_t *vel, int n, uint32_t hold_ms);

/*
 * 回零 (HM)。顺序: 未使能时写 6098h/6099h/609Ah/607Ch (写后回读) -> 6060h = 6
 * -> 使能 -> 6040h 抬 bit4 (0x000F -> 0x001F) 启动 -> 等 6041h bit12
 * (Homing attained); bit13 (Homing error) 一置就失败。
 *
 * **不断言"回零后 6064h == 0"**: 2214h 决定回零完成后 6064h 显示什么, 本函数只把它
 * 读出来打印, 不替驱动器假设。返回 0 / -1 / 1(停止请求)
 */
int em_home(em_axis_t *ax, const em_home_cfg_t *cfg, uint32_t tmo_ms);

#ifdef __cplusplus
}
#endif

#endif /* EC_MOTOR_H */
