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
   int      psize;
   int      wkc;
   int      ecerr;
   int32_t  abort = 0;
   uint32_t t0, dt;

   memset(buf, 0, 4);
   psize = 4;                     /* 给足 4 字节, 让驱动器自报实际宽度 */
   g_ctx.ecaterror = FALSE;       /* 清粘滞位 */

   t0 = sm_now_ms();
   wkc = ecx_SDOread(&g_ctx, (uint16_t)slave, index, sub, FALSE,
                     &psize, buf, timeout);
   dt = sm_now_ms() - t0;

   /*
    * 必须在 sm_take_abort() 之前存下 ecaterror: ecx_poperror() 把错误栈
    * 排空之后会顺手把它清成 FALSE, 之后再读就恒为 0 —— 而"这一笔有没有被
    * 压进错误栈"正是 ABORT 与 TIMEOUT 的唯一分野。
    * (sm_take_abort 里那个 abort 码对 <=4 字节的加急读通常是取不到的,
    *  原因见 sm_xfer_run_t 上方的注释。)
    */
   ecerr = g_ctx.ecaterror ? 1 : 0;

   if (abort_code != NULL)
      *abort_code = 0;

   if (wkc > 0)
   {
      if (size != NULL)
         *size = psize;
      sm_xfer_note('R', slave, index, sub, SM_RD_OK, 0, wkc, ecerr,
                   psize, buf, dt);
      return SM_RD_OK;
   }

   if (ecerr)
   {
      abort = sm_take_abort(slave, index, sub);
      if (abort_code != NULL)
         *abort_code = abort;
      sm_xfer_note('R', slave, index, sub, SM_RD_ABORT, abort, wkc, ecerr,
                   0, NULL, dt);
      return SM_RD_ABORT;
   }
   if (size != NULL)
      *size = 0;
   /* 读失败: buf 是清过零的, 传 size=0/bytes=NULL, 不让它冒充一个读数 */
   sm_xfer_note('R', slave, index, sub, SM_RD_TIMEOUT, 0, wkc, ecerr,
                0, NULL, dt);
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
 * SDO 事务日志
 *
 * 接口与设计理由见 sm.h。这里只补一条实现上的关键点:
 * g_ctx.ecaterror 必须在调用 sm_take_abort() **之前**存下来 ——
 * ecx_poperror() 排空错误栈后会把 ecaterror 清成 FALSE, 之后再读就永远是 0,
 * 而"这次到底有没有被压错误栈"正是区分 ABORT 与 TIMEOUT 的唯一依据。
 * ====================================================================== */
#define SM_XFER_MAX_VALS 6   /* 一行里最多列几个不同的值, 再多就退化成"首->末" */

/*
 * 槽位数 = 同时挂着的不同"结果签名"的最大个数。
 *
 * 为什么必须是**一张表**而不是"记住上一笔": S4 的轮询循环是按
 * 6041h -> 6064h -> 606Ch 交替读的, 单槽的键每一笔都在变, 于是每一笔都会
 * 触发一次 flush —— 归并等于没做, 每轮照样吐 3 行。要真正折叠, 就得让每个
 * 对象各自记住自己的累加器, 不管它们之间隔了多少别的对象。
 * 8 个足够: 轮询里最多 3 个对象, S3 每步也不超过 4 个, 超出会整批交出。
 */
#define SM_XFER_SLOTS 8

typedef struct
{
   char     dir;      /* 'R' / 'W' */
   int      slave;
   uint16_t index;
   uint8_t  sub;
   int      rc;       /* 读: SM_RD_*; 写: 0 / -1 */
   int32_t  abort;
   int      ecerr;
   long     n;        /* 本 run 的笔数 */

   /* 本 run 内出现过的不同值 (最多记 SM_XFER_MAX_VALS 个) */
   int      nval;
   int      vsz[SM_XFER_MAX_VALS];
   uint8_t  v[SM_XFER_MAX_VALS][4];
   int      val_overflow;   /* 不同值超过上限 -> 只打 首 -> 末 */
   int      last_sz;        /* 最新一笔的值 (始终更新, 与去重无关) */
   uint8_t  last_v[4];

   int      wkc_min, wkc_max;
   uint32_t t_min, t_max;
} sm_xfer_run_t;

static sm_xfer_run_t g_slot[SM_XFER_SLOTS];
static int           g_slot_n;
static int           g_xfer_active;

/* 按驱动器自报的字节宽度渲染成 0x...., 不猜类型 */
static void xfer_val_str(char *dst, size_t n, const uint8_t *b, int size)
{
   unsigned v = 0;
   int      i;

   if (size > 4)
      size = 4;
   for (i = 0; i < size; i++)
      v |= (unsigned)b[i] << (8 * i);
   snprintf(dst, n, "0x%0*X", (size > 0) ? size * 2 : 1, v);
}

static const char *xfer_rc_str(char dir, int rc)
{
   /*
    * 写的成功**不能**写 "ok"。
    *
    * 这份 SOEM 在加急写路径 (psize<=4, 6040h 正是 2 字节) 上把从站回的 abort
    * 帧当成写成功 —— abort 响应的 mbxtype/service/index/subindex 与请求完全
    * 一致, 命中 "all OK" 分支, 既不压错误栈也不置 ecaterror, wkc 还 > 0
    * (见 sm_guard.c 里 guard_force_disable 上方那段)。也就是说 rc==0 只证明
    * "有个应答回来了", **不证明驱动器接受了这个值**。写成 "应答" 是这里
    * 能诚实说出的全部; 真要确认写生效, 只能靠回读 (guard_write_verified /
    * 随后的 6041h 轮询)。读就没有这个问题: 读到字节就是真读到了。
    */
   if (dir == 'W')
      return (rc == 0) ? "应答" : "失败";
   switch (rc)
   {
      case SM_RD_OK:    return "ok";
      case SM_RD_ABORT: return "abort";
      default:          return "超时";
   }
}

/* 记一个值; 已在列表中则不算新值 */
static void xfer_add_val(sm_xfer_run_t *r, int size, const uint8_t *bytes)
{
   int i;

   if (bytes == NULL || size <= 0)
      return;
   if (size > 4)
      size = 4;

   for (i = 0; i < r->nval; i++)
   {
      if (r->vsz[i] == size && memcmp(r->v[i], bytes, (size_t)size) == 0)
         return;
   }
   if (r->nval < SM_XFER_MAX_VALS)
   {
      r->vsz[r->nval] = size;
      memcpy(r->v[r->nval], bytes, (size_t)size);
      r->nval++;
      return;
   }
   r->val_overflow = 1;
}

/* 键相同的槽位; 没有返回 -1 */
static int xfer_find(char dir, int slave, uint16_t index, uint8_t sub,
                     int rc, int32_t abort)
{
   int i;

   for (i = 0; i < g_slot_n; i++)
   {
      const sm_xfer_run_t *r = &g_slot[i];

      if (r->dir == dir && r->slave == slave && r->index == index &&
          r->sub == sub && r->rc == rc && r->abort == abort)
         return i;
   }
   return -1;
}

static void xfer_emit(const sm_xfer_run_t *r)
{
   char vs[16];
   int  i;

   printf("      [SDO ] %c %04Xh:%02X %s", r->dir, (unsigned)r->index,
          (unsigned)r->sub, xfer_rc_str(r->dir, r->rc));

   /*
    * abort 码: 只有非 0 才算一个真实的数字。
    *
    * 这份 SOEM 在**加急路径** (psize<=4, 本项目所有对象都是) 上拿不到真正的
    * abort 码, 两头都丢:
    *   读 —— 从站回的 abort 帧通过了结构检查, 于是 Command(0x80) 走进普通
    *         数据分支, abort 码被当成"数据长度", 因为大得离谱而落到
    *         wkc=0 + ecx_packeterror(), 压进错误栈的是 PACKET_ERROR 而不是
    *         SDO_ERROR; 而 sm_take_abort 只认 SDO_ERROR, 所以取回来是 0。
    *   写 —— abort 帧的 mbxtype/service/index/subindex 与请求完全一致, 命中
    *         "all OK" 分支, 被当成写成功 (sm_guard.c 那句注释记的就是这个)。
    * 所以 0 绝不能打成一个数字 —— 那等于把"拿不到"说成"驱动器回了 0",
    * 正是这次要消掉的二义性。说不清的地方就说说不清。
    */
   if (r->abort != 0)
      printf("  abort 0x%08X", (unsigned)r->abort);
   else if (r->rc == SM_RD_ABORT)
      printf("  已推送错误但取不到 SDO abort 码");

   /* 值只在真的读到/写出时才有 (读失败传 size=0, 写失败传 NULL) */
   if (r->nval > 0)
   {
      printf(" .= ");
      if (!r->val_overflow)
      {
         for (i = 0; i < r->nval; i++)
         {
            xfer_val_str(vs, sizeof(vs), r->v[i], r->vsz[i]);
            printf("%s%s", (i > 0) ? " " : "", vs);
         }
      }
      else
      {
         xfer_val_str(vs, sizeof(vs), r->v[0], r->vsz[0]);
         printf("%s -> ", vs);
         xfer_val_str(vs, sizeof(vs), r->last_v, r->last_sz);
         printf("%s (值多变)", vs);
      }
   }

   if (r->n > 1)
      printf("  ×%ld", r->n);

   if (r->n == 1)
      printf("  %ums", (unsigned)r->t_max);
   else if (r->t_min == r->t_max)
      printf("  (%ums/次)", (unsigned)r->t_min);
   else
      printf("  (%u-%ums/次)", (unsigned)r->t_min, (unsigned)r->t_max);

   if (r->wkc_min == r->wkc_max)
      printf("  wkc=%d", r->wkc_min);
   else
      printf("  wkc=%d..%d", r->wkc_min, r->wkc_max);

   /*
    * 超时时 ecaterror 必然是 0 —— 打出来是为了说明"什么都没被压进错误栈",
    * 也就是这一笔连一个像样的应答都没有, 而不是"驱动器拒绝了这个请求"。
    * ABORT 的情况上面那行已经说过, 写路径则由 [WRITE-FAIL] 负责, 不重复。
    */
   if (r->dir == 'R' && r->rc == SM_RD_TIMEOUT)
      printf("  ecaterror=%d", r->ecerr);

   printf("\n");
}

void sm_xfer_flush(void)
{
   int i;

   for (i = 0; i < g_slot_n; i++)
      xfer_emit(&g_slot[i]);
   g_slot_n = 0;
}

void sm_xfer_set_active(int on)
{
   if (!on)
      sm_xfer_flush();   /* 关窗之前先把最后一笔交出来 */
   g_xfer_active = on;
}

void sm_xfer_note(char dir, int slave, uint16_t index, uint8_t sub,
                  int rc, int32_t abort, int wkc, int ecerr, int size,
                  const uint8_t *bytes, uint32_t ms)
{
   sm_xfer_run_t *r;
   int            i;

   if (!g_xfer_active)
      return;

   /*
    * 写不参与归并。
    *
    * 写的频率本来就低 (整个 S3 加收尾也就二十来笔), 而"这一笔写的是什么"
    * 恰恰是要看的东西 —— 把使能序列的 0x0006 / 0x0007 / 0x000F 折叠成一行
    * "×3" 等于把序列本身抹掉了, 签名里再带上值也救不回来 (那样只会得到
    * 三行长得一模一样、分不清谁是谁的输出)。
    * 所以写: 先把挂着的读 run 交出去, 自己单打一行, 立刻打印 ——
    * 顺带保证了日志里读写的先后顺序与总线上真实发生的顺序一致。
    * 需要归并的只有读 (S4 的忙轮询每秒上千笔)。
    */
   if (dir == 'W')
   {
      sm_xfer_run_t one;

      sm_xfer_flush();
      memset(&one, 0, sizeof(one));
      one.dir = dir;
      one.slave = slave;
      one.index = index;
      one.sub = sub;
      one.rc = rc;
      one.abort = abort;
      one.ecerr = ecerr;
      one.n = 1;
      one.wkc_min = one.wkc_max = wkc;
      one.t_min = one.t_max = ms;
      if (bytes != NULL && size > 0)
      {
         int sz = (size > 4) ? 4 : size;

         memcpy(one.last_v, bytes, (size_t)sz);
         one.last_sz = sz;
         one.nval = 1;
         one.vsz[0] = sz;
         memcpy(one.v[0], bytes, (size_t)sz);
      }
      xfer_emit(&one);
      return;
   }

   /*
    * 读: 找签名相同的槽累加。
    *
    * 为什么是表而不是单槽: S4 的 jog_leg 按 6041h -> 6064h -> 606Ch 交替轮询,
    * 相邻两笔的 index 必然不同 —— 单槽下"签名变了"每笔都成立, 于是每笔都
    * flush, 归并等于没做, 日志照样每轮吐三行。8 槽让交错着的几路轮询各自
    * 占一格, 谁也不打断谁。
    */
   i = xfer_find(dir, slave, index, sub, rc, abort);
   if (i < 0)
   {
      /* 表满: 先交出已有的一批再腾空。宁可多打几行, 也不能把新签名丢了。 */
      if (g_slot_n >= SM_XFER_SLOTS)
         sm_xfer_flush();
      i = g_slot_n++;
      memset(&g_slot[i], 0, sizeof(g_slot[i]));
      g_slot[i].dir = dir;
      g_slot[i].slave = slave;
      g_slot[i].index = index;
      g_slot[i].sub = sub;
      g_slot[i].rc = rc;
      g_slot[i].abort = abort;
      g_slot[i].ecerr = ecerr;
      g_slot[i].wkc_min = g_slot[i].wkc_max = wkc;
      g_slot[i].t_min = g_slot[i].t_max = ms;
   }

   r = &g_slot[i];
   r->n++;
   if (wkc < r->wkc_min)
      r->wkc_min = wkc;
   if (wkc > r->wkc_max)
      r->wkc_max = wkc;
   if (ms < r->t_min)
      r->t_min = ms;
   if (ms > r->t_max)
      r->t_max = ms;

   /* 只记真实读到的值。读失败时调用方传 size=0 —— buf 是清过零的,
     记下来就是拿一个 0x0000 冒充驱动器读数 (与 sw_valid 同一个陷阱)。 */
   if (rc == SM_RD_OK && bytes != NULL && size > 0)
   {
      int sz = (size > 4) ? 4 : size;

      memcpy(r->last_v, bytes, (size_t)sz);
      r->last_sz = sz;
      xfer_add_val(r, sz, bytes);
   }
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
