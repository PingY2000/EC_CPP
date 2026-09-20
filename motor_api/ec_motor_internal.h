/*
 * ec_motor_internal.h - ec_motor.c 与 ec_motor_motion.c 之间的私有接口; 调用方只用 ec_motor.h。
 * 不变量: ecx_SDOwrite 只允许出现在 ec_motor.c (em__wr_*() 在此声明, 定义全在 ec_motor.c)。
 */
#ifndef EC_MOTOR_INTERNAL_H
#define EC_MOTOR_INTERNAL_H

#include "ec_motor.h"

/* 内部返回值: 0 = 成功 / -1 = 失败(原因已打印) / 1 = 收到停止请求(已安全停下) */
#define EM_R_OK    0
#define EM_R_FAIL (-1)
#define EM_R_STOP  1

/* 结构体定义 (对外不透明) */

struct em_axis
{
   em_bus_t *bus;
   int       idx;          /* 0 .. naxis-1 */
   int       bus_pos;      /* 0-based 总线位置 */
   int       slave;        /* 1-based 从站序号 */
   int32_t   pos_tol;      /* CSP 到位容差 (pul) */
   uint32_t  move_limit;   /* 单次位移上限 (pul) */

   /* 该轴在组过程数据镜像里的基址 (由 SOEM 定, 逐轴不同) */
   uint8_t  *out;
   uint8_t  *in;
   uint32_t  Obytes;
   uint32_t  Ibytes;

   /* 生效的 PDO 映射对象索引, 由 setup 时实读 1C12h/1C13h 得到
    * (真机上 1C12h 分配的是 1601h, 不可写死 1600h/1A00h) */
   uint16_t rx_pdo;
   uint16_t tx_pdo;

   /* 字段字节偏移, 从该轴自己的映射表实读推出; -1 = 该字段不在映射里 */
   int off_cw;          /* 6040h 控制字 (RxPDO) —— 硬要求 */
   int off_target_pos;  /* 607Ah 目标位置 (RxPDO) —— CSP 要 */
   int off_target_vel;  /* 60FFh 目标速度 (RxPDO) —— PV 要 */
   int off_sw;          /* 6041h 状态字 (TxPDO) —— 硬要求 */

   /* 6060h 运行模式 (RxPDO)。-1 = 不在生效映射里 (在映射里就必须经过程数据驱动) */
   int off_modes;

   /* 6083h/6084h 轮廓加减速度 (RxPDO)。-1 = 不在生效映射里;
    * 在映射里就是主站拥有的, 不写即下发 0, 而 0 会让斜坡起不来 */
   int      off_prof_acc;
   int      off_prof_dec;
   uint32_t prof_acc;   /* 每周期经过程数据下发的值 (pul/s²) */
   uint32_t prof_dec;

   int off_act_pos;     /* 6064h 实际位置 (TxPDO) */
   int off_act_vel;     /* 606Ch 实际速度 (TxPDO) */

   /* 60FDh 数字输入 (TxPDO)。只绑不补: 在生效映射里才用, 否则 -1。
    * 本机实测生效的 1A00h 只有 6041h/6064h/606Ch 三项 10 字节, 没有 60FDh */
   int      off_dig_in;
   uint32_t dig_in;     /* 最新一帧完整过程数据里的 60FDh */

   /* 最新一帧完整过程数据里的采样 */
   uint16_t sw;
   int32_t  pos;
   int32_t  vel;
   int      mirror_ok;  /* 是否收到过至少一帧完整的过程数据 */
   uint32_t frames;     /* 收到过多少个完整帧 */

   uint32_t short_frames;  /* 连续短帧计数 (判"过程数据未落地") */

   int32_t  csp_target;    /* CSP 插值目标的当前值 */

   char     label[32];
};

/* 一个 PDO 映射对象的快照, 用于收尾还原 */
typedef struct
{
   int      have;     /* 是否取到过 */
   int      changed;  /* 是否真写过 */
   uint16_t index;
   int      n;
   uint32_t e[EM_MAP_MAX];
} em_snap_t;

/* 一个方向要还原的两份快照: 映射对象 (1600h/1A00h) 与分配对象 (1C12h/1C13h);
 * 只存其中一个, 还原后 SM 长度会跟分配对不上 */
typedef struct
{
   em_snap_t pdo;
   em_snap_t assign;
} em_dirsnap_t;

struct em_bus
{
   ecx_contextt ctx;                 /* SOEM 上下文 (大, 所以 bus 用 malloc 分配) */
   uint8_t      iomap[EM_IOMAP_MAX]; /* 组过程数据镜像 */

   int nslaves;
   int naxis;
   em_axis_t *axis[EM_MAX_AXES];

   int expected_wkc;
   int verbose;
   int opened;
   int mapped;      /* 是否真的改写过 PDO 映射 */
   /* 是否主动把 60FDh 追加进 TxPDO; 默认 0 = 映射里已经有它时才绑。
    * 连接期参数, 必须由 em_require_dig_in() 在 em_setup 之前设 */
   int want_dig_in;

   /* 是否授权改驱动器参数 (目前只有 2300h); 默认 0 = 一个字节都不写。
    * 与 want_dig_in 分开两道门: 改 PDO 映射是通信配置 (掉电即回), 改参数是驱动器行为 */
   int allow_param;

   /* 2300h 快照, 下标 = 轴序号。sz 是驱动器自报的宽度 —— 手册写 U16 而 slide_motion 的
    * 基线表记成 U8, 两处对不上, 所以读到几字节就按几字节写回, 不猜 */
   int      di_logic_have[EM_MAX_AXES];
   int      di_logic_changed[EM_MAX_AXES];
   int      di_logic_sz[EM_MAX_AXES];
   uint16_t di_logic_orig[EM_MAX_AXES];

   int in_op;
   int prev_manualstatechange;

   em_dirsnap_t snap_rx[EM_MAX_AXES];
   em_dirsnap_t snap_tx[EM_MAX_AXES];
};

/* 内部原语 —— 实现全在 ec_motor.c */

/* 时钟与睡眠 (不在公共头上暴露 windows.h) */
uint32_t em__now_ms(void);
void     em__sleep_ms(int ms);

/* 输出: em__err 总打 (加 [FAIL] 前缀); em__log 只在 verbose 时打 */
void em__err(const char *fmt, ...);
void em__warn(const char *fmt, ...);
void em__log(em_bus_t *bus, const char *fmt, ...);

/* 过程数据一帧 (与 em_service 同一件事, 供运动层内部使用) */
int em__cycle(em_bus_t *bus);

/* 小端读写过程数据镜像 (用 memcpy, 不依赖偏移的对齐)。
 * put/get 只改写本地镜像 (下一帧才发出去), 与经 SDO 写驱动器并回读的 em__wr_*() 不同 */
void     em__put_u8 (uint8_t *m, int off, uint8_t v);
void     em__put_u16(uint8_t *m, int off, uint16_t v);
void     em__put_u32(uint8_t *m, int off, uint32_t v);
void     em__put_i32(uint8_t *m, int off, int32_t v);
uint8_t  em__get_u8 (const uint8_t *m, int off);
uint16_t em__get_u16(const uint8_t *m, int off);
uint32_t em__get_u32(const uint8_t *m, int off);
int32_t  em__get_i32(const uint8_t *m, int off);

/* 写 6040h 到输出镜像 (只写镜像, 下一帧才发出去) */
void em__set_cw(em_axis_t *ax, uint16_t cw);

/* 把在生效 RxPDO 里、由主站拥有的常量项重写进本地输出镜像: 6083h / 6084h。
 * 纯镜像写 —— 不发帧、不做 SDO。必须在进 OP 之前至少调用一次,
 * 否则镜像里这两项是 0, 会往驱动器下发 6083h = 0 (斜坡起不来)。 */
void em__pin_ramp(em_axis_t *ax);

/* 写控制字 -> 每周期打过程数据 -> 等 6041h 满足 (sw & mask) == want。
 * 返回 0 = PASS / -1 = FAIL (已打印期望与实测) / 1 = 收到停止请求 */
int em__cw_step(em_axis_t *ax, const char *name, uint16_t cw,
                uint16_t mask, uint16_t want, uint32_t tmo_ms);

/* 不动控制字, 只等状态字满足断言。返回 0 = PASS / -1 = FAIL / 1 = 收到停止请求 */
int em__wait_sw(em_axis_t *ax, uint16_t mask, uint16_t want,
                uint32_t tmo_ms, const char *what);

/* 只读检查一根轴现在能不能动: 必须镜像可信、无 Fault、无硬件限位。
 * 返回 0 = 干净; -1 = 有理由拒绝 (已打印) */
int em__check_motion_ready(const em_axis_t *ax, const char *stage);

/* 写原语 (唯一出现 ecx_SDOwrite 的那一层, 定义在 ec_motor.c)。
 * 全部写后回读: 本仓库这份 SOEM 在加急路径 (psize<=4) 上把从站回的 SDO abort 帧
 * 当成写成功, wkc > 0 不代表写进去了 —— 回读才是唯一的确认。 */
int em__wr_u8 (em_axis_t *ax, uint16_t index, uint8_t sub, uint8_t  v, const char *why);
int em__wr_i8 (em_axis_t *ax, uint16_t index, uint8_t sub, int8_t   v, const char *why);
int em__wr_u16(em_axis_t *ax, uint16_t index, uint8_t sub, uint16_t v, const char *why);
int em__wr_u32(em_axis_t *ax, uint16_t index, uint8_t sub, uint32_t v, const char *why);
int em__wr_i32(em_axis_t *ax, uint16_t index, uint8_t sub, int32_t  v, const char *why);

#endif /* EC_MOTOR_INTERNAL_H */
