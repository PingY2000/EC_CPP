/*
 * sm_baseline.c - 参数基线与漂移比对 (S1)
 *
 * 目的: 导入验证里"能读"不等于"配置对"。滑台装机后参数被改过 (电子齿轮、
 * 电流、限位输入功能定义) 是动作验证失败最常见的原因, 而且不比对就看不出来。
 *
 * 做法: 把现场实测值导出成一份外部基线文件 (--dump-baseline), 人工审定后
 * 作为"验收基线"; 此后每次导入都用 --baseline 与它比对, 报告漂移。
 *
 * 本文件**不含任何写操作** (纯只读 SDO)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm.h"

/* ======================================================================
 * 参数规格表 —— 加对象只改这张表, 主循环不用动
 *
 * role/dangerous 的取舍:
 *   dangerous=1 表示"这一项错了, S3/S4 的动作结论就不可信", 漂移即禁止动作。
 *   典型是 2310h~2313h (限位/原点输入的功能定义, 决定 60FDh 每一位是什么)
 *   与 2408h/2409h (电子齿轮, 决定脉冲与物理位移的换算)。
 *
 * dt 只是"符号性提示"; 实际字节宽度以驱动器 SDO 自报的 psize 为准,
 * 所以手册没给类型的对象 (2000h~2FFFh 那一批) 也能安全读。
 * ====================================================================== */
static const sm_spec_t sm_spec_table[] = {
   /* ---- 限位/原点输入功能定义: 决定 60FDh 每位的含义, 漂移即禁止动作 ---- */
   { 0x2310, 0x00, SM_DT_U8, SM_CMP_EXACT, 1, 0, "2310h X0 功能 (1原点/2正限/3负限/4停止/5急停)" },
   { 0x2311, 0x00, SM_DT_U8, SM_CMP_EXACT, 1, 0, "2311h X1 功能" },
   { 0x2312, 0x00, SM_DT_U8, SM_CMP_EXACT, 1, 0, "2312h X2 功能" },
   { 0x2313, 0x00, SM_DT_U8, SM_CMP_EXACT, 1, 0, "2313h X3 功能" },

   /* ---- 电子齿轮: 决定脉冲<->位移换算, 漂移即禁止动作 ---- */
   { 0x2408, 0x00, SM_DT_U16, SM_CMP_EXACT, 1, 0, "2408h 电子齿轮 (1~51200)" },
   { 0x2409, 0x00, SM_DT_U16, SM_CMP_EXACT, 1, 0, "2409h 电子齿轮 (1~51200)" },

   /* ---- 超程保护 ---- */
   { 0x2204, 0x00, SM_DT_U8, SM_CMP_EXACT, 1, 0, "2204h 超程停车方式 (0停止/1急停)" },

   /* ---- IO 电平逻辑与滤波 (错了不致命, 但会让 60FDh 反相, 记 WARN) ---- */
   { 0x2300, 0x00, SM_DT_U8, SM_CMP_EXACT, 0, 0, "2300h 输入有效电平逻辑" },
   { 0x2301, 0x00, SM_DT_U8, SM_CMP_EXACT, 0, 0, "2301h 输出有效电平逻辑" },
   { 0x2320, 0x00, SM_DT_U8, SM_CMP_EXACT, 0, 0, "2320h Y0 功能" },
   { 0x2321, 0x00, SM_DT_U8, SM_CMP_EXACT, 0, 0, "2321h Y1 功能" },
   { 0x2330, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2330h X0 滤波时间" },
   { 0x2331, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2331h X1 滤波时间" },
   { 0x2332, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2332h X2 滤波时间" },
   { 0x2333, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2333h X3 滤波时间" },

   /* ---- 电流 (整定值可随负载调整, 用百分比容差) ---- */
   { 0x2400, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 10, "2400h 电子齿轮分子相关" },
   { 0x2401, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 10, "2401h 最大电流 mA" },
   { 0x2402, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 20, "2402h 运行电流 %" },
   { 0x2404, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 20, "2404h 电流 %" },
   { 0x2405, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 20, "2405h 电流 %" },
   { 0x2406, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2406h 时间参数 ms" },

   /* ---- 位置环增益 ---- */
   { 0x2500, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2500h 位置环 Kp" },
   { 0x2501, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2501h Kp" },
   { 0x2502, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2502h Ki" },
   { 0x2503, 0x00, SM_DT_U16, SM_CMP_PCT, 0, 50, "2503h Kc" },

   /* ---- 软限位 (很可能不存在, 读不到就记 INFO; 读得到就是硬约束) ---- */
   { 0x607D, 0x01, SM_DT_I32, SM_CMP_EXACT, 0, 0, "607Dh:01 软限位下限" },
   { 0x607D, 0x02, SM_DT_I32, SM_CMP_EXACT, 0, 0, "607Dh:02 软限位上限" },

   /* ---- 回零 ---- */
   { 0x6098, 0x00, SM_DT_I8,  SM_CMP_EXACT, 0, 0,  "6098h 回零方式 (17/18/24/29)" },
   { 0x6099, 0x01, SM_DT_U32, SM_CMP_PCT, 0, 50, "6099h:01 找开关速度" },
   { 0x6099, 0x02, SM_DT_U32, SM_CMP_PCT, 0, 50, "6099h:02 找零速度" },
   { 0x609A, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 50, "609Ah 回零加速度" },
   { 0x607C, 0x00, SM_DT_I32, SM_CMP_PCT, 0, 100, "607Ch 回零偏移" },

   /* ---- PP 运动参数 (微动会临时改它们, 收尾恢复; 基线用来确认出厂/验收值) ---- */
   { 0x6081, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 50, "6081h PP 速度" },
   { 0x6083, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 50, "6083h 加速度" },
   { 0x6084, 0x00, SM_DT_U32, SM_CMP_PCT, 0, 50, "6084h 减速度" },
   { 0x60FF, 0x00, SM_DT_I32, SM_CMP_PCT, 0, 100, "60FFh 目标速度 (PV)" },
};

#define SM_SPEC_N ((int)(sizeof(sm_spec_table) / sizeof(sm_spec_table[0])))

const sm_spec_t *sm_specs(int *count)
{
   if (count != NULL)
      *count = SM_SPEC_N;
   return sm_spec_table;
}

/* 按 index/sub 找槽位; -1 = 未找到 */
static int spec_find(uint16_t index, uint8_t sub)
{
   int i;

   for (i = 0; i < SM_SPEC_N; i++)
   {
      if (sm_spec_table[i].index == index && sm_spec_table[i].sub == sub)
         return i;
   }
   return -1;
}

/* ======================================================================
 * 文本工具
 * ====================================================================== */
static char *trim(char *s)
{
   char *e;

   while (*s == ' ' || *s == '\t')
      s++;
   e = s + strlen(s);
   while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                    e[-1] == '\n'))
      e--;
   *e = '\0';
   return s;
}

/* 解析 "2310h:00" / "2310h" / "0x2310" -> index/sub。成功返回 0 */
static int parse_key(const char *key, uint16_t *index, uint8_t *sub)
{
   char     buf[64];
   char    *colon;
   char    *endp;
   long     v;

   if (strlen(key) >= sizeof(buf))
      return -1;
   strcpy(buf, key);

   *sub = 0;
   colon = strchr(buf, ':');
   if (colon != NULL)
   {
      long sv;
      *colon = '\0';
      sv = strtol(colon + 1, &endp, 16);   /* 子索引十六进制, 与手册一致 */
      if (*endp != '\0' || sv < 0 || sv > 255)
         return -1;
      *sub = (uint8_t)sv;
   }

   /* 去掉可能存在的结尾 'h'/'H' */
   {
      size_t n = strlen(buf);
      if (n > 0 && (buf[n - 1] == 'h' || buf[n - 1] == 'H'))
         buf[n - 1] = '\0';
   }

   v = strtol(buf, &endp, 16);
   /* "0x2310" 让 strtol 用 base 16 也能吃下; 纯十进制写法不支持 —— 索引一律按十六进制 */
   if (*endp != '\0' || v < 0 || v > 0xFFFF)
      return -1;
   *index = (uint16_t)v;
   return 0;
}

/* ======================================================================
 * 解析
 * ====================================================================== */

/*
 * 极简 INI 解析。支持:
 *   # 或 ; 起注释; [section] 切节; key = value
 *   行首 '!' (在 [axis.N] 节内) = 该项为必需, 不匹配判 FAIL
 *   value 十进制或 0x 前缀十六进制
 * 未知节/未知键 -> 计数并 WARN, 不中断 (基线文件允许先写后面才支持的对象)。
 */
int sm_base_load(const char *path, sm_baseline_t *b)
{
   FILE *f;
   char  line[512];
   int   lineno = 0;
   int   section = 0;       /* 0=无 1=meta 2=motion 3=axis */
   int   cur_axis = -1;
   int   nspec = 0;

   memset(b, 0, sizeof(*b));
   sm_specs(&nspec);

   if (path == NULL)
      return SM_EXIT_BASELINE;

   f = fopen(path, "r");
   if (f == NULL)
   {
      printf("基线文件打不开: %s\n", path);
      return SM_EXIT_USAGE;
   }
   strncpy(b->path, path, sizeof(b->path) - 1);

   while (fgets(line, (int)sizeof(line), f) != NULL)
   {
      char  *s;
      char  *hash;
      int    strict = 0;

      lineno++;

      /* 去注释 */
      hash = strchr(line, '#');
      if (hash != NULL)
         *hash = '\0';
      hash = strchr(line, ';');
      if (hash != NULL)
         *hash = '\0';

      s = trim(line);
      if (*s == '\0')
         continue;

      /* 节 */
      if (*s == '[')
      {
         char *close = strchr(s, ']');
         if (close == NULL)
         {
            printf("基线文件 %s 第 %d 行: 节标签缺少 ']'。\n", path, lineno);
            fclose(f);
            return SM_EXIT_BASELINE;
         }
         *close = '\0';
         s = trim(s + 1);
         if (strcmp(s, "meta") == 0)
         {
            section = 1;
            cur_axis = -1;
         }
         else if (strcmp(s, "motion") == 0)
         {
            section = 2;
            cur_axis = -1;
         }
         else if (strncmp(s, "axis.", 5) == 0)
         {
            long a = strtol(s + 5, NULL, 10);
            if (a < 0 || a >= SM_MAX_AXES)
            {
               printf("基线文件 %s 第 %d 行: 轴号 %ld 超出支持范围 0..%d。\n",
                      path, lineno, a, SM_MAX_AXES - 1);
               fclose(f);
               return SM_EXIT_BASELINE;
            }
            section = 3;
            cur_axis = (int)a;
         }
         else
         {
            printf("基线文件 %s 第 %d 行: 未知节 [%s], 已跳过。\n",
                   path, lineno, s);
            b->warn_count++;
            section = 0;
            cur_axis = -1;
         }
         continue;
      }

      /* 行首 '!' = 必需项 (只在 axis 节内有意义) */
      if (*s == '!')
      {
         strict = 1;
         s = trim(s + 1);
      }

      /* key = value */
      {
         char *eq = strchr(s, '=');
         char *key;
         char *val;
         long long v;

         if (eq == NULL)
         {
            printf("基线文件 %s 第 %d 行: 不是 key = value 形式, 已跳过: %s\n",
                   path, lineno, s);
            b->warn_count++;
            continue;
         }
         *eq = '\0';
         key = trim(s);
         val = trim(eq + 1);
         v = strtoll(val, NULL, 0);   /* base 0: 自动识别 0x 前缀 */

         if (section == 1)            /* [meta] */
         {
            if (strcmp(key, "vendor_id") == 0)
            {
               b->vendor_id = (uint32_t)v;
               b->have_vendor = 1;
            }
            else if (strcmp(key, "product_code") == 0)
            {
               b->product_code = (uint32_t)v;
               b->have_product = 1;
            }
            /* 其它 meta 键 (version/created/...) 忽略 */
            continue;
         }
         if (section == 2)            /* [motion] */
         {
            if (strcmp(key, "jog_pulses") == 0)   { b->jp = v; b->have_jp = 1; }
            else if (strcmp(key, "jog_velocity") == 0) { b->jv = v; b->have_jv = 1; }
            else if (strcmp(key, "jog_accel") == 0)    { b->ja = v; b->have_ja = 1; }
            else if (strcmp(key, "tolerance_pct") == 0){ b->tol = v; b->have_tol = 1; }
            else
            {
               printf("基线文件 %s 第 %d 行: [motion] 未知键 %s, 已跳过。\n",
                      path, lineno, key);
               b->warn_count++;
            }
            continue;
         }
         if (section == 3 && cur_axis >= 0)   /* [axis.N] */
         {
            uint16_t idx;
            uint8_t  sub;
            int      slot;

            if (parse_key(key, &idx, &sub) != 0)
            {
               printf("基线文件 %s 第 %d 行: 无法解析对象名 %s, 已跳过。\n",
                      path, lineno, key);
               b->warn_count++;
               continue;
            }
            slot = spec_find(idx, sub);
            if (slot < 0)
            {
               printf("基线文件 %s 第 %d 行: 对象 %04Xh:%02X 不在校验矩阵里, "
                      "已跳过 (加进 sm_spec_table 才会被比对)。\n",
                      path, lineno, (unsigned)idx, (unsigned)sub);
               b->warn_count++;
               continue;
            }
            b->slot[cur_axis][slot].present = 1;
            b->slot[cur_axis][slot].strict = strict;
            b->slot[cur_axis][slot].val = (int64_t)v;
            continue;
         }

         /* 不在任何已知节里 -> 忽略 */
         continue;
      }
   }

   fclose(f);
   b->loaded = 1;
   return 0;
}

/* ======================================================================
 * 导出
 * ====================================================================== */
/* 逐项导出: 读得到就写 key = value, 读不到就写成注释行 (基线文件自己记录空白) */
static void dump_axis_specs(FILE *f, int slave, int nspec)
{
   int k;

   for (k = 0; k < nspec; k++)
   {
      const sm_spec_t *sp = &sm_spec_table[k];
      char  key[32];
      char  prefix[48];
      uint8_t buf[4];
      int     size = 0;
      int32_t ab = 0;
      int     rc;

      if (sp->sub == 0)
         snprintf(key, sizeof(key), "%04Xh", (unsigned)sp->index);
      else
         snprintf(key, sizeof(key), "%04Xh:%02X", (unsigned)sp->index,
                  (unsigned)sp->sub);
      snprintf(prefix, sizeof(prefix), "  %-10s", key);

      rc = sm_rd_raw(slave, sp->index, sp->sub, SM_SDO_TMO_IDLE, buf, &size, &ab);
      if (rc == SM_RD_OK)
      {
         int64_t v = sm_bytes_to_i64(buf, size, sp->dt);
         fprintf(f, "%-14s= %-12lld # %s\n", prefix, (long long)v, sp->label);
      }
      else if (rc == SM_RD_ABORT)
      {
         fprintf(f, "# %-12s  <不可读: abort 0x%08X>  # %s\n", key,
                 (unsigned)ab, sp->label);
      }
      else
      {
         fprintf(f, "# %-12s  <不可读: 超时/无响应>     # %s\n", key,
                 sp->label);
      }
   }
}

/*
 * 把现场实测值导出为基线文件。
 *
 * 全部项都**不带 '!'** —— 由人读一遍再决定哪些升级成必需项。
 * 不可读的对象写成注释行, 让基线文件自己记录自己的空白, 而不是静默省略。
 */
int sm_base_dump(const char *path, const sm_axis_t *axes, int nslaves)
{
   FILE *f;
   int   i;
   int   nspec = 0;

   sm_specs(&nspec);

   if (path == NULL)
      return SM_EXIT_USAGE;

   f = fopen(path, "w");
   if (f == NULL)
   {
      printf("基线文件写不开: %s (目录不存在/无权限?)\n", path);
      return SM_EXIT_USAGE;
   }

   fprintf(f, "# slide_motion 参数基线 (由 --dump-baseline 生成)\n");
   fprintf(f, "#\n");
   fprintf(f, "# 行首加 '!' 表示该项为**必需**: 不匹配判 FAIL; 不加 '!' 为参考项, 不匹配判 WARN。\n");
   fprintf(f, "# 默认全部不带 '!', 请人工审定后把真正不允许变的对象加上 '!', 尤其是:\n");
   fprintf(f, "#   2310h~2313h (限位/原点输入功能定义) 与 2408h/2409h (电子齿轮)。\n");
   fprintf(f, "# 标了 '!' 的那两项一旦漂移, 工具会拒绝执行使能与微动 (S3/S4)。\n");
   fprintf(f, "\n[meta]\n");
   fprintf(f, "# vendor_id / product_code 与现场设备不一致时, 工具直接退出码 8\n");
   /* 用第一台 YKD 的身份作为基线身份 */
   for (i = 0; i < nslaves; i++)
   {
      if (axes[i].is_ykd)
      {
         fprintf(f, "vendor_id    = 0x%08X\n",
                 (unsigned)g_ctx.slavelist[axes[i].slave].eep_man);
         fprintf(f, "product_code = 0x%08X\n",
                 (unsigned)g_ctx.slavelist[axes[i].slave].eep_id);
         break;
      }
   }

   fprintf(f, "\n[motion]\n");
   fprintf(f, "# 微动默认参数。--jog-pulses 等命令行参数会覆盖这里的值, 且受硬上限约束。\n");
   fprintf(f, "jog_pulses    = %d\n", SM_JOG_PULSES_DEF);
   fprintf(f, "jog_velocity  = %d\n", SM_JOG_VEL_DEF);
   fprintf(f, "jog_accel     = %d\n", SM_JOG_ACC_DEF);
   fprintf(f, "tolerance_pct = 5\n");

   {
      int nykd = 0;
      for (i = 0; i < nslaves; i++)
      {
         if (!axes[i].is_ykd)
            continue;
         nykd++;
         fprintf(f, "\n[axis.%d]\n", axes[i].pos);
         dump_axis_specs(f, axes[i].slave, nspec);
      }
      fclose(f);
      printf("已导出基线文件: %s (%d 台 YKD 轴, 每轴 %d 项)\n",
             path, nykd, nspec);
   }
   return 0;
}

/* ======================================================================
 * 无基线时打印当前实测值表 (只读)
 *
 * 没有期望值就无所谓对错, 所以这里不做 PASS/FAIL 判定 —— 它存在的意义是:
 *   1. 不给 --baseline 时, 默认的只读运行也能看到完整的参数现状;
 *   2. 与 slide_verify 的对象表互为独立实现, 可互相印证读数是否一致。
 * 不可读的项标出来 —— 这本身就是一种信息 (对象不存在 vs 超时)。
 * ====================================================================== */
void sm_base_show(const sm_axis_t *axes, int nslaves)
{
   int i, k;
   int nspec = 0;

   sm_specs(&nspec);

   for (i = 0; i < nslaves; i++)
   {
      int n_read = 0, n_abort = 0, n_tmo = 0;

      if (!axes[i].is_ykd)
         continue;

      printf("\n  ---- 位置 %d: 当前参数现状 (只读; 无基线故不做判定) ----\n",
             axes[i].pos);

      for (k = 0; k < nspec; k++)
      {
         const sm_spec_t *sp = &sm_spec_table[k];
         uint8_t  buf[4];
         int      size = 0;
         int32_t  ab = 0;
         int      rc;

         rc = sm_rd_raw(axes[i].slave, sp->index, sp->sub, SM_SDO_TMO_IDLE,
                        buf, &size, &ab);
         if (rc == SM_RD_OK)
         {
            n_read++;
            printf("      [INFO] %-44s %lld\n", sp->label,
                   (long long)sm_bytes_to_i64(buf, size, sp->dt));
         }
         else if (rc == SM_RD_ABORT)
         {
            n_abort++;
            printf("      [INFO] %-44s <对象不存在: abort 0x%08X>\n",
                   sp->label, (unsigned)ab);
         }
         else
         {
            n_tmo++;
            printf("      [INFO] %-44s <无响应/超时>\n", sp->label);
         }
      }
      printf("      小结: %d 项可读, %d 项对象不存在, %d 项无响应\n",
             n_read, n_abort, n_tmo);
   }
}

/* ======================================================================
 * 比对 (S1)
 * ====================================================================== */
const sm_base_slot_t *sm_base_lookup(const sm_baseline_t *b, int pos,
                                     uint16_t index, uint8_t sub)
{
   int slot;
   int nspec = 0;

   sm_specs(&nspec);
   if (b == NULL || !b->loaded || pos < 0 || pos >= SM_MAX_AXES)
      return NULL;

   slot = spec_find(index, sub);
   if (slot < 0)
      return NULL;
   if (!b->slot[pos][slot].present)
      return NULL;
   return &b->slot[pos][slot];
}

/* 比较实测值 v 与期望值 e, 按 cmp 判等。返回 1 = 匹配 */
static int cmp_match(int cmp, int64_t v, int64_t e, uint32_t tol_pct)
{
   int64_t d, ad, ae, hi;

   if (cmp == SM_CMP_NONE)
      return 1;
   if (cmp == SM_CMP_EXACT)
      return (v == e);

   /* SM_CMP_PCT: |v-e| <= |e| * tol_pct / 100 */
   if (e == 0)
      return (v == 0);
   d  = v - e;
   ad = (d < 0) ? -d : d;
   ae = (e < 0) ? -e : e;
   hi = (ae * (int64_t)tol_pct) / 100;
   return (ad <= hi);
}

/*
 * 逐轴比对并打印。
 *
 * 语义:
 *   基线里列为必需 ('!') 的项不匹配 -> FAIL
 *   基线里列为参考的项不匹配       -> WARN
 *   基线里没有的项                 -> 只读出来显示 (INFO), 不判定
 *   基线里要求但读不到的项         -> FAIL (必需) / WARN (参考)
 *
 * *dangerous_drift 置 1 表示有 dangerous 项漂移 -> 调用者必须拒绝 S3/S4。
 */
int sm_base_compare(const sm_baseline_t *b, sm_axis_t *ax, int nslaves,
                    int *dangerous_drift)
{
   int i;
   int nspec = 0;

   sm_specs(&nspec);
   if (dangerous_drift != NULL)
      *dangerous_drift = 0;

   for (i = 0; i < nslaves; i++)
   {
      int k;
      int n_cmp = 0, n_fail = 0, n_warn = 0, n_danger = 0;

      if (!ax[i].is_ykd)
         continue;

      printf("\n  === 位置 %d: 参数基线比对 ===\n", ax[i].pos);

      for (k = 0; k < nspec; k++)
      {
         const sm_spec_t      *sp = &sm_spec_table[k];
         const sm_base_slot_t *sl;
         uint8_t  buf[4];
         int      size = 0;
         int32_t  ab = 0;
         int      rc;
         int64_t  v;

         sl = sm_base_lookup(b, ax[i].pos, sp->index, sp->sub);
         if (sl == NULL)
            continue;   /* 基线没要求 -> 不比 */

         n_cmp++;
         rc = sm_rd_raw(ax[i].slave, sp->index, sp->sub, SM_SDO_TMO_IDLE,
                        buf, &size, &ab);

         if (rc != SM_RD_OK)
         {
            if (sl->strict)
            {
               n_fail++;
               printf("      [FAIL] %-44s 基线要求但读不到 (%s)\n", sp->label,
                      (rc == SM_RD_ABORT) ? "对象不存在" : "超时/无响应");
            }
            else
            {
               n_warn++;
               printf("      [WARN] %-44s 读不到 (%s), 该项跳过\n", sp->label,
                      (rc == SM_RD_ABORT) ? "对象不存在" : "超时/无响应");
            }
            continue;
         }

         v = sm_bytes_to_i64(buf, size, sp->dt);

         if (cmp_match(sp->cmp, v, sl->val, sp->tol_pct))
         {
            printf("      [PASS] %-44s %lld\n", sp->label, (long long)v);
         }
         else if (sp->dangerous)
         {
            n_fail++;
            n_danger++;
            printf("      [FAIL] %-44s 期望 %lld 实测 %lld"
                   "  <<< 该参数决定动作语义, 漂移即拒绝 S3/S4\n",
                   sp->label, (long long)sl->val, (long long)v);
         }
         else if (sl->strict)
         {
            n_fail++;
            printf("      [FAIL] %-44s 期望 %lld 实测 %lld\n",
                   sp->label, (long long)sl->val, (long long)v);
         }
         else
         {
            n_warn++;
            printf("      [WARN] %-44s 期望 %lld 实测 %lld (参考项, 允许偏差)\n",
                   sp->label, (long long)sl->val, (long long)v);
         }
      }

      if (n_cmp == 0)
      {
         printf("      (基线文件里没有 [axis.%d] 的条目, 无项可比)\n", ax[i].pos);
      }

      if (n_fail > 0)
      {
         ax[i].fail = 1;
         if (n_danger > 0 && dangerous_drift != NULL)
            *dangerous_drift = 1;
      }
      else if (n_warn > 0)
      {
         ax[i].warn = 1;
      }

      printf("  === 位置 %d 基线结论: %s (比对 %d 项, %d FAIL, %d WARN) ===\n",
             ax[i].pos,
             sm_verdict_str(n_fail > 0 ? SM_V_FAIL
                                       : (n_warn > 0 ? SM_V_WARN : SM_V_PASS)),
             n_cmp, n_fail, n_warn);
   }
   return 0;
}
