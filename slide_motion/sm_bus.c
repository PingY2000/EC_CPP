/*
 * sm_bus.c - SOEM 总线底座: 初始化、身份判定、只读 SDO、原始寄存器、诊断
 *
 * 本文件**不含任何写操作**。所有 ecx_SDOwrite 调用点都在 sm_guard.c。
 * 这里只读: SDOread / 寄存器读 / 状态检查。
 */

#include <stdio.h>
#include <string.h>

#include "sm.h"

#ifdef _WIN32
#include <windows.h> /* GetTickCount64 */
#else
#include <time.h>    /* clock_gettime (POSIX 分支用) */
#endif

/* 全局 SOEM 上下文 (全工程一份, 定义在这里) */
ecx_contextt g_ctx;

/* 容忍的 YKD 产品码集合 (与 slide_verify.c 一致: 新旧两版都收) */
static const uint32_t sm_product_codes[] = { 0x2000UL, 0x3000UL };
#define SM_PRODUCT_COUNT \
   ((int)(sizeof(sm_product_codes) / sizeof(sm_product_codes[0])))

/* 让 MSVC/MinGW 控制台按 UTF-8 显示中文 (源码为 UTF-8) */
void sm_console_utf8(void)
{
#ifdef _WIN32
   SetConsoleOutputCP(CP_UTF8);
#else
   (void)0;
#endif
}

/* 单调毫秒时钟。只看差值, 不用来做绝对时间。 */
uint32_t sm_now_ms(void)
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

/* 列出可用网卡 (与 aliasinfo / slide_verify 的 list_adapters 同款) */
void sm_print_adapters(void)
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

/* 是否为受支持的 YKD2205PE 滑台驱动器 (厂商 + 产品码集合) */
int sm_is_ykd(uint32_t eep_man, uint32_t eep_id)
{
   int k;

   if (eep_man != SM_YKD_VENDOR_ID)
      return 0;
   for (k = 0; k < SM_PRODUCT_COUNT; k++)
   {
      if (eep_id == sm_product_codes[k])
         return 1;
   }
   return 0;
}

/* ======================================================================
 * 数据类型
 * ====================================================================== */
int sm_dt_is_signed(int dt)
{
   return (dt == SM_DT_I8) || (dt == SM_DT_I16) || (dt == SM_DT_I32);
}

/*
 * 把驱动器自报的字节解读为 int64。
 * size 来自 SDO 实际返回的字节数 (不是我们猜的类型), 所以即使手册没给类型
 * 也不会读错宽度。dt 只决定符号性。
 */
int64_t sm_bytes_to_i64(const uint8_t *buf, int size, int dt)
{
   int     is_signed = sm_dt_is_signed(dt);
   int64_t v = 0;

   switch (size)
   {
      case 1:
         v = is_signed ? (int64_t)(int8_t)buf[0]
                       : (int64_t)(uint8_t)buf[0];
         break;
      case 2:
      {
         uint16_t u;
         memcpy(&u, buf, 2);
         v = is_signed ? (int64_t)(int16_t)u : (int64_t)u;
         break;
      }
      case 3:
         /*
          * 3 字节也要解对。掉进 default 的话会返回 0 —— 而 0 在这里不是
          * "无法解读", 是一个看起来完全正常的值: 放在 60FDh 上就是
          * "两个限位都没压住", 放在 606Ch 上就是"轴没在动"。宁可解错也不要
          * 用一个合法值去掩盖读失败。
          */
         v = (int64_t)((uint32_t)buf[0] |
                       ((uint32_t)buf[1] << 8) |
                       ((uint32_t)buf[2] << 16));
         if (is_signed && (v & 0x800000LL) != 0)
            v -= 0x1000000LL;
         break;
      case 4:
      {
         uint32_t u;
         memcpy(&u, buf, 4);
         v = is_signed ? (int64_t)(int32_t)u : (int64_t)(uint32_t)u;
         break;
      }
      default:
         /*
          * 到不了这里: sm_rd_raw() 给 ecx_SDOread 的缓冲就是 4 字节, psize
          * 回出来必然 <= 4。保留 8 字节分支只为万一将来放开读宽度。
          */
         if (size >= 8)
         {
            uint32_t u;
            memcpy(&u, buf, 4);
            v = (int64_t)u;
         }
         break;
   }
   return v;
}

/* ======================================================================
 * 只读 SDO
 * ====================================================================== */

/*
 * 消费 ctx.ecaterror 并取回首个匹配本对象的 abort 码。
 * 不清栈 —— 由 sm_drain_errors() 统一排空。
 */
int32_t sm_take_abort(int slave, uint16_t index, uint8_t sub)
{
   ec_errort err;
   int32_t   abort = 0;

   while (ecx_poperror(&g_ctx, &err))
   {
      if (err.Etype == EC_ERR_TYPE_SDO_ERROR &&
          err.Slave == (uint16_t)slave &&
          err.Index == index &&
          err.SubIdx == sub)
      {
         abort = err.AbortCode;
      }
   }
   return abort;
}

/*
 * 读一个对象 (≤4 字节 expedited)。
 *
 * 关键: g_ctx.ecaterror 是粘滞位, SOEM 报错后不会自动清零, 必须在每次
 * SDO 读前手动复位, 否则上一个失败会污染下一次判定 (slide_verify.c 已验证此坑)。
 *
 * 返回 SM_RD_*; *size 返回驱动器自报的字节数; 命中 abort 时 *abort_code 给出中止码。
 */
int sm_rd_raw(int slave, uint16_t index, uint8_t sub, int timeout,
              uint8_t *buf, int *size, int32_t *abort_code)
{
   int  psize;
   int  wkc;
   int32_t abort = 0;

   memset(buf, 0, 4);
   psize = 4;                     /* 给足 4 字节, 让驱动器自报实际宽度 */
   g_ctx.ecaterror = FALSE;       /* 清粘滞位 */

   wkc = ecx_SDOread(&g_ctx, (uint16_t)slave, index, sub, FALSE,
                     &psize, buf, timeout);
   if (abort_code != NULL)
      *abort_code = 0;

   if (wkc > 0)
   {
      if (size != NULL)
         *size = psize;
      return SM_RD_OK;
   }

   if (g_ctx.ecaterror)
   {
      abort = sm_take_abort(slave, index, sub);
      if (abort_code != NULL)
         *abort_code = abort;
      return SM_RD_ABORT;
   }
   if (size != NULL)
      *size = 0;
   return SM_RD_TIMEOUT;
}

int sm_rd_u16(int slave, uint16_t index, uint8_t sub, uint16_t *v, int timeout,
              int32_t *abort_code)
{
   uint8_t buf[4];
   int     size = 0;
   int     rc;

   rc = sm_rd_raw(slave, index, sub, timeout, buf, &size, abort_code);
   if (rc == SM_RD_OK)
      memcpy(v, buf, 2);
   return rc;
}

int sm_rd_i8(int slave, uint16_t index, uint8_t sub, int8_t *v, int timeout,
             int32_t *abort_code)
{
   uint8_t buf[4];
   int     size = 0;
   int     rc;

   rc = sm_rd_raw(slave, index, sub, timeout, buf, &size, abort_code);
   if (rc == SM_RD_OK)
      *v = (int8_t)buf[0];
   return rc;
}

int sm_rd_i32(int slave, uint16_t index, uint8_t sub, int32_t *v, int timeout,
              int32_t *abort_code)
{
   uint8_t buf[4];
   int     size = 0;
   int     rc;

   rc = sm_rd_raw(slave, index, sub, timeout, buf, &size, abort_code);
   if (rc == SM_RD_OK)
      memcpy(v, buf, 4);
   return rc;
}

/* ======================================================================
 * 原始寄存器读
 * 注意: ecx_*RD 的首参是 ecx_portt *, 不是 context —— 传 &g_ctx.port。
 * 返回 wkc > 0 才算成功。
 * ====================================================================== */
int sm_reg_read16(int slave, uint16_t ado, uint16_t *v)
{
   int wkc;

   *v = 0;
   wkc = ecx_FPRD(&g_ctx.port, g_ctx.slavelist[slave].configadr, ado,
                  sizeof(uint16_t), v, EC_TIMEOUTEEP);
   return (wkc > 0) ? 0 : -1;
}

int sm_reg_read8(int slave, uint16_t ado, uint8_t *v)
{
   int wkc;

   *v = 0;
   wkc = ecx_FPRD(&g_ctx.port, g_ctx.slavelist[slave].configadr, ado,
                  sizeof(uint8_t), v, EC_TIMEOUTEEP);
   return (wkc > 0) ? 0 : -1;
}

/*
 * 诊断计数: Rx 错误 / 转发 Rx 错误 / 处理单元错误 / 丢链次数。
 * 用于在动作前后对比, 判断"这次动作有没有伴随通信质量恶化"。
 * 任一寄存器读失败返回 -1。
 */
int sm_diag_counters(int slave, uint32_t *rxerr, uint32_t *frxerr,
                     uint32_t *pe_cnt, uint32_t *ll_cnt)
{
   uint8_t b;

   if (sm_reg_read8(slave, ECT_REG_RXERR, &b) != 0)
      return -1;
   if (rxerr != NULL)
      *rxerr = b;

   if (sm_reg_read8(slave, ECT_REG_FRXERR, &b) != 0)
      return -1;
   if (frxerr != NULL)
      *frxerr = b;

   if (sm_reg_read8(slave, ECT_REG_PECNT, &b) != 0)
      return -1;
   if (pe_cnt != NULL)
      *pe_cnt = b;

   if (sm_reg_read8(slave, ECT_REG_LLCNT, &b) != 0)
      return -1;
   if (ll_cnt != NULL)
      *ll_cnt = b;

   return 0;
}

/*
 * 排空 SOEM 错误栈。返回排空的条数。
 * 运动期必须定期调用: EMCY (紧急事件) 和 SDO 错误都从这里出来,
 * 不排空会静默丢掉驱动器报的故障原因。
 */
int sm_drain_errors(int slave, int print_emcy)
{
   ec_errort err;
   int       n = 0;

   while (ecx_poperror(&g_ctx, &err))
   {
      n++;
      if (!print_emcy)
         continue;
      if (err.Etype == EC_ERR_TYPE_EMERGENCY)
      {
         /* EMCY 字段与 AbortCode 共用一段 union, 这里必须按 EMCY 侧解读 */
         printf("      [EMCY] 从站%d 紧急事件: 代码 0x%04X 寄存器 0x%02X 数据 %u %u\n",
                (int)err.Slave, (unsigned)err.ErrorCode,
                (unsigned)err.ErrorReg, (unsigned)err.w1, (unsigned)err.w2);
      }
      else if (err.Slave == (uint16_t)slave &&
               err.Etype == EC_ERR_TYPE_SDO_ERROR)
      {
         printf("      [SDO ] 从站%d %04Xh:%02X abort 0x%08X\n",
                (int)err.Slave, (unsigned)err.Index, (unsigned)err.SubIdx,
                (unsigned)err.AbortCode);
      }
   }
   return n;
}

/* ======================================================================
 * 状态
 * ====================================================================== */

/*
 * 把全部从站请求到指定 AL 状态并等待。
 *
 * manualstatechange = 1 让 ecx_config_map_group 不再自动请求 SAFE_OP,
 * 由本函数统一负责状态升降, 收尾时才能干净地降回 PRE_OP。
 *
 * 返回 0 = 全部到位; 否则返回未到位的台数。
 */
int sm_enter_state(int requested_state, int verbose)
{
   int slave;
   int bad = 0;

   if (verbose)
      printf("\n请求从站进入 %s ...\n",
             (requested_state == EC_STATE_SAFE_OP) ? "SAFE_OP" : "PRE_OP");

   for (slave = 1; slave <= g_ctx.slavecount; slave++)
   {
      g_ctx.slavelist[slave].state = (uint16)requested_state;
      ecx_writestate(&g_ctx, (uint16)slave);
   }
   ecx_statecheck(&g_ctx, 0, (uint16)requested_state, EC_TIMEOUTSTATE);
   ecx_readstate(&g_ctx);

   for (slave = 1; slave <= g_ctx.slavecount; slave++)
   {
      const struct ec_slave *s = &g_ctx.slavelist[slave];

      if (s->state != (uint16)requested_state)
      {
         bad++;
         if (verbose)
         {
            printf("  位置 %-3d [WARN] 未达 %s (状态 0x%02X, AL 状态码 0x%04X %s)。\n",
                   slave - 1,
                   (requested_state == EC_STATE_SAFE_OP) ? "SAFE_OP" : "PRE_OP",
                   (unsigned)s->state, (unsigned)s->ALstatuscode,
                   ec_ALstatuscode2string(s->ALstatuscode));
         }
      }
   }
   return bad;
}
