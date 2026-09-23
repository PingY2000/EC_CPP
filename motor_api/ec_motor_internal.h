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

/* 一次回零会话的相位 (em_home_prepare/start/step/finish 之间靠它衔接)。
 * 0 = IDLE 与 calloc 一致 —— 于是"没 prepare 过就调 em_home_step"天然落在 IDLE 上,
 * 那里是安静的 no-op, 不需要另加一个 have-inited 标志。
 * DONE 覆盖全部四条出路 (到位/故障/超时/被拉停): 它们对**步进**而言是同一件事 ——
 * 有结局了, 别再写镜像了。是哪一条出路看 em_axis::hm_rc。 */
#define EM_HM_S_IDLE    0
#define EM_HM_S_RUNNING 1   /* bit4 举着, 在等 bit12 */
#define EM_HM_S_DONE    2   /* 已有结局, 等收尾 */

/* em_home_prepare() 每笔 SDO 之间泵的帧数。
 * **这不是性能调优, 是安全参数**: 写超时写死 700 ms (em__verified_write 的 EC_TIMEOUTRXM),
 * 而一份短的写事务本身会静默几十毫秒 —— 没有这几帧, 五笔 SDO 连着写就是最长 3.5 s 的静默,
 * 而本机实测 1.6 s 停顿就足以让从站的 SM 看门狗动作 (AL 0x001B), 之后 mirror_ok 降 0 且
 * 升不回来, 唯一回程是重连。
 * 每笔 2 帧 = 2 × EM_POLL_MS, 单次静默被压回"一笔写"的量级。
 * 此刻该轴已失能 (6040h 镜像 = 0x0000), 这几帧不下任何命令 —— 泵帧本身是安全的。 */
#define EM_HOME_PREP_PUMP_FRAMES 2

/* 起手段与收尾段里, **SDO 写**的超时 (毫秒)。**这不是性能调优, 是安全参数** ——
 * 写超时原来写死 EC_TIMEOUTRXM = 700 ms (em__verified_write), 那与读超时是同一件事的另一半:
 * 它同样是"这次事务最多把过程数据静默多久"。§33.3 记的 1.6 s 看门狗阈值就架在这个值上。
 *
 * 为什么**只在这两段**压短, 不做成全局: 写与读的失败代价不同。一次读失败只是这一圈少一个
 * 诊断量, 下一圈还能再来; 一次写失败会让调用方**当场中止** (em_set_mode / em_enable /
 * PDO 映射全走这条路), 把一个本来会成功的写判死比多静默几十毫秒更坏。而这两段恰好是
 * 唯一"已知会有连续多笔写、且不能失败"的地方 —— 起手那 5 笔决定这一趟能不能开, 收尾那
 * 三级决定轴最后停在哪个状态。
 *
 * 取值依据: 实测正常应答 1~3 ms (见 em_set_sdo_timeout 的说明), 读那边取 60 ms 已是二十来倍;
 * 写多一条回读 (em__verified_write 写完必读回比对), 所以给到 150 ms = 正常应答的 50 倍,
 * 同时是 1.6 s 阈值的十分之一不到。 */
#define EM_SDO_TMO_WRITE_SHORT_MS 150

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

   /* 60FDh 数字输入 (TxPDO)。默认只绑不补: 在生效映射里才用, 否则 -1。
    * 本机实测生效的 1A00h 只有 6041h/6064h/606Ch 三项 10 字节, 没有 60FDh */
   int      off_dig_in;
   uint32_t dig_in;     /* 最新一帧完整过程数据里的 60FDh */

   /* 603Fh 驱动器故障码 (TxPDO)。与 off_dig_in 同一套, 区别是**默认就要求补进去**
    * (em_require_err_code, 由上位机在 setup 之前打开): 6041h bit3 只说"有故障",
    * 说不了是哪一个, 而把它放进过程数据之后报警码与 bit3 落在**同一帧**里 ——
    * 不必等一条 SDO(那条 SDO 会把过程数据整个停掉, 见 em_sdo_read 上面那段)。
    * -1 = 不在生效映射里, 此时上位机只能退回 SDO 读。 */
   int      off_err_code;
   uint16_t err_code;   /* 最新一帧完整过程数据里的 603Fh */

   /* 最新一帧完整过程数据里的采样 */
   uint16_t sw;
   int32_t  pos;
   int32_t  vel;
   int      mirror_ok;  /* **现在**还有完整帧 (连续短帧到 EM_SHORT_FRAMES_LIMIT 就降 0) */
   uint32_t frames;     /* 收到过多少个完整帧 */

   uint32_t short_frames;  /* **连续**短帧计数; 收到一帧完整的就归零 */

   /* 只增不减: 只要在整帧里见过一次 bit2 (Operation enabled) 就置 1, 之后永不清。
    * em_shutdown() 靠它决定"要不要试写 6040h=0x0000" —— 那一步**不能**问 mirror_ok:
    * 链路一断 mirror_ok 就降 0, 而"链路断了"恰恰是最需要试一把的时候 (见该函数注释)。 */
   int      ever_enabled;

   int32_t  csp_target;    /* CSP 插值目标的当前值 */

   /* ---- 回零会话的跨调用状态 (em_home_prepare/start/step/abort/finish 之间传) ----
    * 旧 em_home() 把这一整套放在自己的**局部变量**里 —— 那是"一趟回零 = 一次调用"成立时
    * 才行的写法。拆成四段之后, 这些值必须活得比任何一段长, 只能挂在轴上。
    * 两轴并行时每一根各有自己的一份, 这也正是要挂在这里、不能挂成文件级静态量的原因。 */
   int      hm_state;      /* EM_HM_S_* */
   uint32_t hm_t0;         /* 抬 bit4 的那一刻 (em__now_ms), 超时从它算 */
   uint32_t hm_tmo_ms;     /* 这一趟的上限 */
   uint32_t hm_reads;      /* 取到过多少笔完整 6041h —— 判"状态未知"就靠它 */
   int32_t  hm_pos0;       /* 起手位置 (超时诊断里比"动没动"的基准) */
   int      hm_pos_ok;     /* hm_pos0 是否可信 (取它时镜像就不可信则 0) */
   int      hm_lim_shown;  /* 上次打印过的 60FDh 两位组合; **必须显式置 -1**:
                            * calloc 给的 0 在旧代码里是"上次打印过 0b00", 三态被压成两态,
                            * 起手第一帧两位都没触发时那句就不打了 —— 与改造前不等价 */
   int      hm_lim_all;    /* 两位是否都触发过 (快到位了, 打印改频) */
   uint32_t hm_t_lim_print;/* 上次打印限位组合的时刻 (节流) */
   int      hm_rc;         /* 上一趟的结局 (em_home_step_rc), 收尾时要用它判 attained */

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

   /* 是否主动把 603Fh 追加进 TxPDO; **默认 1** —— 与 want_dig_in 相反。
    * 6041h bit3 只说"有故障"、说不了是哪一个, 603Fh 才是那个"哪一个";
    * 放在过程数据里它就与 bit3 同帧到达, 不必等 SDO(那条 SDO 会把过程数据停掉)。
    * 连接期参数, 必须由 em_require_err_code() 在 em_setup 之前设 */
   int want_err_code;

   /* 是否授权改驱动器参数 (目前只有 2300h); 默认 0 = 一个字节都不写。
    * 与 want_dig_in 分开两道门: 改 PDO 映射是通信配置 (掉电即回), 改参数是驱动器行为 */
   int allow_param;

   /* 所有 SDO 读的超时 (微秒)。**它不只是"等多久算失败"**: 一条 SDO 事务期间过程数据帧
    * 一帧都不发 (SOEM 的 SDO 走邮箱轮询, 见 em_sdo_read 的说明), 所以它就是"这次读最多把
    * 总线静默多久"。默认 EC_TIMEOUTRXM (700ms) 适合还没进 OP 的配置期 (那时没有过程数据,
    * 等久一点反而更容易读到); 进了 OP 之后必须由调用方用 em_set_sdo_timeout 压短 ——
    * 静默够长就会被驱动器看门狗抓住 (AL 0x001B), 而那正是我们要避免的事。
    * 连接期默认值由 em_bus_new 给, 所以每次连接都从 700ms 重新开始。 */
   int sdo_tmo_us;

   /* **SDO 写**的超时 (微秒), 与上面那个读超时分开一格, 因为它俩的默认值就该不同:
    * 读超时进 OP 之后一直被压短 (60000 us), 而写超时平时保持 0 = 用 EC_TIMEOUTRXM,
    * 只有回零的起手/收尾两段由 motor_api 自己临时设成 EM_SDO_TMO_WRITE_SHORT_MS。
    * 0 = 没设过 = 用 EC_TIMEOUTRXM。见那个宏的说明: 两段的边界由 em_home_prepare /
    * em_home_finish 自己兜住, 所以这里不需要对外暴露 setter —— 设了不复位就是永久压短,
    * 那属于"一次调用把全局状态改坏"。 */
   int sdo_wr_tmo_us;

   /* 2300h 快照, 下标 = 轴序号。sz 是驱动器自报的宽度 —— 手册 V2.4 p84 写 U16, 而当时那份
    * 现场基线表把它记成 U8 (那份表与记它的工具 2026-09-21 一起从仓库移除了), 两处对不上,
    * 所以读到几字节就按几字节写回, 不猜 */
   int      di_logic_have[EM_MAX_AXES];
   int      di_logic_changed[EM_MAX_AXES];
   int      di_logic_sz[EM_MAX_AXES];
   uint16_t di_logic_orig[EM_MAX_AXES];

   int in_op;
   int prev_manualstatechange;

   /* 每周期回调 (见公共头 em_set_cycle_hook)。谁都不挂时是 NULL —— calloc 建的 bus,
    * 所以初值就是 NULL。只有 em__cycle 读它, 且只读一次 */
   em_cycle_fn cycle_fn;
   void       *cycle_user;

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
