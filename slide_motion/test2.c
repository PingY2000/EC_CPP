/*
 * test2.c - 最小 PDO 例子 (test1.c 的 PDO 版)
 *
 * 与 test1.c 的关系:
 *   test1.c 名义上叫 "minimal example", 但它把 6040h 经 **SDO** 写下去, 过程数据
 *   那一圈只是空跑 —— 它验证的不是 PDO, 是 SDO。本程序把那条路真正走通: 6040h
 *   写**输出镜像**, 6041h 读**输入镜像**, 每轮都真发真收一帧。
 *
 *   另外 test1.c 调用的 ec_init / ec_config_map / ec_configdc / ec_slave[] 这套扁平
 *   API 在本仓库这份 SOEM 里**不存在** —— 它只暴露 ecx_* 上下文 API。风格照搬, API
 *   换成真实的。
 *
 * 与 sm_pdo.c 的关系:
 *   sm_pdo.c 是这件事的完整版 (1685 行, 带护栏/快照/还原/双通路对照/退出码契约)。
 *   本程序是它的骨架, 只留下"要跑起来必须有的那一部分", 够短到能一眼读完。要看
 *   严谨版看 sm_pdo.c; 想快速确认 PDO 通路通不通看这个。
 *
 * ---- 两个绕不开的前置条件 (都是实测出来的, 不是猜的) ----
 *   1. 出厂默认 TxPDO (1A00h) 是**空的** -> SM3 长度为 0 -> 从站以 AL 状态码
 *      0x001E (Invalid input configuration) 拒绝 SAFE_OP。进不去 SAFE_OP 就没有
 *      OP, 没有 OP 就轮不到 CiA402 状态机响应 6040h。所以必须先补 TxPDO。
 *      必须在 PRE_OP 下做, 且必须在 ecx_config_map_group **之前** —— 映射是配置
 *      时算好的, 组完再改就晚了。
 *   2. 出厂 1600h 只映射了 6040h (16 bit) -> 控制字在输出镜像里的偏移是 0。这个
 *      偏移本程序是**从映射表算出来的**, 不是写死的: 偏移算错 = 把控制字字节写进
 *      一个别的字段, 万一那是 607Ah (目标位置) 就凭空下了一个目标位置。
 *
 * ---- 安全边界 ----
 *   - 会写 PDO 映射对象 1C13h/1A00h。只写 RAM, **从不写 2102h** (EEPROM), 掉电即
 *     回出厂值。所以本程序跑完不会还原 —— 想还原就重新上电。要跑完自动还原用
 *     sm_pdo.c。
 *   - 6040h 只经过程数据镜像写, 推送过的值只有 0x0000 / 0x0006 / 0x0007 / 0x000F。
 *     不发任何运动指令, 滑台不会移动。
 *   - 但**第 3 步之后电机会通电** (有保持力矩)。真跑时人在设备旁, 手放在物理急停上。
 *   - 只请求选中那一根轴的 OP, 别的从站留在 PRE_OP。
 */

/*
 * 头文件顺序是硬的, 不是风格: 必须 soem.h 在前, <windows.h> 在后。
 * soem.h -> nicdrv.h -> wpcap/pcap-stdinc.h 会把 winsock2.h 拉进来 (而且它先
 * #undef _WINSOCKAPI_ 再 include, 防的就是别人先拉老 winsock.h)。windows.h 若先进
 * 来, 它自己会 include <winsock.h> 并定义 _WINSOCKAPI_, 于是 winsock.h 和 winsock2.h
 * 落在同一个编译单元里 -> sockaddr/ip_mreq 重定义 (MSVC: error C2011)。
 * sm_bus.c / slide_motion.c 也是这个顺序。
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "soem/soem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* ======================================================================
 * 常量
 * ====================================================================== */

/* 网卡写死 (与 sm_pdo.c / sm_state.c 同一条)。换机器改这一行, 或用 argv[1] 覆盖。 */
#define IFNAME "\\Device\\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"

/* CiA402 对象 */
#define OID_CONTROLWORD 0x6040
#define OID_STATUSWORD  0x6041

/* PDO 配置对象 (CiA301 通信区) */
#define OID_RXPDO_ASSIGN 0x1C12
#define OID_TXPDO_ASSIGN 0x1C13
#define OID_RXPDO0       0x1600
#define OID_TXPDO0       0x1A00

/* 映射项编码: index<<16 | sub<<8 | 位宽 */
#define MAP_ENTRY(index, sub, bits) \
   (((uint32)(index) << 16) | ((uint32)(sub) << 8) | (uint32)(bits))

/* 控制字 (6040h) */
#define CW_DISABLE_V  0x0000
#define CW_SHUTDOWN   0x0006
#define CW_SWITCHON   0x0007
#define CW_ENABLE_OP  0x000F

/*
 * 状态字 (6041h) 低 4 位在 CiA402 里是**非标准编码** 0000/0001/0011/0111, 所以
 * 全程用位判断, 禁止数值比较。本驱动器手册只定义了位 0/1/2/3/10/12, 没定义 bit6,
 * 所以位 0-3 全 0 时无法区分 Not ready 与 Switch on disabled —— 不替它猜。
 */
#define SW_RTSO     0x0001
#define SW_SWITCHED 0x0002
#define SW_OP_EN    0x0004
#define SW_FAULT    0x0008
#define SW_MASK     (SW_RTSO | SW_SWITCHED | SW_OP_EN | SW_FAULT)

/* DC 同步周期 (µs), 手册给的 250~4000 范围内 */
#define DC_CYCLE_US 1000

/* 等待与节拍 */
#define STEP_TMO_MS 1000  /* 单步等状态字变化的超时 */
#define HOLD_MS     500   /* 第 3 步之后保持观察多久 */
#define POLL_MS     5     /* 状态字轮询节拍里的让步 (给从站喘口气) */
#define OP_TMO_MS   3000  /* 进 OP 的超时 */
#define IOMAP_MAX   4096

/* 映射对象最多几项 (本驱动器只有 1~2 项, 留余量) */
#define MAP_MAX 8

/* 退出码 (照 sm_pdo.c 的口径, 只留够用的几个) */
#define EXIT_OK        0
#define EXIT_USAGE     1
#define EXIT_NO_SLAVE  2
#define EXIT_FAIL      3
#define EXIT_REFUSED   4
#define EXIT_NO_OP     5
#define EXIT_NO_DISABLE 10

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* Ctrl-C 标志。信号处理器只置这一位, 不做任何 I/O (信号上下文里发 SDO 是未定义行为) */
static volatile sig_atomic_t g_stop = 0;

static void on_ctrl_c(int sig)
{
   (void)sig;
   g_stop = 1;
}

/*
 * SOEM 上下文。放在文件作用域而不是栈上 —— 它内嵌 slavelist[EC_MAXSLAVE] 等大数组,
 * 几百 KB, 栈上放不下 (与 sm_bus.c 的 g_ctx 同一个理由)。
 */
static ecx_contextt g_ctx;
static uint8        g_iomap[IOMAP_MAX];

static uint8 *g_out = NULL;      /* 目标轴输出镜像基址 */
static uint8 *g_in  = NULL;      /* 目标轴输入镜像基址 */
static int    g_off_cw = -1;     /* 6040h 在输出镜像里的字节偏移 */
static int    g_off_sw = -1;     /* 6041h 在输入镜像里的字节偏移 */
static int    g_expected_wkc = 0;
static int    g_wrote = 0;       /* 是否写过 6040h */
static int    g_use_dc = 0;      /* --dc: 配置分布式时钟 */

/* ======================================================================
 * 时钟与睡眠
 * ====================================================================== */

static uint32 now_ms(void)
{
#ifdef _WIN32
   return (uint32)GetTickCount64();
#else
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void msleep(int ms)
{
#ifdef _WIN32
   Sleep((DWORD)ms);
#else
   usleep((useconds_t)ms * 1000);
#endif
}

/* ======================================================================
 * SDO 读 / 写
 *
 * 读之前必须清 ctx.ecaterror: 它是粘滞位, SOEM 报错后不会自己清零, 上一次的失败
 * 会污染下一次的判定。
 * ====================================================================== */

/*
 * 读一个对象 (≤4 字节)。返回 0 = 成功, *size 给出驱动器自报的宽度。
 *
 * psize 是**传入传出**: 必须先告诉 SOEM 缓冲有多大, 它才回填实际宽度; 传 0 进去
 * 这次读直接失败 (sm_bus.c 的 sm_rd_raw 里同一个坑, 那里写的是 "给足 4 字节, 让
 * 驱动器自报实际宽度")。
 *
 * 所以这里固定按 4 字节读进**内部**缓冲, 再按自报宽度拷给调用者: 调用者的变量
 * 不必有 4 字节, 也不会被写爆 —— 直接传它进来就等着越界写。
 */
static int sdo_read(int slave, uint16 index, uint8 sub, void *p, int *size)
{
   uint8 buf[4];
   int   psize = 4;

   memset(buf, 0, sizeof(buf));
   g_ctx.ecaterror = FALSE;     /* 粘滞位, 每次读前必须清 */
   if (ecx_SDOread(&g_ctx, (uint16)slave, index, sub, FALSE, &psize, buf,
                   EC_TIMEOUTRXM) <= 0 || g_ctx.ecaterror)
      return -1;
   if (psize < 0 || psize > 4)
      return -1;
   if (size != NULL)
      *size = psize;
   if (p != NULL && psize > 0)
      memcpy(p, buf, (size_t)psize);
   return 0;
}

/*
 * 读 U16 (读取失败时由调用者自己打诊断行的那条路径用)。
 * 宽度必须正好 2: 宽度不足时解出来的数会在高位补零, 看起来像一个完全正常的值 ——
 * 那是"我们不知道"冒充"读到了"。
 */
static int sdo_read_u16(int slave, uint16 index, uint8 sub, uint16 *v)
{
   uint8 buf[4];
   int   size = 0;

   if (sdo_read(slave, index, sub, buf, &size) != 0 || size != 2)
      return -1;
   memcpy(v, buf, 2);
   return 0;
}

/*
 * 上面几个包装把"没读到"和"读到了但宽度不对"合并成一个 -1 —— 对调用者够用, 对
 * 现场排查不够用: 真机上只看到一句 "读 1A00h 失败" 是没法往下走的。所以映射表这
 * 条路径不走包装, 自己读, 把驱动器自报的宽度打出来。
 */
static int sdo_read_checked(int slave, uint16 index, uint8 sub, int want_size,
                            void *p)
{
   uint8 buf[4];
   int   size = 0;

   if (sdo_read(slave, index, sub, buf, &size) != 0)
   {
      printf("  读 %04Xh:%02X 失败 (无响应 / SDO abort)\n",
             (unsigned)index, (unsigned)sub);
      return -1;
   }
   if (size != want_size)
   {
      printf("  %04Xh:%02X 自报宽度 %d 字节, 期望 %d -> 不猜\n",
             (unsigned)index, (unsigned)sub, size, want_size);
      return -1;
   }
   memcpy(p, buf, (size_t)size);
   return 0;
}

/*
 * 写后回读。这份 SOEM 在加急路径 (psize<=4) 上把从站回的 SDO abort 帧当成写成功
 * (abort 帧的 mbxtype/service/index/subindex 与请求完全一致, 命中 "all OK" 分支,
 * 既不压错误栈也不置 ecaterror, wkc 还 > 0)。映射对象全是 1/2/4 字节的加急写, 全在
 * 这条路径上 —— 所以 wkc > 0 不代表写进去了, **回读才是唯一的确认**。
 */
static int sdo_write(int slave, uint16 index, uint8 sub, int size, const void *p)
{
   uint8 back[4];
   int   bsize = 0;

   /* 回读缓冲只有 4 字节, 所以这里不接受更宽的写。放在写之前判: 放在写之后,
      "没写"和"写了但不确认"就分不开了。 */
   if (size < 1 || size > 4)
      return -1;

   g_ctx.ecaterror = FALSE;
   if (ecx_SDOwrite(&g_ctx, (uint16)slave, index, sub, FALSE, size, p,
                    EC_TIMEOUTRXM) <= 0 || g_ctx.ecaterror)
   {
      printf("    写 %04Xh:%02X 未确认\n", (unsigned)index, (unsigned)sub);
      return -1;
   }
   if (sdo_read(slave, index, sub, back, &bsize) != 0 || bsize != size ||
       memcmp(back, p, (size_t)size) != 0)
   {
      printf("    写 %04Xh:%02X 后回读不一致, 写没进去\n",
             (unsigned)index, (unsigned)sub);
      return -1;
   }
   return 0;
}

/* ======================================================================
 * 映射表: 读 / 求偏移
 * ====================================================================== */

/* 读一个 PDO 映射对象 (index 的 :00 是项数, :01.. 是项)。返回 0 = 成功 */
static int map_read(int slave, uint16 index, uint32 *e, int *n)
{
   uint8 cnt = 0;
   int   i;

   *n = 0;
   if (sdo_read_checked(slave, index, 0, 1, &cnt) != 0)
      return -1;
   if (cnt > MAP_MAX)
   {
      printf("  %04Xh:00 = %d 项, 超出本程序上限 %d -> 拒绝\n",
             (unsigned)index, (int)cnt, MAP_MAX);
      return -1;
   }
   for (i = 1; i <= (int)cnt; i++)
   {
      if (sdo_read_checked(slave, index, (uint8)i, 4, &e[i - 1]) != 0)
         return -1;
   }
   *n = (int)cnt;
   return 0;
}

/*
 * 求 (index, sub) 在映射里的**字节偏移**。找不到 / 位宽不符 / 前面累计位数不是
 * 8 的整数倍 -> -1。
 *
 * 为什么死守这条: 偏移算错 = 把控制字字节写进一个别的字段。万一那个字段是 607Ah
 * (目标位置), 我们就在毫不知情的情况下下了一个目标位置。所以证不出偏移必须拒绝,
 * 不允许"大概是 0 吧"。
 */
static int map_offset(const uint32 *e, int n, uint16 index, uint8 sub,
                      int want_bits)
{
   int bit = 0;
   int i;

   for (i = 0; i < n; i++)
   {
      uint16 ei = (uint16)((e[i] >> 16) & 0xFFFFu);
      uint8  es = (uint8)((e[i] >> 8) & 0xFFu);
      int    eb = (int)(e[i] & 0xFFu);

      if (eb <= 0)
         return -1;                 /* 位宽 0 的项推不动偏移, 整张表不可解释 */
      if (ei == index && es == sub)
      {
         if (eb != want_bits)
            return -1;
         if ((bit % 8) != 0)
            return -1;              /* 位对齐的字段不能按字节读写 */
         return bit / 8;
      }
      bit += eb;
   }
   return -1;
}

/* ======================================================================
 * 补 TxPDO —— 让 SM3 非零, 否则从站拒绝 SAFE_OP (AL 0x001E)
 *
 * 只往 1A00h **追加**一项 6041h (16 bit), 已有的项原样保留。必须在 PRE_OP 下、
 * 在 ecx_config_map_group 之前做。返回 0 = 完成 (可能没改动)
 * ====================================================================== */
static int fixup_txpdo(int slave)
{
   uint32 e[MAP_MAX];
   uint32 assign;
   uint16 first = 0;
   uint8  acnt = 0, zero = 0, one = 1, nn;
   int    n, i, off;

   if (map_read(slave, OID_TXPDO0, e, &n) != 0)
   {
      printf("  读 1A00h 失败 -> 拒绝\n");
      return -1;
   }

   off = map_offset(e, n, OID_STATUSWORD, 0, 16);
   if (off >= 0)
   {
      printf("  1A00h 已含 6041h (输入镜像 +%d 字节), 不动它\n", off);
      return 0;
   }

   /*
    * 1C13h 已经分配了**别的东西**就拒绝: 要把它换掉就必须替驱动器决定"哪个 TxPDO
    * 是可有可无的" —— 那是它的配置, 不是我们的。已经分配 1A00h 则可以继续。
    */
   if (sdo_read_checked(slave, OID_TXPDO_ASSIGN, 0, 1, &acnt) != 0)
   {
      printf("  读 1C13h 失败 -> 拒绝\n");
      return -1;
   }
   if (acnt > 0)
   {
      if (sdo_read_checked(slave, OID_TXPDO_ASSIGN, 1, 2, &first) != 0)
         return -1;
      if (first != (uint16)OID_TXPDO0)
      {
         printf("  1C13h 已分配了别的东西 (首项 0x%04X), 不是 1A00h -> 拒绝\n",
                (unsigned)first);
         return -1;
      }
   }

   /* 追加前先证一遍: 追加后整张表必须仍能按字节解释出 6041h 的偏移 */
   if (n >= MAP_MAX)
   {
      printf("  1A00h 已有 %d 项, 加不下 6041h -> 拒绝\n", n);
      return -1;
   }
   e[n] = MAP_ENTRY(OID_STATUSWORD, 0, 16);
   off = map_offset(e, n + 1, OID_STATUSWORD, 0, 16);
   if (off < 0)
   {
      printf("  追加 6041h 之后映射表不再是字节可解释的 (前面有非整字节项)\n"
             "  -> 拒绝, 不猜偏移\n");
      return -1;
   }
   printf("  1A00h 追加 6041h: 项数 %d -> %d, 6041h 将位于输入镜像 +%d 字节\n",
          n, n + 1, off);

   /* 1C13h = { 1A00h }。顺序是规范要求的: 计数归零 -> 写项 -> 计数回值 */
   assign = (uint32)OID_TXPDO0;
   if (sdo_write(slave, OID_TXPDO_ASSIGN, 0, 1, &zero) != 0) return -1;
   if (sdo_write(slave, OID_TXPDO_ASSIGN, 1, 2, &assign) != 0) return -1;
   if (sdo_write(slave, OID_TXPDO_ASSIGN, 0, 1, &one) != 0) return -1;

   /* 1A00h: 计数归零 -> 逐项回写 -> 计数回项数 */
   if (sdo_write(slave, OID_TXPDO0, 0, 1, &zero) != 0)
      return -1;
   for (i = 0; i < n + 1; i++)
   {
      if (sdo_write(slave, OID_TXPDO0, (uint8)(i + 1), 4, &e[i]) != 0)
         return -1;
   }
   nn = (uint8)(n + 1);
   if (sdo_write(slave, OID_TXPDO0, 0, 1, &nn) != 0)
      return -1;

   printf("  1A00h / 1C13h 已写好 (只写 RAM, 掉电回出厂值)\n");
   return 0;
}

/* ======================================================================
 * 过程数据
 * ====================================================================== */

static int cycle(void)
{
   ecx_send_processdata(&g_ctx);
   return ecx_receive_processdata(&g_ctx, EC_TIMEOUTRET);
}

/*
 * 写 6040h 到输出镜像。必须按小端字节写: SOEM 把 IOmap 原样塞进 EtherCAT 帧, 而
 * 线上是小端。本工程只跑 Windows/x86, memcpy 一个 uint16 就是小端表示。用 memcpy
 * 而不是强制转换, 是为了不依赖 g_off_cw 的对齐。
 */
static void set_cw(uint16 cw)
{
   if (g_out == NULL || g_off_cw < 0)
      return;
   memcpy(g_out + g_off_cw, &cw, 2);
   g_wrote = 1;
}

static uint16 get_sw(void)
{
   uint16 sw = 0;

   if (g_in != NULL && g_off_sw >= 0)
      memcpy(&sw, g_in + g_off_sw, 2);
   return sw;
}

/* 6041h 状态名 —— 只按手册定义的低 4 位判读, 不替驱动器猜 bit6 */
static const char *sw_name(uint16 sw)
{
   if ((sw & SW_FAULT) != 0)
      return (sw & SW_OP_EN) ? "Fault reaction active" : "Fault";
   if ((sw & SW_OP_EN) != 0)
      return "Operation enabled";
   if ((sw & SW_SWITCHED) != 0)
      return "Switched on";
   if ((sw & SW_RTSO) != 0)
      return "Ready to switch on";
   return "未使能 (位0-3 全 0)";
}

static const char *state_str(uint16 st)
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

/* ======================================================================
 * AL 状态阶梯
 * ====================================================================== */

static void print_adapters(void)
{
   ec_adaptert *head = ec_find_adapters();
   ec_adaptert *a;

   for (a = head; a != NULL; a = a->next)
      printf("    - %s  (%s)\n", a->name, a->desc);
   ec_free_adapters(head);
}

/*
 * 请求并等待目标轴进入 want。每轮都打过程数据再查状态 —— 状态迁移期间过程数据
 * 不能断, 否则若驱动器的 SM 看门狗是开着的, 会反过来把我们从迁移里踢出来。
 * 返回 0 = 到达 / -1 = 失败 (已打印原因)
 */
static int goto_state(int slave, uint16 want, int tmo_ms)
{
   uint32 t0 = now_ms();

   /*
    * 先取一次目标轴当前的原始 AL 状态字 (timeout=0 -> 一次 FPRD 就返回)。不这么做的话
    * 下面那个 ACK 判断读的是 slavelist[] 里的陈值 —— 它可能是上一次调用留下的。
    */
   (void)ecx_statecheck(&g_ctx, (uint16)slave, (uint16)want, 0);

   /*
    * 若从站当前处于 AL 错误态 (状态字 bit4), 必须把 ACK 一起写进去才能清掉它 ——
    * ecx_writestate() 只把 slavelist[].state 原样写下去, 不会自动置 ACK。
    */
   if ((g_ctx.slavelist[slave].state & EC_STATE_ERROR) != 0)
      g_ctx.slavelist[slave].state = (uint16)(want | EC_STATE_ACK);
   else
      g_ctx.slavelist[slave].state = (uint16)want;
   ecx_writestate(&g_ctx, (uint16)slave);

   for (;;)
   {
      struct ec_slave *s = &g_ctx.slavelist[slave];

      if (g_stop)
         return -1;

      (void)cycle();

      if (ecx_statecheck(&g_ctx, (uint16)slave, want, 1000) == want)
         return 0;

      /*
       * ecx_statecheck() 把状态按 0x000F 掩过, 错误位 (0x10) 在它的返回值里看不见,
       * 必须单独查 slavelist[].state。这正是文档里记的 sm_state 那个"进 SAFE_OP 失败
       * 却只打印一行 0x12"的坑。
       */
      if ((s->state & EC_STATE_ERROR) != 0)
      {
         printf("  [FAIL] 请求 %s 被拒绝: AL 状态 0x%02X (含错误位), "
                "AL 状态码 0x%04X %s\n",
                state_str(want), (unsigned)s->state, (unsigned)s->ALstatuscode,
                ec_ALstatuscode2string(s->ALstatuscode));
         return -1;
      }
      if ((int32)(now_ms() - t0) >= (int32)tmo_ms)
      {
         printf("  [FAIL] %dms 内未进入 %s (当前 AL 状态 0x%02X, "
                "AL 状态码 0x%04X %s)\n",
                tmo_ms, state_str(want), (unsigned)s->state,
                (unsigned)s->ALstatuscode,
                ec_ALstatuscode2string(s->ALstatuscode));
         return -1;
      }
   }
}

/* ======================================================================
 * 一步: 写控制字到输出镜像 -> 等输入镜像里的状态字满足断言
 * 返回 0 = PASS / -1 = FAIL
 * ====================================================================== */
static int step(int slave, const char *name, uint16 cw, uint16 expect)
{
   uint32 t0 = now_ms();
   uint16 sw = 0;
   int    reads = 0;
   int    got = 0;

   printf("  %-26s 写 RxPDO[6040h]=0x%04X ...", name, (unsigned)cw);
   fflush(stdout);

   set_cw(cw);

   while ((int32)(now_ms() - t0) < STEP_TMO_MS)
   {
      int wkc;

      if (g_stop)
      {
         printf(" [中止]\n");
         return -1;
      }

      wkc = cycle();
      if (g_expected_wkc > 0 && wkc < g_expected_wkc)
      {
         printf(" [FAIL] 过程数据 WKC=%d < 期望 %d (从站可能不在 OP / 掉线)\n",
                wkc, g_expected_wkc);
         return -1;
      }

      sw = get_sw();
      reads++;
      if ((sw & SW_MASK) == expect)
      {
         got = 1;
         break;
      }
      msleep(POLL_MS);
   }

   if (!got)
   {
      struct ec_slave *s = &g_ctx.slavelist[slave];
      uint16 back = 0;

      printf(" [FAIL] 断言未成立\n");
      if (reads == 0)
      {
         /*
          * 一笔都没取到。不能拿零初始化的 sw 冒充"实测 0x0000" —— 0x0000 在 CiA402
          * 里是合法的 "未使能", 那会把"我们不知道"说成"驱动器报了个状态"。
          */
         printf("       一笔 6041h 都没从 TxPDO 取到: 状态未知\n");
      }
      else
      {
         printf("       期望 6041h & 0x%04X == 0x%04X, 实测 0x%04X (%s)\n",
                (unsigned)SW_MASK, (unsigned)expect, (unsigned)sw, sw_name(sw));
      }
      printf("       AL 状态=0x%02X, AL 状态码=0x%04X %s\n",
             (unsigned)s->state, (unsigned)s->ALstatuscode,
             ec_ALstatuscode2string(s->ALstatuscode));

      /*
       * 用 SDO 回读 6040h。这一行把两种失败分开, 而它们的修法完全不同:
       *   回读一致 -> 控制字到了驱动器, 驱动器不受理 (问题在驱动器侧);
       *   回读是别的值 -> 过程数据根本没落到控制字对象上 (问题在主站侧:
       *                    偏移算错 / 从站不在 OP / WKC 短)。
       */
      if (sdo_read_u16(slave, OID_CONTROLWORD, 0, &back) == 0)
         printf("       6040h 回读 = 0x%04X (经 RxPDO 写的是 0x%04X) %s\n",
                (unsigned)back, (unsigned)cw,
                (back == cw) ? "一致 -> 控制字确实到了驱动器"
                             : "<<< 不一致 -> 过程数据没落到控制字对象上");
      else
         printf("       6040h 回读失败 (驱动器不允许读控制字?)\n");
      return -1;
   }

   printf(" [PASS] TxPDO 6041h=0x%04X (%s)\n", (unsigned)sw, sw_name(sw));
   return 0;
}

/* ======================================================================
 * 收尾 —— 无条件执行, 覆盖所有退出路径
 * 返回 0 = 已确认失能 / 1 = 未能确认失能 (电机可能仍带电)
 * ====================================================================== */
static int teardown(int slave, int in_op)
{
   uint16 sw = 0;
   int    i;
   int    unconfirmed = 0;

   printf("\n---- 收尾 ----\n");

   /* 1. 只要动过控制字, 就写 0x0000 并继续打一段过程数据 —— 让驱动器真的收到它 */
   if (g_wrote && g_out != NULL && g_off_cw >= 0)
   {
      printf("  写 RxPDO[6040h]=0x0000 (Disable voltage), 继续打 250ms 过程数据\n");
      set_cw(CW_DISABLE_V);
      for (i = 0; i < 50; i++)
      {
         (void)cycle();
         msleep(5);
      }
   }

   /* 2. 到过 OP 的话, 用 TxPDO 的 6041h 确认 bit2 已清 (这是"电机不带电"的正面证据) */
   if (in_op && g_in != NULL && g_off_sw >= 0)
   {
      int reads = 0;

      for (i = 0; i < 200; i++)
      {
         (void)cycle();
         sw = get_sw();
         reads++;
         if ((sw & SW_OP_EN) == 0)
            break;
         msleep(5);
      }
      if (reads == 0 || (sw & SW_OP_EN) != 0)
      {
         printf("  [FAIL] 写了 0x0000 但 6041h 仍报 Operation enabled "
                "(0x%04X) —— 失能无法确认\n", (unsigned)sw);
         unconfirmed = 1;
      }
      else
      {
         printf("  [PASS] 已确认失能 (6041h=0x%04X %s)\n",
                (unsigned)sw, sw_name(sw));
      }
   }

   /* 3. 降回 PRE_OP, 再关掉网卡 */
   printf("  降回 PRE_OP ...\n");
   if (goto_state(slave, EC_STATE_PRE_OP, 1000) != 0)
      printf("  [WARN] 未能确认回到 PRE_OP\n");

   if (unconfirmed)
   {
      printf("  >>> 电机可能仍然带电。立即断开驱动器供电, 不要用手去推滑台。\n");
      return 1;
   }
   return 0;
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char *argv[])
{
   const char *ifname = IFNAME;
   int   cnt, target = -1, i, rc;
   int   exit_code = EXIT_OK;
   int   in_op = 0;
   uint16 sw;
   uint32 e[MAP_MAX];
   int    n;

   /*
    * 源码是 UTF-8, 但 Windows 控制台默认是 GBK 代码页, 不设这一句中文全是乱码
    * (与 sm_bus.c 的 sm_console_utf8() 同一件事; 本程序不链接 sm_bus.c, 所以自己来)。
    */
#ifdef _WIN32
   SetConsoleOutputCP(CP_UTF8);
#endif

   signal(SIGINT, on_ctrl_c);

   printf("test2 - 最小 PDO 例子 (6040h 走 RxPDO, 6041h 读 TxPDO)\n");

   /* 参数只有两个, 不值得写参数解析器: --dc / --help / 裸网卡名 */
   for (i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--dc") == 0)
      {
         g_use_dc = 1;
      }
      else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
      {
         printf("用法: %s [网卡名] [--dc]\n"
                "  网卡名缺省时用代码里写死的 IFNAME; ecx_init 失败会把可用网卡列出来。\n"
                "  --dc  配置分布式时钟 (进 OP 失败且 AL 状态码是 0x001B 一类同步/看门狗\n"
                "        相关时再试它; 默认不碰)。\n"
                "  本程序会写 PDO 映射 1A00h/1C13h (仅 RAM, 掉电回出厂值) 并让电机\n"
                "  在第 3 步后带电。真跑时人在设备旁, 手放在物理急停上。\n",
                argv[0]);
         return EXIT_OK;
      }
      else if (argv[i][0] != '-')
      {
         ifname = argv[i];
      }
      else
      {
         printf("未知参数: %s (试 --help)\n", argv[i]);
         return EXIT_USAGE;
      }
   }
   printf("网卡: %s\n", ifname);

   /* ---- 1. 开总线 + 扫描从站。到这一步为止没写过任何东西 ---- */
   if (!ecx_init(&g_ctx, ifname))
   {
      printf("无法打开网卡 (被独占 / 需管理员 / Npcap 未装)。可用网卡:\n");
      print_adapters();
      return EXIT_USAGE;
   }
   cnt = ecx_config_init(&g_ctx);
   if (cnt <= 0)
   {
      printf("总线上未发现从站 (config_init=%d), 检查网线/供电。\n", cnt);
      ecx_close(&g_ctx);
      return EXIT_NO_SLAVE;
   }
   printf("%d 台从站已配置。\n", cnt);

   ecx_statecheck(&g_ctx, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   ecx_readstate(&g_ctx);

   for (i = 1; i <= cnt; i++)
   {
      struct ec_slave *s = &g_ctx.slavelist[i];

      printf("  从站 %d: AL=0x%02X %-8s 厂商=0x%08X 产品码=0x%08X "
             "输出 %u 字节 / 输入 %u 字节\n",
             i, (unsigned)s->state, state_str(s->state),
             (unsigned)s->eep_man, (unsigned)s->eep_id,
             (unsigned)s->Obytes, (unsigned)s->Ibytes);
   }

   /* 目标 = 第一台从站。多发一台就拒绝, 免得过程数据帧里的偏移跟它对不上 */
   if (cnt > 1)
   {
      printf("\n[拒绝] 总线上有 %d 台从站。本程序只按单台算偏移, 多发一台就得重算\n"
             "       (而且别的从站也可能收到过程数据帧)。先用单台跑, 未写任何东西。\n",
             cnt);
      ecx_close(&g_ctx);
      return EXIT_REFUSED;
   }
   target = 1;

   /*
    * 单独读一次目标轴的状态, 不能用上面那次广播的结果:
    *   - ecx_statecheck(slave=0) 走 BRD, 只把结果填进 slavelist[0], 不填单个从站;
    *   - ecx_readstate() 在"所有从站状态一致且无错误位"时会提前返回, 同样不填单个
    *     从站的 state。
    * slave>=1 的 ecx_statecheck 走 FPRD, 把原始 AL 状态字 (连错误位 0x10 一起)
    * 写回 slavelist[target].state, 这才是能判的读数。
    */
   (void)ecx_statecheck(&g_ctx, (uint16)target, EC_STATE_PRE_OP, 1000);
   if (g_ctx.slavelist[target].state != EC_STATE_PRE_OP)
   {
      printf("\n[拒绝] 从站 %d 不在干净的 PRE_OP: AL 状态 0x%02X, AL 状态码 0x%04X %s。\n"
             "       这种状态下映射对象不可写, 未写任何东西。\n",
             target, (unsigned)g_ctx.slavelist[target].state,
             (unsigned)g_ctx.slavelist[target].ALstatuscode,
             ec_ALstatuscode2string(g_ctx.slavelist[target].ALstatuscode));
      if ((g_ctx.slavelist[target].state & EC_STATE_ERROR) != 0)
         printf("       状态字带错误位 (0x10): 先清掉它再跑 —— 重新上电, 或用\n"
                "       sm_pdo --reset-fault。\n");
      ecx_close(&g_ctx);
      return EXIT_NO_SLAVE;
   }

   /* ---- 2. 补 TxPDO (必须在 PRE_OP 下, 且必须在 config_map 之前) ---- */
   printf("\n---- 补 TxPDO (让 SM3 非零, 否则从站拒绝 SAFE_OP: AL 0x001E) ----\n");
   if (fixup_txpdo(target) != 0)
   {
      printf("[拒绝] TxPDO 无法补成合法的 -> 不改动。\n");
      ecx_close(&g_ctx);
      return EXIT_REFUSED;
   }

   /* ---- 3. 建过程数据映射 ---- */
   {
      int size;

      size = ecx_config_map_group(&g_ctx, g_iomap, 0);
      if (size <= 0)
      {
         printf("\n[拒绝] ecx_config_map_group 返回 %d (没有有效的 PDO 映射)。\n",
                size);
         ecx_close(&g_ctx);
         return EXIT_REFUSED;
      }
      printf("\n过程数据映射建成: IOmap %d 字节 (输出 %u + 输入 %u)\n",
             size, (unsigned)g_ctx.grouplist[0].Obytes,
             (unsigned)g_ctx.grouplist[0].Ibytes);
   }

   /* ecx_config_map_group 已经把从站推到 SAFE_OP (manualstatechange 默认 0) */
   printf("\n等待从站进入 SAFE_OP ...\n");
   if (goto_state(target, EC_STATE_SAFE_OP, OP_TMO_MS) != 0)
   {
      printf("  若 AL 状态码仍是 0x001E (Invalid input configuration), 说明 TxPDO\n"
             "  补写没让 SM3 变有效。\n");
      exit_code = EXIT_NO_OP;
      goto out;
   }
   printf("  [PASS] 已进入 SAFE_OP\n");

   /* ---- 4. 从映射表算偏移, 绝不猜 ---- */
   g_out = g_ctx.slavelist[target].outputs;
   g_in  = g_ctx.slavelist[target].inputs;

   if (map_read(target, OID_RXPDO0, e, &n) != 0 ||
       (g_off_cw = map_offset(e, n, OID_CONTROLWORD, 0, 16)) < 0)
   {
      printf("\n[拒绝] 1600h 读不出 / 或里面没有 16 bit 的 6040h, 无法证出控制字在\n"
             "       输出镜像里的位置 -> 拒绝。绝不拿猜测的偏移去写过程数据:\n"
             "       写错字段可能凭空下位指令。\n");
      exit_code = EXIT_REFUSED;
      goto out;
   }
   if (map_read(target, OID_TXPDO0, e, &n) != 0 ||
       (g_off_sw = map_offset(e, n, OID_STATUSWORD, 0, 16)) < 0)
   {
      printf("\n[拒绝] 1A00h 读不出 / 或里面没有 16 bit 的 6041h -> 拒绝。\n");
      exit_code = EXIT_REFUSED;
      goto out;
   }
   if ((uint32)g_off_cw + 2 > g_ctx.slavelist[target].Obytes ||
       (uint32)g_off_sw + 2 > g_ctx.slavelist[target].Ibytes)
   {
      printf("\n[拒绝] 算出的偏移超出本从站的镜像区间 (Obytes=%u, Ibytes=%u) -> 拒绝。\n",
             (unsigned)g_ctx.slavelist[target].Obytes,
             (unsigned)g_ctx.slavelist[target].Ibytes);
      exit_code = EXIT_REFUSED;
      goto out;
   }
   printf("偏移: 6040h 在输出镜像 +%d 字节, 6041h 在输入镜像 +%d 字节\n",
          g_off_cw, g_off_sw);

   g_expected_wkc = (g_ctx.grouplist[0].outputsWKC * 2)
                    + g_ctx.grouplist[0].inputsWKC;
   printf("期望 WKC = outputsWKC(%u)*2 + inputsWKC(%u) = %d\n",
          (unsigned)g_ctx.grouplist[0].outputsWKC,
          (unsigned)g_ctx.grouplist[0].inputsWKC, g_expected_wkc);

   /*
    * 可选 DC。默认不碰: 自由运行下能进 OP 就别动它。若进 OP 失败且 AL 状态码是
    * 0x001B 一类同步/看门狗相关, 加 --dc 再来 —— 光 ecx_configdc 不够, 还得
    * ecx_dcsync0 把 SYNC0 真的打开。
    */
   if (g_use_dc)
   {
      printf("\n---- 配置 DC (周期 %dus) ----\n", DC_CYCLE_US);
      ecx_configdc(&g_ctx);
      ecx_dcsync0(&g_ctx, (uint16)target, TRUE,
                  (uint32)DC_CYCLE_US * 1000u, 0);
      printf("  SYNC0 已在目标轴上启用\n");
   }

   /* ---- 5. 进 OP ---- */
   printf("\n---- 进 OP ----\n");
   (void)cycle();   /* 先进一帧, 把从站的 SM 数据通路带起来 */
   if (goto_state(target, EC_STATE_OPERATIONAL, OP_TMO_MS) != 0)
   {
      printf("  进不去 OP。若 AL 状态码是 0x001B 一类看门狗/同步相关, 检查 DC 周期。\n");
      exit_code = EXIT_NO_OP;
      goto out;
   }
   in_op = 1;
   printf("  [PASS] 已进入 OP\n");

   /* ---- 6. CiA402 上行: 0x0000 -> 0x0006 -> 0x0007 -> 0x000F ---- */
   printf("\n---- CiA402 状态机 (6040h 走 RxPDO, 6041h 读 TxPDO) ----\n");
   printf("  断言只用手册定义的 6041h 位 0/1/2/3 (掩码 0x%04X); 该驱动器手册未定义\n",
          (unsigned)SW_MASK);
   printf("  bit6, 所以 \"未使能\" 不区分 Not ready / Switch on disabled。\n");

   /* 0. 先归到已知起点, 这样入口状态是什么都不影响后面的判定 */
   if (step(target, "0. 归位 Disable voltage", CW_DISABLE_V, 0x0000) != 0)
   { exit_code = EXIT_FAIL; goto out; }
   if (step(target, "1. Shutdown", CW_SHUTDOWN, SW_RTSO) != 0)
   { exit_code = EXIT_FAIL; goto out; }
   if (step(target, "2. Switch on", CW_SWITCHON, SW_RTSO | SW_SWITCHED) != 0)
   { exit_code = EXIT_FAIL; goto out; }

   /* 第 3 步: 功率级打开, 电机从这里开始带电 */
   if (step(target, "3. Enable Operation", CW_ENABLE_OP,
            SW_RTSO | SW_SWITCHED | SW_OP_EN) != 0)
   {
      printf("      >>> 控制字经 RxPDO 写进去了但状态字没跟上 —— 看上面那行 6040h\n"
             "          回读: 一致 = 问题在驱动器侧; 不一致 = 问题在主站侧 (偏移/OP/WKC)。\n");
      exit_code = EXIT_FAIL;
      goto out;
   }
   printf("      >>> 电机已通电 (有保持力矩, 但未移动)。\n");

   /* 4. 保持观察: 确认 Operation enabled 是稳定态而不是一闪而过 */
   {
      uint32 t0 = now_ms();
      int    reads = 0, bad = 0;

      printf("  4. 保持观察 %dms ...", HOLD_MS);
      fflush(stdout);
      while ((int32)(now_ms() - t0) < HOLD_MS)
      {
         if (g_stop)
            break;
         (void)cycle();
         sw = get_sw();
         reads++;
         if ((sw & SW_OP_EN) == 0 || (sw & SW_FAULT) != 0)
         {
            bad = 1;
            break;
         }
         msleep(POLL_MS);
      }
      if (bad)
      {
         printf(" [FAIL] 保持期间掉出 Operation enabled (6041h=0x%04X %s)\n",
                (unsigned)sw, sw_name(sw));
         exit_code = EXIT_FAIL;
         goto out;
      }
      if (reads == 0)
      {
         printf(" [FAIL] 一笔 6041h 都没从 TxPDO 取到, 无法确认这是稳定态\n");
         exit_code = EXIT_FAIL;
         goto out;
      }
      printf(" [PASS] 稳定 (6041h=0x%04X, %d 次读数)\n", (unsigned)sw, reads);
   }

   /* ---- 7. 下行: 逐级退回去。只测上行的话, "能不能干净地停"恰恰是没测到的 ---- */
   if (step(target, "5. Disable operation", CW_SWITCHON,
            SW_RTSO | SW_SWITCHED) != 0)
   { exit_code = EXIT_FAIL; goto out; }
   if (step(target, "6. Shutdown", CW_SHUTDOWN, SW_RTSO) != 0)
   { exit_code = EXIT_FAIL; goto out; }
   if (step(target, "7. Disable voltage", CW_DISABLE_V, 0x0000) != 0)
   { exit_code = EXIT_FAIL; goto out; }

out:
   /* 收尾无条件执行: 正常跑完 / 某步 FAIL / 前置条件失败 / Ctrl-C 都走这里 */
   rc = teardown(target, in_op);
   if (rc != 0)
      exit_code = EXIT_NO_DISABLE;
   else if (g_stop && exit_code == EXIT_OK)
   {
      printf("\n[中止] 收到 Ctrl-C, 已按失能方向停机。\n");
      exit_code = EXIT_FAIL;
   }

   printf("\n---- 汇总 ----\n");
   if (exit_code == EXIT_OK)
      printf("[OK] 6040h 经 RxPDO 生效、6041h 经 TxPDO 变化, 上行+下行全部切换 PASS。\n"
             "     注意: 本程序只验证 6040h/6041h 的状态迁移, 未验证任何运动功能。\n");
   else if (exit_code == EXIT_NO_OP)
      printf("[FAIL] 没有进到 OP —— 先解决 SAFE_OP/OP 的进入问题, 状态机还轮不到。\n");
   else if (exit_code == EXIT_NO_DISABLE)
      printf("[严重] 收尾未能确认失能 —— 见上。\n");
   else if (exit_code == EXIT_FAIL)
      printf("[FAIL] 存在未通过的步骤。\n");
   else if (exit_code == EXIT_REFUSED)
      printf("[拒绝] 安全护栏拒绝, 未写任何东西 (或只写了映射)。\n");

   ecx_close(&g_ctx);
   return exit_code;
}
