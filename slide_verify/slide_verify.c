/*
 * slide_verify - 滑台设备导入验证程序 (原生 C + SOEM)
 *
 * 用途:
 *   滑台设备(研控 YKD2205PE EtherCAT 驱动器)上电/接入总线后,扫描 EtherCAT
 *   总线,对每台从站读取身份与关键 CiA402 对象,判定是否为受支持的
 *   YKD2205PE 滑台驱动器,逐项给出 PASS / WARN / FAIL,最后汇总并返回退出码。
 *
 *   只读工具:不进 OP、不写 6040h 控制字、不做使能序列、不动电机。
 *   验证深度:进 PRE_OP(SDO 即可用)做对象读取;之后可选尝试进 SAFE_OP,
 *             进不去不致命(空 TxPDO 是正常诱因)。
 *
 * 位置/构建:
 *   本文件在本仓库自己的独立顶层工程 (slide_verify/), 通过根目录 CMakeLists
 *   add_subdirectory(SOEM) 引用下载的 SOEM 库, 不改动 SOEM 源码。
 *     cmake -S . -B build                # 在仓库根 (含顶层 CMakeLists)
 *     cmake --build build --config Release --target slide_verify
 *   产物: build/slide_verify/Release/slide_verify.exe
 *   MinGW/Ninja 示例:
 *     cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
 *           -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
 *     cmake --build build-mingw --target slide_verify
 *
 * 用法:
 *   slide_verify [ifname]
 *     ifname   网卡名。Windows Npcap 形如  \Device\NPF_{GUID} ,
 *              例如: slide_verify '\Device\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}'
 *                                              809345E5-15B7-4552-B72E-9B9C4722D44C
 *     slide_verify ['\Device\NPF_{809345E5-15B7-4552-B72E-9B9C4722D44C}']
 * build\slide_verify\Release\slide_verify.exe "\Device\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"
.\build\slide_verify\slide_verify.exe "\Device\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}"
 *              不带 ifname 时仅列出可用网卡。
 *
 * 退出码:
 *   0 = 通过 (至少 1 台 YKD, 全部 PASS)
 *   1 = 用法错误 / 初始化失败 (网卡打不开、Npcap 被独占、需管理员)
 *   2 = 空总线 (config_init 未发现从站)
 *   3 = 存在 YKD FAIL (必需对象缺失/超时、YKD 未达 PRE_OP)
 *   4 = 无 FAIL 但含 WARN (可选对象缺失、SAFE_OP 未达、未知模式、混挂非 YKD 等)
 *   5 = 总线有从站, 但一台 YKD 都没有 (环境不匹配)
 *
 * 阶段:
 *   P1 发现 + 身份判定      : ecx_init/config_init -> 身份表 + YKD 判定
 *   P2A PRE_OP 对象验证     : 对 YKD 逐对象 SDO 读取与校验
 *   P2B SAFE_OP 探针        : 可选尝试进 SAFE_OP, 失败不致命
 *   P3 汇总报告 + 退出码收口
 */

#include <stdio.h>
#include <string.h>

#include "soem/soem.h"

#ifdef _WIN32
#include <windows.h> /* SetConsoleOutputCP(CP_UTF8) */
#endif

/* ---- 退出码契约 ------------------------------------------------------ */
#define SV_EXIT_OK        0   /* 通过 */
#define SV_EXIT_USAGE     1   /* 用法错误 / 初始化失败 */
#define SV_EXIT_NO_SLAVE  2   /* 空总线 */
#define SV_EXIT_FAIL      3   /* 存在 YKD FAIL */
#define SV_EXIT_WARN      4   /* 无 FAIL 但含 WARN */
#define SV_EXIT_NO_YKD    5   /* 无任何受支持 YKD */

/* ---- 受支持的滑台驱动器识别集合 (YKD2205PE) -------------------------- */
#define YKD_VENDOR_ID     0x0994UL
static const uint32_t ykd_product_codes[] = { 0x2000UL, 0x3000UL };
#define YKD_PRODUCT_COUNT ((int)(sizeof(ykd_product_codes) / sizeof(ykd_product_codes[0])))

/* 逐台/逐项判定 */
#define SV_V_PASS    0
#define SV_V_WARN    1
#define SV_V_FAIL    2
static const char *sv_verdict_str(int v)
{
   switch (v)
   {
      case SV_V_PASS: return "PASS";
      case SV_V_WARN: return "WARN";
      case SV_V_FAIL: return "FAIL";
      default:        return "----";
   }
}

/* ---- SOEM 上下文 (与 aliasinfo/slaveinfo 同构) ----------------------- */
static ecx_contextt ctx;

/* 每台从站的阶段结果 (下标 = SOEM 从站序号 1..EC_MAXSLAVE) */
static char g_is_ykd[EC_MAXSLAVE + 1];     /* 1 = 受支持 YKD2205PE */
static char g_reached_preop[EC_MAXSLAVE + 1]; /* 1 = 已确认进 PRE_OP */
static char g_axis_fail[EC_MAXSLAVE + 1];  /* 1 = 该轴必需项 FAIL(未达 PRE_OP 或必需对象失败) */
static char g_axis_warn[EC_MAXSLAVE + 1];  /* 1 = 该轴有非致命 WARN(可选缺失/异常读数) */

/* 汇总计数 */
static int g_total;
static int g_ykd_count;
static int g_foreign_count;

/* 让 MSVC/MinGW 控制台按 UTF-8 显示中文 (源码为 UTF-8) */
static void sv_console_utf8(void)
{
#ifdef _WIN32
   SetConsoleOutputCP(CP_UTF8);
#else
   (void)0;
#endif
}

/* 列出可用网卡 (照抄 aliasinfo.c 的 list_adapters) */
static void sv_print_adapters(void)
{
   ec_adaptert *adapter = NULL;
   ec_adaptert *head = NULL;

   head = adapter = ec_find_adapters();
   while (adapter != NULL)
   {
      printf("    - %s  (%s)\n", adapter->name, adapter->desc);
      adapter = adapter->next;
   }
   ec_free_adapters(head);
}

/* 该从站是否为受支持的 YKD2205PE 滑台驱动器 (厂商 + 产品码集合) */
static int sv_is_ykd_slave(const struct ec_slave *s)
{
   int k;

   if (s->eep_man != YKD_VENDOR_ID)
      return 0;
   for (k = 0; k < YKD_PRODUCT_COUNT; k++)
   {
      if (s->eep_id == ykd_product_codes[k])
         return 1;
   }
   return 0;
}

/* ======================================================================
 * P2A: PRE_OP 对象级验证 (数据驱动对象矩阵 + SDO 读取)
 * ====================================================================== */

/* 对象数据类型 (决定读取字节数与数值解读) */
#define SV_DT_U8  0
#define SV_DT_I8  1
#define SV_DT_U16 2
#define SV_DT_I32 3
#define SV_DT_U32 4

/* 角色: 必需 = 缺失判 FAIL; 可选 = 缺失判 WARN */
#define SV_ROLE_REQUIRED 0
#define SV_ROLE_OPT      1

typedef struct
{
   uint16_t index;   /* 对象索引 */
   uint8_t  sub;     /* 子索引 */
   const char *label;/* 中文名 (打印用) */
   uint8_t  dt;      /* 数据类型 SV_DT_* */
   uint8_t  role;    /* SV_ROLE_* */
} sv_obj_t;

/* 校验矩阵: 改这里的表即可增删检查对象, 主循环不用动。
 * 必需: 6041h/6060h/6061h/6064h/606Ch (CiA402 轴必备)
 * 可选: 607Ah/6081h/6083h/6084h/60FDh/60FEh */
static const sv_obj_t sv_objs[] = {
   { 0x6041, 0x00, "6041h 状态字",   SV_DT_U16, SV_ROLE_REQUIRED },
   { 0x6060, 0x00, "6060h 操作模式", SV_DT_I8,  SV_ROLE_REQUIRED },
   { 0x6061, 0x00, "6061h 当前模式", SV_DT_I8,  SV_ROLE_REQUIRED },
   { 0x6064, 0x00, "6064h 实际位置", SV_DT_I32, SV_ROLE_REQUIRED },
   { 0x606C, 0x00, "606Ch 实际速度", SV_DT_I32, SV_ROLE_REQUIRED },
   { 0x607A, 0x00, "607Ah 目标位置", SV_DT_U32, SV_ROLE_OPT },
   { 0x6081, 0x00, "6081h PP 速度",  SV_DT_U32, SV_ROLE_OPT },
   { 0x6083, 0x00, "6083h 加速度",   SV_DT_U32, SV_ROLE_OPT },
   { 0x6084, 0x00, "6084h 减速度",   SV_DT_U32, SV_ROLE_OPT },
   { 0x60FD, 0x00, "60FDh 数字输入", SV_DT_U32, SV_ROLE_OPT },
   { 0x60FE, 0x00, "60FEh 数字输出", SV_DT_U32, SV_ROLE_OPT },
};
#define SV_OBJ_COUNT ((int)(sizeof(sv_objs) / sizeof(sv_objs[0])))

static int sv_dt_size(int dt)
{
   switch (dt)
   {
      case SV_DT_U8:  return 1;
      case SV_DT_I8:  return 1;
      case SV_DT_U16: return 2;
      case SV_DT_I32: return 4;
      case SV_DT_U32: return 4;
      default:        return 1;
   }
}

/* SDO 读结果码 */
#define SV_RD_OK      0   /* 成功 */
#define SV_RD_ABORT   1   /* 收到 SDO abort (对象/子索引不存在 或 其它) */
#define SV_RD_TIMEOUT 2   /* 从站无响应/邮箱超时 */

/*
 * 读一个对象 (≤4 字节 expedited)。
 * 关键: ctx.ecaterror 是粘滞位, SOEM 报错后不会自动清零, 必须在每次
 * SDO 读前手动复位, 否则上一个失败会污染下一次判定。
 * 返回 SV_RD_*; 命中 abort 时在 *abort_code 返回首个匹配本对象的中止码。
 */
static int sv_sdo_read(int slave, const sv_obj_t *obj, uint8_t *buf, int32_t *abort_code)
{
   int psize = sv_dt_size(obj->dt);
   int wkc;
   int32_t abort = 0;

   ctx.ecaterror = FALSE;               /* 清粘滞位 */
   wkc = ecx_SDOread(&ctx, slave, obj->index, obj->sub, FALSE,
                     &psize, buf, EC_TIMEOUTRXM);
   if (wkc > 0)
      return SV_RD_OK;

   if (ctx.ecaterror)
   {
      ec_errort err;
      while (ecx_poperror(&ctx, &err))
      {
         if (err.Etype == EC_ERR_TYPE_SDO_ERROR &&
             err.Slave == (uint16_t)slave &&
             err.Index == obj->index &&
             err.SubIdx == obj->sub)
         {
            abort = err.AbortCode;
            break;
         }
      }
      if (abort_code != NULL)
         *abort_code = abort;
      return SV_RD_ABORT;
   }
   return SV_RD_TIMEOUT;
}

/* 把对象原始字节按类型解读成十进制文本 (输出到 out) */
static void sv_value_dec(const sv_obj_t *obj, const uint8_t *buf, char *out, size_t outsz)
{
   uint8_t  b8;  int8_t  s8;
   uint16_t u16; uint32_t u32; int32_t s32;

   switch (obj->dt)
   {
      case SV_DT_U8:
         memcpy(&b8, buf, 1);
         snprintf(out, outsz, "%u", (unsigned)b8);
         break;
      case SV_DT_I8:
         memcpy(&s8, buf, 1);
         snprintf(out, outsz, "%d", (int)s8);
         break;
      case SV_DT_U16:
         memcpy(&u16, buf, 2);
         snprintf(out, outsz, "0x%04X (%u)", (unsigned)u16, (unsigned)u16);
         break;
      case SV_DT_I32:
         memcpy(&s32, buf, 4);
         snprintf(out, outsz, "0x%08X (%d)", (unsigned)s32, (int)s32);
         break;
      case SV_DT_U32:
         memcpy(&u32, buf, 4);
         snprintf(out, outsz, "0x%08X (%u)", (unsigned)u32, (unsigned)u32);
         break;
      default:
         snprintf(out, outsz, "?");
         break;
   }
}

/* 操作模式名称 (6060h/6061h, I8): 1=PP 3=PV 6=HM 8=CSP */
static const char *sv_mode_name(int8_t mode)
{
   switch (mode)
   {
      case 1:  return "PP (位置)";
      case 3:  return "PV (速度)";
      case 6:  return "HM (回零)";
      case 8:  return "CSP (周期同步位置)";
      default: return NULL;
   }
}

/*
 * 合理性/解码: 对必需对象出现的"读数异常"给出 WARN 提示。
 * 把解码说明追加进 dec[]; 返回 1 表示有 WARN (只对必需对象调用)。
 */
static int sv_obj_sanity(const sv_obj_t *obj, const uint8_t *buf, char *dec, size_t decsz)
{
   int warn = 0;
   size_t len = strlen(dec);

   switch (obj->index)
   {
      case 0x6041:                       /* 状态字 (U16) */
      {
         uint16_t st;
         memcpy(&st, buf, 2);
         snprintf(dec + len, decsz - len, " | 低4位=0x%X", (unsigned)(st & 0x000F));
         len = strlen(dec);
         if (st & 0x0001) { snprintf(dec + len, decsz - len, " 就绪"); len = strlen(dec); }
         if (st & 0x0002) { snprintf(dec + len, decsz - len, " 已上电"); len = strlen(dec); }
         if (st & 0x0004) { snprintf(dec + len, decsz - len, " 已使能"); len = strlen(dec); }
         if (st & 0x0008)
         {
            warn = 1;
            snprintf(dec + len, decsz - len, " [WARN: Fault 位 bit3=1, 驱动器处于故障态]");
            len = strlen(dec);
         }
         if (st & 0x0400) { snprintf(dec + len, decsz - len, " 到位(bit10)"); len = strlen(dec); }
         break;
      }
      case 0x6060:                       /* 操作模式 (I8) */
      case 0x6061:                       /* 当前模式 (I8) */
      {
         int8_t m;
         const char *name;
         memcpy(&m, buf, 1);
         name = sv_mode_name(m);
         if (name != NULL)
         {
            snprintf(dec + len, decsz - len, " | %s", name);
            len = strlen(dec);
         }
         else
         {
            warn = 1;
            snprintf(dec + len, decsz - len, " [WARN: 未知操作模式 %d (支持 1/3/6/8)]", (int)m);
            len = strlen(dec);
         }
         break;
      }
      case 0x606C:                       /* 实际速度 (I32) */
      {
         int32_t v;
         memcpy(&v, buf, 4);
         if (v != 0)
         {
            warn = 1;
            snprintf(dec + len, decsz - len, " [WARN: PRE_OP 未使能状态下仍有速度 %d]",
                     (int)v);
            len = strlen(dec);
         }
         break;
      }
      default:
         break;
   }
   return warn;
}

/*
 * 对一台 YKD 从站做整表对象校验并逐行报告。
 * 更新 g_axis_fail/g_axis_warn。
 */
static void sv_verify_objects(int slave)
{
   int pos = slave - 1;
   int k;
   int req_fail = 0, warn = 0;

   printf("\n  === 位置 %d: 对象级验证 (PRE_OP, 只读 SDO) ===\n", pos);
   for (k = 0; k < SV_OBJ_COUNT; k++)
   {
      const sv_obj_t *obj = &sv_objs[k];
      uint8_t buf[4] = { 0, 0, 0, 0 };
      int32_t abort = 0;
      int rc;
      char dec[128], val[48];
      int swarn = 0;

      rc = sv_sdo_read(slave, obj, buf, &abort);
      sv_value_dec(obj, buf, val, sizeof(val));

      if (rc == SV_RD_OK)
      {
         dec[0] = '\0';
         snprintf(dec, sizeof(dec), "值=%s", val);
         if (obj->role == SV_ROLE_REQUIRED)
            swarn = sv_obj_sanity(obj, buf, dec, sizeof(dec));

         if (swarn)
         {
            warn = 1;
            printf("      [WARN] %-14s %s\n", obj->label, dec);
         }
         else
         {
            printf("      [PASS] %-14s %s\n", obj->label, dec);
         }
      }
      else if (rc == SV_RD_ABORT)
      {
         int missing = (abort == 0x06020000L) || (abort == 0x06090011L);
         if (obj->role == SV_ROLE_REQUIRED)
         {
            req_fail = 1;
            printf("      [FAIL] %-14s %s (abort 0x%08X)\n", obj->label,
                   missing ? "对象/子索引不存在" : "SDO 中止", (unsigned)abort);
         }
         else
         {
            warn = 1;
            printf("      [WARN] %-14s 可选对象缺失 (abort 0x%08X)\n",
                   obj->label, (unsigned)abort);
         }
      }
      else /* SV_RD_TIMEOUT */
      {
         if (obj->role == SV_ROLE_REQUIRED)
         {
            req_fail = 1;
            printf("      [FAIL] %-14s 从站无响应/邮箱超时 (检查网线/供电/是否掉线)\n",
                   obj->label);
         }
         else
         {
            warn = 1;
            printf("      [WARN] %-14s 无响应 (从站忙/掉线), 该项跳过\n", obj->label);
         }
      }
   }

   g_axis_fail[slave] = (req_fail != 0);
   g_axis_warn[slave] = (warn != 0);
   printf("  === 位置 %d 对象级结论: %s ===\n",
          pos, sv_verdict_str(g_axis_fail[slave] ? SV_V_FAIL
                                   : (g_axis_warn[slave] ? SV_V_WARN : SV_V_PASS)));
}

/* ======================================================================
 * P2B: SAFE_OP 探针 (可选; 失败不致命)
 * 说明: config_map_group 会写 SM/FMMU 并在自动模式下请求 SAFE_OP。本工具
 *       不依赖 PDO 周期数据, 只把"能否进 SAFE_OP"当作一个 WARN 级提示。
 * ====================================================================== */
static void sv_probe_safeop(void)
{
   int ret, slave;
   int probe_skipped = 0;

   printf("\n---- SAFE_OP 探针 (可选; 失败不致命, 默认空 TxPDO 时进不去属正常) ----\n");

   ret = ecx_config_map_group(&ctx, NULL, 0);
   if (ret <= 0)
   {
      printf("  config_map_group 返回 %d (无有效 PDO 映射), 跳过 SAFE_OP 探针并记 WARN。\n",
             ret);
      probe_skipped = 1;
   }
   else
   {
      /* config_map_group 已请求 SAFE_OP; 阻塞等待全组到位 (写法同 slaveinfo)。 */
      ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 3);
      ecx_readstate(&ctx);
   }

   for (slave = 1; slave <= ctx.slavecount; slave++)
   {
      const struct ec_slave *s = &ctx.slavelist[slave];
      int pos = slave - 1;

      if (!g_is_ykd[slave] || !g_reached_preop[slave])
         continue; /* 只评估已确认进 PRE_OP 的 YKD 轴 */

      if (probe_skipped)
      {
         g_axis_warn[slave] = 1;
         printf("  位置 %-3d [WARN] SAFE_OP 探针被跳过 (无有效 PDO 映射)。\n", pos);
      }
      else if (s->state == EC_STATE_SAFE_OP)
      {
         printf("  位置 %-3d [PASS] 已进入 SAFE_OP (邮箱与同步管理器校验通过)。\n", pos);
      }
      else
      {
         g_axis_warn[slave] = 1;
         printf("  位置 %-3d [WARN] 未能进入 SAFE_OP (状态 0x%02X, AL 状态码 0x%04X %s)。\n",
                pos,
                (unsigned)s->state, (unsigned)s->ALstatuscode,
                ec_ALstatuscode2string(s->ALstatuscode));
      }
   }
}

/* ======================================================================
 * P1: 身份表打印
 * ====================================================================== */
static void sv_print_identity_header(void)
{
   printf("%-5s %-5s %-8s %-12s %-11s %-11s %-11s %-11s %-6s %s\n",
          "位置", "序号", "配置地址", "别名0012h", "厂商ID", "产品码",
          "修订", "序列号", "状态", "名称");
   printf("%-5s %-5s %-8s %-12s %-11s %-11s %-11s %-11s %-6s %s\n",
          "-----", "-----", "--------", "------------", "----------",
          "----------", "----------", "----------", "------", "----");
}

static void sv_print_identity_row(int slave)
{
   const struct ec_slave *s = &ctx.slavelist[slave];

   printf("%-5d %-5d 0x%04X  %-12u 0x%08X  0x%08X  0x%08X  0x%08X  0x%02X  %s\n",
          slave - 1,                    /* 总线位置 0-based (与 python 链 --pos 一致) */
          slave,                        /* SOEM 从站序号 1-based */
          (unsigned)s->configadr,
          (unsigned)s->aliasadr,        /* ESC 0012h-0013h 拨码站号 */
          (unsigned)s->eep_man,
          (unsigned)s->eep_id,
          (unsigned)s->eep_rev,
          (unsigned)s->eep_ser,
          (unsigned)s->state,
          s->name);
}

/* ======================================================================
 * P3: 汇总与退出码收口
 * ====================================================================== */
static int sv_final_exit(void)
{
   int slave;
   int axis_fail_total = 0;
   int any_warn = 0;

   if (g_total == 0)
      return SV_EXIT_NO_SLAVE;
   if (g_ykd_count == 0)
      return SV_EXIT_NO_YKD;

   for (slave = 1; slave <= g_total; slave++)
   {
      if (!g_is_ykd[slave])
         continue;
      if (g_axis_fail[slave])
         axis_fail_total++;
      if (g_axis_warn[slave])
         any_warn = 1;
   }
   if (axis_fail_total > 0)
      return SV_EXIT_FAIL;
   if (any_warn || g_foreign_count > 0)
      return SV_EXIT_WARN;
   return SV_EXIT_OK;
}

int main(int argc, char *argv[])
{
   int cnt, slave;

   sv_console_utf8();

   printf("slide_verify - 滑台设备导入验证 (原生 SOEM, 只读)\n");

   if (argc < 2)
   {
      printf("用法: slide_verify ifname\nifname = 网卡名, 如 \\Device\\NPF_{GUID}\n\n可用网卡:\n");
      sv_print_adapters();
      return SV_EXIT_USAGE;
   }

   if (!ecx_init(&ctx, argv[1]))
   {
      printf("无法打开网卡 %s (被独占/需管理员/驱动未装)。\n", argv[1]);
      return SV_EXIT_USAGE;
   }

   cnt = ecx_config_init(&ctx);
   if (cnt <= 0)
   {
      printf("总线上未发现从站 (config_init=%d), 检查网卡/网线/供电。\n", cnt);
      ecx_close(&ctx);
      return SV_EXIT_NO_SLAVE;
   }
   g_total = cnt;

   /* ecx_config_init 已请求各从站进入 PRE_OP; 逐台确认。 */
   printf("\n等待从站进入 PRE_OP ...\n");
   for (slave = 1; slave <= cnt; slave++)
      ecx_statecheck(&ctx, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   ecx_readstate(&ctx);

   /* ---- 身份表 ---- */
   printf("\n");
   sv_print_identity_header();
   for (slave = 1; slave <= cnt; slave++)
      sv_print_identity_row(slave);

   /* ---- 逐台身份判定 ---- */
   printf("\n---- 身份判定 ----\n");
   for (slave = 1; slave <= cnt; slave++)
   {
      const struct ec_slave *s = &ctx.slavelist[slave];
      int pos = slave - 1;
      int ykd = sv_is_ykd_slave(s);
      int reached_preop = (s->state == EC_STATE_PRE_OP);

      g_is_ykd[slave] = (char)ykd;
      g_reached_preop[slave] = (char)reached_preop;

      if (ykd)
      {
         g_ykd_count++;
         if (!reached_preop)
         {
            g_axis_fail[slave] = 1;      /* 未达 PRE_OP 记整台 FAIL, 跳过对象读 */
            printf("  位置 %-3d [FAIL] YKD2205PE (厂商 0x%08X / 产品 0x%08X) 未进入 PRE_OP"
                   " (状态 0x%02X, AL 状态码 0x%04X %s)。\n",
                   pos,
                   (unsigned)s->eep_man, (unsigned)s->eep_id,
                   (unsigned)s->state, (unsigned)s->ALstatuscode,
                   ec_ALstatuscode2string(s->ALstatuscode));
         }
         else
         {
            printf("  位置 %-3d [PASS] YKD2205PE 滑台驱动器: 厂商=0x%08X 产品码=0x%08X"
                   " 修订=0x%08X, 已进入 PRE_OP。\n",
                   pos,
                   (unsigned)s->eep_man, (unsigned)s->eep_id, (unsigned)s->eep_rev);
         }
      }
      else
      {
         g_foreign_count++;
         printf("  位置 %-3d [WARN] 非受支持从站 (厂商=0x%08X 产品码=0x%08X, 名称=%s): "
                "本工具只验证 YKD2205PE 滑台驱动器。\n",
                pos,
                (unsigned)s->eep_man, (unsigned)s->eep_id, s->name);
      }
   }

   /* 别名 = 0 提示 (同型号多台时按总线位置区分) */
   for (slave = 1; slave <= cnt; slave++)
   {
      if (ctx.slavelist[slave].aliasadr == 0)
      {
         printf("  位置 %d 别名=0 (未设拨码站号或拨码=0), 同型号多台时请按总线位置区分。\n",
                slave - 1);
      }
   }

   /* ---- P2A: 对每台已进 PRE_OP 的 YKD 做对象级验证 ---- */
   for (slave = 1; slave <= cnt; slave++)
   {
      if (g_is_ykd[slave] && g_reached_preop[slave])
         sv_verify_objects(slave);
   }

   /* ---- P2B: 可选 SAFE_OP 探针 (失败不致命, 记 WARN) ---- */
   sv_probe_safeop();

   ecx_close(&ctx);

   /* ---- 汇总 ---- */
   printf("\n==== 汇总 ====\n");
   printf("  总线从站总数 : %d\n", g_total);
   printf("  受支持 YKD   : %d\n", g_ykd_count);
   printf("  非 YKD 从站  : %d\n", g_foreign_count);
   {
      int axis_pass = 0, axis_fail = 0, axis_warn = 0, i;
      for (i = 1; i <= cnt; i++)
      {
         if (!g_is_ykd[i])
            continue;
         if (g_axis_fail[i])      axis_fail++;
         else if (g_axis_warn[i]) axis_warn++;
         else                     axis_pass++;
      }
      printf("  YKD 逐台结果 : %d PASS, %d WARN, %d FAIL\n", axis_pass, axis_warn, axis_fail);
   }

   return sv_final_exit();
}
