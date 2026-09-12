/*
 * sm_pdo - 用 PDO + OP 验证 CiA402 状态机切换 (独立小程序, 只干这一件事)
 *
 * 与 sm_state 的关系 (也是它存在的理由):
 *   sm_state 在 PRE_OP/SAFE_OP 下用 **SDO** 写 6040h, 实测 6041h 恒为 0x0210,
 *   一位未变。当时的结论是"该驱动器要求控制字走 PDO 且要进 OP"。这个归因方向
 *   是对的, 但根因比它更靠前一步, 而且已经记在 docs/slide_motion_verify.md §6:
 *
 *     出厂默认 TxPDO (1A00h) 是**空的** -> SM3 长度为 0 -> 从站以 AL 状态码
 *     0x001E (Invalid input configuration) 拒绝 PRE_OP -> SAFE_OP。
 *     SAFE_OP 和 OP 都进不去, 只剩 PRE_OP; 而 PRE_OP 下 CiA402 状态机根本不
 *     响应 6040h。所以失败不是"功率级打不开", 是"轮不到它"。
 *
 *   本程序把那条断掉的前置条件正面接上: 先让 TxPDO 合法, 再逐级上到 SAFE_OP
 *   与 OP, 然后把 6040h **写进 RxPDO 过程数据镜像**、把 6041h **从 TxPDO 过程
 *   数据镜像读出**, 走完与 sm_state 同构的 8 步闭环。
 *
 *   一句话: sm_state 问的是"PRE_OP + SDO 行不行", 本程序问的是"OP + PDO 行不行"。
 *
 * ---- 本程序必须写 PDO 映射 (与 slide_motion 的承诺不同, 这里是刻意的) ----
 *   slide_motion 承诺"不改 PDO 映射"。本程序存在的唯一目的就是验证 PDO 通路,
 *   而空 TxPDO 会让从站连 SAFE_OP 都进不去, 所以**必须先写 1A00h/1C13h**。
 *   边界收在这几处:
 *     - 只写 RAM, **从不写 2102h** (EEPROM), 掉电即回出厂空 TxPDO。
 *     - 默认**跑完还原**成运行前快照 (加 --keep-mapping 才保留)。
 *     - 默认只往 TxPDO **追加 6041h 一项** (16 bit), 已有的映射项原样保留;
 *       追加后偏移仍要能按字节证出来, 证不出就拒绝, 绝不猜。
 *     - 加 --no-map 可以完全不写映射 (此时映射必须已经合法, 否则拒绝)。
 *
 * ---- 安全边界 (改代码前先读这一段) ----
 *   1. 本文件是整个 slide_motion 目录里**第三个**出现 ecx_SDOwrite 的文件
 *      (另两个是 sm_guard.c 与 sm_state.c)。唯一的写入口是 pd_wr_raw() 的
 *      两个封装, 用途只有两种: (a) PDO 映射对象 1A00h/1C13h, (b) --reset-fault
 *      清故障。6040h **不经 SDO 写** —— 它只经过程数据镜像写, 这正是被测变量。
 *      审阅"什么会动"只需要看 pd_set_cw() 与 main() 里那张 8 步表。
 *   2. 推送过的 6040h 值只有 0x0000 / 0x0006 / 0x0007 / 0x000F (加 --reset-fault
 *      的 0x0080)。**永不写** 607Ah/6060h/6081h/6083h/6084h/607Dh/2102h,
 *      不发任何运动指令, 滑台不会移动。但第 3 步之后电机会通电 (有保持力矩)。
 *   3. 默认只读: 不给 --allow-pdo 就一个字节都不写, 只打印 PDO 现状。
 *   4. 只碰选中的那一根轴。别的从站哪怕在总线上, 我们也不请求它的 OP ——
 *      它留在 PRE_OP, SM2 未使能, 过程数据不会被它用到输出上。
 *   5. 任何异常路径 (Ctrl-C / 故障 / 超时 / 丢站 / 进不去 OP) 都走 pd_teardown():
 *      镜像写 0x0000 -> 降回 PRE_OP -> 还原映射。
 *
 * ---- 顺带补上的一个诊断 (sm_state 缺的) ----
 *   每步结束时用 **SDO 回读 6040h**, 看它是不是真的等于我们经 PDO 写进去的值。
 *   这一行把两种失败分开了, 而它们的修法完全不同:
 *     - 回读一致但 6041h 不变 -> 控制字到了驱动器, 驱动器不受理 (问题在驱动器侧);
 *     - 回读是 0 (或别的值)   -> 我们写的过程数据根本没落到控制字对象上
 *                                (问题在主站侧: 偏移算错 / 从站不在 OP / WKC 短)。
 *
 * 注意: 本文件链接 sm_bus.c (只读底座: 时钟 / SDO 读 / 寄存器读 / 错误栈排空 /
 * 身份判定), 它不含任何写。这不影响"唯一的写入口"这条边界 —— 写只在本文件。
 */

/*
 * 这里**刻意没有** <windows.h>: 本文件不直接调 Sleep()/GetTickCount64() —— 时钟和
 * 睡眠都走 sm_bus.c 的 sm_now_ms()。这不是洁癖, 是必需的: <windows.h> 会把老的
 * <winsock.h> 拉进来, 而 SOEM 的 osal 用 <winsock2.h>; 两个都进同一个编译单元就
 * 会撞 sockaddr/ip_mreq 重定义 (MSVC: error C2011)。
 * 若将来确实需要 windows.h, 必须把它放在 "sm.h" **之后** (sm_bus.c / slide_motion.c
 * 就是这么做的), 让 winsock2.h 先占住位置。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "sm.h"   /* 复用 SOEM 头 + 本工程对象索引/控制字/状态字常量 */

/* ======================================================================
 * 常量
 * ====================================================================== */

/*
 * 网卡写死, 与 sm_state.c 用同一条 (换机器/换网卡改这一行, 或用 --ifname=)。
 * 本程序不带必需的位置参数, 也不列举网卡 (只有 ecx_init 打不开它时才列出来当诊断)。
 */
#define PD_IFNAME "\\Device\\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"

/* 受支持的 YKD2205PE 由 sm_is_ykd() 判定 (厂商 0x0994 + 产品码 0x2000/0x3000) */

/* PDO 配置对象 (CiA301 / CiA402 通信区) */
#define PD_OID_RXPDO_ASSIGN 0x1C12  /* 分配的 RxPDO */
#define PD_OID_TXPDO_ASSIGN 0x1C13  /* 分配的 TxPDO */
#define PD_OID_RXPDO0       0x1600  /* 映射: 默认只有 6040h */
#define PD_OID_TXPDO0       0x1A00  /* 映射: 出厂为空 */

/* 映射项编码: index<<16 | sub<<8 | 位宽 */
#define PD_MAP(index, sub, bits) \
   (((uint32_t)(index) << 16) | ((uint32_t)(sub) << 8) | (uint32_t)(bits))
#define PD_MAP_6041 PD_MAP(0x6041, 0, 16)  /* 状态字, 16 bit */

/* 映射对象一次最多读几项 (本驱动器只有 1~2 项, 留余量) */
#define PD_MAP_MAX 8

/*
 * 断言掩码 = 手册定义的低 4 位。该驱动器手册 (docs/ykd2205pe_ci402.md) 只定义
 * 6041h 的位 0/1/2/3/10/12, **没有定义 bit6**, 所以不碰 bit6、也不做数值比较 ——
 * 沿用的正是 sm_state 当初修掉那个"拿启发式位报失败"的做法。
 */
#define PD_SW_MASK (SM_SW_RTSO | SM_SW_SWITCHED | SM_SW_OP_ENABLED | SM_SW_FAULT)

/* 等待与超时 */
#define PD_TMO_DEF        1000
#define PD_TMO_MAX        5000
#define PD_HOLD_DEF       500
#define PD_HOLD_MAX       5000

/* DC 同步周期 (µs): 手册给的 250µs~4000µs 范围内 */
#define PD_CYCLE_DEF_US   1000
#define PD_CYCLE_MIN_US   250
#define PD_CYCLE_MAX_US   4000

/*
 * 连续多少轮过程数据 WKC 偏短就判"过程数据没落地"。不是 1 轮: 刚进 OP 的
 * 头几帧可能还没排上, 单轮偏短说明不了什么; 一直偏短才是问题。
 */
#define PD_WKC_BAD_MAX    200

/* 过程数据镜像缓冲。与 SOEM samples 同款做法 (固定大缓冲)。 */
#define PD_IOMAP_MAX      65536

/* 收尾: 镜像写 0x0000 之后继续打多久过程数据, 让驱动器真的收到 */
#define PD_TEARDOWN_CYCLE_MS 200

/* 退出码 */
#define PD_EXIT_OK         0   /* 8 步全 PASS */
#define PD_EXIT_USAGE      1   /* 用法 / 初始化失败 */
#define PD_EXIT_NO_SLAVE   2   /* 无从站 / 无目标轴 / 目标不是 YKD */
#define PD_EXIT_FAIL       3   /* 存在 FAIL */
#define PD_EXIT_REFUSED    4   /* 安全护栏拒绝: 映射证不出 / 有别的从站 / 故障未授权。未写任何东西 */
#define PD_EXIT_NO_OP      5   /* 进不去 SAFE_OP / OP */
#define PD_EXIT_NO_PDO     6   /* 过程数据未落地 (WKC 持续偏短) */
#define PD_EXIT_NO_DISABLE 10  /* 收尾写了 0x0000 但 6041h 仍报 Operation enabled: 电机可能仍带电 */

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* Ctrl-C 标志。信号处理器只置这一位, 不做任何 I/O (信号上下文里发 SDO 是未定义行为) */
static volatile sig_atomic_t g_stop = 0;

static int g_tmo_ms  = PD_TMO_DEF;   /* 单步等待超时 */
static int g_cycle_us = PD_CYCLE_DEF_US;
static int g_use_dc  = 0;
static int g_keep_mapping = 0;
static int g_no_map  = 0;
static int g_reset_fault = 0;
static int g_allow_other = 0;
static int g_wrote = 0;              /* 本程序是否发生过任何写 */
static int g_raised_state = 0;       /* 是否请求过目标轴离开 PRE_OP */
static int g_in_op = 0;              /* 目标轴是否已达 OP */

static int g_expected_wkc = 0;       /* (outputsWKC*2)+inputsWKC */

/* 过程数据镜像与目标轴在其中的位置 */
static uint8_t g_iomap[PD_IOMAP_MAX];
static uint8_t *g_out = NULL;        /* 目标轴输出镜像基址 */
static uint8_t *g_in  = NULL;        /* 目标轴输入镜像基址 */
static int      g_off_cw = -1;       /* 6040h 在输出镜像中的字节偏移 */
static int      g_off_sw = -1;       /* 6041h 在输入镜像中的字节偏移 */

/* 证据计数 (汇总里打印) */
static int g_cw_sdo_checked = 0;     /* 回读过几次 6040h */
static int g_cw_sdo_match = 0;       /* 其中几次与经 PDO 写入的值一致 */
static int g_sw_pdo_sdo_checked = 0; /* 步末 PDO/SDO 双读数对照次数 */
static int g_sw_pdo_sdo_match = 0;   /* 其中几次一致 */

static void pd_on_ctrl_c(int sig)
{
   (void)sig;
   g_stop = 1;
}

static const char *pd_state_str(uint16_t st)
{
   switch (st & 0x0Fu)
   {
      case EC_STATE_INIT:        return "INIT";
      case EC_STATE_PRE_OP:      return "PRE_OP";
      case EC_STATE_SAFE_OP:     return "SAFE_OP";
      case EC_STATE_OPERATIONAL: return "OP";
      default:                   return "?";
   }
}

/* 6041h 状态名 —— 只按手册定义的低 4 位判读, 不替驱动器猜 bit6 */
static const char *pd_name(uint16_t sw)
{
   if ((sw & SM_SW_FAULT) != 0)
      return (sw & SM_SW_OP_ENABLED) ? "Fault reaction active" : "Fault";
   if ((sw & SM_SW_OP_ENABLED) != 0)
      return (sw & SM_SW_QUICKSTOP) ? "Operation enabled" : "Quick stop active";
   if ((sw & SM_SW_SWITCHED) != 0)
      return "Switched on";
   if ((sw & SM_SW_RTSO) != 0)
      return "Ready to switch on";
   /* 位 0-3 全 0: 标准 CiA402 里 "Not ready" 与 "Switch on disabled" 靠 bit6 分,
      该驱动器没定义 bit6, 所以合并成一个诚实的说法 */
   return "未使能 (位0-3 全 0)";
}

static void pd_sw_str(char *dst, size_t n, uint16_t sw)
{
   snprintf(dst, n, "0x%04X (%s)", (unsigned)sw, pd_name(sw));
}

/* 把映射项渲染成 " -> 6041h:00 16bit" */
static void pd_entry_str(char *dst, size_t n, uint32_t e)
{
   if (e == 0)
      snprintf(dst, n, " (空项)");
   else
      snprintf(dst, n, " -> %04Xh:%02X %u bit",
               (unsigned)((e >> 16) & 0xFFFFu),
               (unsigned)((e >> 8) & 0xFFu),
               (unsigned)(e & 0xFFu));
}

/* ======================================================================
 * SDO 读 (只读底座来自 sm_bus.c)
 * ====================================================================== */

static int pd_rd_u8(int slave, uint16_t index, uint8_t sub, uint8_t *v)
{
   uint8_t buf[4];
   int     size = 0;
   int32_t ab = 0;

   if (sm_rd_raw(slave, index, sub, SM_SDO_TMO_IDLE, buf, &size, &ab) != SM_RD_OK)
      return -1;
   *v = buf[0];
   return 0;
}

static int pd_rd_u16(int slave, uint16_t index, uint8_t sub, uint16_t *v)
{
   int32_t ab = 0;

   return (sm_rd_u16(slave, index, sub, v, SM_SDO_TMO_IDLE, &ab) == SM_RD_OK)
          ? 0 : -1;
}

/*
 * 读 U32。要求驱动器自报宽度 >= 4 才按 U32 解 —— 宽度不足时解出来的数会在
 * 高位补零, 看起来像一个完全正常的映射项/对象值 (与 sm_bytes_to_i64 同一个理由)。
 */
static int pd_rd_u32(int slave, uint16_t index, uint8_t sub, uint32_t *v)
{
   uint8_t buf[4];
   int     size = 0;
   int32_t ab = 0;

   if (sm_rd_raw(slave, index, sub, SM_SDO_TMO_IDLE, buf, &size, &ab) != SM_RD_OK)
      return -1;
   if (size < 4)
      return -1;
   memcpy(v, buf, 4);
   return 0;
}

/* ======================================================================
 * SDO 写 —— 本文件的唯一写入口
 *
 * 返回值**不代表驱动器接受了**: 这份 SOEM 在加急路径 (psize<=4) 上把从站回的
 * SDO abort 帧当成写成功 (abort 帧的 mbxtype/service/index/subindex 与请求完全
 * 一致, 命中 "all OK" 分支, 既不压错误栈也不置 ecaterror, wkc 还 > 0)。
 * 6040h 与所有映射对象都是 2+ 字节的加急写, 全在这条路径上 —— 所以映射对象的
 * 写一律走 pd_wr_*_verified(), 它写后回读, 回读才是唯一的确认。
 * ====================================================================== */
static int pd_wr_raw(int slave, uint16_t index, uint8_t sub, int size,
                     const void *p, const char *why)
{
   int wkc, ecerr;

   g_ctx.ecaterror = FALSE;
   wkc = ecx_SDOwrite(&g_ctx, (uint16_t)slave, index, sub, FALSE, size, p,
                      g_tmo_ms);
   ecerr = g_ctx.ecaterror ? 1 : 0;
   g_wrote = 1;

   if (wkc > 0 && !ecerr)
      return 0;

   printf("       写 %04Xh:%02X 未确认 (wkc=%d, ecaterror=%d) [%s]\n",
          (unsigned)index, (unsigned)sub, wkc, ecerr, why);
   return -1;
}

static int pd_wr_u16_verified(int slave, uint16_t index, uint8_t sub,
                              uint16_t v, const char *why)
{
   uint16_t back = 0;

   if (pd_wr_raw(slave, index, sub, 2, &v, why) != 0)
      return -1;
   if (pd_rd_u16(slave, index, sub, &back) != 0)
   {
      printf("       %04Xh:%02X 写后回读失败, 无法确认写是否生效 [%s]\n",
             (unsigned)index, (unsigned)sub, why);
      return -1;
   }
   if (back != v)
   {
      printf("       %04Xh:%02X 回读 0x%04X != 写入 0x%04X, 写没进去 [%s]\n",
             (unsigned)index, (unsigned)sub, (unsigned)back, (unsigned)v, why);
      return -1;
   }
   return 0;
}

static int pd_wr_u8_verified(int slave, uint16_t index, uint8_t sub,
                             uint8_t v, const char *why)
{
   uint8_t back = 0;

   if (pd_wr_raw(slave, index, sub, 1, &v, why) != 0)
      return -1;
   if (pd_rd_u8(slave, index, sub, &back) != 0)
   {
      printf("       %04Xh:%02X 写后回读失败, 无法确认写是否生效 [%s]\n",
             (unsigned)index, (unsigned)sub, why);
      return -1;
   }
   if (back != v)
   {
      printf("       %04Xh:%02X 回读 %u != 写入 %u, 写没进去 [%s]\n",
             (unsigned)index, (unsigned)sub, (unsigned)back, (unsigned)v, why);
      return -1;
   }
   return 0;
}

static int pd_wr_u32_verified(int slave, uint16_t index, uint8_t sub,
                              uint32_t v, const char *why)
{
   uint32_t back = 0;

   if (pd_wr_raw(slave, index, sub, 4, &v, why) != 0)
      return -1;
   if (pd_rd_u32(slave, index, sub, &back) != 0)
   {
      printf("       %04Xh:%02X 写后回读失败, 无法确认写是否生效 [%s]\n",
             (unsigned)index, (unsigned)sub, why);
      return -1;
   }
   if (back != v)
   {
      printf("       %04Xh:%02X 回读 0x%08X != 写入 0x%08X, 写没进去 [%s]\n",
             (unsigned)index, (unsigned)sub, (unsigned)back, (unsigned)v, why);
      return -1;
   }
   return 0;
}

/* ======================================================================
 * PDO 映射: 读 / 解析 / 偏移 / 快照 / 还原
 * ====================================================================== */

typedef struct
{
   int      n;
   uint32_t e[PD_MAP_MAX];
} pd_map_t;

static int pd_read_map(int slave, uint16_t index, pd_map_t *m)
{
   uint8_t cnt = 0;
   int     i;

   m->n = 0;
   if (pd_rd_u8(slave, index, 0, &cnt) != 0)
      return -1;
   if (cnt > PD_MAP_MAX)
      return -1;
   for (i = 1; i <= (int)cnt; i++)
   {
      if (pd_rd_u32(slave, index, (uint8_t)i, &m->e[m->n]) != 0)
         return -1;
      m->n++;
   }
   return 0;
}

/*
 * 求 (index, sub) 在映射里的**字节偏移**。找不到 / 位宽不符 / 前面累计位数不是
 * 8 的整数倍 -> 返回 -1。
 *
 * 为什么死守这条: 偏移算错 = 把控制字字节写进一个别的字段。万一那个字段是 607Ah
 * (目标位置), 我们就在毫不知情的情况下下了一个目标位置。所以证不出偏移必须拒绝,
 * 不允许"大概是 0 吧"。
 */
static int pd_map_offset(const pd_map_t *m, uint16_t index, uint8_t sub,
                         int want_bits)
{
   int bit = 0;
   int i;

   for (i = 0; i < m->n; i++)
   {
      uint16_t ei = (uint16_t)((m->e[i] >> 16) & 0xFFFFu);
      uint8_t  es = (uint8_t)((m->e[i] >> 8) & 0xFFu);
      int      eb = (int)(m->e[i] & 0xFFu);

      if (eb <= 0)
         return -1;                 /* 位宽 0 的项没法推进偏移, 整张表不可解释 */
      if (ei == index && es == sub)
      {
         if (eb != want_bits)
            return -1;
         if ((bit % 8) != 0)
            return -1;              /* 位对齐的字段不能按字节读 */
         return bit / 8;
      }
      bit += eb;
   }
   return -1;
}

/* 映射对象快照 (还原用) */
typedef struct
{
   uint16_t index;
   uint8_t  cnt;
   int      have;
   int      n;
   uint32_t e[PD_MAP_MAX];
   int      changed;
} pd_snap_t;

static int pd_snap_take(int slave, uint16_t index, pd_snap_t *s)
{
   pd_map_t m;

   memset(s, 0, sizeof(*s));
   s->index = index;
   if (pd_read_map(slave, index, &m) != 0)
   {
      printf("       %04Xh 读不出原始映射, 无法快照 -> 拒绝改动它\n",
             (unsigned)index);
      return -1;
   }
   s->cnt = (uint8_t)m.n;
   s->n = m.n;
   memcpy(s->e, m.e, sizeof(m.e));
   s->have = 1;
   return 0;
}

/*
 * 还原快照。顺序是规范要求的: 计数写 0 (关闭映射) -> 逐项写 -> 计数回原值。
 * 直接从原值改到原值是不行的: 映射对象在计数非 0 时子索引不可写。
 */
static int pd_snap_restore(int slave, const pd_snap_t *s)
{
   int i;

   if (!s->have || !s->changed)
      return 0;

   printf("  还原 %04Xh ...\n", (unsigned)s->index);
   if (pd_wr_u8_verified(slave, s->index, 0, 0, "还原: 计数归零") != 0)
      return -1;
   for (i = 0; i < s->n; i++)
   {
      if (pd_wr_u32_verified(slave, s->index, (uint8_t)(i + 1), s->e[i],
                             "还原: 回写映射项") != 0)
         return -1;
   }
   if (pd_wr_u8_verified(slave, s->index, 0, s->cnt, "还原: 计数回原值") != 0)
      return -1;
   printf("  还原完成 (%04Xh:00 = %u)\n", (unsigned)s->index, (unsigned)s->cnt);
   return 0;
}

/* ======================================================================
 * 只读诊断: PDO 现状
 * ====================================================================== */

static void pd_dump_map_obj(int slave, uint16_t index, const char *label)
{
   pd_map_t m;
   int      i;

   if (pd_read_map(slave, index, &m) != 0)
   {
      printf("    %-14s %04Xh  读失败 (对象不存在或不可读)\n", label,
             (unsigned)index);
      return;
   }
   printf("    %-14s %04Xh:00 = %d", label, (unsigned)index, m.n);
   for (i = 0; i < m.n; i++)
   {
      char s[48];

      pd_entry_str(s, sizeof(s), m.e[i]);
      printf("\n                     :%02X = 0x%08X%s", i + 1,
             (unsigned)m.e[i], s);
   }
   printf("\n");
}

/*
 * SM 起始地址与长度。长度 0 就是"这个 SM 没被使能" —— 空 TxPDO 的直接证据,
 * 也是跑完补映射之后该消失的东西。
 */
static void pd_dump_sm_one(int slave, const char *label, uint16_t reg)
{
   uint16_t start = 0, len = 0;
   int      ok1 = (sm_reg_read16(slave, reg, &start) == 0);
   int      ok2 = (sm_reg_read16(slave, (uint16_t)(reg + 2), &len) == 0);

   if (!ok1 || !ok2)
   {
      printf("    %-14s 寄存器读失败\n", label);
      return;
   }
   printf("    %-14s 起始 0x%04X  长度 %u 字节%s\n", label,
          (unsigned)start, (unsigned)len,
          (len == 0) ? "   <- 长度 0 = SM 未使能" : "");
}

static void pd_dump_pdo(int slave)
{
   pd_dump_map_obj(slave, PD_OID_RXPDO_ASSIGN, "1C12h RxPDO 分配");
   pd_dump_map_obj(slave, PD_OID_TXPDO_ASSIGN, "1C13h TxPDO 分配");
   pd_dump_map_obj(slave, PD_OID_RXPDO0, "1600h RxPDO0");
   pd_dump_map_obj(slave, PD_OID_TXPDO0, "1A00h TxPDO0");
   pd_dump_sm_one(slave, "SM2 (RxPDO)", ECT_REG_SM2);
   pd_dump_sm_one(slave, "SM3 (TxPDO)", ECT_REG_SM3);
}

/* ======================================================================
 * 补 TxPDO: 确保 1C13h 分配 1A00h, 且 1A00h 含 6041h
 *
 * 返回 0 = 已完成 (可能没改动) / -1 = 读失败 / -2 = 拒绝 (不替驱动器猜)
 * ====================================================================== */

static int pd_ensure_tx_assign(int slave, pd_snap_t *s13)
{
   pd_map_t a;
   int      i;

   if (pd_read_map(slave, PD_OID_TXPDO_ASSIGN, &a) != 0)
   {
      printf("  读 1C13h 失败, 无法确认 TxPDO 分配 -> 拒绝\n");
      return -1;
   }
   for (i = 0; i < a.n; i++)
   {
      if (a.e[i] == PD_OID_TXPDO0)
      {
         printf("  1C13h 已分配 1A00h, 无需改动\n");
         return 0;
      }
   }
   if (a.n > 0)
   {
      /*
       * 已经分配了别的 TxPDO, 而且不是 1A00h。要把它换掉就必须替驱动器决定
       * "哪个 TxPDO 是可有可无的" —— 那是它的配置, 不是我们的。拒绝。
       */
      printf("  1C13h 已分配了别的东西 (count=%d, 首项 0x%08X), 不是 1A00h。\n",
             a.n, (unsigned)a.e[0]);
      printf("  不替驱动器猜该替换哪一项 -> 拒绝。请先用厂商工具把 1A00h 配成\n");
      printf("  TxPDO0, 或干脆把它改空 (count=0) 后重试。\n");
      return -2;
   }

   if (pd_snap_take(slave, PD_OID_TXPDO_ASSIGN, s13) != 0)
      return -1;
   printf("  1C13h 为空 -> 写 count=1, :01 = 0x%04X (1A00h)\n",
          (unsigned)PD_OID_TXPDO0);
   if (pd_wr_u8_verified(slave, PD_OID_TXPDO_ASSIGN, 0, 0, "1C13h 计数归零") != 0)
      return -1;
   if (pd_wr_u16_verified(slave, PD_OID_TXPDO_ASSIGN, 1,
                          (uint16_t)PD_OID_TXPDO0, "1C13h 分配 1A00h") != 0)
      return -1;
   if (pd_wr_u8_verified(slave, PD_OID_TXPDO_ASSIGN, 0, 1, "1C13h 计数=1") != 0)
      return -1;
   s13->changed = 1;
   return 0;
}

static int pd_ensure_tx_map(int slave, pd_snap_t *s1a)
{
   pd_map_t m;
   int      i;

   if (pd_read_map(slave, PD_OID_TXPDO0, &m) != 0)
   {
      printf("  读 1A00h 失败 -> 拒绝\n");
      return -1;
   }
   if (pd_map_offset(&m, 0x6041, 0, 16) >= 0)
   {
      int off = pd_map_offset(&m, 0x6041, 0, 16);

      printf("  1A00h 已含 6041h (字节偏移 %d), 无需补写\n", off);
      return 0;
   }

   /*
    * 追加一项 6041h, 已有的项原样保留 (不覆盖别人的配置)。追加后整张表必须仍能
    * 按字节解释出 6041h 的偏移, 否则拒绝 —— 宁可不动, 也不要留下一个读不准的表。
    */
   if (m.n >= PD_MAP_MAX)
   {
      printf("  1A00h 已有 %d 项, 加不下 6041h -> 拒绝\n", m.n);
      return -2;
   }
   if (pd_snap_take(slave, PD_OID_TXPDO0, s1a) != 0)
      return -1;

   {
      uint32_t e[PD_MAP_MAX];
      pd_map_t probe;
      int      n = m.n;

      memcpy(e, m.e, sizeof(e));
      e[n++] = PD_MAP_6041;
      probe.n = n;
      memcpy(probe.e, e, sizeof(e));

      if (pd_map_offset(&probe, 0x6041, 0, 16) < 0)
      {
         printf("  追加 6041h 之后映射表不再是字节可解释的 (前面有非整字节项)\n");
         printf("  -> 拒绝。请用厂商工具把 1A00h 配成只有整字节项。\n");
         return -2;
      }

      printf("  1A00h 追加 6041h: 项数 %d -> %d, 6041h 位于字节偏移 %d\n",
             m.n, n, pd_map_offset(&probe, 0x6041, 0, 16));

      if (pd_wr_u8_verified(slave, PD_OID_TXPDO0, 0, 0, "1A00h 计数归零") != 0)
         return -1;
      for (i = 0; i < n; i++)
      {
         if (pd_wr_u32_verified(slave, PD_OID_TXPDO0, (uint8_t)(i + 1), e[i],
                                "1A00h 写映射项") != 0)
            return -1;
      }
      if (pd_wr_u8_verified(slave, PD_OID_TXPDO0, 0, (uint8_t)n,
                            "1A00h 计数=项数") != 0)
         return -1;
   }

   s1a->changed = 1;
   return 0;
}

/* ======================================================================
 * 过程数据
 * ====================================================================== */

/* 打一轮过程数据 (发 + 收), 返回接收侧的 WKC */
static int pd_cycle(void)
{
   ecx_send_processdata(&g_ctx);
   return ecx_receive_processdata(&g_ctx, EC_TIMEOUTRET);
}

/*
 * 镜像写入必须按小端字节写: SOEM 把 IOmap 原样塞进 EtherCAT 帧, 而 EtherCAT
 * 线上是小端。本工程只跑 Windows/x86, 所以 memcpy 一个 uint16 就是小端表示。
 * 用 memcpy 而不是强制转换, 是为了不依赖 off_cw 的对齐。
 */
static void pd_set_cw(uint16_t cw)
{
   if (g_out == NULL || g_off_cw < 0)
      return;
   memcpy(g_out + g_off_cw, &cw, 2);
}

static int pd_rd_sw_pdo(uint16_t *sw)
{
   if (g_in == NULL || g_off_sw < 0)
      return -1;
   memcpy(sw, g_in + g_off_sw, 2);
   return 0;
}

/*
 * 用 SDO 回读 6040h —— 这是"控制字到底有没有进到驱动器"的直接证据 (见文件头)。
 * 与经 PDO 写入的值比对, 计数进汇总。
 */
static void pd_check_cw_landed(int slave, uint16_t wrote)
{
   uint16_t back = 0;

   if (pd_rd_u16(slave, SM_OID_CONTROLWORD, 0, &back) != 0)
   {
      printf("       6040h 回读失败 (驱动器不允许读控制字?)\n");
      return;
   }
   g_cw_sdo_checked++;
   if (back == wrote)
      g_cw_sdo_match++;
   printf("       6040h 回读 = 0x%04X (经 RxPDO 写入 0x%04X) %s\n",
          (unsigned)back, (unsigned)wrote,
          (back == wrote) ? "一致 -> 控制字确实到了驱动器"
                          : "<<< 不一致 -> 过程数据没落到控制字对象上");
}

/* ======================================================================
 * AL 状态阶梯
 * ====================================================================== */

/*
 * 请求目标轴的 AL 状态。
 * 若从站当前处于 AL 错误态 (状态字 bit4), 必须把 ACK 一起写进去才能清掉它 ——
 * ecx_writestate() 只把 slavelist[].state 原样写下去, 不会自动置 ACK。
 */
static void pd_request_state(int slave, uint16_t want)
{
   uint16_t cur = g_ctx.slavelist[slave].state;

   if ((cur & EC_STATE_ERROR) != 0)
   {
      printf("  (从站当前 AL 状态 0x%02X 带错误位, 以 0x%02X = %s|ACK 请求清除)\n",
             (unsigned)cur, (unsigned)(want | EC_STATE_ACK), pd_state_str(want));
      g_ctx.slavelist[slave].state = (uint16_t)(want | EC_STATE_ACK);
   }
   else
   {
      g_ctx.slavelist[slave].state = (uint16_t)want;
   }
   ecx_writestate(&g_ctx, (uint16_t)slave);
}

/*
 * 等目标轴进入 want。**每轮都打过程数据**再查状态: 状态迁移期间 (尤其进 OP)
 * 过程数据不能断, 否则若驱动器的 SM 看门狗是开着的, 会反过来把我们从迁移里踢出来。
 *
 * 返回 0 = 到达; -1 = 失败 (已打印原因); -2 = 被 Ctrl-C 中止。
 */
static int pd_wait_state(int slave, uint16_t want, uint32_t tmo_ms)
{
   uint32_t t0 = sm_now_ms();

   for (;;)
   {
      uint16_t st;

      if (g_stop)
         return -2;

      /*
       * 状态迁移期间过程数据不能断 (驱动器若开着 SM/过程数据看门狗, 断流会把它
       * 从迁移里踢出来)。这里特意**不**判 WKC: 迁移途中它本来就可能偏短, 判了
       * 只会误报; 过程数据的判据在 pd_step(), 那时从站必须已经稳定在 OP。
       */
      (void)pd_cycle();

      st = ecx_statecheck(&g_ctx, (uint16_t)slave, want, 1000);

      /*
       * ecx_statecheck() 把状态按 0x000F 掩过, 所以错误位(0x10)在它的返回值里
       * 看不见 —— 必须单独查 slavelist[].state。这正是文档里记的 sm_state 那个
       * "进 SAFE_OP 失败却只打印一行 0x12" 的坑, 这里正面处理成显式失败。
       */
      if ((g_ctx.slavelist[slave].state & EC_STATE_ERROR) != 0)
      {
         printf("  [FAIL] 请求 %s 被拒绝: AL 状态 0x%02X (含错误位), "
                "AL 状态码 0x%04X %s\n",
                pd_state_str(want), (unsigned)g_ctx.slavelist[slave].state,
                (unsigned)g_ctx.slavelist[slave].ALstatuscode,
                ec_ALstatuscode2string(g_ctx.slavelist[slave].ALstatuscode));
         return -1;
      }
      if (st == want)
         return 0;

      if ((int32_t)(sm_now_ms() - t0) >= (int32_t)tmo_ms)
      {
         printf("  [FAIL] %ums 内未进入 %s (当前 AL 状态 0x%02X, "
                "AL 状态码 0x%04X %s)\n",
                (unsigned)tmo_ms, pd_state_str(want),
                (unsigned)g_ctx.slavelist[slave].state,
                (unsigned)g_ctx.slavelist[slave].ALstatuscode,
                ec_ALstatuscode2string(g_ctx.slavelist[slave].ALstatuscode));
         return -1;
      }
   }
}

/* ======================================================================
 * 一步切换: 写 RxPDO 里的 6040h -> 等 TxPDO 里的 6041h 符合断言
 *
 * 返回 0 = PASS / -1 = FAIL / -2 = 被中止 / -3 = 过程数据未落地
 * ====================================================================== */
static int pd_step(int slave, const char *name, uint16_t cw, uint16_t mask,
                   uint16_t expect, int fail_on_fault, uint16_t *sw_out)
{
   uint32_t t0;
   uint16_t sw = 0;
   int      got = 0;
   int      reads = 0;      /* 从 PDO 成功取到状态的次数: 0 表示全程没有过程数据 */
   int      bad_wkc = 0;
   char     buf[64];

   printf("  %-28s 写 RxPDO[6040h]=0x%04X ...", name, (unsigned)cw);
   fflush(stdout);

   pd_set_cw(cw);

   t0 = sm_now_ms();
   while ((int32_t)(sm_now_ms() - t0) < g_tmo_ms)
   {
      int wkc;

      if (g_stop)
      {
         printf(" [中止]\n");
         return -2;
      }

      wkc = pd_cycle();
      if (g_expected_wkc > 0 && wkc < g_expected_wkc)
      {
         if (bad_wkc == 0)
            printf("\n       过程数据 WKC=%d < 期望 %d (从站可能不在 OP / 掉线)\n",
                   wkc, g_expected_wkc);
         if (++bad_wkc > PD_WKC_BAD_MAX)
         {
            printf(" [FAIL] 连续 %d 轮过程数据 WKC 偏短, 判『过程数据未落地』\n",
                   PD_WKC_BAD_MAX);
            return -3;
         }
      }
      else
         bad_wkc = 0;

      if (pd_rd_sw_pdo(&sw) == 0)
      {
         reads++;
         if (fail_on_fault && (sw & SM_SW_FAULT) != 0)
         {
            pd_sw_str(buf, sizeof(buf), sw);
            printf(" [FAIL] 等待期间进入故障态 (TxPDO 6041h=%s)\n", buf);
            return -1;
         }
         if ((sw & mask) == expect)
         {
            got = 1;
            break;
         }
      }
   }

   if (sw_out != NULL)
      *sw_out = sw;

   if (!got)
   {
      struct ec_slave *s = &g_ctx.slavelist[slave];

      printf(" [FAIL] 断言未成立\n");
      if (reads == 0)
      {
         /*
          * 一笔都没取到。这里**不能**把 sw (零初始化的变量) 当成"实测 0x0000" ——
          * 0x0000 在 CiA402 里是合法的 "未使能", 那会把"我们不知道"说成
          * "驱动器报了个状态"。
          */
         printf("       一笔 6041h 都没从 TxPDO 取到 (%ums 内): 状态未知\n",
                (unsigned)g_tmo_ms);
      }
      else
      {
         pd_sw_str(buf, sizeof(buf), sw);
         printf("       期望 6041h & 0x%04X == 0x%04X, 实测 %s (%d 次读数)\n",
                (unsigned)mask, (unsigned)expect, buf, reads);
      }
      printf("       EtherCAT AL 状态=0x%02X, AL 状态码=0x%04X %s\n",
             (unsigned)s->state, (unsigned)s->ALstatuscode,
             ec_ALstatuscode2string(s->ALstatuscode));
      pd_check_cw_landed(slave, cw);
      return -1;
   }

   pd_sw_str(buf, sizeof(buf), sw);
   printf(" [PASS] TxPDO 6041h=%s\n", buf);

   /* 步末对照: 同一条 6041h 用 SDO 再读一次, 看两条通路是否一致 */
   {
      uint16_t sdo = 0;

      if (pd_rd_u16(slave, SM_OID_STATUSWORD, 0, &sdo) == 0)
      {
         g_sw_pdo_sdo_checked++;
         if (sdo == sw)
            g_sw_pdo_sdo_match++;
         printf("           6041h 对照: PDO=0x%04X SDO=0x%04X %s\n",
                (unsigned)sw, (unsigned)sdo,
                (sdo == sw) ? "一致" : "<<< 两条通路读数不一致");
      }
   }
   pd_check_cw_landed(slave, cw);
   return 0;
}

/* ======================================================================
 * 收尾 (幂等; 覆盖所有退出路径)
 *
 * 返回 0 = 已确认失能 / 1 = 未能确认失能 (电机可能仍带电)
 * ====================================================================== */
static int pd_teardown(int slave, pd_snap_t *s13, pd_snap_t *s1a)
{
   uint16_t sw = 0;
   int      reads = 0;
   uint32_t t0;
   char     buf[64];
   int      unconfirmed = 0;

   printf("\n---- 收尾 ----\n");

   /*
    * 1. 只要动过控制字, 就先在镜像里写 0x0000 并把过程数据继续打一段 ——
    *    让驱动器**真的收到**这个失能字, 而不是把它留在缓冲里。
    */
   if (g_wrote && g_out != NULL && g_off_cw >= 0)
   {
      printf("  写 RxPDO[6040h]=0x0000 (Disable voltage), 继续打 %dms 过程数据\n",
             PD_TEARDOWN_CYCLE_MS);
      pd_set_cw(SM_CW_DISABLE_V);
      t0 = sm_now_ms();
      while ((int32_t)(sm_now_ms() - t0) < PD_TEARDOWN_CYCLE_MS)
         (void)pd_cycle();
   }

   /* 2. 还在 OP 的话, 用 TxPDO 的 6041h 确认 bit2 已清 (这是"电机不带电"的正面证据) */
   if (g_in_op && g_off_sw >= 0)
   {
      t0 = sm_now_ms();
      while ((int32_t)(sm_now_ms() - t0) < g_tmo_ms)
      {
         if (pd_rd_sw_pdo(&sw) == 0)
         {
            reads++;
            if ((sw & SM_SW_OP_ENABLED) == 0)
               break;
         }
         (void)pd_cycle();
      }
      if (reads == 0)
      {
         printf("  [FAIL] 收尾期间一笔 6041h 都没能从 TxPDO 取到, 失能**无法确认**\n");
         unconfirmed = 1;
      }
      else if ((sw & SM_SW_OP_ENABLED) != 0)
      {
         pd_sw_str(buf, sizeof(buf), sw);
         printf("  [FAIL] 写了 0x0000 但 TxPDO 6041h 仍报 Operation enabled (%s)\n", buf);
         unconfirmed = 1;
      }
      else
      {
         pd_sw_str(buf, sizeof(buf), sw);
         printf("  [PASS] TxPDO 已确认失能 (6041h=%s)\n", buf);
      }
   }

   /* 3. 降回 PRE_OP (映射对象只在 PRE_OP 可写, 还原必须在它之后) */
   if (g_raised_state)
   {
      printf("  降回 PRE_OP ...\n");
      pd_request_state(slave, EC_STATE_PRE_OP);
      if (pd_wait_state(slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE / 1000) != 0)
         printf("  [WARN] 未能确认回到 PRE_OP; 映射还原可能失败\n");
      g_in_op = 0;
   }

   /*
    * 4. 用 SDO 独立复核一次 6041h。PDO 那一路的确认只在我们确实到过 OP 时才有效,
    *    这一步在**任何**状态下都能做, 所以它是最后一道保险。
    */
   if (g_wrote)
   {
      uint16_t sdo_sw = 0;

      if (pd_rd_u16(slave, SM_OID_STATUSWORD, 0, &sdo_sw) != 0)
      {
         printf("  [FAIL] 最终 SDO 回读 6041h 失败, 失能状态无法确认\n");
         unconfirmed = 1;
      }
      else
      {
         pd_sw_str(buf, sizeof(buf), sdo_sw);
         printf("  最终 SDO 6041h = %s\n", buf);
         if ((sdo_sw & SM_SW_OP_ENABLED) != 0)
         {
            printf("  [FAIL] SDO 回读仍报 Operation enabled\n");
            unconfirmed = 1;
         }
      }
   }

   /* 5. 还原 PDO 映射 (默认做; --keep-mapping 跳过) */
   if (g_keep_mapping)
   {
      if (s13->changed || s1a->changed)
         printf("  [--keep-mapping] 保留本次改动过的 PDO 映射 (掉电即恢复出厂值)\n");
   }
   else
   {
      if (pd_snap_restore(slave, s1a) != 0)
         printf("  [WARN] 1A00h 还原失败; 它只在 RAM, 掉电即恢复出厂值\n");
      if (pd_snap_restore(slave, s13) != 0)
         printf("  [WARN] 1C13h 还原失败; 它只在 RAM, 掉电即恢复出厂值\n");
   }

   if (unconfirmed)
   {
      printf("  >>> 电机可能仍然带电。立即断开驱动器供电, 不要用手去推滑台。\n");
      return 1;
   }
   return 0;
}

/* ======================================================================
 * 用法
 * ====================================================================== */
static void pd_usage(void)
{
   printf(
      "\n用法: sm_pdo [网卡] [选项]        (默认网卡写死在代码里: %s)\n"
      "\n"
      "  --allow-pdo           允许写 PDO 映射并进 OP 跑周期通信。不加则只读快照。\n"
      "  --axis N              选哪根轴 (总线位置, 0 起)。默认第一台 YKD2205PE。\n"
      "  --no-map              不写 PDO 映射 (要求映射已经合法, 否则拒绝)。\n"
      "  --keep-mapping        跑完保留改过的映射 (默认还原成运行前快照)。\n"
      "  --state op|safe-op    跑到哪个 AL 状态, 默认 op。safe-op 只验证 AL 0x001E 是否消失。\n"
      "  --dc                  配置分布式时钟 (自由运行进不去 OP 时试它)。\n"
      "  --cycle US            DC 同步周期, 默认 %d (范围 %d~%d)。\n"
      "  --reset-fault         允许清故障 (在 OP 下经 PDO 推 0x0080)。\n"
      "  --allow-other-slaves  总线上还有别的从站时也继续 (它们留在 PRE_OP)。\n"
      "  --hold MS             停在 Operation enabled 的观察时长, 默认 %d (上限 %d)。\n"
      "  --tmo MS              单步等待超时, 默认 %d (上限 %d)。\n"
      "  --ifname=GUID         覆盖写死的网卡。\n"
      "\n"
      "退出码: 0 全 PASS / 1 用法或初始化 / 2 无从站或无目标轴 /\n"
      "        3 有 FAIL / 4 安全护栏拒绝 (未写任何东西) / 5 进不去 SAFE_OP 或 OP /\n"
      "        6 过程数据未落地 (WKC 持续偏短) /\n"
      "        10 收尾后 6041h 仍报 Operation enabled (电机可能仍带电)\n"
      "\n"
      "本程序**会写 PDO 映射对象 1A00h/1C13h** (只写 RAM, 从不写 2102h, 默认跑完\n"
      "还原) —— 因为出厂空 TxPDO 让从站连 SAFE_OP 都进不去。6040h 只经过程数据\n"
      "写, 推送过的值只有 0x0000/0x0006/0x0007/0x000F。不发运动指令, 滑台不会移动,\n"
      "但第 3 步 (Enable Operation) 之后电机会带电并有保持力矩 ——\n"
      "  >>> 真跑时人在设备旁, 手放在物理急停上。\n",
      PD_IFNAME, PD_CYCLE_DEF_US, PD_CYCLE_MIN_US, PD_CYCLE_MAX_US,
      PD_HOLD_DEF, PD_HOLD_MAX, PD_TMO_DEF, PD_TMO_MAX);
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char *argv[])
{
   const char *ifname = PD_IFNAME;
   int   ifname_given = 0;   /* 是否被 --ifname= 或裸位置参数覆盖过 */
   int   allow_pdo = 0;
   int   axis_only = -1;
   int   want_safe_op = 0;
   int   hold_ms = PD_HOLD_DEF;
   int   cnt, slave, i;
   int   target = -1;
   int   rc, exit_code = PD_EXIT_OK;
   int   any_fail = 0;
   int   nopdo = 0;
   uint16_t sw = 0;
   char  buf[64];
   pd_snap_t s13, s1a;

   memset(&s13, 0, sizeof(s13));
   memset(&s1a, 0, sizeof(s1a));

   sm_console_utf8();
   signal(SIGINT, pd_on_ctrl_c);

   printf("sm_pdo - 用 PDO + OP 验证 CiA402 状态机切换\n");

   /* ---- 参数 ---- */
   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];

      if (strcmp(a, "--allow-pdo") == 0)
         allow_pdo = 1;
      else if (strcmp(a, "--no-map") == 0)
         g_no_map = 1;
      else if (strcmp(a, "--keep-mapping") == 0)
         g_keep_mapping = 1;
      else if (strcmp(a, "--dc") == 0)
         g_use_dc = 1;
      else if (strcmp(a, "--reset-fault") == 0)
         g_reset_fault = 1;
      else if (strcmp(a, "--allow-other-slaves") == 0)
         g_allow_other = 1;
      else if (strcmp(a, "--axis") == 0 && i + 1 < argc)
         axis_only = atoi(argv[++i]);
      else if (strcmp(a, "--hold") == 0 && i + 1 < argc)
         hold_ms = atoi(argv[++i]);
      else if (strcmp(a, "--tmo") == 0 && i + 1 < argc)
         g_tmo_ms = atoi(argv[++i]);
      else if (strcmp(a, "--cycle") == 0 && i + 1 < argc)
         g_cycle_us = atoi(argv[++i]);
      else if (strncmp(a, "--ifname=", 9) == 0)
      {
         ifname = a + 9;
         ifname_given = 1;
      }
      else if (strcmp(a, "--state") == 0 && i + 1 < argc)
      {
         const char *v = argv[++i];

         if (strcmp(v, "safe-op") == 0)
            want_safe_op = 1;
         else if (strcmp(v, "pre-op") == 0)
         {
            printf("--state pre-op 没有意义: 本程序验证的就是 OP 下的 PDO 通路。\n");
            printf("若只想看能不能进 SAFE_OP, 用 --state safe-op。\n");
            return PD_EXIT_USAGE;
         }
         else if (strcmp(v, "op") != 0)
         {
            printf("未知 --state: %s (只支持 op / safe-op)\n", v);
            return PD_EXIT_USAGE;
         }
      }
      else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
      {
         pd_usage();
         return PD_EXIT_OK;
      }
      else if (a[0] != '-' && !ifname_given)
      {
         /* 裸的位置参数当网卡名 (与 slide_verify 等一致)。
            用标志位判断"是否已被覆盖", 不用 ifname == PD_IFNAME 比指针 ——
            那是在比字符串字面量的地址, 行为未定义 (编译器可能把相同的字面量
            合并, 也可能不合并)。 */
         ifname = a;
         ifname_given = 1;
      }
      else
      {
         printf("未知参数: %s\n", a);
         pd_usage();
         return PD_EXIT_USAGE;
      }
   }

   /* 上限只能收紧: 超了就夹回去, 不报错 —— 这几个值只是等待时长/周期, 夹住更安全 */
   if (hold_ms < 0)              hold_ms = 0;
   if (hold_ms > PD_HOLD_MAX)    hold_ms = PD_HOLD_MAX;
   if (g_tmo_ms < 100)           g_tmo_ms = 100;
   if (g_tmo_ms > PD_TMO_MAX)    g_tmo_ms = PD_TMO_MAX;
   if (g_cycle_us < PD_CYCLE_MIN_US) g_cycle_us = PD_CYCLE_MIN_US;
   if (g_cycle_us > PD_CYCLE_MAX_US) g_cycle_us = PD_CYCLE_MAX_US;

   if (axis_only < -1)
   {
      printf("--axis 不能是负数\n");
      return PD_EXIT_USAGE;
   }
   if (!allow_pdo)
      printf("未给 --allow-pdo: 本次只读快照, 不写任何字节、不进 OP。\n");
   if (g_no_map)
      printf("--no-map: 不写 PDO 映射 (要求 1A00h 已含 6041h, 否则拒绝)。\n");

   printf("网卡: %s\n", ifname);

   /* ---- 开总线。到这一步为止没写过任何东西 ---- */
   if (!ecx_init(&g_ctx, ifname))
   {
      printf("无法打开网卡 (被独占 / 需管理员 / Npcap 未装)。可用网卡:\n");
      sm_print_adapters();
      return PD_EXIT_USAGE;
   }
   cnt = ecx_config_init(&g_ctx);
   if (cnt <= 0)
   {
      printf("总线上未发现从站 (config_init=%d), 检查网线/供电。\n", cnt);
      ecx_close(&g_ctx);
      return PD_EXIT_NO_SLAVE;
   }

   printf("\n等待从站进入 PRE_OP ...\n");
   for (slave = 1; slave <= cnt; slave++)
      ecx_statecheck(&g_ctx, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   ecx_readstate(&g_ctx);

   /* ---- 选轴: 只认受支持的 YKD2205PE ---- */
   printf("\n---- 总线上的从站 ----\n");
   for (slave = 1; slave <= cnt; slave++)
   {
      const struct ec_slave *s = &g_ctx.slavelist[slave];
      int is_ykd = sm_is_ykd(s->eep_man, s->eep_id);
      int selected = (axis_only < 0) ? is_ykd : (axis_only == slave - 1);

      printf("  位置 %-3d AL=0x%02X %-10s 厂商=0x%08X 产品码=0x%08X %s\n",
             slave - 1, (unsigned)s->state, pd_state_str(s->state),
             (unsigned)s->eep_man, (unsigned)s->eep_id,
             is_ykd ? "YKD2205PE" : "(非受支持)");

      if (selected && target < 0)
      {
         if (!is_ykd)
         {
            printf("\n[拒绝] 位置 %d 不是受支持的 YKD2205PE, 未写任何东西。\n",
                   slave - 1);
            ecx_close(&g_ctx);
            return PD_EXIT_NO_SLAVE;
         }
         if (s->state != EC_STATE_PRE_OP && s->state != EC_STATE_SAFE_OP)
         {
            printf("\n[拒绝] 位置 %d 未进入 PRE_OP/SAFE_OP (AL 状态 0x%02X, "
                   "AL 状态码 0x%04X %s), 未写任何东西。\n",
                   slave - 1, (unsigned)s->state, (unsigned)s->ALstatuscode,
                   ec_ALstatuscode2string(s->ALstatuscode));
            ecx_close(&g_ctx);
            return PD_EXIT_NO_SLAVE;
         }
         target = slave;
      }
   }

   if (target < 0)
   {
      if (axis_only >= 0)
         printf("\n[拒绝] --axis %d 不在总线上 (共 %d 台从站), 未写任何东西。\n",
                axis_only, cnt);
      else
         printf("\n[拒绝] 总线上没有受支持的 YKD2205PE 可作目标, 未写任何东西。\n");
      ecx_close(&g_ctx);
      return PD_EXIT_NO_SLAVE;
   }

   printf("\n目标轴: 位置 %d (从站序号 %d)\n", target - 1, target);

   /* ---- 只读报告 (这一段不写任何字节) ---- */
   printf("\n---- 目标轴 PDO 现状 (只读) ----\n");
   pd_dump_pdo(target);

   if (pd_rd_u16(target, SM_OID_STATUSWORD, 0, &sw) != 0)
   {
      printf("\n[拒绝] 读不到 6041h, 无法判断当前状态, 未写任何东西。\n");
      ecx_close(&g_ctx);
      return PD_EXIT_NO_SLAVE;
   }
   pd_sw_str(buf, sizeof(buf), sw);
   printf("\n初始 6041h (SDO) = %s\n", buf);

   /* ---- 门禁 ---- */
   if (cnt > 1 && !g_allow_other)
   {
      printf("\n[拒绝] 总线上有 %d 台从站, 过程数据帧会发给全组。虽然本程序只请求\n"
             "       目标轴的 OP (别的从站留在 PRE_OP, SM2 未使能, 不会动到输出),\n"
             "       但这条边界需要你显式确认: 加 --allow-other-slaves 再跑。\n", cnt);
      ecx_close(&g_ctx);
      return PD_EXIT_REFUSED;
   }

   if (!allow_pdo)
   {
      printf("\n(只读快照到此为止。加 --allow-pdo 才会补映射、进 OP、跑 8 步切换。)\n");
      ecx_close(&g_ctx);
      return PD_EXIT_OK;
   }

   /*
    * 故障门。放这里而不是等到 OP 之后, 是为了保住退出码 4 的"未写任何东西"承诺。
    * --reset-fault 的复位动作要等到进 OP 之后经 PDO 做 —— 故障复位需要 CiA402
    * 状态机在跑, 而 PRE_OP 下它根本没启动 (这正是 sm_state 里那一步无效的原因)。
    */
   if ((sw & SM_SW_FAULT) != 0)
   {
      if (!g_reset_fault)
      {
         printf("\n[拒绝] 驱动器处于故障态 (6041h bit3 = 1), 且未给 --reset-fault。\n"
                "       未写任何东西。故障复位需要 CiA402 状态机在运行 (即 OP 下),\n"
                "       所以本程序会先补映射进 OP, 再经 PDO 推 0x0080 复位。\n");
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }
      printf("\n故障位已置: 进 OP 之后会经 PDO 推 0x%04X (Fault reset) 再回 0x0000。\n",
             (unsigned)SM_CW_FAULT_RST);
   }

   /* ================================================================
    * 1. 补 PDO 映射 (快照 - 改动 - 默认跑完还原)
    * ================================================================ */
   printf("\n---- 补 TxPDO (让 SM3 非零, 否则从站拒绝 SAFE_OP: AL 0x001E) ----\n");

   if (g_no_map)
   {
      pd_map_t m;

      if (pd_read_map(target, PD_OID_TXPDO0, &m) != 0 ||
          pd_map_offset(&m, 0x6041, 0, 16) < 0)
      {
         printf("[拒绝] --no-map 但 1A00h 不含 6041h。要么去掉 --no-map 让本程序补,\n"
                "       要么先用厂商工具把它配好。未写任何东西。\n");
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }
      printf("  --no-map: 1A00h 已含 6041h, 沿用现有映射\n");
   }
   else
   {
      /*
       * 注意顺序: 先快照, 再改。如果先改再快照, 快照记的就是改后的值,
       * "还原"会还原到我们自己改出来的状态 —— 那就不是还原了。
       */
      rc = pd_ensure_tx_assign(target, &s13);
      if (rc == -2)
      {
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }
      if (rc != 0)
      {
         printf("[拒绝] TxPDO 分配无法确认 -> 不改动。\n");
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }

      rc = pd_ensure_tx_map(target, &s1a);
      if (rc == -2)
      {
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }
      if (rc != 0)
      {
         printf("[拒绝] TxPDO 映射无法确认 -> 不改动。\n");
         ecx_close(&g_ctx);
         return PD_EXIT_REFUSED;
      }
   }

   printf("\n---- 补完后的 PDO 现状 ----\n");
   pd_dump_pdo(target);

   /* ================================================================
    * 2. 建过程数据映射
    *
    * manualstatechange = 1: 让 ecx_config_map_group 不要顺手把从站推进 SAFE_OP,
    * 状态阶梯归我们自己管 —— 否则收尾时降不回干净的状态。
    * ================================================================ */
   g_ctx.manualstatechange = 1;

   {
      int size = ecx_config_map_group(&g_ctx, g_iomap, 0);

      if (size <= 0)
      {
         printf("\n[拒绝] ecx_config_map_group 返回 %d (没有有效的 PDO 映射)。\n", size);
         printf("       在写完映射之后仍然如此, 说明 TxPDO 没被接受。\n");
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }
      printf("\n过程数据映射建成: IOmap %d 字节 "
             "(输出 %u + 输入 %u, 邮箱状态 %u)\n",
             size,
             (unsigned)g_ctx.grouplist[0].Obytes,
             (unsigned)g_ctx.grouplist[0].Ibytes,
             (unsigned)g_ctx.grouplist[0].mbxstatuslength);
   }

   g_expected_wkc = (g_ctx.grouplist[0].outputsWKC * 2)
                    + g_ctx.grouplist[0].inputsWKC;
   printf("期望 WKC = outputsWKC(%u)*2 + inputsWKC(%u) = %d\n",
          (unsigned)g_ctx.grouplist[0].outputsWKC,
          (unsigned)g_ctx.grouplist[0].inputsWKC, g_expected_wkc);

   g_out = g_ctx.slavelist[target].outputs;
   g_in = g_ctx.slavelist[target].inputs;

   /*
    * 3. 证出 6040h / 6041h 在过程数据镜像里的字节偏移。
    *
    * 证不出就拒绝: 偏移算错 = 把控制字字节写进一个别的字段, 万一那是 607Ah
    * (目标位置) 就凭空下了一个目标位置。这个"绝不猜"的底线比"能跑起来"重要。
    */
   {
      pd_map_t mo, mi;

      if (pd_read_map(target, PD_OID_RXPDO0, &mo) != 0)
      {
         printf("\n[拒绝] 读不出 1600h 映射, 无法证出 6040h 的偏移 -> 拒绝。\n");
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }
      g_off_cw = pd_map_offset(&mo, 0x6040, 0, 16);
      if (g_off_cw < 0)
      {
         printf("\n[拒绝] 1600h 里没有 16 bit 的 6040h (或它前面有非整字节项),\n"
                "       无法证出控制字在输出镜像里的位置 -> 拒绝。绝不拿猜测的\n"
                "       偏移去写过程数据: 写错字段可能凭空下位指令。\n");
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }

      if (pd_read_map(target, PD_OID_TXPDO0, &mi) != 0)
      {
         printf("\n[拒绝] 读不出 1A00h 映射, 无法证出 6041h 的偏移 -> 拒绝。\n");
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }
      g_off_sw = pd_map_offset(&mi, 0x6041, 0, 16);
      if (g_off_sw < 0)
      {
         printf("\n[拒绝] 1A00h 里没有 16 bit 的 6041h, 无法证出状态字在输入镜像\n"
                "       里的位置 -> 拒绝。\n");
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }

      printf("偏移: 6040h 在输出镜像 +%d 字节, 6041h 在输入镜像 +%d 字节\n",
             g_off_cw, g_off_sw);
      if (g_out == NULL || (uint32_t)g_off_cw + 2 > g_ctx.slavelist[target].Obytes ||
          g_in == NULL || (uint32_t)g_off_sw + 2 > g_ctx.slavelist[target].Ibytes)
      {
         printf("\n[拒绝] 算出的偏移超出本从站的镜像区间 (Obytes=%u, Ibytes=%u)\n"
                "       -> 拒绝。\n",
                (unsigned)g_ctx.slavelist[target].Obytes,
                (unsigned)g_ctx.slavelist[target].Ibytes);
         exit_code = PD_EXIT_REFUSED;
         any_fail = 1;
         goto out;
      }
   }

   /* ================================================================
    * 4. 可选 DC
    * ================================================================ */
   if (g_use_dc)
   {
      printf("\n---- 配置 DC (周期 %dus) ----\n", g_cycle_us);
      if (!ecx_configdc(&g_ctx))
         printf("  [WARN] ecx_configdc 返回假; 继续 (从站可能不支持 DC)\n");
      ecx_dcsync0(&g_ctx, (uint16_t)target, TRUE, (uint32_t)g_cycle_us * 1000u, 0);
      printf("  SYNC0 已在目标轴上启用\n");
   }

   /* ================================================================
    * 5. 状态阶梯: 先喂一轮过程数据 -> SAFE_OP -> (OP)
    * ================================================================ */
   printf("\n---- 进 AL 状态 ----\n");
   (void)pd_cycle();   /* 先进一帧, 把从站的 SM 数据通路带起来 */

   g_raised_state = 1;
   pd_request_state(target, EC_STATE_SAFE_OP);
   rc = pd_wait_state(target, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE / 1000);
   if (rc != 0)
   {
      printf("  目标轴未能进入 SAFE_OP。AL 状态码 0x001E (Invalid input\n"
             "  configuration) 若仍在, 说明 TxPDO 补写没有让 SM3 变有效。\n");
      exit_code = PD_EXIT_NO_OP;
      any_fail = 1;
      goto out;
   }
   printf("  [PASS] 已进入 SAFE_OP (SM3 长度已非零, AL 0x001E 消失)\n");

   if (want_safe_op)
   {
      printf("\n[--state safe-op] 到此为止: 前置条件已通, 未进 OP、未写任何控制字。\n");
      goto out;
   }

   pd_request_state(target, EC_STATE_OPERATIONAL);
   rc = pd_wait_state(target, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 1000);
   if (rc != 0)
   {
      printf("  目标轴未能进入 OP。若 AL 状态码是 0x001B 一类看门狗/同步相关, 试 --dc。\n");
      exit_code = PD_EXIT_NO_OP;
      any_fail = 1;
      goto out;
   }
   g_in_op = 1;
   printf("  [PASS] 已进入 OP (过程数据通路已建立)\n");

   /* ================================================================
    * 6. CiA402 8 步闭环 —— 控制字经 RxPDO 写, 状态字从 TxPDO 读
    * ================================================================ */
   printf("\n---- CiA402 状态机切换 (6040h 走 RxPDO, 6041h 读 TxPDO) ----\n");
   printf("  断言只用手册定义的 6041h 位 0/1/2/3 (掩码 0x%04X);\n", PD_SW_MASK);
   printf("  该驱动器手册未定义 bit6, 所以 \"未使能\" 不区分 Not ready / Switch on disabled。\n");
   printf("  每步末尾用 SDO 回读 6040h 与 6041h 作为独立证据。\n");

   /* 若起始就是故障态且授权了复位, 先经 PDO 推 0x0080 -> 0x0000 */
   if ((sw & SM_SW_FAULT) != 0 && g_reset_fault)
   {
      uint16_t s0 = 0;

      printf("\n故障复位 (经 PDO): 0x%04X -> 0x0000\n", (unsigned)SM_CW_FAULT_RST);
      rc = pd_step(target, "F. Fault reset", SM_CW_FAULT_RST, PD_SW_MASK,
                   0x0000, 0, &s0);
      if (rc == 0)
         rc = pd_step(target, "F. 复位后 Disable voltage", SM_CW_DISABLE_V,
                      SM_SW_FAULT, 0x0000, 0, &s0);
      if (rc == -2)
         goto out;
      if (rc == -3)
      {
         nopdo = 1;
         goto out;
      }
      if (rc != 0)
      {
         printf("[FAIL] 故障未清除, 停止。\n");
         any_fail = 1;
         goto out;
      }
   }

   /* 0. 先归到已知起点, 这样入口状态是什么都不影响后面的判定 */
   rc = pd_step(target, "0. 归位 Disable voltage", SM_CW_DISABLE_V,
                PD_SW_MASK, 0x0000, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

   rc = pd_step(target, "1. Shutdown", SM_CW_SHUTDOWN,
                PD_SW_MASK, SM_SW_RTSO, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

   rc = pd_step(target, "2. Switch on", SM_CW_SWITCHON,
                PD_SW_MASK, SM_SW_RTSO | SM_SW_SWITCHED, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

   /* 第 3 步: 功率级打开, 电机从这里开始带电 */
   rc = pd_step(target, "3. Enable Operation", SM_CW_ENABLE_OP,
                PD_SW_MASK,
                SM_SW_RTSO | SM_SW_SWITCHED | SM_SW_OP_ENABLED, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0)
   {
      printf("      >>> 控制字经 RxPDO 写进去了但状态字没跟上 —— 看上面那行\n"
             "          6040h 回读: 若回读一致, 问题在驱动器侧; 若回读是别的值,\n"
             "          问题在主站侧 (偏移/OP/WKC)。\n");
      any_fail = 1;
      goto out;
   }
   printf("      >>> 电机已通电 (有保持力矩, 但未移动)。\n");

   /* 4. 保持观察: 确认 Operation enabled 是稳定态而不是一闪而过 */
   if (hold_ms > 0)
   {
      uint32_t t0 = sm_now_ms();
      int      faulted = 0;
      int      reads = 0;
      int      bad_wkc = 0;

      printf("  4. 保持观察 %dms ...", hold_ms);
      fflush(stdout);
      while ((int32_t)(sm_now_ms() - t0) < hold_ms)
      {
         int wkc;

         if (g_stop)
         {
            printf(" [中止]\n");
            goto out;
         }
         wkc = pd_cycle();
         if (g_expected_wkc > 0 && wkc < g_expected_wkc)
            bad_wkc++;
         if (pd_rd_sw_pdo(&sw) == 0)
         {
            reads++;
            if ((sw & SM_SW_OP_ENABLED) == 0 || (sw & SM_SW_FAULT) != 0)
            {
               faulted = 1;
               break;
            }
         }
      }
      if (faulted)
      {
         pd_sw_str(buf, sizeof(buf), sw);
         printf(" [FAIL] 保持期间掉出 Operation enabled (TxPDO 6041h=%s)\n", buf);
         any_fail = 1;
         goto out;
      }
      if (reads == 0)
      {
         /* 同 pd_step: 没读到就说没读到, 不拿"没消息"当"稳定的证据" */
         printf(" [FAIL] 保持期间一笔 6041h 都没从 TxPDO 取到, 无法确认这是稳定态\n");
         any_fail = 1;
         goto out;
      }
      if (bad_wkc > PD_WKC_BAD_MAX)
      {
         printf(" [FAIL] 保持期间过程数据 WKC 持续偏短\n");
         nopdo = 1;
         goto out;
      }
      pd_sw_str(buf, sizeof(buf), sw);
      printf(" [PASS] 稳定 (TxPDO 6041h=%s, %d 次读数)\n", buf, reads);
   }

   /* 5-7. 下行: 逐级退回去。每一步同样是独立断言 —— 只测上行的话,
          "能不能干净地停"这件事恰恰是没测到的。 */
   rc = pd_step(target, "5. Disable operation", SM_CW_SWITCHON,
                PD_SW_MASK, SM_SW_RTSO | SM_SW_SWITCHED, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

   rc = pd_step(target, "6. Shutdown", SM_CW_SHUTDOWN,
                PD_SW_MASK, SM_SW_RTSO, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

   rc = pd_step(target, "7. Disable voltage", SM_CW_DISABLE_V,
                PD_SW_MASK, 0x0000, 1, &sw);
   if (rc == -2) goto out;
   if (rc == -3) { nopdo = 1; goto out; }
   if (rc != 0) { any_fail = 1; goto out; }

out:
   /*
    * 收尾无条件执行。走到这里有三类路径: 正常跑完 / 某步 FAIL / Ctrl-C /
    * 前置条件失败。pd_teardown() 自己会在没写过东西时按需跳过, 所以不用在外面判。
    */
   if (pd_teardown(target, &s13, &s1a) != 0)
      exit_code = PD_EXIT_NO_DISABLE;
   else if (g_stop)
   {
      printf("\n[中止] 收到 Ctrl-C, 已按失能方向停机。\n");
      if (exit_code == PD_EXIT_OK)
         exit_code = PD_EXIT_FAIL;
   }
   else if (exit_code == PD_EXIT_OK && any_fail)
      exit_code = nopdo ? PD_EXIT_NO_PDO : PD_EXIT_FAIL;

   /* ---- 汇总 ---- */
   printf("\n---- 汇总 ----\n");
   if (g_cw_sdo_checked > 0)
      printf("  6040h 回读核对: %d/%d 次与经 RxPDO 写入的值一致\n",
             g_cw_sdo_match, g_cw_sdo_checked);
   if (g_sw_pdo_sdo_checked > 0)
      printf("  6041h 双通路对照: %d/%d 次 PDO 与 SDO 读数一致\n",
             g_sw_pdo_sdo_match, g_sw_pdo_sdo_checked);

   if (exit_code == PD_EXIT_OK)
   {
      printf("\n[OK] PDO + OP 下 CiA402 状态机 上行+下行 全部切换 PASS。\n");
      printf("     6040h 经 RxPDO 生效、6041h 经 TxPDO 变化 —— 对照 sm_state 的\n"
             "     PRE_OP + SDO 结果 (6041h 恒 0x0210), 假设成立: 该驱动器确实要求\n"
             "     控制字走过程数据且要进 OP, 纯 SDO 在 PRE_OP 下切不动状态机。\n");
      printf("     (本次只验证 6040h/6041h 的状态迁移, 未验证任何运动功能。)\n");
   }
   else if (exit_code == PD_EXIT_NO_PDO)
      printf("\n[FAIL] 过程数据没有落到从站 (WKC 持续偏短) —— 结论不成立, 不是\n"
             "       状态机的问题, 是主站侧过程数据没通。\n");
   else if (exit_code == PD_EXIT_NO_DISABLE)
      printf("\n[严重] 收尾未能确认失能 —— 见上。\n");
   else if (exit_code == PD_EXIT_FAIL)
   {
      printf("\n[FAIL] 存在未通过的步骤。若每一处 6040h 回读都与写入一致而 6041h\n"
             "       仍不动, 那说明控制字确实到达了驱动器 (PDO 通路是通的), 但\n"
             "       驱动器在 OP 下也不响应它 —— 那问题就不在通路, 而在驱动器配置\n"
             "       或它要求的启动条件上。\n");
   }

   ecx_close(&g_ctx);
   return exit_code;
}
