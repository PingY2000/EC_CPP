/*
 * sm_state - CiA402 状态机切换验证 (独立小程序, 只干这一件事)
 *
 * 与 slide_motion 的区别 (也是它存在的理由):
 *   slide_motion 的 S3 只走**上行** (0x0006 -> 0x0007 -> 0x000F), 下行只在
 *   收尾里顺带做一遍, 而且中间态没有独立断言。本程序走一个**完整闭环**,
 *   每一步都回读 6041h 并断言该步应有的状态位:
 *
 *     0. 0x0000 -> 0x0000  未使能且无故障        (先归到已知起点, 入口状态无关)
 *     1. 0x0006 -> 0x0001  Ready to switch on
 *     2. 0x0007 -> 0x0003  Switched on
 *     3. 0x000F -> 0x0007  Operation enabled    <<< 功率级打开, 电机带电
 *     4. 保持观察 hold 毫秒, 期间轮询 Fault 位
 *     5. 0x0007 -> 0x0003  Switched on          (Disable operation)
 *     6. 0x0006 -> 0x0001  Ready to switch on   (Shutdown)
 *     7. 0x0000 -> 0x0000  未使能且无故障        (Disable voltage)
 *
 *   想验证"驱动器在 PRE_OP / SAFE_OP 下能不能用 SDO 打开功率级", 看第 3 步。
 *
 * 本程序**只写 6040h**。不写 6060h, 不写 607Ah, 不写任何参数对象, 不发运动
 * 指令 —— 全程没有一个字节会让滑台动起来。第 3 步之后电机会通电 (有保持力
 * 矩), 所以真跑时必须有人在设备旁、手放在物理急停上。
 *
 * 安全约定 (与 slide_motion 一致, 但复用的是同一套写法而不是同一份代码):
 *   - 默认只读: 不给 --allow-enable 就一个字节都不写, 只打印当前 6041h 就退出。
 *   - 只碰选中的那一根轴。别的轴哪怕也在总线上, 我们一个字节都不写过去 ——
 *     它可能正被另一套程序控制。
 *   - 任何异常退出路径 (Ctrl-C / 故障 / 超时 / 丢站) 都要把 6040h 写回 0x0000。
 *   - 收尾写完 0x0000 后必须回读 6041h 确认 bit2 已清。本仓库这份 SOEM 的
 *     ecx_SDOwrite 在加急路径 (psize<=4) 上把从站回的 SDO abort 帧也当成成功,
 *     而 6040h 正好是 2 字节加急写 —— "写失能有没有真的进去"唯一的证据就是
 *     回读。回读仍报 Operation enabled = 电机可能还带电, 退出码 10。
 *
 * 注意: 本文件是 slide_motion 目录下**第二个**出现 ecx_SDOwrite 的文件
 * (第一个是 sm_guard.c)。它刻意不链接 sm_guard.c/sm_bus.c —— 那套护栏是为
 * "带动作验收"设计的, 对本程序是多余的复杂度。审计本程序"会写什么"只需要
 * 看 st_wr_cw() 和 main() 里那 8 行步骤表。
 */

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "soem/soem.h"

/* ======================================================================
 * 常量
 * ====================================================================== */

/*
 * 网卡写死。换机器/换网卡就改这一行 —— 本程序不带网卡参数, 也不列举网卡
 * (只有在 ecx_init 打不开它的时候才把可用网卡列出来当诊断)。
 */
#define ST_IFNAME "\\Device\\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"

/* 受支持的 YKD2205PE (厂商 + 产品码集合, 与 slide_verify / slide_motion 一致) */
#define ST_VENDOR_ID 0x0994UL

/* CiA402 对象 */
#define ST_OID_CONTROLWORD 0x6040
#define ST_OID_STATUSWORD  0x6041

/* 控制字 (6040h) */
#define CW_DISABLE_V 0x0000  /* -> 未使能 (Disable voltage)  */
#define CW_SHUTDOWN  0x0006  /* -> Ready to switch on        */
#define CW_SWITCHON  0x0007  /* -> Switched on / Disable op  */
#define CW_ENABLE_OP 0x000F  /* -> Operation enabled         */
#define CW_FAULT_RST 0x0080  /* Fault reset, 随后回 0x0000   */

/*
 * 状态字 (6041h) 位。
 * 6041h 低 4 位用 0000/0001/0011/0111 编码, 所以全程用位判断, 禁止数值比较。
 *
 * **只定义 YKD2205PE 手册 (docs/ykd2205pe_ci402.md) 明确列出的位**: 0/1/2/3。
 * 标准 CiA402 里区分 "Not ready to switch on" 与 "Switch on disabled" 的 bit6,
 * 该驱动器手册**没有定义** —— sm.h 里也没有。所以本程序不碰 bit6:
 * 拿一个手册没承诺的位做 PASS/FAIL 断言, 会在驱动器本来正常的时候报失败,
 * 而那个失败还长得像"驱动器坏了"。
 */
#define SW_RTSO      0x0001  /* bit0  Ready to switch on   */
#define SW_SWITCHED  0x0002  /* bit1  Switched on          */
#define SW_OP_ENABLED 0x0004 /* bit2  Operation enabled    */
#define SW_FAULT     0x0008  /* bit3  Fault                */
#define SW_QUICKSTOP 0x0020  /* bit5  Quick stop (未定义, 仅用于状态名) */

/*
 * 断言掩码 = 手册定义的那四位。bit4(Voltage enabled)/bit9(Remote) 等只记录不断言。
 *
 * 第 0/7 步期望 (sw & SW_MASK) == 0x0000, 也就是"四位全 0 = 未使能且无故障"。
 * 这同时覆盖 "Not ready to switch on" 与 "Switch on disabled" 两种标准状态 ——
 * 而写 0x0000 (Disable voltage) 只负责把驱动器拉进后者, 不对前者作任何承诺
 * ("Not ready → Switch on disabled" 是驱动器内部初始化完成的自动迁移, 控制字
 * 触发不了它)。所以这里断言"未使能且无故障"是对的, 断言某一个具体状态名不是。
 */
#define SW_MASK 0x000F

#define ST_TMO_DEF 1000
#define ST_TMO_MAX 5000
#define ST_HOLD_DEF 500
#define ST_HOLD_MAX 5000
#define ST_POLL_MS 5

/* 退出码 */
#define ST_EXIT_OK       0  /* 全部切换 PASS */
#define ST_EXIT_USAGE    1  /* 用法 / 初始化失败 */
#define ST_EXIT_NO_SLAVE 2  /* 空总线, 或选中的轴不是受支持的 YKD */
#define ST_EXIT_FAIL     3  /* 存在 FAIL */
#define ST_EXIT_NO_DISABLE 10 /* 收尾写了 6040h=0 但 6041h 仍报 Operation enabled:
                                 电机可能仍然带电 —— 最严重的一类失败, 覆盖其它码 */

/* ======================================================================
 * 全局状态
 * ====================================================================== */

static ecx_contextt g_ctx;

/*
 * Ctrl-C 标志。信号处理器**只置这一位, 不做任何 I/O** —— 在信号上下文里发
 * SDO 是非法的。停机由主循环看到这一位之后走正常路径去做。
 */
static volatile sig_atomic_t g_stop = 0;

/* 本次运行是否写过 6040h。收尾只在写过的时候动手。 */
static int g_wrote = 0;

/* 当前单步超时, 由 CLI 设定 (放全局免得层层传参) */
static int g_tmo_ms = ST_TMO_DEF;

static void st_on_ctrl_c(int sig)
{
   (void)sig;
   g_stop = 1;
}

/* ======================================================================
 * 基础设施 (为了不链接 sm_bus.c, 这几件小事在这里各来一份)
 * ====================================================================== */

static void st_console_utf8(void)
{
#ifdef _WIN32
   SetConsoleOutputCP(CP_UTF8);
#endif
}

static uint32_t st_now_ms(void)
{
#ifdef _WIN32
   return (uint32_t)GetTickCount64();
#else
   {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (uint32_t)((uint64_t)ts.tv_sec * 1000u
                        + (uint64_t)(ts.tv_nsec / 1000000));
   }
#endif
}

static void st_sleep_ms(uint32_t ms)
{
#ifdef _WIN32
   Sleep(ms);
#else
   struct timespec ts;
   ts.tv_sec = (time_t)(ms / 1000u);
   ts.tv_nsec = (long)((ms % 1000u) * 1000000L);
   nanosleep(&ts, NULL);
#endif
}

static void st_print_adapters(void)
{
   ec_adaptert *head = ec_find_adapters();
   ec_adaptert *a = head;

   while (a != NULL)
   {
      printf("    - %s  (%s)\n", a->name, a->desc);
      a = a->next;
   }
   ec_free_adapters(head);
}

static int st_is_ykd(uint32_t eep_man, uint32_t eep_id)
{
   return (eep_man == ST_VENDOR_ID) && (eep_id == 0x2000UL || eep_id == 0x3000UL);
}

/* ======================================================================
 * 6041h 解读
 * ====================================================================== */

/*
 * 状态名 —— 只按手册定义的低 4 位判读, 不替驱动器猜 bit6。
 * 四位全 0 时, 标准 CiA402 里 "Not ready to switch on" 与 "Switch on disabled"
 * 靠 bit6 区分, 而本驱动器没定义 bit6, 所以这里统一写作 "未使能": 把这两种可能
 * 合并成一个诚实的说法, 好过挑一个听起来更像故障的。
 */
static const char *st_name(uint16_t sw)
{
   if ((sw & SW_FAULT) != 0)
      return (sw & SW_OP_ENABLED) ? "Fault reaction active" : "Fault";
   if ((sw & SW_OP_ENABLED) != 0)
      return (sw & SW_QUICKSTOP) ? "Operation enabled" : "Quick stop active";
   if ((sw & SW_SWITCHED) != 0)
      return "Switched on";
   if ((sw & SW_RTSO) != 0)
      return "Ready to switch on";
   return "未使能 (位0-3 全 0)";
}

/* 把 6041h 打成 "0x0027 (Operation enabled)" */
static void st_sw_str(char *dst, size_t n, uint16_t sw)
{
   snprintf(dst, n, "0x%04X (%s)", (unsigned)sw, st_name(sw));
}

/* ======================================================================
 * SDO 读写
 * ====================================================================== */

/* 读 6041h。返回 0 = 读到, -1 = 超时/无回音 (此时 *v 无意义, 不要拿它当读数) */
static int st_rd_sw(int slave, uint16_t *v)
{
   int psize = 2;
   int wkc;

   *v = 0;
   /* 粘滞位必须每次清, 否则上一次失败会污染下一次判定 */
   g_ctx.ecaterror = FALSE;
   wkc = ecx_SDOread(&g_ctx, (uint16_t)slave, ST_OID_STATUSWORD, 0, FALSE,
                     &psize, v, g_tmo_ms);
   return (wkc > 0) ? 0 : -1;
}

/* 取回错误栈里本对象的 abort 码 (排空整栈, 诊断用) */
static int32_t st_take_abort(int slave, uint16_t index, uint8_t sub)
{
   ec_errort err;
   int32_t   abort = 0;

   while (ecx_poperror(&g_ctx, &err))
   {
      if (err.Etype == EC_ERR_TYPE_SDO_ERROR &&
          err.Slave == (uint16_t)slave && err.Index == index && err.SubIdx == sub)
         abort = err.AbortCode;
   }
   return abort;
}

/*
 * 写 6040h —— 本程序唯一的写入口 (也是本文件唯一的 ecx_SDOwrite)。
 *
 * 返回值**不代表驱动器接受了**: 加急路径会把 abort 帧当成功 (见文件头注释)。
 * 真正的判据是写完之后回读 6041h, 也就是 st_step() 干的事。
 */
static int st_wr_cw(int slave, uint16_t cw, int timeout)
{
   int      wkc;
   int      ecerr;
   uint32_t t0, dt;

   g_ctx.ecaterror = FALSE;
   t0 = st_now_ms();
   wkc = ecx_SDOwrite(&g_ctx, (uint16_t)slave, ST_OID_CONTROLWORD, 0, FALSE,
                      2, &cw, timeout);
   dt = st_now_ms() - t0;
   ecerr = g_ctx.ecaterror ? 1 : 0;
   g_wrote = 1;

   if (wkc > 0 && !ecerr)
      return 0;

   printf("       邮件箱写 6040h=0x%04X 未确认 (wkc=%d, ecaterror=%d, %ums)\n",
          (unsigned)cw, wkc, ecerr, (unsigned)dt);
   return -1;
}

/* ======================================================================
 * 一步切换: 写控制字 -> 等到状态字符合断言
 *
 * fail_on_fault: 见到 Fault 位是否立即判失败。除"故障复位"之外的每一步都是 1
 *               —— 那些步的期望状态里 Fault 本来就是 0, 早报早停, 不用等满超时。
 *
 * 返回 0 = PASS, -1 = FAIL, -2 = 被中止 (Ctrl-C)
 * ====================================================================== */
static int st_step(int slave, const char *name, uint16_t cw, uint16_t mask,
                   uint16_t expect, int fail_on_fault, uint16_t *sw_out)
{
   uint32_t t0;
   uint16_t sw = 0;
   int      got = 0;
   int      reads = 0;   /* 成功读到的次数: 0 表示这一等全程总线静默 */
   char     buf[64];

   printf("  %-28s 写 6040h=0x%04X ...", name, (unsigned)cw);
   fflush(stdout);

   if (st_wr_cw(slave, cw, g_tmo_ms) != 0)
   {
      printf(" [FAIL] 写未确认\n");
      return -1;
   }

   /* 等驱动器消化这个控制字: 轮询 6041h 直到断言成立 */
   t0 = st_now_ms();
   while ((int32_t)(st_now_ms() - t0) < g_tmo_ms)
   {
      if (g_stop)
      {
         printf(" [中止]\n");
         return -2;
      }
      if (st_rd_sw(slave, &sw) == 0)
      {
         reads++;
         if (fail_on_fault && (sw & SW_FAULT) != 0)
         {
            st_sw_str(buf, sizeof(buf), sw);
            printf(" [FAIL] 等待期间进入故障态 (6041h=%s)\n", buf);
            return -1;
         }
         if ((sw & mask) == expect)
         {
            got = 1;
            break;
         }
      }
      st_sleep_ms(ST_POLL_MS);
   }

   if (sw_out != NULL)
      *sw_out = sw;

   if (!got)
   {
      struct ec_slave *s = &g_ctx.slavelist[slave];
      int32_t abort = st_take_abort(slave, ST_OID_CONTROLWORD, 0);

      printf(" [FAIL] 断言未成立\n");
      if (reads == 0)
      {
         /*
          * 一笔都没读到。这里**不能**把 sw (被清过零的缓冲) 当成"实测 0x0000" ——
          * 那是把"我们不知道"说成了"驱动器报了个 Not ready to switch on"。
          */
         printf("       6041h 一次都没读到 (%ums 内总线静默): 状态未知, "
                "检查从站是否掉线\n", (unsigned)g_tmo_ms);
      }
      else
      {
         st_sw_str(buf, sizeof(buf), sw);
         printf("       期望 6041h & 0x%04X == 0x%04X, 实测 %s (%d 次读数)\n",
                (unsigned)mask, (unsigned)expect, buf, reads);
      }
      printf("       EtherCAT AL 状态=0x%02X, AL 状态码=0x%04X %s\n",
             (unsigned)s->state, (unsigned)s->ALstatuscode,
             ec_ALstatuscode2string(s->ALstatuscode));
      if (abort != 0)
         printf("       6040h 的 SDO abort 码: 0x%08X\n", (unsigned)abort);
      return -1;
   }

   st_sw_str(buf, sizeof(buf), sw);
   printf(" [PASS] %s\n", buf);
   return 0;
}

/*
 * 收尾: 无条件把 6040h 写回 0x0000, 并回读 6041h 确认 bit2 已清。
 * 返回 0 = 已确认失能, 1 = 未确认 (电机可能仍带电)。
 */
static int st_shutdown(int slave)
{
   uint16_t sw = 0;
   uint32_t t0;
   int      reads = 0;
   char     buf[64];

   if (!g_wrote)
      return 0;

   printf("\n---- 收尾: 写 6040h=0x0000 (Disable voltage) ----\n");

   /*
    * 这里不过 st_wr_cw 之外的检查: 即写失败也要继续回读, 因为"写失败"和
    * "没停下来"是两件事, 而后者才是要命的那个。
    */
   (void)st_wr_cw(slave, CW_DISABLE_V, g_tmo_ms);

   t0 = st_now_ms();
   while ((int32_t)(st_now_ms() - t0) < g_tmo_ms)
   {
      if (st_rd_sw(slave, &sw) == 0)
      {
         reads++;
         if ((sw & SW_OP_ENABLED) == 0)
            break;
      }
      st_sleep_ms(ST_POLL_MS);
   }

   /*
    * "读到了 bit2=0" 才算确认。"一笔都没读到" 不是确认 —— 在最坏的一类判断
    * 上(电机还带不带电), 没有消息不能当成好消息。这条路径宁可报 10 让用户
    * 去看一眼, 也不能打一行 [PASS] 把一次失联说成一次成功停机。
    */
   if (reads == 0)
   {
      printf("  [FAIL] 收尾期间一笔 6041h 都没读到, 失能状态**无法确认**。\n");
      printf("  >>> 电机可能仍然带电。立即断开驱动器供电, 不要用手去推滑台。\n");
      return 1;
   }

   if ((sw & SW_OP_ENABLED) != 0)
   {
      st_sw_str(buf, sizeof(buf), sw);
      printf("  [FAIL] 写了 0x0000 但 6041h 仍报 Operation enabled (6041h=%s)\n", buf);
      printf("  >>> 电机可能仍然带电。立即断开驱动器供电, 不要用手去推滑台。\n");
      return 1;
   }

   st_sw_str(buf, sizeof(buf), sw);
   printf("  [PASS] 已确认失能 (6041h=%s)\n", buf);
   return 0;
}

/* ======================================================================
 * 用法
 * ====================================================================== */
static void st_usage(void)
{
   printf(
      "\n用法: sm_state [选项]        (网卡写死在代码里: %s)\n"
      "\n"
      "  --allow-enable        允许写 6040h。不加则只读快照, 一个字节都不写。\n"
      "  --axis N              选哪根轴 (总线位置, 0 起)。默认第一台 YKD2205PE。\n"
      "  --state pre-op|safe-op  在哪个 AL 状态下跑, 默认 pre-op。\n"
      "  --reset-fault         起始 6041h 报 Fault 时允许写 0x0080 复位。\n"
      "  --hold MS             停在 Operation enabled 的观察时长, 默认 %d (上限 %d)。\n"
      "  --tmo MS              单步等待超时, 默认 %d (上限 %d)。\n"
      "\n"
      "退出码: 0 全 PASS / 1 用法或初始化 / 2 无从站或无目标轴 / 3 有 FAIL /\n"
      "        10 收尾后 6041h 仍报 Operation enabled (电机可能仍带电)\n"
      "\n"
      "本程序只写 6040h, 不发运动指令, 滑台不会移动。但第 3 步 (Enable\n"
      "Operation) 之后电机会带电并有保持力矩 —— 真跑时人在设备旁, 手放在急停上。\n",
      ST_IFNAME, ST_HOLD_DEF, ST_HOLD_MAX, ST_TMO_DEF, ST_TMO_MAX);
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char *argv[])
{
   int   allow_enable = 0;
   int   axis_only = -1;
   int   want_safe_op = 0;
   int   reset_fault = 0;
   int   hold_ms = ST_HOLD_DEF;
   int   tmo_ms = ST_TMO_DEF;
   int   cnt, slave, i;
   int   target = -1;
   int   rc, exit_code = ST_EXIT_OK;
   int   any_fail = 0;
   uint16_t sw = 0;
   uint16_t req_state = EC_STATE_PRE_OP;
   char  buf[64];

   st_console_utf8();
   signal(SIGINT, st_on_ctrl_c);

   printf("sm_state - CiA402 状态机切换验证 (只写 6040h, 不发运动指令)\n");

   /* ---- 参数 ---- */
   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];

      if (strcmp(a, "--allow-enable") == 0)
         allow_enable = 1;
      else if (strcmp(a, "--reset-fault") == 0)
         reset_fault = 1;
      else if (strcmp(a, "--axis") == 0 && i + 1 < argc)
         axis_only = atoi(argv[++i]);
      else if (strcmp(a, "--hold") == 0 && i + 1 < argc)
         hold_ms = atoi(argv[++i]);
      else if (strcmp(a, "--tmo") == 0 && i + 1 < argc)
         tmo_ms = atoi(argv[++i]);
      else if (strcmp(a, "--state") == 0 && i + 1 < argc)
      {
         const char *v = argv[++i];

         if (strcmp(v, "safe-op") == 0)
            want_safe_op = 1;
         else if (strcmp(v, "pre-op") != 0)
         {
            printf("未知 --state: %s (只支持 pre-op / safe-op)\n", v);
            return ST_EXIT_USAGE;
         }
      }
      else
      {
         printf("未知参数: %s (本程序不带网卡参数, 网卡写在代码里)\n", a);
         st_usage();
         return ST_EXIT_USAGE;
      }
   }

   /* 上限只能收紧: 超了就夹回去, 不报错 —— 这两个值只是等待时长, 夹住更安全 */
   if (hold_ms < 0)          hold_ms = 0;
   if (hold_ms > ST_HOLD_MAX) hold_ms = ST_HOLD_MAX;
   if (tmo_ms < 100)         tmo_ms = 100;
   if (tmo_ms > ST_TMO_MAX)  tmo_ms = ST_TMO_MAX;
   g_tmo_ms = tmo_ms;
   req_state = want_safe_op ? EC_STATE_SAFE_OP : EC_STATE_PRE_OP;

   if (axis_only < -1)
   {
      printf("--axis 不能是负数\n");
      return ST_EXIT_USAGE;
   }

   if (!allow_enable)
      printf("未给 --allow-enable: 本次只读快照, 不执行任何切换。\n");

   printf("网卡 (写死): %s\n", ST_IFNAME);

   /* ---- 开总线 (只读, 到这一步为止没写过任何东西) ---- */
   if (!ecx_init(&g_ctx, ST_IFNAME))
   {
      printf("无法打开网卡 (被独占 / 需管理员 / Npcap 未装)。\n");
      printf("若这台机器上换了网卡, 请改 sm_state.c 里的 ST_IFNAME。"
             "当前可用网卡:\n");
      st_print_adapters();
      return ST_EXIT_USAGE;
   }
   cnt = ecx_config_init(&g_ctx);
   if (cnt <= 0)
   {
      printf("总线上未发现从站 (config_init=%d), 检查网线/供电。\n", cnt);
      ecx_close(&g_ctx);
      return ST_EXIT_NO_SLAVE;
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
      int is_ykd = st_is_ykd(s->eep_man, s->eep_id);
      int selected = (axis_only < 0) ? is_ykd : (axis_only == slave - 1);

      printf("  位置 %-3d AL=0x%02X 厂商=0x%08X 产品码=0x%08X %-16s %s\n",
             slave - 1, (unsigned)s->state, (unsigned)s->eep_man,
             (unsigned)s->eep_id, s->name,
             is_ykd ? "YKD2205PE" : "(非受支持)");

      if (selected && target < 0)
      {
         if (!is_ykd)
         {
            printf("\n[拒绝] 位置 %d 不是受支持的 YKD2205PE, 未写任何东西。\n",
                   slave - 1);
            ecx_close(&g_ctx);
            return ST_EXIT_NO_SLAVE;
         }
         if (s->state != EC_STATE_PRE_OP && s->state != EC_STATE_SAFE_OP)
         {
            printf("\n[拒绝] 位置 %d 未进入 PRE_OP/SAFE_OP (AL 状态 0x%02X, "
                   "AL 状态码 0x%04X %s), 未写任何东西。\n",
                   slave - 1, (unsigned)s->state, (unsigned)s->ALstatuscode,
                   ec_ALstatuscode2string(s->ALstatuscode));
            ecx_close(&g_ctx);
            return ST_EXIT_NO_SLAVE;
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
      return ST_EXIT_NO_SLAVE;
   }

   /* ---- 抬到目标 AL 状态 ---- */
   g_ctx.slavelist[target].state = req_state;
   ecx_writestate(&g_ctx, (uint16_t)target);
   ecx_statecheck(&g_ctx, (uint16_t)target, req_state, EC_TIMEOUTSTATE);
   ecx_readstate(&g_ctx);
   printf("\n目标轴: 位置 %d, AL 状态 0x%02X (请求 %s)\n",
          target - 1, (unsigned)g_ctx.slavelist[target].state,
          want_safe_op ? "SAFE_OP" : "PRE_OP");

   /* ---- 初始 6041h ---- */
   if (st_rd_sw(target, &sw) != 0)
   {
      printf("\n[拒绝] 读不到 6041h, 无法判断当前状态, 未写任何东西。\n");
      ecx_close(&g_ctx);
      return ST_EXIT_NO_SLAVE;
   }
   st_sw_str(buf, sizeof(buf), sw);
   printf("初始 6041h = %s\n", buf);

   if (!allow_enable)
   {
      printf("\n(只读快照到此为止。加 --allow-enable 才会执行上面的 8 步切换。)\n");
      ecx_close(&g_ctx);
      return ST_EXIT_OK;
   }

   /* ---- 故障门: 起始就报 Fault 的话, 后面的步骤全是白等 ---- */
   if ((sw & SW_FAULT) != 0)
   {
      if (!reset_fault)
      {
         printf("\n[拒绝] 驱动器处于故障态, 且未给 --reset-fault。未写任何东西。\n");
         ecx_close(&g_ctx);
         return ST_EXIT_FAIL;
      }
      printf("\n故障复位: 写 6040h=0x%04X, 随后回 0x0000\n", CW_FAULT_RST);
      (void)st_wr_cw(target, CW_FAULT_RST, tmo_ms);
      (void)st_wr_cw(target, CW_DISABLE_V, tmo_ms);
      rc = st_step(target, "故障位清零", CW_DISABLE_V, SW_FAULT, 0, 0, &sw);
      if (rc == -1)
      {
         printf("[FAIL] 故障未清除, 停止。\n");
         any_fail = 1;
         goto out;
      }
      if (rc == -2)
         goto out;
   }

   /* ================================================================
    * 状态机切换闭环
    *
    * 每一步都在 st_step() 里: 写 6040h -> 轮询 6041h 直到断言成立。
    * 断言表的 "期望值" 就是 CiA402 规定的该状态的状态字低 4 位 + bit6。
    * ================================================================ */
   printf("\n---- CiA402 状态机切换 ----\n");
   printf("  断言只用手册定义的 6041h 位 0/1/2/3 (SW_MASK=0x%04X);\n", SW_MASK);
   printf("  该驱动器手册未定义 bit6, 所以 \"未使能\" 不区分 Not ready / Switch on disabled。\n");

   /* 0. 先归到已知起点, 这样入口状态是什么都不影响后面的判定 */
   rc = st_step(target, "0. 归位 Disable voltage", CW_DISABLE_V,
                SW_MASK, 0x0000, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

   rc = st_step(target, "1. Shutdown", CW_SHUTDOWN,
                SW_MASK, SW_RTSO, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

   rc = st_step(target, "2. Switch on", CW_SWITCHON,
                SW_MASK, SW_RTSO | SW_SWITCHED, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

   /* 第 3 步: 功率级打开, 电机从这里开始带电 */
   rc = st_step(target, "3. Enable Operation", CW_ENABLE_OP,
                SW_MASK, SW_RTSO | SW_SWITCHED | SW_OP_ENABLED, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }
   printf("      >>> 电机已通电 (有保持力矩, 但未移动)。\n");

   /* 4. 保持观察: 确认 Operation enabled 是稳定态而不是一闪而过 */
   if (hold_ms > 0)
   {
      uint32_t t0 = st_now_ms();
      int      faulted = 0;
      int      reads = 0;

      printf("  4. 保持观察 %dms ...", hold_ms);
      fflush(stdout);
      while ((int32_t)(st_now_ms() - t0) < hold_ms)
      {
         if (g_stop)
         {
            printf(" [中止]\n");
            goto out;
         }
         if (st_rd_sw(target, &sw) == 0)
         {
            reads++;
            if ((sw & SW_OP_ENABLED) == 0 || (sw & SW_FAULT) != 0)
            {
               faulted = 1;
               break;
            }
         }
         st_sleep_ms(ST_POLL_MS);
      }
      if (faulted)
      {
         st_sw_str(buf, sizeof(buf), sw);
         printf(" [FAIL] 保持期间掉出 Operation enabled (6041h=%s)\n", buf);
         any_fail = 1;
         goto out;
      }
      if (reads == 0)
      {
         /* 同 st_step: 没读到就说没读到, 不能拿"没消息"当"稳定的证据" */
         printf(" [FAIL] 保持期间一笔 6041h 都没读到, 无法确认这是稳定态\n");
         any_fail = 1;
         goto out;
      }
      st_sw_str(buf, sizeof(buf), sw);
      printf(" [PASS] 稳定 (6041h=%s, %d 次读数)\n", buf, reads);
   }

   /* 5-7. 下行: 逐级退回去。每一步同样是独立断言 —— 只测上行的话,
          "能不能干净地停"这件事恰恰是没测到的。 */
   rc = st_step(target, "5. Disable operation", CW_SWITCHON,
                SW_MASK, SW_RTSO | SW_SWITCHED, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

   rc = st_step(target, "6. Shutdown", CW_SHUTDOWN,
                SW_MASK, SW_RTSO, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

   rc = st_step(target, "7. Disable voltage", CW_DISABLE_V,
                SW_MASK, 0x0000, 1, &sw);
   if (rc != 0) { any_fail = 1; goto out; }

out:
   /*
    * 收尾无条件执行。走到这里的路径有三种: 正常跑完 / 某步 FAIL / Ctrl-C。
    * st_shutdown() 自己会在没写过东西时直接返回, 所以不用在外面判。
    */
   if (st_shutdown(target) != 0)
      exit_code = ST_EXIT_NO_DISABLE;
   else if (g_stop)
   {
      printf("\n[中止] 收到 Ctrl-C, 已按失能方向停机。\n");
      if (exit_code == ST_EXIT_OK)
         exit_code = ST_EXIT_FAIL;
   }
   else if (any_fail)
   {
      exit_code = ST_EXIT_FAIL;
      /*
       * 逐级上行没动, 而 6041h 也不是故障态 —— 最值得先试的一步是换 AL 状态。
       * 很多驱动器要求至少 SAFE_OP 才肯跑 CiA402 状态机 (PRE_OP 下没有过程
       * 数据, 状态机根本没启动), 这时无论写什么控制字 6041h 都纹丝不动。
       */
      if (req_state == EC_STATE_PRE_OP &&
          g_ctx.slavelist[target].state == EC_STATE_PRE_OP)
         printf("\n提示: 本次在 PRE_OP 下跑, 从站也停在 PRE_OP。若每步 6041h 都\n"
                "      纹丝不动, 先试 --state safe-op —— 不少驱动器要至少 SAFE_OP\n"
                "      才启动 CiA402 状态机, PRE_OP 下控制字不会被受理。\n");
   }
   else
   {
      printf("\n[OK] CiA402 状态机 上行+下行 全部切换 PASS。\n");
      printf("     (本次只验证 6040h/6041h 的状态迁移, 未验证任何运动功能。)\n");
   }

   ecx_close(&g_ctx);
   return exit_code;
}
