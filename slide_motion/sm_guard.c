/*
 * sm_guard.c - 安全护栏与**全工程唯一的写入口**
 *
 * 这是 slide_motion 里唯一出现 ecx_SDOwrite 的文件。
 * 审阅"这个工具到底会往驱动器里写什么"时, 只看这一个文件就够了:
 *
 *      grep -rn ecx_SDOwrite slide_motion   ->  只有本文件一处
 *
 * 三个层次:
 *   guard_write()       低层原语, 不做授权判断。**只允许本文件的停机/收尾路径调用**。
 *   sm_wr_*()/sm_set_cw()  带授权的写, 供 sm_motion.c 调用。未授权直接拒绝。
 *   sm_guard_emergency_stop() / sm_guard_teardown()
 *                       绕过授权 (否则 Ctrl-C 之后反而停不下来), 但只写"去使能"
 *                       方向的值, 结构上不可能让轴动起来。
 *
 * 关键安全不变量:
 *   I1. 未给 --allow-motion 时, sm_wr_* / sm_set_cw 一律拒绝 -> 一根线都不会动。
 *   I2. 停机与收尾永远可写, 不受 abort 标志阻挡。
 *   I3. 上限只能收紧; 放宽必须显式 --force-caps。
 *   I4. Ctrl-C 处理器只置标志位, 绝不碰 SOEM (它在另一个线程里跑, SOEM 非线程安全)。
 *   I5. 收尾幂等, 且在任何错误路径上都会被调用一次。
 */

#include <stdio.h>
#include <string.h>

#include "sm.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>   /* SetConsoleCtrlHandler, InterlockedExchange */
#else
#include <signal.h>
#endif

/* 护栏全局状态 */
sm_guard_t g_guard;

/* ======================================================================
 * 判定码 / 中止原因 文本
 * ====================================================================== */
const char *sm_verdict_str(int v)
{
   switch (v)
   {
      case SM_V_PASS: return "PASS";
      case SM_V_WARN: return "WARN";
      case SM_V_FAIL: return "FAIL";
      case SM_V_SKIP: return "SKIP";
      case SM_V_INFO: return "INFO";
      default:        return "----";
   }
}

const char *sm_abort_str(int r)
{
   switch (r)
   {
      case SM_ABORT_NONE:     return "无";
      case SM_ABORT_CTRL_C:   return "收到 Ctrl-C";
      case SM_ABORT_FAULT:    return "驱动器故障位 (6041h bit3) 置位";
      case SM_ABORT_TIMEOUT:  return "等待到位超时";
      case SM_ABORT_STALL:    return "设定点已被接受但位置无变化 (失速)";
      case SM_ABORT_IO:       return "总线读写失败";
      case SM_ABORT_LOST:     return "从站掉线";
      case SM_ABORT_DEADLINE: return "超过总动作时间预算";
      default:                return "未知";
   }
}

/* ======================================================================
 * I4: Ctrl-C 处理器
 *
 * Windows 的控制处理器运行在**另一个线程**上, 而 SOEM 的上下文 (邮箱状态、
 * 收发缓冲) 不是线程安全的。所以这里**只置标志位**: 不发 SDO、不 printf、不分配。
 * 主循环在下一个轮询周期 (≤ SM_POLL_MS) 察觉, 由主线程执行安全停机。
 *
 * 最坏反应时间 ≈ SM_SDO_TMO_MOTION (200ms) + 一次轮询 —— 因为主线程可能正卡在
 * 一次 SDO 读里。这就是运动期必须把 SDO 超时从 EC_TIMEOUTRXM (700ms) 降到
 * 200ms 的原因。
 *
 * 第二次 Ctrl-C 交给系统默认处理, 立即结束进程 —— 这是最后的逃生通道。
 * ====================================================================== */
#ifdef _WIN32
static BOOL WINAPI sm_ctrl_handler(DWORD type)
{
   if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
   {
      if (InterlockedExchange((volatile LONG *)&g_guard.abort, 1) == 0)
      {
         g_guard.abort_reason = SM_ABORT_CTRL_C;
         return TRUE;    /* 第一次: 主循环负责安全停机 */
      }
      return FALSE;      /* 第二次: 立即结束 */
   }
   return FALSE;
}
#else
static void sm_sig_handler(int sig)
{
   if (sig == SIGINT)
   {
      if (g_guard.abort == 0)
      {
         g_guard.abort = 1;
         g_guard.abort_reason = SM_ABORT_CTRL_C;
         return;
      }
      signal(SIGINT, SIG_DFL);
      raise(SIGINT);
   }
}
#endif

void sm_guard_install_ctrl_handler(void)
{
#ifdef _WIN32
   SetConsoleCtrlHandler(sm_ctrl_handler, TRUE);
#else
   signal(SIGINT, sm_sig_handler);
#endif
}

void sm_guard_abort(int reason)
{
   g_guard.abort = 1;
   if (g_guard.abort_reason == SM_ABORT_NONE)
      g_guard.abort_reason = reason;
}

int sm_guard_should_abort(void)
{
   return (g_guard.abort != 0);
}

void sm_guard_feed(void)
{
   g_guard.last_io_ms = sm_now_ms();
}

int sm_guard_watchdog_expired(void)
{
   if (g_guard.watchdog_ms == 0)
      return 0;
   /* 差值法, 回绕安全 (见 guard_wait_sw 的说明) */
   return ((int32_t)(sm_now_ms() - g_guard.last_io_ms) >
           (int32_t)g_guard.watchdog_ms) ? 1 : 0;
}

/* ======================================================================
 * I3: 上限收紧
 * ====================================================================== */
uint32_t sm_guard_clamp_u32(const char *name, uint32_t val,
                            uint32_t hard_max, uint32_t dflt)
{
   if (val == 0)
      return dflt;                     /* 未指定 -> 默认值 */
   if (val > hard_max)
   {
      if (g_guard.force_caps)
      {
         printf("  [上限] %s = %u 超过硬上限 %u, 因 --force-caps 而放行。\n",
                name, (unsigned)val, (unsigned)hard_max);
         return val;
      }
      printf("  [上限] %s = %u 超过硬上限 %u, 已收紧为 %u "
             "(要用更大的值请加 --force-caps)。\n",
             name, (unsigned)val, (unsigned)hard_max, (unsigned)hard_max);
      return hard_max;
   }
   return val;
}

int32_t sm_guard_clamp_i32(const char *name, int32_t val,
                           int32_t hard_max, int32_t dflt)
{
   /*
    * 绝对值必须在 int64 里取。若写成 int32 的 (val < 0) ? -val : val,
    * 当 val == INT32_MIN 时 -val 溢出仍是 INT32_MIN(负), 于是 a > hard_max
    * 恒为假, INT32_MIN 会**原样**穿过上限检查 —— 那是一条 2^31 脉冲的
    * 相对运动指令, 一路直达机械死挡。这是本函数唯一的值域陷阱。
    */
   int64_t a = ((int64_t)val < 0) ? -(int64_t)val : (int64_t)val;

   if (val == 0)
      return dflt;

   if (a > (int64_t)hard_max)
   {
      if (g_guard.force_caps && a <= SM_CAP_ABS_MAX)
      {
         printf("  [上限] %s = %d 超过硬上限 %d, 因 --force-caps 而放行。\n",
                name, (int)val, (int)hard_max);
         return val;
      }

      /* --force-caps 只能把硬上限放宽到绝对天花板, 再往上仍然收紧 ——
         否则一个 INT32_MIN 就能变成 2^31 脉冲的行程。 */
      {
         int32_t lim = (g_guard.force_caps && a > SM_CAP_ABS_MAX)
                          ? (int32_t)SM_CAP_ABS_MAX
                          : hard_max;
         int32_t c = (val < 0) ? -lim : lim;

         if (g_guard.force_caps)
            printf("  [上限] %s = %d 超过绝对上限 %ld, 已强制收紧为 %d "
                   "(--force-caps 对该上限无效)。\n",
                   name, (int)val, (long)SM_CAP_ABS_MAX, (int)c);
         else
            printf("  [上限] %s = %d 超过硬上限 %d, 已收紧为 %d "
                   "(要用更大的值请加 --force-caps)。\n",
                   name, (int)val, (int)hard_max, (int)c);
         return c;
      }
   }
   return val;
}

/*
 * 看门狗必须严格大于"一次轮询的最坏耗时", 否则正常慢轮询会被误判成故障。
 * 一次轮询 = 3 次 SDO 读 (6041h / 6064h / 606Ch) × SM_SDO_TMO_MOTION。
 */
int sm_guard_check_caps(uint32_t watchdog_ms)
{
   uint32_t floor_ms = (uint32_t)SM_SDO_TMO_MOTION * 3u;

   if (watchdog_ms <= floor_ms)
   {
      printf("看门狗 %ums 必须大于单次轮询最坏耗时 %ums "
             "(3 次 SDO 读 × %dms)。\n",
             (unsigned)watchdog_ms, (unsigned)floor_ms, SM_SDO_TMO_MOTION);
      return -1;
   }
   return 0;
}

/* ======================================================================
 * I1: 授权
 * ====================================================================== */
int sm_guard_check_authorization(int want_motion, int want_jog)
{
   if (want_jog && !want_motion)
   {
      printf("--allow-jog 必须与 --allow-motion 一起给 "
             "(移动必然先要使能)。\n");
      return SM_EXIT_USAGE;
   }
   if (want_jog && !g_guard.authorized_jog)
   {
      printf("拒绝: 要求执行微动 (S4) 但没有 --allow-jog。未写任何东西。\n");
      return SM_EXIT_REFUSED;
   }
   if (want_motion && !g_guard.authorized_motion)
   {
      printf("拒绝: 要求执行使能/微动但没有 --allow-motion。未写任何东西。\n");
      return SM_EXIT_REFUSED;
   }
   return 0;
}

/* 打开总线后立刻打印的"我现在处于什么授权级别"横幅 —— 让日志第一屏就能看出
   这次运行有没有可能写东西。 */
void sm_guard_banner(const char *ifname, int nslaves)
{
   printf("\n==================== 授权级别 ====================\n");
   printf("  网卡       : %s\n", (ifname != NULL) ? ifname : "(未指定)");
   printf("  从站数     : %d\n", nslaves);
   printf("  使能授权   : %s\n",
          g_guard.authorized_motion ? "已给 (--allow-motion)" : "无 (全程只读)");
   printf("  移动授权   : %s\n",
          g_guard.authorized_jog ? "已给 (--allow-jog)" : "无 (不会移动)");
   printf("  目标 AL 状态: %s\n",
          (g_guard.requested_state == SM_STATE_SAFE_OP) ? "SAFE_OP" : "PRE_OP");
   if (!g_guard.authorized_motion)
      printf("  本次运行不写任何从站: 不写 SDO 对象, 也不改 AL 状态寄存器。\n");
   printf("==================================================\n");
}

/* ======================================================================
 * 授权横幅 + 交互确认
 * ====================================================================== */
int sm_guard_confirm(const char *ifname, int nslaves, int n_jog_axes,
                     int32_t jog_pulses, uint32_t jog_vel, uint32_t jog_acc,
                     uint32_t move_tmo_ms, uint32_t watchdog_ms)
{
   char line[64];

   printf("\n==================== 动作授权确认 ====================\n");
   printf("  网卡        : %s\n", (ifname != NULL) ? ifname : "(未指定)");
   printf("  总线上从站数: %d\n", nslaves);
   printf("  将动作的轴数: %d\n", n_jog_axes);
   printf("  单次微动行程: %d 脉冲 (往返各一次)\n", (int)jog_pulses);
   printf("  微动速度    : %u 脉冲/s\n", (unsigned)jog_vel);
   printf("  加减速度    : %u 脉冲/s^2\n", (unsigned)jog_acc);
   printf("  单腿超时    : %u ms\n", (unsigned)move_tmo_ms);
   printf("  心跳看门狗  : %u ms\n", (unsigned)watchdog_ms);
   printf("------------------------------------------------------\n");
   printf("  本工具会: 使能驱动器 (电机通电) 并按上面的行程实际移动滑台。\n");
   printf("  本工具不会: 写 2102h 保存到 EEPROM、改 PDO 映射、写软限位。\n");
   printf("  软件急停是尽力而为, 最坏反应约 %d ms。\n", SM_SDO_TMO_MOTION + 300);
   printf("  真正的保护是物理急停与驱动器急停输入 —— 请确认它们在手边。\n");
   printf("  运行中按一次 Ctrl-C 安全停机; 连按两次立即强制结束进程。\n");
   printf("======================================================\n");

   if (g_guard.non_interactive)
   {
      printf("--yes 已给出, 视为已确认。\n");
      return 1;
   }

   printf("\n确认要让滑台实际移动吗? 输入 yes 继续, 其它任何输入都取消: ");
   fflush(stdout);
   if (fgets(line, (int)sizeof(line), stdin) == NULL)
   {
      printf("\n未读到输入, 视为取消。\n");
      return 0;
   }
   /* 去掉行尾换行 */
   line[strcspn(line, "\r\n")] = '\0';
   if (strcmp(line, "yes") == 0)
      return 1;
   printf("输入 \"%s\" 不是 yes, 已取消, 未写任何东西。\n", line);
   return 0;
}

/* ======================================================================
 * 低层写原语 —— 全工程唯一的 ecx_SDOwrite 调用点
 *
 * **不做授权判断**。调用者负责。只有下面三类调用者:
 *   1. sm_wr_* / sm_set_cw        (已做授权判断)
 *   2. sm_guard_emergency_stop()  (去使能方向, 必须绕过授权)
 *   3. sm_guard_teardown()        (去使能方向 + 恢复参数, 必须绕过授权)
 * ====================================================================== */
static int guard_write(int slave, uint16_t index, uint8_t sub, int size,
                       const void *p, const char *why)
{
   int wkc;
   int timeo;

   /*
    * 运动期与非运动期用不同超时。运动期必须短: 主线程可能正卡在这里,
    * 而 Ctrl-C / 故障停机要在这个窗口之后才能发出去。
    */
   timeo = g_guard.ds402_enabled ? SM_SDO_TMO_MOTION : SM_SDO_TMO_IDLE;

   g_ctx.ecaterror = FALSE;   /* 清粘滞位, 与读路径同理 */
   wkc = ecx_SDOwrite(&g_ctx, (uint16_t)slave, index, sub, FALSE,
                      size, p, timeo);
   if (wkc > 0)
   {
      g_guard.wrote_anything = 1;
      sm_guard_feed();
      return 0;
   }

   /* 失败: 把 abort 码捞出来给调用者看 */
   {
      int32_t abort = sm_take_abort(slave, index, sub);

      printf("      [WRITE-FAIL] 从站%d %04Xh:%02X <- %s 失败 (abort 0x%08X)\n",
             slave, (unsigned)index, (unsigned)sub,
             (why != NULL) ? why : "", (unsigned)abort);
   }
   return -1;
}

/* 带授权的写 (供 sm_motion.c 使用) */
int sm_wr_raw(int slave, uint16_t index, uint8_t sub, int size, const void *p,
              const char *why)
{
   if (!g_guard.authorized_motion)
      return -1;
   return guard_write(slave, index, sub, size, p, why);
}

int sm_wr_u8(int slave, uint16_t index, uint8_t sub, uint8_t v, const char *why)
{
   return sm_wr_raw(slave, index, sub, 1, &v, why);
}

int sm_wr_u16(int slave, uint16_t index, uint8_t sub, uint16_t v, const char *why)
{
   return sm_wr_raw(slave, index, sub, 2, &v, why);
}

int sm_wr_u32(int slave, uint16_t index, uint8_t sub, uint32_t v, const char *why)
{
   return sm_wr_raw(slave, index, sub, 4, &v, why);
}

int sm_wr_i32(int slave, uint16_t index, uint8_t sub, int32_t v, const char *why)
{
   return sm_wr_raw(slave, index, sub, 4, &v, why);
}

/* ======================================================================
 * 控制字写入 (内部版, 不做授权判断; 停机/收尾也用这个)
 * ====================================================================== */
static int guard_set_cw(sm_axis_t *ax, uint16_t cw, const char *why)
{
   uint16_t old = 0;
   int32_t  ab = 0;
   uint32_t t;
   int      rc;

   if (ax == NULL)
      return -1;

   /* 读旧值只为打印审计行。固定用短超时 —— 这是审计用途,
      在停机/收尾路径上不能让一次读超时把反应时间拖到 700ms。 */
   (void)sm_rd_u16(ax->slave, SM_OID_CONTROLWORD, 0, &old,
                   SM_SDO_TMO_MOTION, &ab);

   rc = guard_write(ax->slave, SM_OID_CONTROLWORD, 0, 2, &cw, why);

   t = sm_now_ms();
   if (rc == 0)
   {
      printf("      [WRITE] %s: 6040h 0x%04X -> 0x%04X\n",
             (why != NULL) ? why : "", (unsigned)old, (unsigned)cw);
   }

   /* 记录轨迹。只在写**成功**时记录 —— 轨迹表是审计材料, 记下没送出去的
      值会让它看起来比实际顺利。 */
   if (rc == 0 && ax->trace_n < SM_TRACE_MAX)
   {
      ax->trace[ax->trace_n].cw = cw;
      ax->trace[ax->trace_n].sw = 0; /* 由调用者读回后回填 */
      ax->trace[ax->trace_n].ms = t - g_guard.t_start_ms;
      ax->trace[ax->trace_n].step = ax->trace_n;
      ax->trace_n++;
   }

   /* 我们自己的使能状态认知 (bit3=1 才是 Operation enabled 的判据) */
   if (rc == 0)
   {
      if ((cw & 0x0008) != 0)
         g_guard.ds402_enabled = 1;
      else
         g_guard.ds402_enabled = 0;
   }
   return rc;
}

/* 带授权的控制字写 (供 sm_motion.c 使用) */
int sm_set_cw(sm_axis_t *ax, uint16_t cw, const char *why)
{
   if (!g_guard.authorized_motion)
      return -1;
   return guard_set_cw(ax, cw, why);
}

/* 由调用者读回的 6041h 回填到最近一条轨迹 (声明见 sm.h) */
void sm_trace_fill_sw(sm_axis_t *ax, uint16_t sw)
{
   if (ax != NULL && ax->trace_n > 0)
      ax->trace[ax->trace_n - 1].sw = sw;
}

/* ======================================================================
 * 停机与收尾
 *
 * I2: 这两个函数**不受 abort 标志影响**。Ctrl-C 之后最需要的就是它们能跑完。
 * I5: teardown 幂等, 且每一步都尽力执行 —— 前一步失败不阻止后一步。
 * ====================================================================== */

/* 等 6041h 满足条件, 最多 tmo_ms。返回 1 = 满足 */
static int guard_wait_sw(sm_axis_t *ax, uint16_t mask, uint16_t want,
                         uint32_t tmo_ms)
{
   /*
    * 用"已过去多少"做比较, 不用绝对 deadline。sm_now_ms() 是 32 位毫秒计数,
    * 约 49.7 天回绕一次; 若写成 now >= t0 + tmo, 当 t0 落在回绕点前 tmo 之内
    * 时小端会立刻成立, 等待窗口直接塌成 0 —— 健康的轴会被判成"没停住"。
    * 差值法在回绕处依然正确 (只要间隔远小于 2^31 ms)。
    */
   uint32_t t0 = sm_now_ms();

   for (;;)
   {
      uint16_t sw = 0;
      int32_t  ab = 0;

      if (sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &sw,
                    SM_SDO_TMO_MOTION, &ab) == SM_RD_OK)
      {
         if ((sw & mask) == want)
            return 1;
      }
      if ((int32_t)(sm_now_ms() - t0) >= (int32_t)tmo_ms)
         return 0;
   }
}

/*
 * 读 6041h 判断是否已脱离 Operation enabled。
 * 返回 1 = 已脱离; 0 = 仍在使能; -1 = 读不到 (读不到**不能**当成功)。
 */
static int guard_is_disabled(sm_axis_t *ax)
{
   uint16_t sw = 0;
   int32_t  ab = 0;

   if (sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &sw,
                 SM_SDO_TMO_MOTION, &ab) != SM_RD_OK)
      return -1;
   return ((sw & SM_SW_OP_ENABLED) == 0) ? 1 : 0;
}

/*
 * 写一个参数并**回读确认**。
 *
 * 不能只看 guard_write 的返回值 —— 见 guard_force_disable 的注释: 这份 SOEM
 * 在加急写路径上会把从站的 SDO abort 帧当成成功。返回值 1 = 已回读确认一致。
 */
static int guard_write_verified(int slave, uint16_t index, uint8_t sub,
                                int size, const void *p, const char *why)
{
   uint8_t rb[8];
   int     rsz = 0;
   int32_t ab = 0;

   if (guard_write(slave, index, sub, size, p, why) != 0)
      return 0;

   if (sm_rd_raw(slave, index, sub, SM_SDO_TMO_MOTION, rb, &rsz, &ab)
       != SM_RD_OK || rsz != size)
   {
      printf("      [WARN] %s 写入后无法回读确认 (对象不可读或已失联)。\n",
             (why != NULL) ? why : "");
      return 0;
   }
   if (memcmp(rb, p, (size_t)size) != 0)
   {
      printf("      [WARN] %s 写入后回读不一致 —— 该写可能被驱动器拒绝。\n",
             (why != NULL) ? why : "");
      return 0;
   }
   return 1;
}

/*
 * 把一根轴强制降到失能态, 并回读确认, 必要时重试。
 *
 * 为什么必须回读: 本仓库这份 SOEM 的 ecx_SDOwrite 在**加急**路径 (psize<=4)
 * 上把从站回的 SDO abort 帧也当成成功 —— abort 响应的 mbxtype/service/index/
 * subindex 与请求完全相同, 于是命中 ec_coe.c 里 "all OK" 那个分支, 既不压
 * 错误栈也不置 ctx.ecaterror, wkc 还是 > 0。而 6040h 正好是 2 字节的加急写:
 * 也就是说"写失能"有没有真的进驱动器, 唯一的证据是回读 6041h。
 * (这份 SOEM 是下载的第三方库, 按约定不改它, 所以在本层补上回读。)
 *
 * 返回 1 = 已确认失能; 0 = 未能确认 (调用者必须当成严重失败上报)。
 */
static int guard_force_disable(sm_axis_t *ax)
{
   int attempt;

   for (attempt = 0; attempt < 3; attempt++)
   {
      /*
       * 每一步都走完整顺序。这里**不**用 g_guard.ds402_enabled 做条件: 那是
       * 一个全局标志, 处理完第一根轴就被清成 0, 第二根轴就会从 Operation
       * enabled 直接跳到 Shutdown。那虽也是合法降级路径, 但"每根轴都按完整
       * 顺序降级"更可预期。对本来就没使能的轴多写一次 0x0007 是无害的。
       */
      guard_set_cw(ax, SM_CW_SWITCHON, "收尾 Disable operation");
      guard_wait_sw(ax, SM_SW_OP_ENABLED, 0, SM_TEARDOWN_TMO_MS);

      guard_set_cw(ax, SM_CW_SHUTDOWN, "收尾 Shutdown");
      guard_wait_sw(ax, SM_SW_OP_ENABLED, 0, SM_TEARDOWN_TMO_MS);

      guard_set_cw(ax, SM_CW_DISABLE_V, "收尾 Disable voltage");

      if (guard_is_disabled(ax) == 1)
         return 1;

      if (attempt < 2)
         printf("      [重试] 轴%d 仍报告 Operation enabled, 再降一次 "
                "(%d/3)。\n", ax->pos, attempt + 2);
   }

   g_guard.disable_unconfirmed = 1;
   printf("      [!! 严重] 轴%d 写完 6040h=0x0000 后 6041h 仍报告 "
          "Operation enabled —— 电机可能仍然带电。\n", ax->pos);
   printf("      [!! 严重] 请立即按物理急停或断开驱动器电源确认。\n");
   return 0;
}

/*
 * 急停: 按"最快停住"的顺序尝试, 每步短超时, best-effort。
 * 只写去使能方向的值 —— 结构上不可能让轴动起来。
 */
void sm_guard_emergency_stop(sm_axis_t *axes, int nslaves)
{
   int i;

   printf("\n  [急停] 尝试安全停机 ...\n");
   for (i = 0; i < nslaves; i++)
   {
      sm_axis_t *ax = &axes[i];

      /* 只停我们接管过的轴: 别的轴我们没检查过它的状态, 贸然写 0x0000
         可能把另一套程序正在控制的轴拽停。 */
      if (!ax->is_ykd || !ax->engaged)
         continue;

      /* quick stop 依赖驱动器内置的减速 (605Ah), 先试它 */
      guard_set_cw(ax, SM_CW_QUICKSTOP, "急停 Quick stop");
      if (guard_wait_sw(ax, SM_SW_OP_ENABLED, 0, SM_TEARDOWN_TMO_MS))
      {
         printf("      轴%d 已脱离 Operation enabled。\n", ax->pos);
         continue;
      }

      /* quick stop 没有确认脱离就硬断电压 —— 宁可断得粗暴, 不能留着使能 */
      guard_set_cw(ax, SM_CW_DISABLE_V, "急停 Disable voltage");
      if (guard_is_disabled(ax) != 1)
      {
         g_guard.disable_unconfirmed = 1;
         printf("      [!! 严重] 轴%d 急停后 6041h 仍报告 Operation enabled "
                "—— 请立即按物理急停或断电。\n", ax->pos);
      }
   }
}

void sm_guard_teardown(sm_axis_t *axes, int nslaves)
{
   int i;

   if (g_guard.teardown_done)
      return;
   g_guard.teardown_done = 1;

   if (!g_guard.wrote_anything)
   {
      /* 一个字节都没写过, 无需恢复, 也不打印噪音 */
      g_guard.ds402_enabled = 0;
      return;
   }

   printf("\n---- 收尾 (无条件执行) ----\n");

   for (i = 0; i < nslaves; i++)
   {
      sm_axis_t *ax = &axes[i];
      int        restored = 0;
      int        n;

      /* 同上: 只清理自己接管过的轴 */
      if (!ax->is_ykd || !ax->engaged)
         continue;

      /* 1~3. 按 Quick stop 之外的标准顺序降级到失能, 并回读确认 */
      guard_force_disable(ax);
      g_guard.ds402_enabled = 0;

      /* 4. 恢复微动期间改过的 PP 参数 (逐项回读确认) */
      if (ax->have_prof_vel &&
          guard_write_verified(ax->slave, SM_OID_PROF_VEL, 0, 4,
                               &ax->prof_vel_snap, "恢复 6081h"))
         restored++;
      if (ax->have_prof_acc &&
          guard_write_verified(ax->slave, SM_OID_PROF_ACC, 0, 4,
                               &ax->prof_acc_snap, "恢复 6083h"))
         restored++;
      if (ax->have_prof_dec &&
          guard_write_verified(ax->slave, SM_OID_PROF_DEC, 0, 4,
                               &ax->prof_dec_snap, "恢复 6084h"))
         restored++;
      /* 5. 恢复操作模式 */
      if (ax->have_mode)
      {
         uint8_t m = (uint8_t)ax->mode_snap;
         if (guard_write_verified(ax->slave, SM_OID_MODES, 0, 1,
                                  &m, "恢复 6060h"))
            restored++;
      }

      n = (ax->have_prof_vel ? 1 : 0) + (ax->have_prof_acc ? 1 : 0) +
          (ax->have_prof_dec ? 1 : 0) + (ax->have_mode ? 1 : 0);

      printf("  轴%d: 6040h=0x0000 (失能), 已回读确认恢复 %d/%d 项参数。\n",
             ax->pos, restored, n);
      if (restored != n)
         printf("      注意: 有参数未能确认恢复, 该轴下次上电前请核对 "
                "6081h/6083h/6084h/6060h。\n");
   }
}
