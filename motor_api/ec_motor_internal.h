/*
 * ec_motor_internal.h - ec_motor.c 与 ec_motor_motion.c 之间的私有接口
 *
 * 调用方只用 ec_motor.h; 本文件不对外。
 *
 * 为什么要有它: ec_motor.h 里 em_bus_t / em_axis_t 是**不透明**的 (那样调用方
 * 不必看到 SOEM 的 ecx_contextt)。但运动层 ec_motor_motion.c 必须能看到轴结构体
 * 的字段 (镜像偏移、最新采样、容差...)。所以把定义与内部原语放这里, 两个 .c
 * 共享, 而对外仍然只有一个干净的 ec_motor.h。
 *
 * ============================================================================
 * 不变量: ecx_SDOwrite 只允许出现在 ec_motor.c
 * ============================================================================
 * 本文件里的 em__wr_*() 就是那一层的全部写原语 —— 它们**声明**在这里供运动层调用,
 * **定义**全部在 ec_motor.c。所以:
 *
 *     grep -rn ecx_SDOwrite motor_api/    ->    只有 ec_motor.c 一处
 *
 * 与 slide_motion/ 里"只有 sm_guard.c 写"是同一条不变量。
 * 运动层负责"写什么、按什么顺序写、写完等哪个位", 不负责"怎么发出去"。
 */
#ifndef EC_MOTOR_INTERNAL_H
#define EC_MOTOR_INTERNAL_H

#include "ec_motor.h"

/*
 * 内部返回值 —— 对外的语义在 ec_motor.h 的"运动"一节:
 *   0 = 成功 / -1 = 失败(原因已打印) / 1 = 收到停止请求(已安全停下)
 * 两层共用一份定义, 免得两边各写一套再对不上。
 */
#define EM_R_OK    0
#define EM_R_FAIL (-1)
#define EM_R_STOP  1

/* ======================================================================
 * 结构体定义 (对外不透明)
 * ====================================================================== */

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

   /*
    * 该轴**生效**的 PDO 映射对象索引, 由 setup 时实读 1C12h / 1C13h 得到。
    *
    * 不写死 1600h / 1A00h: 真机上 1C12h 分配的是 **1601h**。把 1600h 写死就会去读写
    * 一张**没有生效**的表 —— 那张表里就算字段齐全, 过程数据也一个字节都不会经过它,
    * 于是"映射看着没问题"和"控制字根本发不出去"可以同时成立。
    */
   uint16_t rx_pdo;
   uint16_t tx_pdo;

   /*
    * 字段字节偏移, **从该轴自己的映射表实读推出**。-1 = 该字段不在映射里
    * (此时用得到它的模式会被拒绝, 而不是拿猜测的偏移去写)。
    */
   int off_cw;          /* 6040h 控制字 (RxPDO) —— 硬要求 */
   int off_target_pos;  /* 607Ah 目标位置 (RxPDO) —— CSP 要 */
   int off_target_vel;  /* 60FFh 目标速度 (RxPDO) —— PV 要 */
   int off_sw;          /* 6041h 状态字 (TxPDO) —— 硬要求 */
   int off_act_pos;     /* 6064h 实际位置 (TxPDO) */
   int off_act_vel;     /* 606Ch 实际速度 (TxPDO) */

   /* 最新一帧完整过程数据里的采样 */
   uint16_t sw;
   int32_t  pos;
   int32_t  vel;
   int      mirror_ok;  /* 是否至少收到过一帧完整的 */
   uint32_t frames;     /* 收到过多少个完整帧 */

   uint32_t short_frames;  /* 连续短帧计数 (判"过程数据未落地") */

   int32_t  csp_target;    /* CSP 插值目标的当前值 */

   char     label[32];
};

/* 一个 PDO 映射对象的快照, 用于收尾还原 */
typedef struct
{
   int      have;     /* 是否取到过 */
   int      changed;  /* 是否被本程序改过 */
   uint16_t index;
   int      n;
   uint32_t e[EM_MAP_MAX];
} em_snap_t;

/*
 * 一个方向 (RxPDO 或 TxPDO) 要还原的东西有**两个对象**:
 * 映射对象本身 (1600h / 1A00h) 和它的分配对象 (1C12h / 1C13h)。
 * 只存其中一个, 还原之后 SM 长度就跟分配对不上了 —— 所以两个一起存。
 */
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
   int in_op;
   int prev_manualstatechange;

   em_dirsnap_t snap_rx[EM_MAX_AXES];
   em_dirsnap_t snap_tx[EM_MAX_AXES];
};

/* ======================================================================
 * 内部原语 —— 实现全在 ec_motor.c
 * ====================================================================== */

/* 时钟与睡眠 (头文件顺序原因, 不在公共头上暴露 windows.h) */
uint32_t em__now_ms(void);
void     em__sleep_ms(int ms);

/* 输出: em__err 总打 (加 [FAIL] 前缀); em__log 只在 verbose 时打 */
void em__err(const char *fmt, ...);
void em__warn(const char *fmt, ...);
void em__log(em_bus_t *bus, const char *fmt, ...);

/* 过程数据一帧 (与 em_service 同一件事, 供运动层内部使用) */
int em__cycle(em_bus_t *bus);

/*
 * 小端读写**过程数据镜像**。用 memcpy 而不是强制转换 —— 不依赖偏移的对齐。
 *
 * 命名上刻意与下面的 em__wr_*() 分开 (put/get 对 wr): em__wr_* 是"经 SDO 写进
 * 驱动器, 并回读确认", em__put_* 是"改写本地那一块镜像缓冲, 下一帧才发出去"。
 * 两者混起来看代码会误判"写成功"的含义 —— 一个是驱动器收下了, 一个只是我们改了
 * 自己的内存。
 */
void     em__put_u16(uint8_t *m, int off, uint16_t v);
void     em__put_i32(uint8_t *m, int off, int32_t v);
uint16_t em__get_u16(const uint8_t *m, int off);
int32_t  em__get_i32(const uint8_t *m, int off);

/* 写 6040h 到输出镜像 (只写镜像, 下一帧才发出去) */
void em__set_cw(em_axis_t *ax, uint16_t cw);

/*
 * 写控制字 -> 每周期打过程数据 -> 等 6041h 满足 (sw & mask) == want。
 * 返回 0 = PASS / -1 = FAIL (已打印期望与实测) / 1 = 收到停止请求
 */
int em__cw_step(em_axis_t *ax, const char *name, uint16_t cw,
                uint16_t mask, uint16_t want, uint32_t tmo_ms);

/*
 * 不动控制字, 只等状态字满足断言。
 * 返回 0 = PASS / -1 = FAIL / 1 = 收到停止请求
 */
int em__wait_sw(em_axis_t *ax, uint16_t mask, uint16_t want,
                uint32_t tmo_ms, const char *what);

/*
 * 只读检查一根轴现在能不能动: 必须镜像可信、无 Fault、无硬件限位。
 * 返回 0 = 干净; -1 = 有理由拒绝 (已打印)
 */
int em__check_motion_ready(const em_axis_t *ax, const char *stage);

/* ======================================================================
 * 写原语 (唯一出现 ecx_SDOwrite 的那一层, 定义在 ec_motor.c)
 *
 * 全部**写后回读**: 本仓库这份 SOEM 的 ecx_SDOwrite 在加急路径 (psize<=4) 上把
 * 从站回的 SDO abort 帧也当成写成功 (abort 帧的 mbxtype/service/index/subindex
 * 与请求完全一致, 命中它的 "all OK" 分支, 既不压错误栈也不置 ecaterror, wkc 还 > 0)。
 * 这几个对象全是 1/2/4 字节的加急写, 全在这条路径上 —— 所以 wkc > 0 **不代表写进去了**,
 * 回读才是唯一的确认。
 * ====================================================================== */
int em__wr_u8 (em_axis_t *ax, uint16_t index, uint8_t sub, uint8_t  v, const char *why);
int em__wr_i8 (em_axis_t *ax, uint16_t index, uint8_t sub, int8_t   v, const char *why);
int em__wr_u16(em_axis_t *ax, uint16_t index, uint8_t sub, uint16_t v, const char *why);
int em__wr_u32(em_axis_t *ax, uint16_t index, uint8_t sub, uint32_t v, const char *why);
int em__wr_i32(em_axis_t *ax, uint16_t index, uint8_t sub, int32_t  v, const char *why);

#endif /* EC_MOTOR_INTERNAL_H */
