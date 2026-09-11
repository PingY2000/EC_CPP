/*
 * slide_motion.c - 主程序: 参数解析 / 阶段编排 / 报告 / CSV / 退出码收口
 *
 * 阶段:
 *   S0 身份与总线门禁      (只读)
 *   S1 参数基线与漂移      (只读)
 *   S2 OP 循环 + DC        (本版未实现, 只在阶段编号上留位置)
 *   S3 使能状态机          (需 --allow-motion)
 *   S4 微动 + 反馈闭环     (需 --allow-jog)
 *   S5 收尾 / 安全停机     (无条件执行, 含所有错误路径)
 *build\slide_motion\slide_motion.exe "\Device\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}" --allow-motion
 * 本文件**不含任何 ecx_SDOwrite** —— 所有写都经由 sm_guard.c。
 *
 * 编排上的两条硬规矩:
 *   1. 所有门禁 (授权 / 身份门 / dangerous 漂移 / 逐轴前置检查) 都在
 *      **第一次写之前** 全部跑完, 且逐轴前置检查是"全过才动":
 *      这样"拒绝"这个结论永远等价于"一个字节都没写", 报告可以干净地说清楚。
 *   2. sm_guard_teardown() 在总线打开之后的每一条退出路径上都会被调用一次
 *      (用 goto out_bus 统一收口)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ======================================================================
 * 命令行选项
 * ====================================================================== */
typedef struct
{
   const char *ifname;
   const char *baseline_path;
   const char *dump_path;
   const char *csv_path;
   int         dry_run;
   int         want_motion;
   int         want_jog;
   int         axis_only;      /* -1 = 全部 */
   int         repeats;
   int         have_tol;
   int32_t     tol;
   int64_t     raw_jog, raw_vel, raw_acc, raw_dec, raw_tmo, raw_wd;
} sm_opts_t;

static void sm_usage(void)
{
   printf(
"\n用法:\n"
"  slide_motion <ifname> [选项]\n"
"\n"
"  ifname = 网卡名, 如 \\Device\\NPF_{GUID} (不给则只列出可用网卡)\n"
"\n"
"只读阶段 (默认, 永远安全):\n"
"  --baseline=<文件>       与基线文件比对参数漂移 (S1)\n"
"  --dump-baseline=<文件>  把现场实测参数导出为基线文件后退出 (不做比对/不动作)\n"
"  --dry-run               预演 S3/S4 将要写什么, 一个字节都不写\n"
"  --axis=<n>              只处理总线位置 n 的轴 (默认全部 YKD 轴)\n"
"  --state=pre-op|safe-op  把从站抬到哪个 AL 状态 (默认 pre-op)\n"
"\n"
"动作阶段 (必须显式授权; 会真的让电机通电并移动):\n"
"  --allow-motion          允许 S3 使能 (电机通电)\n"
"  --allow-jog             允许 S4 微动 (电机移动), 必须与 --allow-motion 同给\n"
"  --yes                   跳过交互确认 (给自动化用; 交互确认默认开启)\n"
"  --reset-fault           故障态时尝试 6040h bit7 故障复位\n"
"  --allow-foreign         允许总线上存在非 YKD 从站 (默认拒绝动作)\n"
"  --force-caps            允许突破硬上限 (危险, 需要自己清楚在做什么)\n"
"\n"
"动作参数 (未给就用默认值; 一律受硬上限约束):\n"
"  --jog=<脉冲>            单次微动行程 (默认 %d, 上限 %d)\n"
"  --vel=<脉冲/s>          微动速度 (默认 %d, 上限 %d)\n"
"  --accel=<脉冲/s^2>      加速度 (默认 %d, 上限 %d)\n"
"  --decel=<脉冲/s^2>      减速度 (默认同加速度)\n"
"  --move-tmo=<ms>         单腿动作超时 (默认 %d, 上限 %d)\n"
"  --repeats=<n>           往返重复轮数 (默认 %d, 上限 %d)\n"
"  --tol=<脉冲>            行程/归位容差 (默认取命令行程的 2%%)\n"
"  --watchdog=<ms>         心跳看门狗 (默认 %d, 允许 %d..%d)\n"
"\n"
"报告:\n"
"  --csv=<文件>            把身份/参数/轨迹/微动结果导出为 CSV\n"
"  -h, --help              显示本帮助\n"
"\n"
"退出码:\n"
"  0 通过   1 用法/初始化失败   2 空总线   3 存在 FAIL   4 仅 WARN\n"
"  5 无 YKD 从站   6 安全护栏拒绝(未写任何东西)\n"
"  7 动作中途异常中止(已安全停机)   8 基线文件错误   9 未达使能态\n"
"  10 收尾后仍无法确认电机失能 (电机可能带电; 覆盖其它码) —— 立即断电确认\n",
          SM_JOG_PULSES_DEF, SM_JOG_PULSES_MAX,
          SM_JOG_VEL_DEF, SM_JOG_VEL_MAX,
          SM_JOG_ACC_DEF, SM_JOG_ACC_MAX,
          SM_MOVE_TMO_DEF, SM_MOVE_TMO_MAX,
          SM_REPEATS_DEF, SM_REPEATS_MAX,
          SM_WATCHDOG_DEF, SM_WATCHDOG_MIN, SM_WATCHDOG_MAX);
}

/* 匹配 "--name" / "--name=value"。返回 1 = 命中; *val 为 NULL 表示没带 "=" */
static int sm_opt_match(const char *arg, const char *name, const char **val)
{
   size_t n = strlen(name);

   if (strncmp(arg, name, n) != 0)
      return 0;
   if (arg[n] == '=')
   {
      *val = arg + n + 1;
      return 1;
   }
   if (arg[n] == '\0')
   {
      *val = NULL;
      return 1;
   }
   return 0;
}

/* 需要带值的选项; 缺值就报错 */
static int sm_need_val(const char *name, const char *v, const char **out)
{
   if (v == NULL || *v == '\0')
   {
      printf("选项 %s 需要取值, 例如 %s=<值>。\n", name, name);
      return -1;
   }
   *out = v;
   return 0;
}

/*
 * int64 -> int32 的收窄。
 *
 * 直接 (int32_t) 转换在超出值域时是实现定义的行为, 实践里多为截断 ——
 * `--jog=4294967496` 会变成 200, 也就是"用户想要一次超大行程, 却静默地
 * 得到默认行程"。这类静默变化在任何方向上都不可取, 所以显式夹住: 超出
 * 值域就保持"超出值域", 让后面的上限检查照常收紧它。
 */
static int32_t sm_narrow_i32(int64_t v)
{
   if (v > 2147483647LL)
      return 2147483647;
   if (v < -2147483648LL)
      return (int32_t)(-2147483647 - 1);
   return (int32_t)v;
}

static uint32_t sm_narrow_u32(int64_t v)
{
   if (v < 0)
      return 0;
   if (v > 4294967295LL)
      return 4294967295u;
   return (uint32_t)v;
}

static int sm_parse_args(int argc, char *argv[], sm_opts_t *o)
{
   int i;

   memset(o, 0, sizeof(*o));
   o->axis_only = -1;
   o->repeats = SM_REPEATS_DEF;

   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];
      const char *v = NULL;

      if (strcmp(a, "-h") == 0 || sm_opt_match(a, "--help", &v))
      {
         sm_usage();
         exit(SM_EXIT_OK);
      }
      else if (sm_opt_match(a, "--baseline", &v))
      {
         if (sm_need_val("--baseline", v, &o->baseline_path) != 0) return -1;
      }
      else if (sm_opt_match(a, "--dump-baseline", &v))
      {
         if (sm_need_val("--dump-baseline", v, &o->dump_path) != 0) return -1;
      }
      else if (sm_opt_match(a, "--csv", &v))
      {
         if (sm_need_val("--csv", v, &o->csv_path) != 0) return -1;
      }
      else if (sm_opt_match(a, "--axis", &v))
      {
         if (sm_need_val("--axis", v, &v) != 0) return -1;
         o->axis_only = (int)strtol(v, NULL, 0);
         if (o->axis_only < 0 || o->axis_only >= SM_MAX_AXES)
         {
            printf("--axis 取值 %d 超出 0..%d。\n", o->axis_only,
                   SM_MAX_AXES - 1);
            return -1;
         }
      }
      else if (sm_opt_match(a, "--state", &v))
      {
         if (sm_need_val("--state", v, &v) != 0) return -1;
         if (strcmp(v, "pre-op") == 0)       g_guard.requested_state = SM_STATE_PRE_OP;
         else if (strcmp(v, "safe-op") == 0) g_guard.requested_state = SM_STATE_SAFE_OP;
         else
         {
            printf("--state 只能是 pre-op 或 safe-op, 收到 \"%s\"。\n", v);
            return -1;
         }
      }
      else if (sm_opt_match(a, "--jog", &v))
      {
         if (sm_need_val("--jog", v, &v) != 0) return -1;
         o->raw_jog = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--vel", &v))
      {
         if (sm_need_val("--vel", v, &v) != 0) return -1;
         o->raw_vel = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--accel", &v))
      {
         if (sm_need_val("--accel", v, &v) != 0) return -1;
         o->raw_acc = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--decel", &v))
      {
         if (sm_need_val("--decel", v, &v) != 0) return -1;
         o->raw_dec = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--move-tmo", &v))
      {
         if (sm_need_val("--move-tmo", v, &v) != 0) return -1;
         o->raw_tmo = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--watchdog", &v))
      {
         if (sm_need_val("--watchdog", v, &v) != 0) return -1;
         o->raw_wd = strtoll(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--repeats", &v))
      {
         if (sm_need_val("--repeats", v, &v) != 0) return -1;
         o->repeats = (int)strtol(v, NULL, 0);
      }
      else if (sm_opt_match(a, "--tol", &v))
      {
         if (sm_need_val("--tol", v, &v) != 0) return -1;
         o->tol = (int32_t)strtol(v, NULL, 0);
         o->have_tol = 1;
      }
      else if (sm_opt_match(a, "--dry-run", &v))     o->dry_run = 1;
      else if (sm_opt_match(a, "--allow-motion", &v)) o->want_motion = 1;
      else if (sm_opt_match(a, "--allow-jog", &v))   o->want_jog = 1;
      else if (sm_opt_match(a, "--yes", &v))         g_guard.non_interactive = 1;
      else if (sm_opt_match(a, "--reset-fault", &v)) g_guard.reset_fault = 1;
      else if (sm_opt_match(a, "--allow-foreign", &v)) g_guard.allow_foreign = 1;
      else if (sm_opt_match(a, "--force-caps", &v))  g_guard.force_caps = 1;
      else if (a[0] == '-')
      {
         printf("未知选项: %s (用 --help 看用法)\n", a);
         return -1;
      }
      else if (o->ifname == NULL)
      {
         o->ifname = a;
      }
      else
      {
         printf("多余的位置参数: %s\n", a);
         return -1;
      }
   }

   if (o->repeats < 1)
   {
      printf("--repeats 至少为 1。\n");
      return -1;
   }
   if (o->repeats > SM_REPEATS_MAX)
   {
      printf("--repeats 上限 %d, 已收紧为 %d。\n", SM_REPEATS_MAX, SM_REPEATS_MAX);
      o->repeats = SM_REPEATS_MAX;
   }
   if (o->dry_run && o->want_jog)
   {
      printf("--dry-run 与 --allow-jog 同时给出没有意义: 预演不写任何东西。\n");
      return -1;
   }
   return 0;
}

/* ======================================================================
 * CSV 导出 (可选)
 * ====================================================================== */
static FILE *g_csv;

#define CSV(...) do { if (g_csv != NULL) fprintf(g_csv, __VA_ARGS__); } while (0)

/* ======================================================================
 * 报告
 * ====================================================================== */
static void sm_report_identity_header(void)
{
   printf("%-5s %-5s %-8s %-12s %-11s %-11s %-11s %-6s %s\n",
          "位置", "序号", "配置地址", "别名0012h", "厂商ID", "产品码",
          "修订", "状态", "名称");
   printf("%-5s %-5s %-8s %-12s %-11s %-11s %-11s %-6s %s\n",
          "-----", "-----", "--------", "------------", "----------",
          "----------", "----------", "------", "----");
}

/*
 * 渲染一条轨迹里的 6041h。没读到 -> "----"。
 *
 * 不能用 0x0000 顶替: 那是 CiA402 合法的 "Not ready to switch on", 会把
 * "我们没读到" 伪装成 "驱动器报了个状态", 读报告的人会去查一个不存在的故障。
 * CSV 同理写 "----" 不写空字段 —— 空字段和输出被截断分不开, "----" 自解释
 * 且搜得出来。缓冲由调用者给 (报告里一处 printf 只用一次)。
 */
static void trace_sw_str(char *dst, size_t n, const sm_trace_t *t)
{
   if (t->sw_valid)
      snprintf(dst, n, "0x%04X", (unsigned)t->sw);
   else
      snprintf(dst, n, "----");
}

static void sm_report_s3(const sm_axis_t *ax)
{
   int i;

   printf("\n  ---- 位置 %d: 使能状态字迁移轨迹 ----\n", ax->pos);
   if (ax->trace_n == 0)
   {
      printf("  (无轨迹记录)\n");
      return;
   }
   printf("  %-4s %-9s %-9s %-10s\n", "步", "写 6040h", "实测 6041h", "相对 ms");
   printf("  %-4s %-9s %-9s %-10s\n", "----", "-------", "-------", "-------");
   for (i = 0; i < ax->trace_n; i++)
   {
      char sws[8];

      trace_sw_str(sws, sizeof(sws), &ax->trace[i]);
      printf("  %-4d 0x%04X    %-8s  %u\n",
             ax->trace[i].step, (unsigned)ax->trace[i].cw,
             sws, (unsigned)ax->trace[i].ms);
      CSV("S3,%d,%d,%u,0x%04X,%s\n", ax->pos, ax->trace[i].step,
          (unsigned)ax->trace[i].ms, (unsigned)ax->trace[i].cw, sws);
   }
}

static void sm_report_s4(const sm_axis_t *ax, int32_t cmd, int32_t tol)
{
   if (!ax->jog_done)
   {
      printf("\n  ---- 位置 %d: 微动结果 ----\n  (未执行, 无数据)\n", ax->pos);
      return;
   }

   printf("\n  ---- 位置 %d: 微动与反馈闭环结果 ----\n", ax->pos);
   printf("  %-6s %-11s %-9s %-11s %-9s %-8s %-10s %-9s\n",
          "腿", "起点 6064h", "命令", "终点 6064h", "实测 Δ",
          "误差", "峰值 606Ch", "耗时 ms");
   printf("  %-6s %-11s %-9s %-11s %-9s %-8s %-10s %-9s\n",
          "------", "-----------", "---------", "-----------", "---------",
          "--------", "----------", "--------");
   printf("  %-6s %-11d %-+9d %-11d %-+9d %-+8d %-10d %-9u\n",
          "正向", (int)ax->p_start, (int)cmd, (int)ax->p_fwd_end,
          (int)ax->d_fwd, (int)ax->travel_err, (int)ax->peak_fwd,
          (unsigned)ax->ms_fwd);
   printf("  %-6s %-11d %-+9d %-11d %-+9d %-+8d %-10d %-9u\n",
          "反向", (int)ax->p_fwd_end, (int)-cmd, (int)ax->p_rev_end,
          (int)ax->d_rev, (int)ax->home_err, (int)ax->peak_rev,
          (unsigned)ax->ms_rev);
   printf("  容差 ±%d 脉冲; 方向冲突次数 %d\n", (int)tol, ax->dir_violations);

   /* 列: 段,位置,起点,正向终点,反向终点,命令Δ,实测Δ正,实测Δ反,
          行程误差,归位误差,峰值正,峰值反,耗时正,耗时反 */
   CSV("S4,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u\n",
       ax->pos, (int)ax->p_start, (int)ax->p_fwd_end, (int)ax->p_rev_end,
       (int)cmd, (int)ax->d_fwd, (int)ax->d_rev,
       (int)ax->travel_err, (int)ax->home_err,
       (int)ax->peak_fwd, (int)ax->peak_rev,
       (unsigned)ax->ms_fwd, (unsigned)ax->ms_rev);
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char *argv[])
{
   sm_opts_t     o;
   sm_baseline_t base;
   sm_axis_t     axes[SM_MAX_AXES];
   int           cnt, i, slave;
   int           n_ykd = 0, n_foreign = 0;
   int           axis_ok = 0;
   int           base_fail = 0, base_warn = 0, dangerous_drift = 0;
   int           motion_fail = 0, motion_warn = 0, enable_failed = 0, aborted = 0;
   int           got_refused = 0;
   int           mv[SM_MAX_AXES];
   int           verify_rc;
   int32_t       jog_pulses, tol;
   uint32_t      vel, acc, dec, move_tmo, watchdog;
   int           exit_code = SM_EXIT_OK;

   sm_console_utf8();
   g_guard.requested_state = SM_STATE_PRE_OP;
   g_guard.watchdog_ms = SM_WATCHDOG_DEF;

   printf("slide_motion - 滑台带动作验收验证 (SOEM, S0/S1 只读 + S3/S4 可选动作)\n");
   printf("本工具不会写 2102h (EEPROM), 不会改 PDO 映射, 不会写软限位。\n");

   if (sm_parse_args(argc, argv, &o) != 0)
   {
      sm_usage();
      return SM_EXIT_USAGE;
   }

   /* ---- 授权门 (在任何总线操作之前) ---- */
   g_guard.authorized_motion = o.want_motion;
   g_guard.authorized_jog    = o.want_jog;

   verify_rc = sm_guard_check_authorization(o.want_motion, o.want_jog);
   if (verify_rc != 0)
      return verify_rc;



   if (o.ifname == NULL)
   {
      printf("\n未指定网卡。可用网卡:\n");
      sm_print_adapters();
      sm_usage();
      return SM_EXIT_USAGE;
   }

   /* ---- CSV (可选) ---- */
   if (o.csv_path != NULL)
   {
      g_csv = fopen(o.csv_path, "w");
      if (g_csv == NULL)
      {
         printf("CSV 文件写不开: %s (目录不存在/无权限?)\n", o.csv_path);
         return SM_EXIT_USAGE;
      }
      /* 各段列数不同, 所以不写统一表头; 每行首列是段名 (S0/S3/S4), 按段解析 */
      CSV("# slide_motion 报告, 首列为段名: S0 身份 / S3 使能轨迹 / S4 微动结果\n");
   }

   /* ---- 上限收紧 (只能收紧, 除非 --force-caps) ---- */
   jog_pulses = sm_guard_clamp_i32("--jog", sm_narrow_i32(o.raw_jog),
                                   SM_JOG_PULSES_MAX, SM_JOG_PULSES_DEF);
   vel = sm_guard_clamp_u32("--vel", sm_narrow_u32(o.raw_vel),
                            SM_JOG_VEL_MAX, SM_JOG_VEL_DEF);
   acc = sm_guard_clamp_u32("--accel", sm_narrow_u32(o.raw_acc),
                            SM_JOG_ACC_MAX, SM_JOG_ACC_DEF);
   dec = sm_guard_clamp_u32("--decel", sm_narrow_u32(o.raw_dec),
                            SM_JOG_ACC_MAX, acc);
   move_tmo = sm_guard_clamp_u32("--move-tmo", sm_narrow_u32(o.raw_tmo),
                                 SM_MOVE_TMO_MAX, SM_MOVE_TMO_DEF);
   watchdog = sm_guard_clamp_u32("--watchdog", sm_narrow_u32(o.raw_wd),
                                 SM_WATCHDOG_MAX, SM_WATCHDOG_DEF);
   if (watchdog < SM_WATCHDOG_MIN)
   {
      printf("  [上限] 看门狗 %ums 低于下限 %dms, 已抬到 %dms "
             "(低于下限会把自己的慢轮询误判成掉线)。\n",
             (unsigned)watchdog, SM_WATCHDOG_MIN, SM_WATCHDOG_MIN);
      watchdog = SM_WATCHDOG_MIN;
   }
   g_guard.watchdog_ms = watchdog;

   if (o.want_motion && sm_guard_check_caps(watchdog) != 0)
      return SM_EXIT_USAGE;

   /* ---- 基线文件 (只读, 与总线无关, 先解析以便早报错) ---- */
   memset(&base, 0, sizeof(base));
   if (o.baseline_path != NULL)
   {
      verify_rc = sm_base_load(o.baseline_path, &base);
      if (verify_rc != 0)
         return verify_rc;
      printf("已载入基线文件: %s", base.path);
      if (base.warn_count > 0)
         printf(" (%d 处警告, 详见上面输出)", base.warn_count);
      printf("\n");
   }
   if (base.have_jp && o.raw_jog == 0)
   {
      jog_pulses = sm_guard_clamp_i32("[motion]jog_pulses", sm_narrow_i32(base.jp),
                                      SM_JOG_PULSES_MAX, SM_JOG_PULSES_DEF);
      printf("  [基线] 采用基线里的行程 %d 脉冲\n", (int)jog_pulses);
   }
   if (base.have_jv && o.raw_vel == 0)
   {
      vel = sm_guard_clamp_u32("[motion]jog_velocity", sm_narrow_u32(base.jv),
                               SM_JOG_VEL_MAX, SM_JOG_VEL_DEF);
      printf("  [基线] 采用基线里的速度 %u\n", (unsigned)vel);
   }
   if (base.have_ja && o.raw_acc == 0)
   {
      acc = sm_guard_clamp_u32("[motion]jog_accel", (uint32_t)base.ja,
                               SM_JOG_ACC_MAX, SM_JOG_ACC_DEF);
      dec = acc;
      printf("  [基线] 采用基线里的加速度 %u\n", (unsigned)acc);
   }

   /* 容差: CLI > 基线百分比 > 命令行程的 2% */
   if (o.have_tol)
      tol = o.tol;
   else
   {
      /*
       * 百分比也要夹住。容差是"实测行程与命令行程允许差多少", 把它放到比
       * 行程本身还大, A1/A6 两条断言就永远成立 —— 等于把"动得对不对"的
       * 检查整条删掉, 而运行日志还会显示 PASS。上限 50% 已经宽到不像验收,
       * 再宽就不是在测精度了。
       */
      int32_t pct = base.have_tol ? (int32_t)base.tol : 2;
      int32_t a = (jog_pulses < 0) ? -jog_pulses : jog_pulses;

      if (pct < 0)
         pct = 0;
      if (pct > 50)
      {
         printf("  [上限] [motion]tolerance_pct = %d%% 超过上限 50%%, 已收紧为 50%%。\n",
                (int)pct);
         pct = 50;
      }
      tol = (int32_t)(((int64_t)a * pct) / 100);
   }
   if (tol < SM_POS_NOISE_PULSES)
      tol = SM_POS_NOISE_PULSES;

   /*
    * 绝对上限也用上 (--force-caps 方能越过), 并且不能超过行程的 1/4 ——
    * 容差比行程还大时 A1 恒真, 这个上限保证断言始终还有意义。
    */
   tol = sm_guard_clamp_i32("--tol", tol, SM_TOL_PULSES_MAX, tol);
   {
      int64_t a = (jog_pulses < 0) ? -(int64_t)jog_pulses : (int64_t)jog_pulses;
      int64_t cap = a / 4;

      if (cap < SM_POS_NOISE_PULSES)
         cap = SM_POS_NOISE_PULSES;
      if ((int64_t)tol > cap)
      {
         printf("  [上限] 容差 ±%d 超过行程的 1/4 (±%d), 已收紧为 ±%d "
                "—— 容差大于行程时行程断言恒成立。\n",
                (int)tol, (int)cap, (int)cap);
         tol = (int32_t)cap;
      }
   }

   printf("\n生效参数: 行程 %d 脉冲, 速度 %u, 加/减速 %u/%u, 单腿超时 %ums, "
         "重复 %d 轮, 容差 ±%d, 看门狗 %ums\n",
         (int)jog_pulses, (unsigned)vel, (unsigned)acc, (unsigned)dec,
         (unsigned)move_tmo, o.repeats, (int)tol, (unsigned)watchdog);

   /* ---- 打开总线 ---- */
   sm_guard_install_ctrl_handler();

   if (!ecx_init(&g_ctx, o.ifname))
   {
      printf("\n无法打开网卡 %s (被独占 / 需管理员 / 驱动未装)。\n", o.ifname);
      return SM_EXIT_USAGE;
   }
   cnt = ecx_config_init(&g_ctx);
   if (cnt <= 0)
   {
      printf("\n总线上未发现从站 (config_init=%d), 检查网卡/网线/供电。\n", cnt);
      ecx_close(&g_ctx);
      return SM_EXIT_NO_SLAVE;
   }
   if (cnt > SM_MAX_AXES)
   {
      printf("\n从站数 %d 超过本工具支持上限 %d, 拒绝继续。\n", cnt, SM_MAX_AXES);
      ecx_close(&g_ctx);
      return SM_EXIT_USAGE;
   }

   printf("\n等待从站进入 PRE_OP ...\n");
   for (slave = 1; slave <= cnt; slave++)
      ecx_statecheck(&g_ctx, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   ecx_readstate(&g_ctx);

   /* ---- 逐台登记 ---- */
   memset(axes, 0, sizeof(axes));
   for (slave = 1; slave <= cnt; slave++)
   {
      const struct ec_slave *s = &g_ctx.slavelist[slave];
      sm_axis_t *ax = &axes[slave - 1];

      ax->slave = slave;
      ax->pos = slave - 1;
      ax->is_ykd = (char)sm_is_ykd(s->eep_man, s->eep_id);
      /*
       * PRE_OP 与 SAFE_OP 都算"可用": 两者都能跑 SDO 读写, 正是本工具需要的。
       * 只认 PRE_OP 的话, 上一次 --state=safe-op 的运行会把从站留在 SAFE_OP,
       * 于是**下一次**运行一进来就判 FAIL —— 状态是上一次自己留下的, 却成了
       * 这一次的失败。sm_enter_state() 之后还会按 requested_state 再收紧一次。
       */
      ax->reached_state = (char)(s->state == SM_STATE_PRE_OP ||
                                 s->state == SM_STATE_SAFE_OP);
      mv[slave - 1] = SM_V_SKIP;   /* 提前置好: 后面任何早退路径的汇总都要读它 */
      if (ax->is_ykd)
         n_ykd++;
      else
         n_foreign++;
   }
   

   /* ---- S0: 身份表与门禁 ---- */
   printf("\n---- S0: 身份表 ----\n");
   sm_report_identity_header();
   for (slave = 1; slave <= cnt; slave++)
   {
      const struct ec_slave *s = &g_ctx.slavelist[slave];
      printf("%-5d %-5d 0x%04X  %-12u 0x%08X  0x%08X  0x%08X  0x%02X  %s\n",
             slave - 1, slave, (unsigned)s->configadr, (unsigned)s->aliasadr,
             (unsigned)s->eep_man, (unsigned)s->eep_id, (unsigned)s->eep_rev,
             (unsigned)s->state, s->name);
      CSV("S0,%d,%d,0x%04X,%u,0x%08X,0x%08X,0x%08X,0x%02X,%s,%d\n",
          slave - 1, slave, (unsigned)s->configadr, (unsigned)s->aliasadr,
          (unsigned)s->eep_man, (unsigned)s->eep_id, (unsigned)s->eep_rev,
          (unsigned)s->state, s->name, (int)axes[slave - 1].is_ykd);
   }

   printf("\n---- S0: 身份判定与总线门禁 ----\n");
   if (n_ykd == 0)
   {
      printf("  总线上 %d 台从站, 没有一台是受支持的 YKD2205PE "
             "(厂商 0x%08X, 产品码 0x2000/0x3000)。\n", cnt, (unsigned)SM_YKD_VENDOR_ID);
      exit_code = SM_EXIT_NO_YKD;
      goto out_bus;
   }
   printf("  受支持 YKD2205PE: %d 台; 其它从站: %d 台\n", n_ykd, n_foreign);
   for (slave = 1; slave <= cnt; slave++)
   {
      const sm_axis_t *ax = &axes[slave - 1];
      int selected = (o.axis_only < 0) || (o.axis_only == ax->pos);

      if (!ax->is_ykd)
      {
         printf("  位置 %-3d [WARN] 非受支持从站, 动作阶段会跳过它。\n", ax->pos);
         continue;
      }
      if (!ax->reached_state)
      {
         const struct ec_slave *s = &g_ctx.slavelist[slave];
         printf("  位置 %-3d [FAIL] YKD2205PE 未进入 PRE_OP/SAFE_OP (状态 0x%02X, "
                "AL 状态码 0x%04X %s)\n", ax->pos, (unsigned)s->state,
                (unsigned)s->ALstatuscode,
                ec_ALstatuscode2string(s->ALstatuscode));
         base_fail = 1;
         continue;
      }
      printf("  位置 %-3d [PASS] YKD2205PE 滑台驱动器%s\n", ax->pos,
             selected ? "" : " (本轴未被 --axis 选中, 跳过)");
      if (selected)
         axis_ok++;
   }

   /* 是否允许进入写阶段: 身份门 */
   if (o.want_motion && n_foreign > 0 && !g_guard.allow_foreign)
   {
      printf("\n[拒绝] 总线上有 %d 台非受支持从站。默认拒绝对这样的总线执行写动作 ——\n"
             "       我们无法预知那些从站会对 SDO 写做出什么反应。\n"
             "       确认安全后可以加 --allow-foreign 强制继续。未写任何东西。\n",
             n_foreign);
      exit_code = SM_EXIT_REFUSED;
      goto out_bus;
   }
   if (o.want_motion && axis_ok == 0)
   {
      printf("\n[拒绝] 没有可执行动作的轴 (全部未进 PRE_OP 或被 --axis 排除)。"
             "未写任何东西。\n");
      exit_code = SM_EXIT_REFUSED;
      goto out_bus;
   }

   /* ---- 抬到目标 AL 状态 ---- */
   if (o.want_motion)
   {
      int bad = sm_enter_state(g_guard.requested_state, 1);

      if (bad > 0)
      {
         for (slave = 1; slave <= cnt; slave++)
         {
            sm_axis_t *ax = &axes[slave - 1];
            if (!ax->is_ykd)
               continue;
            if (g_ctx.slavelist[slave].state != (uint16)g_guard.requested_state)
               ax->reached_state = 0;
         }
         printf("\n[拒绝] %d 台从站未到达 %s。在该状态下继续写动作不安全。\n",
                bad,
                (g_guard.requested_state == SM_STATE_SAFE_OP) ? "SAFE_OP"
                                                              : "PRE_OP");
         exit_code = SM_EXIT_REFUSED;
         goto out_bus;
      }
   }
   else
   {
      /*
       * 只读运行**不**抬 AL 状态。
       *
       * ecx_writestate 写的是从站的 AL 控制寄存器, 而且作用于总线上每一台
       * 从站 —— 包括非 YKD 的第三方从站。之前 --dump-baseline / --dry-run
       * 也会走这里, 于是 `--dry-run --state=safe-op` 会把别人的从站一起推进
       * SAFE_OP, 与"一个字节都不写"的承诺自相矛盾 (虽然写的不是 SDO, 但对
       * 那台第三方从站来说同样是我们在动它)。
       *
       * config_init() 结束后从站本来就在 PRE_OP, 而只读路径需要的正是这个
       * 状态, 所以什么都不做就是正确的。这里只把"期望状态"的结论标出来。
       */
      for (slave = 1; slave <= cnt; slave++)
      {
         sm_axis_t *ax = &axes[slave - 1];
         if (!ax->is_ykd)
            continue;
         if (g_ctx.slavelist[slave].state != (uint16)g_guard.requested_state)
            ax->reached_state = 0;
      }
      printf("\n[只读] 未给 --allow-motion, 不改任何从站的 AL 状态 "
             "(保持 config_init 之后的 PRE_OP)。\n");
   }

   /* ---- 授权横幅 ---- */
   sm_guard_banner(o.ifname, cnt);

   /* ---- S1: 参数基线与漂移 ---- */
   if (o.baseline_path != NULL)
   {
      printf("\n---- S1: 参数基线比对 (相对 %s) ----\n", base.path);

      /* 身份一致性: 基线不属于这台设备就退出 8, 否则比对结论没有意义 */
      if (base.have_vendor || base.have_product)
      {
         for (slave = 1; slave <= cnt; slave++)
         {
            const struct ec_slave *s = &g_ctx.slavelist[slave];
            if (!axes[slave - 1].is_ykd)
               continue;
            if ((base.have_vendor && base.vendor_id != s->eep_man) ||
                (base.have_product && base.product_code != s->eep_id))
            {
               printf("  [错误] 基线文件的身份与现场设备不符:\n"
                      "         基线 vendor=0x%08X product=0x%08X\n"
                      "         现场 位置 %d vendor=0x%08X product=0x%08X\n"
                      "         该基线可能来自另一台/另一批次设备, 比对无意义。\n",
                      (unsigned)base.vendor_id, (unsigned)base.product_code,
                      axes[slave - 1].pos, (unsigned)s->eep_man,
                      (unsigned)s->eep_id);
               exit_code = SM_EXIT_BASELINE;
               goto out_bus;
            }
            break;
         }
      }

      sm_base_compare(&base, axes, cnt, &dangerous_drift);
   }
   else
   {
      printf("\n---- S1: 参数基线比对 ----\n"
             "  (未给 --baseline, 不做比对。下面是当前实测值表; "
             "可用 --dump-baseline=<文件> 导出一份基线。)\n");
      sm_base_show(axes, cnt);
   }

   for (i = 0; i < cnt; i++)
   {
      if (!axes[i].is_ykd)
         continue;
      if (axes[i].fail) base_fail = 1;
      if (axes[i].warn) base_warn = 1;
   }

   /* ---- S11: 导出基线后退出 (只读) ---- */
   if (o.dump_path != NULL)
   {
      verify_rc = sm_base_dump(o.dump_path, axes, cnt);
      if (verify_rc != 0)
         exit_code = verify_rc;
      goto out_bus;
   }

   /* ---- 预演: 打印将要写什么, 一个字节都不写 ---- */
   if (o.dry_run)
   {
      /* 先把 PP 参数读出来 —— 这样预演里"收尾会恢复成什么"是真实值而不是问号。 */
      for (i = 0; i < cnt; i++)
      {
         if (axes[i].is_ykd && axes[i].reached_state)
            sm_snapshot_pp(&axes[i]);
      }

      /* 顺带把前置检查也跑一遍 —— 它全程只读, 是"真动之前最值得先验证"的一段逻辑。
         这样 --dry-run 覆盖的是从身份门到前置检查的完整决策链, 而写入仍然为零。
         注意: 预演阶段的前置检查结论**不参与退出码** —— 它只是把真动时会得到的
         拒绝理由提前展示出来, 不算本次运行的失败。 */
      printf("\n---- 动作前置检查预演 (只读; 结论不计入退出码) ----\n");
      for (i = 0; i < cnt; i++)
      {
         if (!axes[i].is_ykd || !axes[i].reached_state)
            continue;
         if (o.axis_only >= 0 && o.axis_only != axes[i].pos)
            continue;
         sm_preflight(&axes[i], cnt, jog_pulses);
      }

      sm_stage_dry_run(axes, cnt, jog_pulses, vel, acc, dec, move_tmo, o.repeats);
      exit_code = base_fail ? SM_EXIT_FAIL : (base_warn ? SM_EXIT_WARN : SM_EXIT_OK);
      goto out_bus;
   }

   /* ---- 只读模式到此结束 ---- */
   if (!o.want_motion)
   {
      printf("\n---- S3/S4: 使能与微动 ----\n"
             "  (未给 --allow-motion, 跳过。这是默认的安全路径。)\n");
      exit_code = base_fail ? SM_EXIT_FAIL : (base_warn ? SM_EXIT_WARN : SM_EXIT_OK);
      goto out_bus;
   }

   /* ==================================================================
    * 从这里往下会真的写。所有门禁都必须在第一次写之前跑完。
    * ================================================================== */

   /* 门禁 1: dangerous 参数漂移 */
   if (dangerous_drift)
   {
      printf("\n[拒绝] 有 dangerous 参数漂移 (限位/原点输入功能定义或电子齿轮)。\n"
             "       这些参数决定 6040h/60FDh 的语义, 在它们不对时做动作可能让\n"
             "       限位失效或行程换算错误。请先修正参数或改用正确的基线文件。\n"
             "       未写任何东西。\n");
      exit_code = SM_EXIT_REFUSED;
      goto out_bus;
   }

   /* 门禁 2: 人在旁边 + 二次确认 */
   if (!sm_guard_confirm(o.ifname, cnt, axis_ok, jog_pulses, vel, acc, move_tmo,
                         watchdog))
   {
      printf("已取消, 未写任何东西。\n");
      exit_code = SM_EXIT_REFUSED;
      goto out_bus;
   }

   /* 门禁 3: 逐轴前置检查 (只读)。**全部通过才动** ——
      这样"拒绝"永远等价于"一个字节都没写"。 */
   {
      int32_t pf_delta = o.want_jog ? jog_pulses : 0;

      printf("\n---- 动作前置检查 (全程只读, 全部通过才会开始写) ----\n");
      for (slave = 1; slave <= cnt; slave++)
      {
         sm_axis_t *ax = &axes[slave - 1];

         if (!ax->is_ykd || !ax->reached_state)
            continue;
         if (o.axis_only >= 0 && o.axis_only != ax->pos)
            continue;

         verify_rc = sm_preflight(ax, cnt, pf_delta);
         if (verify_rc != 0)
            got_refused = 1;
      }
      if (got_refused)
      {
         printf("\n[拒绝] 前置检查未通过, 已放弃全部动作。未写任何东西。\n");
         exit_code = SM_EXIT_REFUSED;
         goto out_bus;
      }
   }

   /* ---- 动作前诊断计数 (用于对比动作是否伴随通信恶化) ---- */
   {
      uint32_t b_rx = 0, b_fr = 0, b_pe = 0, b_ll = 0;
      int first = -1;

      for (slave = 1; slave <= cnt && first < 0; slave++)
      {
         if (axes[slave - 1].is_ykd)
            first = slave;
      }
      if (first > 0 && sm_diag_counters(first, &b_rx, &b_fr, &b_pe, &b_ll) == 0)
         printf("\n  动作前诊断计数 (从站 %d): RxErr=%u FwdRxErr=%u PE=%u LostLink=%u\n",
                first, (unsigned)b_rx, (unsigned)b_fr, (unsigned)b_pe,
                (unsigned)b_ll);
   }



   /* ---- S3 / S4 ---- */
   for (i = 0; i < cnt; i++)
   {
      sm_axis_t *ax = &axes[i];
      int        v;


      mv[i] = SM_V_SKIP;

      if (!ax->is_ykd || !ax->reached_state)
         continue;
      if (o.axis_only >= 0 && o.axis_only != ax->pos)
         continue;

      /* 绝不在这里清 abort 标志 —— 两轴之间按下的 Ctrl-C 必须被下一轴看见。
         标志只由信号处理器/故障路径置位, 由 sm_guard_teardown 之后的退出收口。 */
      if (sm_guard_should_abort())
      {
         printf("\n[中止] 上一轴的测试被中止, 不再继续下一轴。\n");
         break;
      }

      v = sm_stage_enable(ax, SM_ENABLE_HOLD_MS);
      mv[i] = v;


      if (v == SM_V_FAIL)
      {
         enable_failed = 1;
         motion_fail = 1;
      }
      else if (v == SM_V_WARN)
      {
         /* S3 只到 WARN 意味着某一步没完全到位 —— 通常是 6060h 写了但 6061h
            回读不是 PP。此时**绝不能**进入 S4: 607Ah 与 6040h bit4 的语义依赖
            当前操作模式, 在未知模式下翻 bit4 可能触发完全不同的动作。
            "使能没完全成功就不动" —— 电机通电但不移动, 比动错方向安全得多。 */
         motion_warn = 1;
         printf("      [跳过] S3 未完全通过 (WARN), 不执行 S4 —— "
                "在操作模式未确认的情况下不发运动指令。\n");
      }
      else if (o.want_jog && !g_guard.abort)
      {
         v = sm_stage_jog(ax, jog_pulses, vel, acc, dec, move_tmo,
                          o.repeats, tol);
         mv[i] = v;
         if (v == SM_V_FAIL)
            motion_fail = 1;
         else if (v == SM_V_WARN)
            motion_warn = 1;
      }

      /* 本轴测完立刻去使能, 不让两台轴同时带电 */
      if (ax->enable_ok)
      {
         sm_set_cw(ax, SM_CW_SWITCHON, "本轴结束 去使能");
         sm_set_cw(ax, SM_CW_DISABLE_V, "本轴结束 断电压");
         ax->enable_ok = 0;
      }

      sm_report_s3(ax);
      if (o.want_jog)
         sm_report_s4(ax, jog_pulses, tol);

      if (g_guard.abort_reason != SM_ABORT_NONE)
      {
         printf("\n[中止] 中止原因: %s\n", sm_abort_str(g_guard.abort_reason));
         /* 立刻停机, 不等收尾 —— 故障/失速之后每多一毫秒都是电机还在带电的
            时间。sm_guard_emergency_stop 绕过授权检查, 只写"去使能"方向的值。 */
         sm_guard_emergency_stop(axes, cnt);
         break;
      }
   }

   /* 只要开始过动, 且中止原因不是 NONE, 就按"已安全停机"返回 7 */
   if (g_guard.wrote_anything && g_guard.abort_reason != SM_ABORT_NONE)
      aborted = 1;

   /* ---- 动作后诊断计数 ---- */
   {
      uint32_t a_rx = 0, a_fr = 0, a_pe = 0, a_ll = 0;
      int first = -1;

      for (slave = 1; slave <= cnt && first < 0; slave++)
      {
         if (axes[slave - 1].is_ykd)
            first = slave;
      }
      if (first > 0 && sm_diag_counters(first, &a_rx, &a_fr, &a_pe, &a_ll) == 0)
         printf("\n  动作后诊断计数 (从站 %d): RxErr=%u FwdRxErr=%u PE=%u LostLink=%u\n",
                first, (unsigned)a_rx, (unsigned)a_fr, (unsigned)a_pe,
                (unsigned)a_ll);
   }

   /* ---- 退出码收口 ---- */
   if (aborted)
      exit_code = SM_EXIT_ABORTED;
   else if (enable_failed)
      exit_code = SM_EXIT_NO_ENABLE;
   else if (motion_fail || base_fail)
      exit_code = SM_EXIT_FAIL;
   else if (motion_warn || base_warn || n_foreign > 0)
      exit_code = SM_EXIT_WARN;
   else
      exit_code = SM_EXIT_OK;

out_bus:
   /* 无条件收尾: 让 6040h 回到失能态, 并恢复被临时改过的参数 */
   sm_guard_teardown(axes, cnt);

   /*
    * 收尾之后才能判断 6040h=0 到底有没有生效 (要回读 6041h)。这一条**覆盖**
    * 其它所有退出码: 电机可能还在带电, 比"某个参数比对不过"严重得多, 不能
    * 被一个 3 或 4 盖过去。
    */
   if (g_guard.disable_unconfirmed)
      exit_code = SM_EXIT_NOT_DISABLED;

   ecx_close(&g_ctx);

   /* ---- 汇总 ---- */
   printf("\n==================== 汇总 ====================\n");
   printf("  总线从站总数 : %d\n", cnt);
   printf("  受支持 YKD   : %d\n", n_ykd);
   printf("  非 YKD 从站  : %d\n", n_foreign);

   if (o.want_motion)
   {
      int a_pass = 0, a_warn = 0, a_fail = 0, a_skip = 0;

      for (i = 0; i < cnt; i++)
      {
         if (!axes[i].is_ykd || !axes[i].reached_state)
            continue;
         switch (mv[i])
         {
            case SM_V_PASS: a_pass++; break;
            case SM_V_WARN: a_warn++; break;
            case SM_V_FAIL: a_fail++; break;
            default:        a_skip++; break;
         }
      }
      printf("  S3/S4 逐轴   : %d PASS, %d WARN, %d FAIL, %d SKIP\n",
             a_pass, a_warn, a_fail, a_skip);
      if (g_guard.abort_reason != SM_ABORT_NONE)
         printf("  中止原因     : %s\n", sm_abort_str(g_guard.abort_reason));
      if (g_guard.disable_unconfirmed)
         printf("  6040h 状态   : [!! 未能确认失能] 收尾写了 0 但 6041h 仍报 "
                "Operation enabled —— 电机可能带电, 请立即按物理急停/断电。\n");
      else
         printf("  6040h 状态   : %s\n",
                g_guard.wrote_anything
                   ? "已收尾至失能态, 并经 6041h 回读确认 (见上面收尾日志)"
                   : "未写过 (本次没有发生写操作)");
   }
   else
   {
      printf("  S3/S4        : 未执行 (需 --allow-motion)\n");
   }
   if (o.baseline_path != NULL)
      printf("  S1 基线比对  : %s\n",
             dangerous_drift ? "存在 dangerous 漂移"
                             : (base_fail ? "存在 FAIL" : (base_warn ? "存在 WARN" : "全部通过")));
   printf("  退出码       : %d\n", exit_code);
   printf("==============================================\n");

   if (g_csv != NULL)
   {
      fclose(g_csv);
      printf("CSV 已写出: %s\n", o.csv_path);
   }
   return exit_code;
}
