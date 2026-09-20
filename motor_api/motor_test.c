/*
 * motor_test.c - 调用 motor_api 的验收程序: PRE_OP 补映射 -> config_map_group -> 逐轴证明偏移
 * -> SAFE_OP -> OP -> 6040h 走 RxPDO / 6041h 读 TxPDO。比最小 PDO 例子多的三件事: 多轴
 * (一帧喂所有轴) / CSP 位置同步 (6060h=8) / PV 速度 (6060h=3) 与 HM 回零 (6060h=6)。
 * 本驱动器没有 CSV(9): 手册 6502h = 0x00A5 = PP + PV + HM + CSP, 所以"速度模式"只有 PV。
 *
 * 阶段 S0~S8 见正文, 每步独立判定, 失败即停; 授权门: S2 为止无需授权 (先跑它们: 一个字节
 * 都不写, 实读值与推导偏移是后面所有动作的事实依据), S3/S4 需 --allow-pdo, S5/S6 需
 * --allow-motion, S7 再加 --home, S8 无条件跑。
 *
 * 安全边界: 默认一个字节都不写; --allow-motion 本身就是那句确认, 运行时不再问 y/N; 上限只能
 * 收紧 (--force-caps 才放宽); PDO 映射收尾默认还原 (只写 RAM, 从不写 2102h); 6040h 只推
 * 0x0000/0x0006/0x0007/0x000F (回零另加 0x001F, 故障复位另加 0x0080); Ctrl-C 只置标志;
 * 收尾时若电机带电只是停住不是卸力; 收尾后 6041h 仍报 Operation enabled -> 退出码 10 (覆盖
 * 其它所有码); Npcap 独占网卡; 真跑时人在设备旁, 手放在物理急停上。
 */

/* 头文件顺序是硬的: soem.h 必须在前 —— windows.h 会拉进 winsock.h 与 _WINSOCKAPI_, 冲突 (MSVC error C2011) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "soem/soem.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "ec_motor.h"

/* 网卡缺省值。换机器改这一行, 或用 argv 覆盖。 */
#define IFNAME "\\Device\\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"

/* 上限 (只能收紧, 放宽要 --force-caps) */
#define CAP_DIST      50000u   /* pul */
#define CAP_VEL       50000u   /* pul/s */
#define CAP_RAMP    5000000u   /* pul/s² */
#define CAP_PV_HOLD    3000u   /* ms */
#define CAP_TMO       60000u   /* ms */

#define DEF_DIST       5000    /* pul ≈ 0.1 圈 (按 2400h = 50000 pul/圈) */
#define DEF_VEL        5000    /* pul/s */
#define DEF_PV_VEL     1000    /* pul/s —— PV 没有我们控制的加减速, 所以比 CSP 更慢 */
#define DEF_RAMP     500000    /* pul/s² —— 驱动器 6083h/6084h 的实读值, 见 em_ramp_acc */
#define DEF_PV_HOLD    1000    /* ms */
#define DEF_TMO       10000    /* ms */

/* 进 OP 之后等过程数据落地的时间 */
#define SETTLE_MS       500

/* Ctrl-C: 信号处理器里只允许置标志 (发 SDO / 打 printf 是未定义行为); em_request_stop() 只置标志, 安全 */

static void on_ctrl_c(int sig)
{
   (void)sig;
   em_request_stop();
}

typedef struct
{
   const char *ifname;
   int  allow_pdo;
   int  allow_motion;
   int  allow_home;
   int  keep_mapping;
   int  force_caps;
   int  use_dc;
   uint32_t cycle_us;
   int  dist;
   int  vel;
   int  pv_vel;
   int  pv_opposite;
   int  ramp_acc;                 /* 6083h 轮廓加速度 (pul/s²) —— 0 会让 PV 的斜坡起不来 */
   int  ramp_dec;                 /* 6084h 轮廓减速度 */
   int  pv_hold;
   int  tmo;
   int  mode;                     /* S6 跑哪种模式: EM_MODE_CSP / EM_MODE_PV */
   int  axis_given[EM_MAX_AXES];  /* 下标 = 总线位置, 1 = 显式选中 */
   int  naxis_given;
} opts_t;

static void usage(const char *prog)
{
   printf(
      "用法: %s [网卡名] [选项]\n"
      "\n"
      "不带 --allow-pdo 时本程序是**只读**的: 跑完 S0~S2 打印实读映射后退出 0。\n"
      "**先跑这一步** —— 它打印的 1600h/1A00h 实读值与推导偏移, 是后面所有动作的依据。\n"
      "\n"
      "授权(缺省全关):\n"
      "  --allow-pdo       允许写 PDO 映射 (1C12h/1C13h/1600h/1A00h, 仅 RAM)\n"
      "  --allow-motion    允许使能 / CSP / PV。需要同时有 --allow-pdo\n"
      "  --home            允许回零 (方式 24: 原点开关 X0, 正向先找)\n"
      "  --keep-mapping    收尾不还原 PDO 映射 (缺省还原)\n"
      "\n"
      "参数(只能收紧, 放宽要 --force-caps):\n"
      "  --mode csp|pv     S6 跑哪种模式, 缺省 csp\n"
      "                      csp = 位置同步 (6060h=8), 相对各自起点走 --dist\n"
      "                      pv  = 速度模式 (6060h=3), 跑 --pv-hold\n"
      "  --dist N          CSP 位移, 缺省 %d, 上限 %u pul\n"
      "  --vel  N          CSP 速度, 缺省 %d, 上限 %u pul/s\n"
      "  --pv-vel N        PV 速度, 缺省 %d, 上限 %u pul/s\n"
      "  --ramp-acc N      6083h 轮廓加速度, 缺省 %d, 上限 %u pul/s²\n"
      "                      驱动器在 PV 下靠它爬坡; 它能收到 0, 但 0 意味着\n"
      "                      「斜坡永远起不来」—— 会报速度指令已接受却一步不走\n"
      "  --ramp-dec N      6084h 轮廓减速度, 同上\n"
      "  --pv-hold N       PV 保持时间, 缺省 %d, 上限 %u ms\n"
      "  --pv-opposite     PV 时让第 2 根轴反向跑 (缺省两轴同向)\n"
      "  --tmo N           单个运动超时, 缺省 %d ms\n"
      "  --axis a,b        选轴 (总线位置 0-based), 缺省 = 总线上**全部**从站\n"
      "  --dc [周期us]     配置 DC (缺省 2000us)。进不去 OP 且 AL 状态码是 0x001B\n"
      "                    一类同步/看门狗相关时再试它; 缺省不碰。\n"
      "  --force-caps      放宽上面那些上限 (应当是有意的动作)\n"
      "  -h, --help        本说明\n"
      "\n"
      "退出码: 0 全 PASS / 1 用法或初始化失败 / 2 无可用轴 / 3 有 FAIL /\n"
      "        4 护栏拒绝 / 5 进不去 OP / 6 过程数据未落地 / 7 回零未达位 /\n"
      "        10 收尾未能确认失能 (**电机可能仍带电**)\n"
      "\n"
      "注意: --axis 给子集时 em_setup 会拒绝 —— 留在 PRE_OP 的从站不参与过程数据\n"
      "      交换, 会让帧的 WKC 持续偏短, 与「过程数据没落地」分不开。\n",
      prog, DEF_DIST, CAP_DIST, DEF_VEL, CAP_VEL, DEF_PV_VEL, CAP_VEL,
      DEF_RAMP, CAP_RAMP,
      DEF_PV_HOLD, CAP_PV_HOLD, DEF_TMO);
}

static int parse_args(int argc, char *argv[], opts_t *o)
{
   int i;

   memset(o, 0, sizeof(*o));
   o->ifname    = IFNAME;
   o->dist      = DEF_DIST;
   o->vel       = DEF_VEL;
   o->pv_vel    = DEF_PV_VEL;
   o->ramp_acc  = DEF_RAMP;
   o->ramp_dec  = DEF_RAMP;
   o->pv_hold   = DEF_PV_HOLD;
   o->tmo       = DEF_TMO;
   o->cycle_us  = 2000;
   o->mode      = EM_MODE_CSP;

   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];

      if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
      {
         usage(argv[0]);
         exit(EM_EXIT_OK);
      }
      else if (strcmp(a, "--allow-pdo") == 0)    o->allow_pdo = 1;
      else if (strcmp(a, "--allow-motion") == 0) o->allow_motion = 1;
      else if (strcmp(a, "--home") == 0)         o->allow_home = 1;
      else if (strcmp(a, "--keep-mapping") == 0) o->keep_mapping = 1;
      else if (strcmp(a, "--force-caps") == 0)   o->force_caps = 1;
      else if (strcmp(a, "--pv-opposite") == 0)  o->pv_opposite = 1;
      else if (strcmp(a, "--mode") == 0)
      {
         if (i + 1 >= argc)
         {
            printf("--mode 后面要跟 csp 或 pv (试 --help)\n");
            return EM_EXIT_USAGE;
         }
         {
            const char *m = argv[++i];

            if (strcmp(m, "csp") == 0)
               o->mode = EM_MODE_CSP;
            else if (strcmp(m, "pv") == 0)
               o->mode = EM_MODE_PV;
            else
            {
               printf("--mode 只认 csp 或 pv, 不认「%s」(试 --help)\n", m);
               return EM_EXIT_USAGE;
            }
         }
      }
      else if (strcmp(a, "--dc") == 0)
      {
         o->use_dc = 1;
         if (i + 1 < argc && argv[i + 1][0] != '-')
            o->cycle_us = (uint32_t)atoi(argv[++i]);
      }
      else if (strcmp(a, "--axis") == 0)
      {
         char *p;

         if (i + 1 >= argc)
         {
            printf("--axis 后面要跟轴号\n");
            return EM_EXIT_USAGE;
         }
         p = argv[++i];
         while (*p != '\0')
         {
            char *end;
            long  v = strtol(p, &end, 10);

            if (end == p || v < 0 || v >= EM_MAX_AXES)
            {
               printf("--axis 里的轴号 %s 不合法 (只能是 0..%d)\n", p, EM_MAX_AXES - 1);
               return EM_EXIT_USAGE;
            }
            if (!o->axis_given[v])
            {
               o->axis_given[v] = 1;
               o->naxis_given++;
            }
            p = (*end == ',') ? end + 1 : end;
         }
      }
      else if (strcmp(a, "--dist") == 0 || strcmp(a, "--vel") == 0 ||
               strcmp(a, "--pv-vel") == 0 || strcmp(a, "--pv-hold") == 0 ||
               strcmp(a, "--tmo") == 0)
      {
         int *dst = (strcmp(a, "--dist") == 0)    ? &o->dist :
                    (strcmp(a, "--vel") == 0)     ? &o->vel :
                    (strcmp(a, "--pv-vel") == 0)  ? &o->pv_vel :
                    (strcmp(a, "--pv-hold") == 0) ? &o->pv_hold : &o->tmo;
         int v;

         if (i + 1 >= argc)
         {
            printf("%s 后面要跟一个整数\n", a);
            return EM_EXIT_USAGE;
         }
         v = atoi(argv[++i]);

         /* --dist 可以为负 (负号表达方向, 不取绝对值 —— 悄悄变号会让滑台朝反方向走, 真机会撞限位) */
         if (dst == &o->dist)
         {
            if (v == 0)
            {
               printf("--dist 不能是 0 (0 是「不动」, 不是「不指定」)\n");
               return EM_EXIT_USAGE;
            }
         }
         else if (v <= 0)
         {
            printf("%s 必须为正\n", a);
            return EM_EXIT_USAGE;
         }
         *dst = v;
      }
      else if (strcmp(a, "--ramp-acc") == 0 || strcmp(a, "--ramp-dec") == 0)
      {
         /* 这一对单独一条分支, 因为它允许 0 (本机 0 = 斜坡起不来), WARN 放到 S6 里针对本机打 */
         int *dst = (strcmp(a, "--ramp-acc") == 0) ? &o->ramp_acc : &o->ramp_dec;

         if (i + 1 >= argc)
         {
            printf("%s 后面要跟一个整数\n", a);
            return EM_EXIT_USAGE;
         }
         *dst = atoi(argv[++i]);
         if (*dst < 0)
         {
            printf("%s 不能是负数 (加减速度没有方向)\n", a);
            return EM_EXIT_USAGE;
         }
      }
      else if (a[0] != '-')
      {
         o->ifname = a;
      }
      else
      {
         printf("未知参数: %s (试 --help)\n", a);
         return EM_EXIT_USAGE;
      }
   }

   /* ---- 上限: 只能收紧, 放宽要 --force-caps ---- */
   {
      long adist = (o->dist < 0) ? -(long)o->dist : (long)o->dist;

      if (adist > (long)CAP_DIST && !o->force_caps)
      {
         printf("--dist %d 超过上限 %u pul。"
                "按 2400h 细分 50000 pul/圈算, %d pul ≈ %.2f 圈。\n"
                "确认要跑这么大再显式加 --force-caps。\n",
                o->dist, CAP_DIST, o->dist, (double)adist / 50000.0);
         return EM_EXIT_USAGE;
      }
      if (o->vel > (int)CAP_VEL && !o->force_caps)
      {
         printf("--vel %d 超过上限 %u pul/s (加 --force-caps 可放宽)\n",
                o->vel, CAP_VEL);
         return EM_EXIT_USAGE;
      }
      if (o->pv_vel > (int)CAP_VEL && !o->force_caps)
      {
         printf("--pv-vel %d 超过上限 %u pul/s (加 --force-caps 可放宽)\n",
                o->pv_vel, CAP_VEL);
         return EM_EXIT_USAGE;
      }
      if ((o->ramp_acc > (int)CAP_RAMP || o->ramp_dec > (int)CAP_RAMP) &&
          !o->force_caps)
      {
         printf("--ramp-acc/--ramp-dec 超过上限 %u pul/s² (加 --force-caps 可放宽)\n",
                CAP_RAMP);
         return EM_EXIT_USAGE;
      }
      if (o->pv_hold > (int)CAP_PV_HOLD && !o->force_caps)
      {
         printf("--pv-hold %d 超过上限 %u ms (加 --force-caps 可放宽)\n",
                o->pv_hold, CAP_PV_HOLD);
         return EM_EXIT_USAGE;
      }
      if (o->tmo > (int)CAP_TMO && !o->force_caps)
      {
         printf("--tmo %d 超过上限 %u ms (加 --force-caps 可放宽)\n",
                o->tmo, CAP_TMO);
         return EM_EXIT_USAGE;
      }
   }

   /* PV 期间是"以 vel 跑 hold_ms", 位移是算得出来的 —— 先跟位移上限比一下 */
   {
      double travel = (double)o->pv_vel * (double)o->pv_hold / 1000.0;

      if (travel > (double)CAP_DIST && !o->force_caps)
      {
         printf("PV 这趟会走约 %.0f pul (--pv-vel %d x --pv-hold %d ms), "
                "超过位移上限 %u pul。收紧参数或加 --force-caps。\n",
                travel, o->pv_vel, o->pv_hold, CAP_DIST);
         return EM_EXIT_USAGE;
      }
   }

   return EM_EXIT_OK;
}

static const char *al_str(uint16_t st)
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

/* 打印一根轴的实时快照 */
static void print_axis_live(const em_axis_t *ax, const char *tag)
{
   printf("    %s [%s] 6041h=%-40s 6064h=%-12d 606Ch=%-10d 帧=%u\n",
          em_axis_label(ax), tag,
          em_mirror_ok(ax) ? em_sw_describe(em_sw(ax)) : "(镜像不可信, 未取到完整帧)",
          em_pos(ax), em_vel(ax), (unsigned)em_mirror_frames(ax));
}

/* 读一个只读参数并打印, 连驱动器自报的宽度一起打: 用 em_rd_any 而不是 em_rd_u32 —— 本机宽度
 * 并不一致 (2201h/2400h/2408h/2409h 自报 2 字节, 6502h 是 4 字节), 按 4 字节读一个都打不出来 */
static void print_ro_num(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                         const char *name, const char *note)
{
   uint32_t v = 0;
   int      size = 0;

   if (em_rd_any(bus, slave, index, sub, &v, &size) != 0)
   {
      printf("    %-22s = (读失败 / 该对象不存在)\n", name);
      return;
   }
   printf("    %-22s = %u (0x%X, %d 字节)%s\n", name, (unsigned)v, (unsigned)v,
          size, note != NULL ? note : "");
}

static void print_ro_i32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                         const char *name)
{
   int32_t v = 0;

   if (em_rd_i32(bus, slave, index, sub, &v) == 0)
      printf("    %-22s = %d (4 字节)\n", name, v);
   else
      printf("    %-22s = (读失败 / 不是 4 字节的 I32)\n", name);
}

/* 2004h 输入端子物理电平 (U16 RO, 手册 V2.4): Bit0~Bit3 = X0~X3, 常开/常闭由 2300h 决定,
 * 报的是 60FDh 之前那一层 (未经 2300h 反转与 2310h~2312h 功能映射) 的物理状态; 与 60FDh
 * 对不上 = 2300h 配反, 手按开关时某位不动 = 那一路没接线。宽度自报读 (理由同 print_ro_num) */
static void print_di_levels(em_bus_t *bus, int slave)
{
   static const char *const nm[4] = { "X0", "X1", "X2", "X3" };
   uint32_t v = 0;
   int      size = 0;
   int      b;

   if (em_rd_any(bus, slave, 0x2004, 0, &v, &size) != 0)
   {
      printf("    %-22s = (读失败 / 该对象不存在)\n", "2004h 输入端子物理电平");
      return;
   }

   printf("    %-22s = 0x%X (%d 字节)  ", "2004h 输入端子物理电平", (unsigned)v, size);
   for (b = 0; b < 4; b++)
      printf("%s=%s ", nm[b], (v & (1u << b)) ? "高" : "低");
   printf("  <- 2300h **之前**的物理电平\n");
}

/* 60FDh 数字输入当下这一帧 (SDO 读), 与上面 2004h 成对看: 手册 V2.4 里 60FDh (U32 RO) 的
 * bit2/bit1/bit0 = 原点/正限位/负限位功能, 是 2004h 经 2300h 电平反转与 2310h~2312h 功能
 * 映射之后的结果; 按功能点名而不是端子名 (bit2 是哪个端子由 2310h 定, 本机 = 1 = X0) */
static void print_dig_in_now(em_bus_t *bus, int slave)
{
   static const struct
   {
      uint16_t    bit;
      const char *nm;
   } f[3] = {
      { 0x0004u, "原点(bit2)" },
      { 0x0002u, "正限位(bit1)" },
      { 0x0001u, "负限位(bit0)" },
   };
   uint32_t v = 0;
   int      size = 0;
   int      k;

   if (em_rd_any(bus, slave, 0x60FD, 0, &v, &size) != 0)
   {
      printf("    %-22s = (读失败 —— 本驱动器这个对象可能不响应; 界面上那三个开关灯"
             "读的是过程数据, 不受影响)\n", "60FDh 数字输入");
      return;
   }

   printf("    %-22s = 0x%X (%d 字节)  ", "60FDh 数字输入", (unsigned)v, size);
   for (k = 0; k < 3; k++)
      printf("%s=%s ", f[k].nm, (v & f[k].bit) ? "有效" : "无效");
   printf("  <- 2004h 经 2300h + 2310h 之后\n");
}

/* 6041h 状态字当下这一帧 (SDO 读, 打这一排时还没建过程数据镜像)。自己拼而不用
 * em_sw_describe: 后者不报按模式解释的三位 —— bit10 Target reached / bit12 (HM 下 =
 * Homing attained) / bit13 (HM 下 = Homing error) */
static void print_sw_now(em_bus_t *bus, int slave)
{
   uint32_t v = 0;
   uint16_t sw;
   int      size = 0;

   if (em_rd_any(bus, slave, 0x6041, 0, &v, &size) != 0 || size < 1)
   {
      printf("    %-22s = (读失败 / 该对象不存在)\n", "6041h 状态字");
      return;
   }
   sw = (uint16_t)(v & 0xFFFFu);

   printf("    %-22s = %s   [bit10 按模式 = %d, bit12 按模式 = %d, bit13 按模式 = %d]\n",
          "6041h 状态字", em_sw_describe(sw),
          (sw & EM_SW_TARGET) ? 1 : 0, (sw & EM_SW_BIT12) ? 1 : 0,
          (sw & EM_SW_BIT13) ? 1 : 0);
}

int main(int argc, char *argv[])
{
   opts_t       opt;
   em_bus_t    *bus = NULL;
   em_axis_cfg_t cfg[EM_MAX_AXES];
   em_axis_t   *ax[EM_MAX_AXES];
   int          naxis = 0;
   int          exit_code = EM_EXIT_OK;
   int          maybe_live = 0;
   int          rc, i;

   /* 第一句就设控制台代码页: parse_args 自己会打印中文错误信息, 否则最该看清的那几行恰好是乱码 */
   em_console_init();

   signal(SIGINT, on_ctrl_c);

   rc = parse_args(argc, argv, &opt);
   if (rc != EM_EXIT_OK)
      return rc;

   printf("test motor_test - 多轴 CiA402 接口验收 (%s)\n", em_version());
   printf("网卡: %s\n", opt.ifname);
   printf("授权: --allow-pdo=%s --allow-motion=%s --home=%s%s\n",
          opt.allow_pdo ? "是" : "否", opt.allow_motion ? "是" : "否",
          opt.allow_home ? "是" : "否",
          opt.keep_mapping ? " (收尾保留映射)" : " (收尾还原映射)");

   /* 运动必须要能补映射 —— 607Ah/60FFh 不在映射里就发不出去 */
   if (opt.allow_motion && !opt.allow_pdo)
   {
      printf("\n--allow-motion 必须同时给 --allow-pdo: CSP/PV 的目标值(607Ah/60FFh)\n"
             "必须走过程数据, 而它们要能被发出去就得先补进 RxPDO 映射。\n"
             "本程序**不会**为了让参数看起来合理而自动替你开这个授权。\n");
      return EM_EXIT_REFUSED;
   }

   bus = em_bus_new();
   if (bus == NULL)
   {
      printf("内存不足\n");
      return EM_EXIT_USAGE;
   }

   /* S0 开总线 / 扫描从站 / 身份门 (到这一步为止没写过任何东西) */
   printf("\n==== S0 开总线 / 扫描从站 ====\n");

   rc = em_open(bus, opt.ifname);
   if (rc < 0)
   {
      printf("\n可用网卡:\n");
      em_print_adapters();
      exit_code = EM_EXIT_USAGE;
      goto out;
   }
   printf("  发现 %d 台从站\n", rc);

   {
      int nykd = 0, non_ykd = 0;

      printf("\n  序号  总线位置  厂商        产品码      修订    AL 状态  输出  输入  身份门\n");
      for (i = 1; i <= rc; i++)
      {
         em_slave_info_t si;

         if (em_slave_info(bus, i, &si) != EM_EXIT_OK)
            continue;
         printf("  %-4d  %-8d  0x%08X  0x%08X  0x%04X  %-8s %-5u %-5u %s\n",
                si.slave, si.pos, (unsigned)si.eep_man, (unsigned)si.eep_id,
                (unsigned)si.eep_rev,
                (si.state & EC_STATE_ERROR) ? "带错误位" : al_str(si.state),
                (unsigned)si.Obytes, (unsigned)si.Ibytes,
                si.is_ykd ? "YKD2205PE" : "<<< 不是本接口支持的型号");
         if (si.is_ykd)
            nykd++;
         else
            non_ykd++;
      }

      printf("\n  身份门: 厂商必须是 0x%04X, 产品码必须是 0x%04X 或 0x%04X\n",
             (unsigned)EM_YKD_VENDOR_ID, (unsigned)EM_YKD_PRODUCT_1,
             (unsigned)EM_YKD_PRODUCT_2);
      if (nykd == 0)
      {
         printf("\n  总线上没有一台是受支持的 YKD2205PE -> 没有可用的轴, 退出\n");
         exit_code = EM_EXIT_NO_SLAVE;
         goto out;
      }
      if (non_ykd > 0)
      {
         printf("\n  有 %d 台不是受支持的型号。本接口要求**全部从站都选中**"
                "(留在 PRE_OP 的从站不参与过程数据交换, 会让 WKC 持续偏短, 与"
                "「过程数据没落地」分不开), 所以 em_setup 会拒绝 -> 本程序跑不了。\n",
                non_ykd);
         exit_code = EM_EXIT_NO_SLAVE;
         goto out;
      }
   }

   /* S1 选轴 */
   printf("\n==== S1 选轴 ====\n");

   if (opt.naxis_given > 0)
   {
      for (i = 0; i < EM_MAX_AXES; i++)
      {
         if (opt.axis_given[i])
         {
            if (i >= em_slave_count(bus))
            {
               printf("  --axis %d 超出总线范围 (总线上只有 %d 台)\n", i,
                      em_slave_count(bus));
               exit_code = EM_EXIT_NO_SLAVE;
               goto out;
            }
            cfg[naxis].bus_pos = i;
            cfg[naxis].pos_tol = 0;   /* 0 = 用 EM_POS_TOL_DEF */
            naxis++;
         }
      }
      printf("  按 --axis 选了 %d 根轴\n", naxis);
      if (naxis != em_slave_count(bus))
         printf("  >>> 注意: 这不是总线上全部从站, em_setup 会拒绝 (见上面那段说明)\n");
   }
   else
   {
      /* 缺省 = 总线上全部从站, 不是"前两台": em_setup 的硬要求是全部选中, 缺省值照着给 */
      for (i = 1; i <= em_slave_count(bus) && naxis < EM_MAX_AXES; i++)
      {
         cfg[naxis].bus_pos = i - 1;
         cfg[naxis].pos_tol = 0;
         naxis++;
      }
      printf("  未给 --axis -> 选总线上全部 %d 台从站\n", naxis);
   }
   printf("  共 %d 根轴\n", naxis);

   /* S2 实读映射 + 只读参数 —— 到此为止退出 0 */
   printf("\n==== S2 实读 PDO 映射与只读参数 (一个字节都不写) ====\n");

   for (i = 0; i < naxis; i++)
   {
      int slave = cfg[i].bus_pos + 1;

      printf("\n  ################ 从站 %d (总线位置 %d) ################\n",
             slave, cfg[i].bus_pos);

      /* 偏移只能从实读的映射表推: 手册值 2 / ESI 声明值 16 / ESI 字典默认值 3 字节互不相同,
       * 真机上量到的是第四种 */
      em_dump_pdo(bus, slave);

      printf("\n    只读参数:\n");

      /* 行程当量: 2201h 决定细分(2400h)还是电子齿轮(2408h/2409h)生效, 两者差 50 倍 */
      print_ro_num(bus, slave, EM_OID_GEAR_ENABLE, 0,
                   "2201h 当量选择",
                   "  (0 = 细分有效 / 1 = 电子齿轮有效)");
      print_ro_num(bus, slave, EM_OID_SUBDIV, 0,
                   "2400h 细分", "  (电机一圈脉冲数)");
      print_ro_num(bus, slave, 0x2408, 0, "2408h 电子齿轮分子", "  (仅 V1.0 手册有)");
      print_ro_num(bus, slave, 0x2409, 0, "2409h 电子齿轮分母", "  (仅 V1.0 手册有)");
      /* 607Dh 的 :01 是负向(最小)限位, :02 是正向(最大)限位 —— 不按":01 在前所以是正向"想当然 */
      print_ro_i32(bus, slave, EM_OID_SOFTLIM, 1, "607Dh:01 负向软限位");
      print_ro_i32(bus, slave, EM_OID_SOFTLIM, 2, "607Dh:02 正向软限位");
      print_ro_i32(bus, slave, EM_OID_ACT_POS, 0, "6064h 当前位置");
      print_ro_num(bus, slave, 0x6502, 0, "6502h 支持的模式",
                   "  (位图; 本驱动器 = 0x00A5 = PP+PV+HM+CSP, **无 CSV**)");

      /* 与上面那组分开: 2201h/2400h/607Dh 是配置, 静止时不该变, 能当基线比; 下面这几行是
       * 此刻的状态, 每时每刻都在变, 拿它们比基线必然天天 FAIL —— 但"轴为什么不动"最先要看
       * 的就是它们: 2004h 物理电平 / 6041h 状态字 / 603Fh 故障码 (上电就 bit3 = Fault 时) */
      printf("\n    只读状态 (SDO 读; 这里还没有过程数据镜像):\n");
      print_di_levels(bus, slave);
      print_dig_in_now(bus, slave);
      print_sw_now(bus, slave);
      print_ro_num(bus, slave, 0x603F, 0, "603Fh 故障码",
                   "  (0 = 无故障; 非 0 时对照手册故障码表)");
   }
   printf("\n  >>> 上面这些是**实读**值。若某个需要的字段不在映射里, S3 会尝试追加它\n"
          "      (只追加缺项, 只写 RAM); 追加不了就拒绝, 不猜偏移、不改走 SDO 硬凑。\n");

   if (!opt.allow_pdo)
   {
      printf("\n==== 到此为止 (未给 --allow-pdo, 一个字节都没写) ====\n");
      exit_code = EM_EXIT_OK;
      goto out;
   }

   /* S3 补映射 + 建过程数据 + 逐轴证明偏移 + SAFE_OP */
   printf("\n==== S3 补 PDO 映射 + 建过程数据 + 上 SAFE_OP ====\n");

   rc = em_setup(bus, cfg, naxis, opt.allow_pdo);
   if (rc != EM_EXIT_OK)
   {
      /* 到这里的失败都是护栏拒绝或写不进去, em_setup 会把每一层原因打出来; 退出码 4 = 没有在不该动的地方动手 */
      printf("\n  em_setup 失败 —— 上面有具体原因。本程序不降级、不猜测、"
             "不改走 SDO 硬凑 (SDO 做不到每周期刷新目标值)。\n");
      exit_code = EM_EXIT_REFUSED;
      goto out;
   }
   printf("  [PASS] 选择与偏移证明都过了\n");

   /* 各模式现在能不能用 —— 取决于各自字段在不在该轴实读的映射里 */
   for (i = 0; i < naxis; i++)
   {
      em_axis_t *a = em_axis(bus, i);

      /* 出问题时第一件事就是看这两个值: 真机 1C12h 指的是 1601h (不是默认的 1600h), 偏移按它们推 */
      printf("  %s: RxPDO %04Xh / TxPDO %04Xh, CSP %s / PV %s\n", em_axis_label(a),
             (unsigned)em_rx_pdo(a), (unsigned)em_tx_pdo(a),
             em_csp_available(a) ? "可用" : "**不可用**",
             em_pv_available(a) ? "可用" : "**不可用**");

      /* 给"运行模式改不动"那种故障用: 写着"经过程数据"就对它做 SDO 写一定不生效 (下一帧被镜像盖回去) */
      if (em_modes_via_pdo(a))
         printf("      6060h 运行模式: 在生效 RxPDO 的 +%d -> 经**过程数据**下发\n",
                em_modes_offset(a));
      else
         printf("      6060h 运行模式: 不在生效 RxPDO 里 -> 经 SDO 写\n");

      /* 6083h/6084h 是"PV 收了指令却不转"的第一嫌疑: 在生效 RxPDO 里不写就是下发 0, 斜坡起不来 */
      if (em_ramp_offset(a) >= 0)
         printf("      6083h/6084h 加减速度: 在生效 RxPDO 的 +%d -> 主站每周期下发 "
                "%u/%u pul/s²\n", em_ramp_offset(a),
                (unsigned)em_ramp_acc(a), (unsigned)em_ramp_dec(a));
      else
         printf("      6083h/6084h 加减速度: 不在生效 RxPDO 里 -> 主站不覆盖, "
                "用驱动器自己的值\n");

      /* 6081h 是本接口唯一有意不驱动的一项过程数据: 它只在 PP 下当轮廓速度用, 而 PP 是
       * slide_motion 的活。写出来是为了让"表里每一项都有交代"完整 (它在不在映射里由
       * em_dump_pdo 的处置表逐项列出, 这里不重算) */
      printf("      6081h 轮廓速度: 本接口不做 PP -> **不驱动**, 主站下发 0 "
             "(驱动器基线本来就是 0)\n");
   }

   /* S4 进 OP + 确认过程数据落地 */
   printf("\n==== S4 进 OP ====\n");

   rc = em_enter_op(bus, opt.use_dc, opt.cycle_us);
   if (rc != EM_EXIT_OK)
   {
      exit_code = EM_EXIT_NO_OP;
      goto out;
   }

   /* "进了 OP"和"过程数据真的在落地"是两件事。判据是镜像里确实收到了完整的帧 (mirror_ok /
    * frames), 不是"WKC 够大"就行; 反过来, 镜像一直不更新也一定有问题。这里给 SETTLE_MS 宽限,
    * 期间持续打过程数据 (OP 下停发帧本身就是故障源, 有的 SM 看门狗会因此把从站踢出去) */
   {
      int waited = 0;

      while (waited < SETTLE_MS)
      {
         int all = 1;

         (void)em_service(bus);
         for (i = 0; i < naxis; i++)
         {
            if (!em_mirror_ok(em_axis(bus, i)))
               all = 0;
         }
         if (all)
            break;

         em_sleep_ms(10);
         waited += 10;
      }
   }

   {
      int all_ok = 1;

      printf("\n  过程数据落地检查 (期望 WKC = %d):\n", em_expected_wkc(bus));
      for (i = 0; i < naxis; i++)
      {
         print_axis_live(em_axis(bus, i), "OP");
         if (!em_mirror_ok(em_axis(bus, i)))
            all_ok = 0;
      }
      if (!all_ok)
      {
         printf("\n  有轴一笔完整的 6041h 都没取到 —— 过程数据没落地。"
                "真机上这一步先于任何动作被判定, 所以后面不会带着一个不可信的状态字去动电机。\n");
         exit_code = EM_EXIT_NO_PDO;
         goto out;
      }
   }

   /* S5 使能 */
   printf("\n==== S5 使能 ====\n");

   if (!opt.allow_motion)
   {
      printf("  未给 --allow-motion -> 在此停下 (PDO 通路已验证, 电机不会带电)\n");
      exit_code = EM_EXIT_OK;
      goto out;
   }

   /* --allow-motion 本身就是那句确认, 运行时不再问 y/N; 不再等输入, 就把这趟要多远/多久先打出来 */
   printf("  即将(带电): %d 根轴 ", naxis);
   if (opt.mode == EM_MODE_CSP)
      printf("CSP 各走 %d pul 再走回", opt.dist);
   else
      printf("PV 各跑 %ums%s", opt.pv_hold,
             opt.pv_opposite ? " (第 2 根反向)" : "");
   if (opt.allow_home)
      printf(" -> 回零(方式 24, 会去找原点开关、可能撞限位)");
   printf("\n");

   rc = em_enable_all(bus);
   if (rc != EM_EXIT_OK)
   {
      /* em_enable_all 内部已经把已使能的轴退回去了。若它是被 Ctrl-C 打断的 (1), 不是失败 */
      if (rc == 1)
      {
         printf("  使能过程被中止\n");
         exit_code = EM_EXIT_OK;
      }
      else
      {
         exit_code = EM_EXIT_FAIL;
      }
      goto out;
   }
   printf("  [PASS] 全部 %d 轴已使能\n", naxis);
   for (i = 0; i < naxis; i++)
      print_axis_live(em_axis(bus, i), "带电");

   /* S6 CSP 或 PV (--mode 选)。两者都是多轴、同一周期帧、各轴下发各自目标值, 公共前置一样 */
   printf("\n==== S6 %s (6060h=%d) ====\n",
          (opt.mode == EM_MODE_CSP) ? "CSP 位置同步模式" : "PV 速度模式",
          opt.mode);

   {
      int32_t  d[EM_MAX_AXES];    /* CSP: 相对位移 (去程正向 / 回程取负) */
      uint32_t v[EM_MAX_AXES];    /* CSP: 各轴速度 */
      int32_t  pv[EM_MAX_AXES];   /* PV:  各轴速度 (可负) */
      int      can = 1;

      for (i = 0; i < naxis; i++)
      {
         ax[i] = em_axis(bus, i);
         if (opt.mode == EM_MODE_CSP)
         {
            if (!em_csp_available(ax[i]))
               can = 0;
         }
         else
         {
            if (!em_pv_available(ax[i]))
               can = 0;
         }
      }

      if (!can)
      {
         printf("  有轴的 %s 不在映射里 -> 跳过 S6 (不影响后面的回零)\n",
                (opt.mode == EM_MODE_CSP) ? "607Ah/6064h" : "60FFh");
      }
      else
      {
         /* 未使能时才能改模式 —— 所以先失能, 切模式, 再使能 */
         rc = em_disable_all(bus);
         if (rc != EM_EXIT_OK)
         {
            printf("  先失能以便切模式, 但失能失败 -> 不再继续 "
                   "(不要在使能状态下改模式)\n");
            exit_code = EM_EXIT_FAIL;
            goto out;
         }

         for (i = 0; i < naxis; i++)
         {
            if (em_set_mode(ax[i], opt.mode) != EM_EXIT_OK)
            {
               exit_code = EM_EXIT_FAIL;
               goto out;
            }
         }

         rc = em_enable_all(bus);
         if (rc != EM_EXIT_OK)
         {
            exit_code = EM_EXIT_FAIL;
            goto out;
         }

         if (opt.mode == EM_MODE_CSP)
         {
            /* 方向交替 (+dist / -dist): 同一帧里下发不同的目标才算真的测到"同时驱动多台" */
            for (i = 0; i < naxis; i++)
            {
               d[i] = (i % 2 == 0) ? opt.dist : -opt.dist;
               v[i] = (uint32_t)opt.vel;
            }

            printf("  %d 根轴各走各的, 同一个周期帧下发 "
                   "(目标位置以**调用这一刻各自的实际位置**为基准):\n", naxis);

            /* 去程 —— 基准由接口内部取 (em_csp_move_rel_multi) */
            rc = em_csp_move_rel_multi(ax, d, v, naxis, (uint32_t)opt.tmo);
            if (rc != EM_EXIT_OK)
            {
               printf("  CSP 去程%s\n", (rc == 1) ? "被中止" : "失败");
               exit_code = (rc == 1) ? EM_EXIT_OK : EM_EXIT_FAIL;
               goto out;
            }

            /* 回程用取负的同一组位移, 而不是"回到 start[] 那个绝对位置": 去程结束时已经断言过
             * |实际 - 终点| <= 容差, 位置是确认到了的; 回程拿旧绝对值去追只会让"中间丢了步"更难
             * 看出来, 相对回来则把误差如实留在原地 */
            printf("\n  再一起走回 (位移取负, 相对此刻的实际位置):\n");
            for (i = 0; i < naxis; i++)
               d[i] = -d[i];

            rc = em_csp_move_rel_multi(ax, d, v, naxis, (uint32_t)opt.tmo);
            if (rc != EM_EXIT_OK)
            {
               printf("  CSP 回程%s\n", (rc == 1) ? "被中止" : "失败");
               exit_code = (rc == 1) ? EM_EXIT_OK : EM_EXIT_FAIL;
               goto out;
            }
         }
         else
         {
            /* 方向缺省两轴同向: PV 期间没有任何位置约束, 两根轴反向跑就是在机械上互相拉开,
             * 而这两根轴的机械关系 (是不是同一个龙门的两侧) 从这里看不出来 ——
             * 知道安全的人可以加 --pv-opposite */
            printf("  PV 方向: %s\n", opt.pv_opposite
                   ? "第 1 根正向 / 第 2 根反向 (--pv-opposite, 已确认机械上安全)"
                   : "两轴同向 (缺省; 不知道机械关系时不互相拉开)");

            /* 6083h/6084h 是 PV 的斜坡, 必做的一步而不是可选微调: 它们在生效 RxPDO 里, 不设就是
             * 下发 0, 而 0 让斜坡永远起不来 (症状: 收下速度指令、606Ch 恒为 0、6064h 一步不走,
             * 而所有断言都过) */
            if (opt.ramp_acc == 0 || opt.ramp_dec == 0)
               printf("  [WARN] --ramp-acc/--ramp-dec 传了 0: 本机驱动器在 PV 下会因此"
                      "一步不走 (收下速度指令但斜坡起不来)\n");
            for (i = 0; i < naxis; i++)
               em_set_ramp(ax[i], (uint32_t)opt.ramp_acc, (uint32_t)opt.ramp_dec);
            printf("  6083h/6084h 轮廓加减速度: %d/%d pul/s² (逐轴已写进输出镜像)\n",
                   opt.ramp_acc, opt.ramp_dec);

            for (i = 0; i < naxis; i++)
               pv[i] = (i % 2 == 1 && opt.pv_opposite)
                          ? -opt.pv_vel : opt.pv_vel;

            /* 一次调用, 一帧喂所有轴 */
            rc = em_pv_run_multi(ax, pv, naxis, (uint32_t)opt.pv_hold);
            if (rc != EM_EXIT_OK)
            {
               printf("  PV %s\n", (rc == 1) ? "被中止" : "失败");
               exit_code = (rc == 1) ? EM_EXIT_OK : EM_EXIT_FAIL;
               goto out;
            }
            for (i = 0; i < naxis; i++)
               print_axis_live(ax[i], "PV 停后");
         }
      }
   }

   /* S7 回零 (HM, 6060h=6) */
   printf("\n==== S7 回零 (HM, 6060h=6) ====\n");

   if (!opt.allow_home)
   {
      printf("  未给 --home -> 跳过 (回零会去找原点开关、可能撞限位)\n");
   }
   else
   {
      em_home_cfg_t hc;

      em_home_cfg_default(&hc);
      /* 回零速度跟着 --vel 收紧, 但不超过默认的保守值 —— 首次回零务必慢 */
      hc.vel_fast = ((uint32_t)opt.vel < 2000u) ? (uint32_t)opt.vel : 2000u;
      hc.vel_slow = hc.vel_fast / 4u;
      if (hc.vel_slow == 0)
         hc.vel_slow = 1;

      printf("  方式 %d (原点开关 X0, 正向先找), 找原点速度 %u, 返回速度 %u pul/s\n",
             hc.method, (unsigned)hc.vel_fast, (unsigned)hc.vel_slow);
      printf("  >>> 方向不对就停下来换方式 29 或 35, 别硬顶。\n");

      for (i = 0; i < naxis; i++)
      {
         em_axis_t *a = em_axis(bus, i);

         /* 一根一根来, 不是同时: 回零是"以某个速度去找开关"的过程, 两根同时找就是各自朝自己
          * 的方向冲, 而方向对不对只有试过才知道。每根回零前都要先失能 —— 6098h/6099h/609Ah/607Ch
          * 只能在未使能时写 (em_home 自己也拒绝在使能状态下写) */
         rc = em_disable(a);
         if (rc != EM_EXIT_OK)
         {
            exit_code = EM_EXIT_FAIL;
            goto out;
         }

         rc = em_home(a, &hc, (uint32_t)opt.tmo * 3u);
         if (rc != EM_EXIT_OK)
         {
            if (rc == 1)
            {
               printf("  %s 回零被中止\n", em_axis_label(a));
               exit_code = EM_EXIT_OK;
            }
            else
            {
               exit_code = EM_EXIT_HOMING;
            }
            goto out;
         }
         print_axis_live(a, "回零后");
      }
      printf("  [PASS] 全部 %d 轴回零到位 (6041h bit12), bit13 未置\n", naxis);
   }

   exit_code = EM_EXIT_OK;

   /* S8 收尾 —— 无条件执行 */
out:
   /* 走到这里的原因有: 正常跑完 / 某一步 FAIL / 前置条件不满足 / 护栏拒绝 / Ctrl-C; 收尾对每一种都执行 */
   em_shutdown(bus, !opt.keep_mapping, &maybe_live);
   em_bus_free(bus);

   if (maybe_live)
   {
      printf("\n  >>> 收尾未能确认失能: 电机可能仍然带电。立即断开驱动器供电, "
             "不要用手去推滑台。\n");
      exit_code = EM_EXIT_NO_DISABLE;   /* 覆盖其它所有码 */
   }

   {
      static const char *names[] = {
         "全部 PASS", "用法/初始化失败", "无可用轴", "存在 FAIL", "护栏拒绝",
         "进不去 OP", "过程数据未落地", "回零未达位"
      };
      const char *nm = (exit_code <= 7) ? names[exit_code] : "收尾失能未确认";

      printf("\n==== 结论: %s (退出码 %d) ====\n", nm, exit_code);
   }
   return exit_code;
}
