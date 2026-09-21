/*
 * ec_motor.c - 多轴 CiA402 电机接口的底座层: 网卡与上下文 / 时钟 / SDO 读写 /
 * PDO 映射的实读与追加 / 偏移证明 / AL 状态阶梯 / 过程数据收发 / 6041h 判读 / 收尾。
 * 运动层 (使能状态机 / CSP 轨迹 / PV / 回零) 在 ec_motor_motion.c。
 *
 * 全工程唯一的 ecx_SDOwrite 调用点, 头文件不导出裸写函数。会写: 6060h;
 * 6098h/6099h/609Ah/607Ch 回零参数; 1C12h/1C13h/1600h/1A00h PDO 映射 (仅 RAM);
 * 6040h 只经过程数据镜像写。从不写: 2102h (EEPROM) / 607Dh / 2400h / 2408h / 2409h / 2201h。
 * include 顺序是硬的: soem.h 必须在前 —— windows.h 先进来会拉进 winsock.h 并定义
 * _WINSOCKAPI_, 与 soem.h 的 winsock2.h 冲突 (MSVC: error C2011)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

#include "soem/soem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "ec_motor_internal.h"

#define EM_VERSION "motor_api 1.0"

/* SM2 = RxPDO (输出), SM3 = TxPDO (输入) —— 手册: SM0/SM1 邮箱, SM2 RxPDO, SM3 TxPDO */
#define EM_SM_RXPDO 2
#define EM_SM_TXPDO 3

/* 全局停止标志: Ctrl-C 的处理器只置这一位, 不做 I/O; 用文件作用域是因为处理器拿不到 bus 指针 */

static volatile sig_atomic_t g_stop = 0;

void em_request_stop(void)   { g_stop = 1; }
int  em_stop_requested(void) { return g_stop != 0; }
void em_clear_stop(void)     { g_stop = 0; }

static int g_console_done = 0;

/* 把控制台代码页设成 UTF-8, 幂等。在 main() 第一句调: 那之前打印的汉字是乱码 (默认 GBK) */
void em_console_init(void)
{
   if (g_console_done)
      return;
   g_console_done = 1;
#ifdef _WIN32
   /* 源码是 UTF-8, Windows 控制台默认是 GBK 代码页, 不设这一句中文全是乱码 */
   SetConsoleOutputCP(CP_UTF8);
#endif
}

static void em__console_init(void) { em_console_init(); }

uint32_t em__now_ms(void)
{
#ifdef _WIN32
   return (uint32_t)GetTickCount64();
#else
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

void em__sleep_ms(int ms)
{
#ifdef _WIN32
   Sleep((DWORD)ms);
#else
   usleep((useconds_t)ms * 1000);
#endif
}

void em_sleep_ms(int ms) { em__sleep_ms(ms); }

void em__err(const char *fmt, ...)
{
   va_list ap;

   em__console_init();
   printf("  [FAIL] ");
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

void em__warn(const char *fmt, ...)
{
   va_list ap;

   em__console_init();
   printf("  [WARN] ");
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

void em__log(em_bus_t *bus, const char *fmt, ...)
{
   va_list ap;

   if (bus != NULL && !bus->verbose)
      return;
   em__console_init();
   printf("  ");
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

const char *em_version(void) { return EM_VERSION; }

void em_set_verbose(em_bus_t *bus, int on) { if (bus) bus->verbose = on ? 1 : 0; }

/* 镜像的小端读写: SOEM 把 IOmap 原样塞进 EtherCAT 帧 (线上小端); 用 memcpy 以不依赖偏移对齐 */

void em__put_u8(uint8_t *m, int off, uint8_t v)
{
   if (m != NULL && off >= 0)
      m[off] = v;
}

uint8_t em__get_u8(const uint8_t *m, int off)
{
   if (m != NULL && off >= 0)
      return m[off];
   return 0;
}

void em__put_u16(uint8_t *m, int off, uint16_t v)
{
   if (m != NULL && off >= 0)
      memcpy(m + off, &v, 2);
}

void em__put_i32(uint8_t *m, int off, int32_t v)
{
   if (m != NULL && off >= 0)
      memcpy(m + off, &v, 4);
}

void em__put_u32(uint8_t *m, int off, uint32_t v)
{
   if (m != NULL && off >= 0)
      memcpy(m + off, &v, 4);
}

uint16_t em__get_u16(const uint8_t *m, int off)
{
   uint16_t v = 0;

   if (m != NULL && off >= 0)
      memcpy(&v, m + off, 2);
   return v;
}

uint32_t em__get_u32(const uint8_t *m, int off)
{
   uint32_t v = 0;

   if (m != NULL && off >= 0)
      memcpy(&v, m + off, 4);
   return v;
}

int32_t em__get_i32(const uint8_t *m, int off)
{
   int32_t v = 0;

   if (m != NULL && off >= 0)
      memcpy(&v, m + off, 4);
   return v;
}

void em__set_cw(em_axis_t *ax, uint16_t cw)
{
   if (ax == NULL || ax->out == NULL || ax->off_cw < 0)
      return;
   em__put_u16(ax->out, ax->off_cw, cw);
}

void em__pin_ramp(em_axis_t *ax)
{
   if (ax == NULL || ax->out == NULL)
      return;
   if (ax->off_prof_acc >= 0)
      em__put_u32(ax->out, ax->off_prof_acc, ax->prof_acc);
   if (ax->off_prof_dec >= 0)
      em__put_u32(ax->out, ax->off_prof_dec, ax->prof_dec);
}

int em_sdo_read(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                void *p, int *size)
{
   uint8_t buf[4];
   int     psize = (int)sizeof(buf);

   if (bus == NULL || !bus->opened || slave < 1 || slave > bus->nslaves)
      return EM_R_FAIL;

   memset(buf, 0, sizeof(buf));

   /* 读之前必须清 ctx.ecaterror: 它是粘滞位, 上一次的失败会污染下一次的判定 */
   bus->ctx.ecaterror = FALSE;
   if (ecx_SDOread(&bus->ctx, (uint16_t)slave, index, sub, FALSE, &psize, buf,
                   EC_TIMEOUTRXM) <= 0 || bus->ctx.ecaterror)
      return EM_R_FAIL;

   /* psize 是传入传出: 必须先用缓冲大小初始化。这里按 4 字节读进内部缓冲, 再按驱动器自报宽度拷出 */
   if (psize < 0 || psize > (int)sizeof(buf))
      return EM_R_FAIL;

   if (size != NULL)
      *size = psize;
   if (p != NULL && psize > 0)
      memcpy(p, buf, (size_t)psize);
   return EM_R_OK;
}

/* 固定宽度的读取包装: 宽度不符就算失败 —— 宽度不足时高位补零会解出一个看着正常的值 */
static int em__rd_fixed(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                        int want, void *p, const char *what)
{
   int size = 0;

   if (em_sdo_read(bus, slave, index, sub, p, &size) != 0)
   {
      em__err("读 %04Xh:%02X (%s) 失败 (无响应 / SDO abort)",
              (unsigned)index, (unsigned)sub, what);
      return EM_R_FAIL;
   }
   if (size != want)
   {
      em__err("%04Xh:%02X (%s) 自报宽度 %d 字节, 期望 %d -> 不猜",
              (unsigned)index, (unsigned)sub, what, size, want);
      return EM_R_FAIL;
   }
   return EM_R_OK;
}

int em_rd_u8(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint8_t *v)
{
   return em__rd_fixed(bus, slave, index, sub, 1, v, "U8");
}

int em_rd_i8(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int8_t *v)
{
   return em__rd_fixed(bus, slave, index, sub, 1, v, "I8");
}

int em_rd_u16(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint16_t *v)
{
   return em__rd_fixed(bus, slave, index, sub, 2, v, "U16");
}

int em_rd_u32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, uint32_t *v)
{
   return em__rd_fixed(bus, slave, index, sub, 4, v, "U32");
}

int em_rd_i32(em_bus_t *bus, int slave, uint16_t index, uint8_t sub, int32_t *v)
{
   return em__rd_fixed(bus, slave, index, sub, 4, v, "I32");
}

int em_rd_any(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
              uint32_t *val, int *size_bytes)
{
   uint8_t buf[4];
   int     size = 0;
   uint32_t v    = 0;
   int     i;

   if (val != NULL)
      *val = 0;
   if (size_bytes != NULL)
      *size_bytes = 0;

   if (em_sdo_read(bus, slave, index, sub, buf, &size) != EM_R_OK)
      return EM_R_FAIL;
   if (size < 0 || size > 4)
      return EM_R_FAIL;

   /* 小端拼进 32 位; 不足 4 字节的高位补 0。调用方拿 *size_bytes 自己判断。 */
   for (i = 0; i < size; i++)
      v |= (uint32_t)buf[i] << (8 * i);

   if (val != NULL)
      *val = v;
   if (size_bytes != NULL)
      *size_bytes = size;
   return EM_R_OK;
}

/* SDO 写 —— 全工程唯一的 ecx_SDOwrite 调用点。
 * 每次回读确认: 本仓库这份 SOEM 的 ecx_SDOwrite 在加急路径 (psize<=4) 上把从站回的
 * SDO abort 帧当成写成功 (wkc 还 > 0), 而本接口要写的对象全是 1/2/4 字节加急写。 */

static int em__sdo_write_raw(em_bus_t *bus, int slave, uint16_t index, uint8_t sub,
                             int size, const void *p, int tmo)
{
   if (bus == NULL || !bus->opened || slave < 1 || slave > bus->nslaves)
      return EM_R_FAIL;

   bus->ctx.ecaterror = FALSE;
   if (ecx_SDOwrite(&bus->ctx, (uint16_t)slave, index, sub, FALSE, size, p,
                    tmo) <= 0 || bus->ctx.ecaterror)
      return EM_R_FAIL;
   return EM_R_OK;
}

static int em__verified_write(em_axis_t *ax, uint16_t index, uint8_t sub,
                              int size, const void *p, const char *why)
{
   uint8_t back[4];
   int     bsize = 0;

   /* 回读缓冲只有 4 字节, 所以不接受更宽的写; 必须在写之前判, 否则分不清"没写"与"写了但没确认" */
   if (size < 1 || size > 4)
      return EM_R_FAIL;

   if (em__sdo_write_raw(ax->bus, ax->slave, index, sub, size, p,
                         EC_TIMEOUTRXM) != EM_R_OK)
   {
      em__err("写 %04Xh:%02X (%s) 未确认 (无响应 / SDO abort)",
              (unsigned)index, (unsigned)sub, why);
      return EM_R_FAIL;
   }
   if (em_sdo_read(ax->bus, ax->slave, index, sub, back, &bsize) != EM_R_OK ||
       bsize != size || memcmp(back, p, (size_t)size) != 0)
   {
      em__err("写 %04Xh:%02X (%s) 后回读不一致 -> 没写进去",
              (unsigned)index, (unsigned)sub, why);
      return EM_R_FAIL;
   }
   return EM_R_OK;
}

int em__wr_u8(em_axis_t *ax, uint16_t index, uint8_t sub, uint8_t v, const char *why)
{
   return em__verified_write(ax, index, sub, 1, &v, why);
}

int em__wr_i8(em_axis_t *ax, uint16_t index, uint8_t sub, int8_t v, const char *why)
{
   return em__verified_write(ax, index, sub, 1, &v, why);
}

int em__wr_u16(em_axis_t *ax, uint16_t index, uint8_t sub, uint16_t v, const char *why)
{
   return em__verified_write(ax, index, sub, 2, &v, why);
}

int em__wr_u32(em_axis_t *ax, uint16_t index, uint8_t sub, uint32_t v, const char *why)
{
   return em__verified_write(ax, index, sub, 4, &v, why);
}

int em__wr_i32(em_axis_t *ax, uint16_t index, uint8_t sub, int32_t v, const char *why)
{
   return em__verified_write(ax, index, sub, 4, &v, why);
}

/* ---- 2300h 输入端子有效电平逻辑 (唯一一处改驱动器行为参数的写) ----
 * 现场是 NPN 传感器 (高电平 = 未触发) 而驱动器配着常开, 两者反着: 60FDh 的 bit1/bit2 恒
 * 同时置起 -> 6041h bit11 恒为 1 -> 2204h = 0 把两个方向都挡死 (回零进得去、一动不动)。
 * 置 0x0007 (bit0~bit2 全 1 = 常闭) 是修根: 驱动器自己的限位保护与回零一起跟着对。
 * 宽度按驱动器自报的来 (手册 V2.4 p84 写 U16, 而当时那份现场基线表记成 U8 —— 两处对不上,
 * 所以不猜, 读到几字节就按几字节写回), bit3 以上的位原样保留。 */

static int em__di_read(em_bus_t *bus, int slave, uint16_t *val, int *sz)
{
   uint32_t v = 0;
   int      n = 0;

   if (em_rd_any(bus, slave, EM_OID_DI_LOGIC, 0, &v, &n) != EM_R_OK)
   {
      em__err("读 2300h (输入有效电平逻辑) 失败");
      return EM_R_FAIL;
   }
   if (n != 1 && n != 2)
   {
      em__err("2300h 自报 %d 字节, 本接口只认 1 / 2 字节 -> 不猜, 不写", n);
      return EM_R_FAIL;
   }
   *val = (uint16_t)v;
   *sz  = n;
   return EM_R_OK;
}

static int em__di_write(em_axis_t *ax, uint16_t v, int sz, const char *why)
{
   if (sz == 1)
   {
      uint8_t b = (uint8_t)(v & 0xFFu);
      return em__verified_write(ax, EM_OID_DI_LOGIC, 0, 1, &b, why);
   }
   return em__verified_write(ax, EM_OID_DI_LOGIC, 0, 2, &v, why);
}

/* 映射项编码: index<<16 | sub<<8 | 位宽 */
#define EM_MAP_ENTRY(index, sub, bits) \
   (((uint32_t)(index) << 16) | ((uint32_t)(sub) << 8) | (uint32_t)(bits))

typedef struct
{
   uint16_t    index;
   uint8_t     sub;
   int         bits;
   const char *name;
} em_field_t;

/* RxPDO 必须含的三项: 这张表给 em_map_ensure() 用, 列进来的项缺失就补写 PDO 映射;
 * 6060h 故意不列 —— 它由 setup 实读的 ax->off_modes 决定走过程数据还是 SDO。
 * 通用约束: 对象只要在生效 RxPDO 里就是主站拥有的, SDO 写会被下一帧撤销。 */
static const em_field_t EM_NEED_RX[] = {
   { EM_OID_CONTROLWORD, 0, 16, "6040h 控制字"        },
   { EM_OID_TARGET_POS,  0, 32, "607Ah 目标位置 (CSP)" },
   { EM_OID_TARGET_VEL,  0, 32, "60FFh 目标速度 (PV)"  },
};
static const em_field_t EM_NEED_TX[] = {
   { EM_OID_STATUSWORD, 0, 16, "6041h 状态字"    },
   { EM_OID_ACT_POS,    0, 32, "6064h 实际位置"  },
   { EM_OID_ACT_VEL,    0, 32, "606Ch 实际速度"  },
};

/* 60FDh 数字输入 —— 不放进 EM_NEED_TX (那张表的语义是"缺了就追加进 PDO 映射"),
 * 它只是只读监视量。默认由 em__find_field() 在生效映射里才绑, 强制追加要 em_require_dig_in() */
static const em_field_t EM_FIELD_DIG_IN[] = {
   { EM_OID_DIG_IN,     0, 32, "60FDh 数字输入"  },
};
#define EM_NEED_RX_N ((int)(sizeof(EM_NEED_RX) / sizeof(EM_NEED_RX[0])))
#define EM_NEED_TX_N ((int)(sizeof(EM_NEED_TX) / sizeof(EM_NEED_TX[0])))

/* 读一个 PDO 对象 (:00 项数, :01.. 项), 返回 0 / -1。ent_size 必须由调用方给, 不能写死 4:
 * 分配对象 1C12h/1C13h 项是 U16 (2 字节); 映射对象 1600h/1A00h 项是 U32 (4 字节)。 */
static int em_map_read(em_bus_t *bus, int slave, uint16_t index, int ent_size,
                       int *n, uint32_t *e)
{
   uint8_t cnt = 0;
   int     i, size = 0;

   *n = 0;

   /* 这里不走"没读到与宽度不对合并成 -1"的包装: 要把驱动器自报的宽度打出来才能排查 */
   if (em_sdo_read(bus, slave, index, 0, &cnt, &size) != EM_R_OK)
   {
      em__err("读 %04Xh:00 (项数) 失败", (unsigned)index);
      return EM_R_FAIL;
   }
   if (size != 1)
   {
      em__err("%04Xh:00 自报宽度 %d 字节, 期望 1 -> 不猜", (unsigned)index, size);
      return EM_R_FAIL;
   }
   if (cnt > EM_MAP_MAX)
   {
      em__err("%04Xh:00 = %d 项, 超出本接口上限 %d -> 拒绝",
              (unsigned)index, (int)cnt, EM_MAP_MAX);
      return EM_R_FAIL;
   }

   for (i = 1; i <= (int)cnt; i++)
   {
      size = 0;
      if (em_sdo_read(bus, slave, index, (uint8_t)i, &e[i - 1], &size) != EM_R_OK)
      {
         em__err("读 %04Xh:%02X (第 %d 项) 失败", (unsigned)index, i, i);
         return EM_R_FAIL;
      }
      if (size != ent_size)
      {
         em__err("%04Xh:%02X 自报宽度 %d 字节, 期望 %d -> 不猜",
                 (unsigned)index, i, size, ent_size);
         return EM_R_FAIL;
      }
   }
   *n = (int)cnt;
   return EM_R_OK;
}

/* 求 (index, sub) 在映射里的字节偏移。找不到 / 位宽不符 / 前面累计位数不是 8 的整数倍 -> -1。
 * 必须死守: 偏移算错 = 把控制字写进别的字段 (万一那是 607Ah 就下了个位置指令)。 */
static int em_map_offset(const uint32_t *e, int n, uint16_t index, uint8_t sub,
                         int want_bits)
{
   int bit = 0;
   int i;

   for (i = 0; i < n; i++)
   {
      uint16_t ei = (uint16_t)((e[i] >> 16) & 0xFFFFu);
      uint8_t  es = (uint8_t)((e[i] >> 8) & 0xFFu);
      int      eb = (int)(e[i] & 0xFFu);

      /* 位宽 0 的项推不动偏移, 整张表不可解释 */
      if (eb <= 0)
         return -1;
      if (ei == index && es == sub)
      {
         if (eb != want_bits)
            return -1;
         /* 位对齐的字段不能按字节读写 */
         if ((bit % 8) != 0)
            return -1;
         return bit / 8;
      }
      bit += eb;
   }
   return -1;
}

/* 映射表里所有项的总位宽 */
static int em_map_bits(const uint32_t *e, int n)
{
   int bit = 0;
   int i;

   for (i = 0; i < n; i++)
      bit += (int)(e[i] & 0xFFu);
   return bit;
}

/* 在指定的映射对象里查一个字段的字节偏移, 查不到返回 -1 (偏移一律实读出来, 无一处写死)。
 * 用途是 6060h 这类"在映射里就得走过程数据"的量, 不能塞进 EM_NEED_RX 的"缺了就补"语义里。 */
static int em__find_field(em_bus_t *bus, int slave, uint16_t pdo_index,
                          uint16_t index, uint8_t sub, int want_bits)
{
   uint32_t e[EM_MAP_MAX];
   int      n = 0;

   if (pdo_index == 0)
      return -1;
   if (em_map_read(bus, slave, pdo_index, 4, &n, e) != EM_R_OK)
      return -1;
   return em_map_offset(e, n, index, sub, want_bits);
}

/* 定 6083h/6084h 的兜底值: 先问驱动器自己, 读不到或读回 0 才用缺省。"实读 0"不能采信 ——
 * 这两个对象在生效 RxPDO 里, 回读 0 只代表被主站覆盖过, 不代表驱动器配置成 0 */
static uint32_t em__pick_ramp(em_bus_t *bus, int slave, uint16_t index,
                              uint32_t dflt, const char *what)
{
   uint32_t v = 0;

   if (em_rd_u32(bus, slave, index, 0, &v) != EM_R_OK)
   {
      printf("      %s 读不到 -> 用缺省 %u pul/s²\n",
             what, (unsigned)dflt);
      return dflt;
   }
   if (v == 0)
   {
      printf("      %s 实读 0 —— 它就在生效 RxPDO 里, 说明已被主站下发过 0 "
             "(不等于驱动器配置成 0)。本次下发 %u pul/s²\n",
             what, (unsigned)dflt);
      return dflt;
   }
   printf("      %s 实读 %u -> 采信驱动器自己的值\n", what, (unsigned)v);
   return v;
}

/* 快照一个映射对象 (必须在写之前取, 供收尾还原) */
static int em_snap_take(em_bus_t *bus, int slave, uint16_t index, int ent_size,
                        em_snap_t *s)
{
   uint8_t cnt = 0;
   int     i, size = 0;

   memset(s, 0, sizeof(*s));
   s->index = index;

   if (em_sdo_read(bus, slave, index, 0, &cnt, &size) != EM_R_OK || size != 1)
      return EM_R_FAIL;
   if (cnt > EM_MAP_MAX)
      return EM_R_FAIL;

   for (i = 1; i <= (int)cnt; i++)
   {
      uint32_t v = 0;

      size = 0;
      if (em_sdo_read(bus, slave, index, (uint8_t)i, &v, &size) != EM_R_OK)
         return EM_R_FAIL;
      if (size != ent_size)
         return EM_R_FAIL;
      s->e[i - 1] = v;
   }
   s->n = (int)cnt;
   s->have = 1;
   return EM_R_OK;
}

/* 把一个快照写回去。返回 0 / -1 (调用方按 WARN 处理: 这些对象只写 RAM) */
static int em_snap_restore(em_bus_t *bus, int slave, em_snap_t *s, int ent_size,
                           const char *label)
{
   uint8_t  zero = 0;
   uint8_t  nn;
   int      i;

   if (!s->have || !s->changed)
      return EM_R_OK;

   nn = (uint8_t)s->n;

   if (em__sdo_write_raw(bus, slave, s->index, 0, 1, &zero, EC_TIMEOUTRXM) != EM_R_OK)
   {
      em__warn("还原 %s (%04Xh) 失败: 项数归零没写进去", label,
               (unsigned)s->index);
      return EM_R_FAIL;
   }
   for (i = 0; i < s->n; i++)
   {
      if (em__sdo_write_raw(bus, slave, s->index, (uint8_t)(i + 1), ent_size,
                            &s->e[i], EC_TIMEOUTRXM) != EM_R_OK)
      {
         em__warn("还原 %s (%04Xh) 失败: 第 %d 项没写进去", label,
                  (unsigned)s->index, i + 1);
         return EM_R_FAIL;
      }
   }
   if (em__sdo_write_raw(bus, slave, s->index, 0, 1, &nn, EC_TIMEOUTRXM) != EM_R_OK)
   {
      em__warn("还原 %s (%04Xh) 失败: 项数没回值", label, (unsigned)s->index);
      return EM_R_FAIL;
   }
   em__log(bus, "已还原 %s (%04Xh) 为 %d 项", label, (unsigned)s->index, s->n);
   return EM_R_OK;
}

/* 实读出一个方向生效的 PDO 映射对象索引。真机 1C12h = { 0x1601 } (不是 1600h), 1C13h = { 0x1A00 };
 * 写死 1600h 不报错, 只会悄悄去改一张没生效的表。*out 在分配为空时取 fallback, 由调用方指派。 */
static int em_discover_pdo(em_bus_t *bus, int slave, uint16_t assign_index,
                           uint16_t fallback, uint16_t *out, const char *label)
{
   uint32_t e[EM_MAP_MAX];
   int      n = 0, i;

   if (em_map_read(bus, slave, assign_index, 2, &n, e) != EM_R_OK)
   {
      em__err("%s: 读 %04Xh (PDO 分配) 失败 -> 拒绝", label,
              (unsigned)assign_index);
      return EM_R_FAIL;
   }

   if (n == 0)
   {
      /* 分配为空 = 该方向没有生效的 PDO = SM 长度为 0。取 fallback, 由调用方去写分配对象 */
      *out = fallback;
      em__log(bus, "%s: %04Xh 分配为空 (该方向没有生效的 PDO) -> 本接口将指派 %04Xh",
              label, (unsigned)assign_index, (unsigned)fallback);
      return EM_R_OK;
   }

   if (n > 1)
   {
      /* 多个 PDO 拼接同一 SM 时映射是按顺序首尾相接的, 往哪张表、哪一端追加没验证过 -> 拒绝 */
      em__err("%s: %04Xh 分配了 %d 个 PDO", label, (unsigned)assign_index, n);
      for (i = 0; i < n; i++)
         printf(" 0x%04X", (unsigned)(e[i] & 0xFFFFu));
      em__err("。多个 PDO 拼接同一 SM 的情况本接口没有验证过, 不猜它们怎么拼 -> 拒绝");
      return EM_R_FAIL;
   }

   *out = (uint16_t)(e[0] & 0xFFFFu);
   em__log(bus, "%s: %04Xh 实读分配的是 %04Xh%s", label, (unsigned)assign_index,
           (unsigned)*out,
           (*out != fallback) ? "  <<< 不是通常的默认值, 按实读的来" : "");
   return EM_R_OK;
}

/* 确保一个 PDO 映射对象含 need[] 里的每一项: 只追加缺失项, 已有的原样保留; 写之前先把追加后的
 * 整张表证明一遍 (每个字段都能算出字节偏移), 证不出就拒绝。pdo_index 由调用方实读得到。
 * 返回 0 = 已满足 (可能没写) / -1 = 拒绝或失败; *changed 告知是否真写过 (供收尾还原)。 */
static int em_map_ensure(em_bus_t *bus, int slave, uint16_t assign_index,
                         uint16_t pdo_index, const em_field_t *need, int nneed,
                         int allow_remap, em_snap_t *snap_assign,
                         em_snap_t *snap_pdo, const char *label)
{
   uint32_t newtab[EM_MAP_MAX + EM_MAP_MAX];
   uint32_t assign_e[EM_MAP_MAX];
   uint32_t cur[EM_MAP_MAX];
   int      an = 0, n = 0, i, k, added = 0;
   int      assign_needs_write = 0;

   /* ---- 1. 看分配对象是否为空 (项是 U16, 所以 ent_size = 2) ---- */
   if (em_map_read(bus, slave, assign_index, 2, &an, assign_e) != EM_R_OK)
   {
      em__err("读 %04Xh (PDO 分配) 失败 -> 拒绝", (unsigned)assign_index);
      return EM_R_FAIL;
   }
   if (an > 0 && (uint16_t)(assign_e[0] & 0xFFFFu) != pdo_index)
   {
      /* 调用方给的 pdo_index 与实读的分配不一致 = "改错表"的入口, 不静默按其中一个来 */
      em__err("%04Xh 实读分配的是 0x%04X, 但本函数被要求处理 0x%04X -> 拒绝",
              (unsigned)assign_index, (unsigned)(assign_e[0] & 0xFFFFu),
              (unsigned)pdo_index);
      return EM_R_FAIL;
   }
   if (an == 0)
      assign_needs_write = 1;

   /* ---- 2. 读映射对象 (1600h/1601h/1A00h...) —— 项是 U32, 所以 ent_size = 4 ---- */
   if (em_map_read(bus, slave, pdo_index, 4, &n, cur) != EM_R_OK)
   {
      em__err("读 %04Xh (PDO 映射) 失败 -> 拒绝", (unsigned)pdo_index);
      return EM_R_FAIL;
   }

   /* ---- 3. 找出缺哪些项 ---- */
   for (i = 0; i < n; i++)
      newtab[i] = cur[i];
   for (k = 0; k < nneed; k++)
   {
      if (em_map_offset(cur, n, need[k].index, need[k].sub, need[k].bits) >= 0)
         continue;

      if (n + added >= EM_MAP_MAX + EM_MAP_MAX)
      {
         em__err("%s: 映射项数超出上限 -> 拒绝", label);
         return EM_R_FAIL;
      }
      newtab[n + added] = EM_MAP_ENTRY(need[k].index, need[k].sub, need[k].bits);
      added++;
      em__log(bus, "%s: 缺 %s (%04Xh:%02X, %d bit) -> 追加为第 %d 项",
              label, need[k].name, (unsigned)need[k].index, (unsigned)need[k].sub,
              need[k].bits, n + added);
   }

   if (added == 0 && !assign_needs_write)
   {
      em__log(bus, "%s: 需要的项都已在映射里 (%d 项), 不动它", label, n);
      return EM_R_OK;
   }

   /* ---- 4. 写之前先证明: 追加后每个需要的字段都能算出字节偏移 ---- */
   for (k = 0; k < nneed; k++)
   {
      if (em_map_offset(newtab, n + added, need[k].index, need[k].sub,
                        need[k].bits) < 0)
      {
         em__err("%s: 追加之后 %s 仍算不出字节偏移 "
                 "(前面的项里有非整字节字段) -> 拒绝, 不猜偏移",
                 label, need[k].name);
         return EM_R_FAIL;
      }
   }

   /* ---- 5. 授权 ---- */
   if (!allow_remap)
   {
      em__err("%s: 缺 %d 项, 但未授权改写 PDO 映射 (需要 allow_remap / --allow-pdo)"
              " -> 拒绝, 一个字节都没写", label, added);
      return EM_R_FAIL;
   }

   /* ---- 6. 快照原值 (必须在写之前取, 写之后取就等于"还原成改过的值") ---- */
   if (em_snap_take(bus, slave, pdo_index, 4, snap_pdo) != EM_R_OK ||
       em_snap_take(bus, slave, assign_index, 2, snap_assign) != EM_R_OK)
   {
      em__err("%s: 取原始快照失败 -> 拒绝, 不写", label);
      return EM_R_FAIL;
   }

   /* ---- 7. 写分配对象 (只在它为空时才需要) ---- */
   if (assign_needs_write)
   {
      uint8_t  zero = 0, one = 1;
      uint16_t pdo16 = pdo_index;

      /* 顺序是规范要求的: 计数归零 -> 写项 -> 计数回值 */
      if (em__sdo_write_raw(bus, slave, assign_index, 0, 1, &zero, EC_TIMEOUTRXM) != EM_R_OK ||
          em__sdo_write_raw(bus, slave, assign_index, 1, 2, &pdo16, EC_TIMEOUTRXM) != EM_R_OK ||
          em__sdo_write_raw(bus, slave, assign_index, 0, 1, &one, EC_TIMEOUTRXM) != EM_R_OK)
      {
         em__err("%s: 写 %04Xh (PDO 分配) 失败", label, (unsigned)assign_index);
         return EM_R_FAIL;
      }
      snap_assign->changed = 1;
      em__log(bus, "%s: %04Xh 已分配为 { %04Xh }", label,
              (unsigned)assign_index, (unsigned)pdo_index);
   }

   /* ---- 8. 写映射对象 ---- */
   {
      uint8_t zero = 0;
      uint8_t nn = (uint8_t)(n + added);

      if (em__sdo_write_raw(bus, slave, pdo_index, 0, 1, &zero, EC_TIMEOUTRXM) != EM_R_OK)
      {
         em__err("%s: %04Xh 项数归零失败", label, (unsigned)pdo_index);
         return EM_R_FAIL;
      }
      for (i = 0; i < n + added; i++)
      {
         if (em__sdo_write_raw(bus, slave, pdo_index, (uint8_t)(i + 1), 4,
                               &newtab[i], EC_TIMEOUTRXM) != EM_R_OK)
         {
            em__err("%s: %04Xh 第 %d 项写失败", label, (unsigned)pdo_index, i + 1);
            return EM_R_FAIL;
         }
      }
      if (em__sdo_write_raw(bus, slave, pdo_index, 0, 1, &nn, EC_TIMEOUTRXM) != EM_R_OK)
      {
         em__err("%s: %04Xh 项数回值失败", label, (unsigned)pdo_index);
         return EM_R_FAIL;
      }
      snap_pdo->changed = 1;
   }

   em__log(bus, "%s: %04Xh 已写为 %d 项 (%d 字节), 仅 RAM, 掉电回出厂值",
           label, (unsigned)pdo_index, n + added,
           (em_map_bits(newtab, n + added) + 7) / 8);
   return EM_R_OK;
}

em_sw_state_t em_sw_state(uint16_t sw)
{
   /* 按手册的 8 态迁移表 (V2.4 p9), 逐条判 bit6/5/3/2/1/0。
    * 低 4 位是非标准编码 0000/0001/0011/0111, 所以全程用位判断, 禁止数值比较。 */
   if ((sw & EM_SW_FAULT) != 0)
   {
      if ((sw & EM_SW_OP_ENABLED) != 0)
         return EM_ST_FAULT_REACTION_ACTIVE;
      return EM_ST_FAULT;
   }
   if ((sw & EM_SW_OP_ENABLED) != 0)
   {
      if ((sw & EM_SW_QUICKSTOP) == 0)
         return EM_ST_QUICK_STOP_ACTIVE;
      return EM_ST_OPERATION_ENABLED;
   }
   if ((sw & EM_SW_SWITCHED) != 0 && (sw & EM_SW_QUICKSTOP) != 0)
      return EM_ST_SWITCHED_ON;
   if ((sw & EM_SW_RTSO) != 0 && (sw & EM_SW_QUICKSTOP) != 0)
      return EM_ST_READY_TO_SWITCH_ON;
   if ((sw & EM_SW_SOD) != 0)
      return EM_ST_SWITCH_ON_DISABLED;
   return EM_ST_NOT_READY;
}

const char *em_sw_state_str(uint16_t sw)
{
   switch (em_sw_state(sw))
   {
      case EM_ST_NOT_READY:             return "Not ready to switch on";
      case EM_ST_SWITCH_ON_DISABLED:    return "Switch on disabled";
      case EM_ST_READY_TO_SWITCH_ON:    return "Ready to switch on (电机释放)";
      case EM_ST_SWITCHED_ON:           return "Switched on";
      case EM_ST_OPERATION_ENABLED:     return "Operation enabled (电机带电)";
      case EM_ST_QUICK_STOP_ACTIVE:     return "Quick stop active";
      case EM_ST_FAULT_REACTION_ACTIVE: return "Fault reaction active";
      case EM_ST_FAULT:                 return "Fault";
      default:                          return "未知";
   }
}

const char *em_sw_describe(uint16_t sw)
{
   static char buf[160];

   snprintf(buf, sizeof(buf),
            "0x%04X %s%s%s%s%s",
            (unsigned)sw, em_sw_state_str(sw),
            (sw & EM_SW_VOLTAGE) ? " | Voltage" : "",
            (sw & EM_SW_WARNING) ? " | Warning" : "",
            (sw & EM_SW_INTLIMIT) ? " | 硬件限位有效" : "",
            em_sw_remote_ok(sw) ? "" : " | **Remote=0 控制字不可操作**");
   return buf;
}

int em_sw_disabled_no_fault(uint16_t sw)
{
   /* 「未使能且无故障」 = bit2 未使能 + bit3 无故障。不要写成 (sw & 0x000F) == 0 ——
    * 上电自检完成后驱动器合法地停在低 4 位 = 0001 (Ready to switch on, 电机释放)。 */
   return (sw & (EM_SW_OP_ENABLED | EM_SW_FAULT)) == 0;
}

int em_sw_remote_ok(uint16_t sw) { return (sw & EM_SW_REMOTE) != 0; }

static const char *em_al_str(uint16_t st)
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

uint16_t em_al_state(em_bus_t *bus, int slave)
{
   if (bus == NULL || slave < 1 || slave > bus->nslaves)
      return 0;

   /* slave>=1 走 FPRD, 把原始 AL 状态字 (连错误位 0x10 一起) 写回 slavelist[].state;
    * ecx_statecheck(0) 走 BRD, 只填 slavelist[0], 不填单个从站。 */
   (void)ecx_statecheck(&bus->ctx, (uint16_t)slave, bus->ctx.slavelist[slave].state,
                        0);
   return bus->ctx.slavelist[slave].state;
}

static void em_request_state(em_bus_t *bus, int slave, uint16_t want)
{
   /* 从站处于 AL 错误态 (状态字 bit4) 时必须把 ACK 一起写进去才能清掉 ——
    * ecx_writestate() 只把 slavelist[].state 原样写下去, 不会自动置 ACK。 */
   uint16_t cur = bus->ctx.slavelist[slave].state;

   if ((cur & EC_STATE_ERROR) != 0)
      bus->ctx.slavelist[slave].state = (uint16_t)(want | EC_STATE_ACK);
   else
      bus->ctx.slavelist[slave].state = (uint16_t)want;
   (void)ecx_writestate(&bus->ctx, (uint16_t)slave);
}

/* 请求并等待某根轴进入 want。每轮都打过程数据再查状态 —— 状态迁移期间过程数据不能断,
 * 否则开着 SM 看门狗的驱动器会把我们从迁移里踢出来。返回 0 = 到达 / -1 = 失败 (已打印) */
static int em_wait_state(em_bus_t *bus, int slave, uint16_t want, uint32_t tmo_ms)
{
   uint32_t t0 = em__now_ms();

   /* 先取一次原始 AL 状态字 (timeout=0 -> 一次 FPRD 就返回), 否则下面的 ACK 判断读的是陈值 */
   (void)em_al_state(bus, slave);
   em_request_state(bus, slave, want);

   for (;;)
   {
      struct ec_slave *s = &bus->ctx.slavelist[slave];

      if (em_stop_requested())
         return EM_R_FAIL;

      (void)em__cycle(bus);

      if (ecx_statecheck(&bus->ctx, (uint16_t)slave, want, 1000) == want)
         return EM_R_OK;

      /* ecx_statecheck() 把状态按 0x000F 掩过, 错误位 (0x10) 在返回值里看不见 */
      if ((s->state & EC_STATE_ERROR) != 0)
      {
         em__err("请求 %s 被拒绝: AL 状态 0x%02X (含错误位), AL 状态码 0x%04X %s",
                 em_al_str(want), (unsigned)s->state, (unsigned)s->ALstatuscode,
                 ec_ALstatuscode2string(s->ALstatuscode));
         return EM_R_FAIL;
      }
      if ((int32_t)(em__now_ms() - t0) >= (int32_t)tmo_ms)
      {
         em__err("%ums 内未进入 %s (当前 AL 状态 0x%02X, AL 状态码 0x%04X %s)",
                 (unsigned)tmo_ms, em_al_str(want), (unsigned)s->state,
                 (unsigned)s->ALstatuscode,
                 ec_ALstatuscode2string(s->ALstatuscode));
         return EM_R_FAIL;
      }
      em__sleep_ms(EM_POLL_MS);
   }
}

int em__cycle(em_bus_t *bus)
{
   int wkc;
   int i;

   if (bus == NULL || !bus->mapped)
      return EM_R_FAIL;

   (void)ecx_send_processdata(&bus->ctx);
   wkc = ecx_receive_processdata(&bus->ctx, EC_TIMEOUTRET);

   /* 镜像只在整帧完整时更新: 短帧时输入镜像是陈值或半个帧, 而 0x0000 在 CiA402 里
    * 是个合法的"未使能", 拿它断言会把"没读到"说成"驱动器报了个状态"。 */
   if (bus->expected_wkc <= 0 || wkc >= bus->expected_wkc)
   {
      for (i = 0; i < bus->naxis; i++)
      {
         em_axis_t *ax = bus->axis[i];

         ax->sw = em__get_u16(ax->in, ax->off_sw);
         if (ax->off_act_pos >= 0)
            ax->pos = em__get_i32(ax->in, ax->off_act_pos);
         if (ax->off_act_vel >= 0)
            ax->vel = em__get_i32(ax->in, ax->off_act_vel);
         /* 60FDh: 不在映射里 (off_dig_in < 0) 就一直是 0, 而 0 看着就像"三个开关都没压住" ——
          * 调用方必须先问 em_dig_in_known()。 */
         if (ax->off_dig_in >= 0)
            ax->dig_in = em__get_u32(ax->in, ax->off_dig_in);

         ax->mirror_ok = 1;
         ax->frames++;
         ax->short_frames = 0;
      }
   }
   else
   {
      for (i = 0; i < bus->naxis; i++)
         bus->axis[i]->short_frames++;
   }

   /* 回调放在**镜像更新之后**: 它读的就是这些镜像, 早一步就会拿到上一帧的值。
    * 短帧那一支不跳过 —— "这帧不完整"本身就是调用方要知道的事 (见公共头 em_set_cycle_hook) */
   if (bus->cycle_fn != NULL)
      bus->cycle_fn(bus->cycle_user, wkc);

   return wkc;
}

int em_service(em_bus_t *bus) { return em__cycle(bus); }

void em_set_cycle_hook(em_bus_t *bus, em_cycle_fn fn, void *user)
{
   if (bus == NULL)
      return;

   bus->cycle_fn   = fn;
   bus->cycle_user = user;
}

static int em_axis_bind(em_axis_t *ax, uint16_t pdo_index, const em_field_t *need,
                        int nneed, int *offs_out, const char *label)
{
   uint32_t e[EM_MAP_MAX];
   int      n = 0, k;

   if (em_map_read(ax->bus, ax->slave, pdo_index, 4, &n, e) != EM_R_OK)
   {
      em__err("%s: 读 %04Xh 失败", label, (unsigned)pdo_index);
      return EM_R_FAIL;
   }

   for (k = 0; k < nneed; k++)
   {
      int off = em_map_offset(e, n, need[k].index, need[k].sub, need[k].bits);

      offs_out[k] = off;

      if (off < 0)
         em__warn("%s: %s 不在 %04Xh 映射里 (或位不对齐) -> em_*_available() 会拒绝"
                  "用它的模式", label, need[k].name, (unsigned)pdo_index);
      else
         em__log(ax->bus, "%s: %s 位于 +%d 字节", label, need[k].name, off);
   }
   return EM_R_OK;
}

em_bus_t *em_bus_new(void)
{
   em_bus_t *bus;

   em__console_init();
   bus = (em_bus_t *)calloc(1, sizeof(em_bus_t));
   if (bus != NULL)
      bus->verbose = 0;
   return bus;
}

void em_bus_free(em_bus_t *bus)
{
   if (bus != NULL)
   {
      int i;

      for (i = 0; i < EM_MAX_AXES; i++)
      {
         free(bus->axis[i]);
         bus->axis[i] = NULL;
      }
      free(bus);
   }
}

/* 网卡上限 (实机上一般个位数); 打印与填数组共用它, 免得两份遍历漏掉新字段 */
#define EM__MAX_ADAPTERS 32

/* 遍历一次 SOEM 的网卡链表, 填进数组。返回**总条数**(可能大于 max) */
static int em__fill_adapters(em_adapter_t *out, int max)
{
   ec_adaptert *head;
   ec_adaptert *a;
   int          n = 0;

   head = ec_find_adapters();
   for (a = head; a != NULL; a = a->next)
   {
      if (out != NULL && n < max)
      {
         /* 逐个 snprintf 而不是 strcpy: 与 SOEM 字段同宽 (128) 不等于一定带了结束符 */
         (void)snprintf(out[n].name, sizeof(out[n].name), "%s", a->name);
         (void)snprintf(out[n].desc, sizeof(out[n].desc), "%s", a->desc);
      }
      n++;
   }
   ec_free_adapters(head);
   return n;
}

int em_list_adapters(em_adapter_t *out, int max)
{
   em__console_init();
   if (out == NULL || max < 0)
      max = 0;
   return em__fill_adapters(out, max);
}

void em_print_adapters(void)
{
   static em_adapter_t list[EM__MAX_ADAPTERS];
   int n, i;

   n = em_list_adapters(list, EM__MAX_ADAPTERS);
   if (n > EM__MAX_ADAPTERS)
      n = EM__MAX_ADAPTERS;
   for (i = 0; i < n; i++)
      printf("    - %s  (%s)\n", list[i].name, list[i].desc);
}

int em_open(em_bus_t *bus, const char *ifname)
{
   int cnt;

   if (bus == NULL || ifname == NULL)
      return EM_R_FAIL;

   em__console_init();

   if (!ecx_init(&bus->ctx, ifname))
   {
      em__err("无法打开网卡 (被独占 / 需管理员 / Npcap 未装): %s", ifname);
      return EM_R_FAIL;
   }
   bus->opened = 1;

   cnt = ecx_config_init(&bus->ctx);
   if (cnt <= 0)
   {
      em__err("总线上未发现从站 (config_init = %d), 检查网线/供电", cnt);
      ecx_close(&bus->ctx);
      bus->opened = 0;
      return EM_R_FAIL;
   }
   bus->nslaves = cnt;

   (void)ecx_statecheck(&bus->ctx, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   (void)ecx_readstate(&bus->ctx);

   return cnt;
}

int em_slave_count(const em_bus_t *bus) { return bus ? bus->nslaves : 0; }

static int em_is_ykd(uint32_t man, uint32_t id)
{
   return man == EM_YKD_VENDOR_ID &&
          (id == EM_YKD_PRODUCT_1 || id == EM_YKD_PRODUCT_2);
}

int em_slave_info(const em_bus_t *bus, int slave, em_slave_info_t *out)
{
   struct ec_slave *s;

   if (bus == NULL || out == NULL || slave < 1 || slave > bus->nslaves)
      return EM_R_FAIL;

   s = (struct ec_slave *)&bus->ctx.slavelist[slave];
   memset(out, 0, sizeof(*out));
   out->pos          = slave - 1;
   out->slave        = slave;
   out->configadr    = s->configadr;
   out->aliasadr     = s->aliasadr;
   out->eep_man      = s->eep_man;
   out->eep_id       = s->eep_id;
   out->eep_rev      = s->eep_rev;
   out->state        = s->state;
   out->alstatuscode = s->ALstatuscode;
   out->Obytes       = s->Obytes;
   out->Ibytes       = s->Ibytes;
   out->is_ykd       = em_is_ykd(s->eep_man, s->eep_id);
   return EM_R_OK;
}

int em_setup(em_bus_t *bus, const em_axis_cfg_t *cfg, int naxis, int allow_remap)
{
   int i;

   if (bus == NULL || cfg == NULL || naxis < 1 || naxis > EM_MAX_AXES)
   {
      em__err("em_setup: 轴数必须在 1..%d 之间", EM_MAX_AXES);
      return EM_R_FAIL;
   }
   if (bus->mapped)
   {
      em__err("em_setup: 已经建过过程数据映射, 不能重复调用");
      return EM_R_FAIL;
   }

   /* 每一台从站都必须是被选中的轴: WKC 期望值按组里全部从站算, 留在 PRE_OP 的从站不参与
    * 过程数据交换, 帧的 WKC 会持续偏短, 与"过程数据没落地"分不开。 */
   if (naxis != bus->nslaves)
   {
      em__err("总线上有 %d 台从站, 但只选了 %d 根轴。本接口要求**全部选中** —— "
              "留在 PRE_OP 的从站不参与过程数据交换, 会让帧的 WKC 持续偏短, "
              "与「过程数据没落地」分不开。未写任何东西。",
              bus->nslaves, naxis);
      return EM_R_FAIL;
   }

   /* ---- 1. 身份门 + 必须处于干净的 PRE_OP ---- */
   for (i = 0; i < naxis; i++)
   {
      struct ec_slave *s;
      int    slave = cfg[i].bus_pos + 1;
      em_axis_t *ax;

      if (cfg[i].bus_pos < 0 || slave > bus->nslaves)
      {
         em__err("em_setup: 第 %d 根轴的 bus_pos=%d 超出范围", i, cfg[i].bus_pos);
         return EM_R_FAIL;
      }
      s = &bus->ctx.slavelist[slave];

      if (!em_is_ykd(s->eep_man, s->eep_id))
      {
         em__err("从站 %d (总线位置 %d) 不是受支持的 YKD2205PE "
                 "(厂商 0x%08X 产品码 0x%08X; 期望厂商 0x%04X 产品码 0x%04X/0x%04X) "
                 "-> 拒绝, 未写任何东西",
                 slave, cfg[i].bus_pos, (unsigned)s->eep_man, (unsigned)s->eep_id,
                 (unsigned)EM_YKD_VENDOR_ID, (unsigned)EM_YKD_PRODUCT_1,
                 (unsigned)EM_YKD_PRODUCT_2);
         return EM_R_FAIL;
      }

      /* 单独读一次目标轴的状态, 不能用 ecx_readstate() 的广播结果:
       * 它在"所有从站状态一致且无错误位"时会提前返回, 不填单个从站的 state。 */
      (void)em_al_state(bus, slave);
      if (bus->ctx.slavelist[slave].state != EC_STATE_PRE_OP)
      {
         em__err("从站 %d 不在干净的 PRE_OP: AL 状态 0x%02X, AL 状态码 0x%04X %s。"
                 "这种状态下映射对象不可写, 未写任何东西。",
                 slave, (unsigned)bus->ctx.slavelist[slave].state,
                 (unsigned)bus->ctx.slavelist[slave].ALstatuscode,
                 ec_ALstatuscode2string(bus->ctx.slavelist[slave].ALstatuscode));
         if ((bus->ctx.slavelist[slave].state & EC_STATE_ERROR) != 0)
            printf("        状态字带错误位 (0x10): 先清掉它再跑 —— 重新上电。\n");
         return EM_R_FAIL;
      }

      ax = (em_axis_t *)calloc(1, sizeof(em_axis_t));
      if (ax == NULL)
      {
         em__err("em_setup: 分配轴对象失败");
         return EM_R_FAIL;
      }
      ax->bus       = bus;
      ax->idx       = i;
      ax->bus_pos   = cfg[i].bus_pos;
      ax->slave     = slave;
      ax->pos_tol   = (cfg[i].pos_tol > 0) ? cfg[i].pos_tol : EM_POS_TOL_DEF;
      ax->move_limit = EM_MAX_DELTA_DEF;
      ax->off_cw = ax->off_target_pos = ax->off_target_vel = -1;
      ax->off_sw = ax->off_act_pos = ax->off_act_vel = -1;
      /* 必须显式置 -1: calloc 给的是 0, 而 0 是输入镜像的字节 0 (6041h 状态字的位置),
       * 会把状态字当成 60FDh 读 */
      ax->off_dig_in = -1;
      ax->dig_in = 0;
      /* 同上必须置 -1: calloc 给的 0 是个合法偏移 (输出镜像字节 0 正是 6040h 低字节),
       * 会把运行模式写进控制字里 */
      ax->off_modes = -1;
      /* 同上: +0 正是 6040h 控制字, em__pin_ramp 会把加减速度写进控制字里 */
      ax->off_prof_acc = ax->off_prof_dec = -1;
      snprintf(ax->label, sizeof(ax->label), "轴%d(从站%d)", i, slave);

      bus->axis[i] = ax;
      bus->naxis = i + 1;
   }

   /* ---- 2. 补 PDO 映射 (必须在 PRE_OP 下, 且必须在 config_map_group 之前) ---- */
   printf("\n---- 补 PDO 映射 (只追加缺项, 仅写 RAM) ----\n");
   for (i = 0; i < bus->naxis; i++)
   {
      em_axis_t *ax = bus->axis[i];

      /* 先问"哪个 PDO 生效"再补它, 必须排在写之前: 真机上 1C12h 分配的是 1601h,
       * 去补没生效的 1600h 不报错, 只会让后面所有偏移证明都在一张空表上通过。 */
      if (em_discover_pdo(bus, ax->slave, EM_OID_RXPDO_ASSIGN, EM_OID_RXPDO0,
                          &ax->rx_pdo, "RxPDO") != EM_R_OK)
         return EM_R_FAIL;
      if (em_discover_pdo(bus, ax->slave, EM_OID_TXPDO_ASSIGN, EM_OID_TXPDO0,
                          &ax->tx_pdo, "TxPDO") != EM_R_OK)
         return EM_R_FAIL;

      if (em_map_ensure(bus, ax->slave, EM_OID_RXPDO_ASSIGN, ax->rx_pdo,
                        EM_NEED_RX, EM_NEED_RX_N, allow_remap,
                        &bus->snap_rx[i].assign, &bus->snap_rx[i].pdo,
                        "RxPDO") != EM_R_OK)
         return EM_R_FAIL;

      /* TxPDO 的需项表在这里拼: 常规三项, 只有 em_require_dig_in() 明确要求过才把 60FDh
       * 拼进去 (不拼就是"只绑不补"的默认路径)。 */
      em_field_t need_tx[EM_NEED_TX_N + 1];
      int        n_need_tx = EM_NEED_TX_N;

      memcpy(need_tx, EM_NEED_TX, sizeof(EM_NEED_TX));
      if (bus->want_dig_in)
         need_tx[n_need_tx++] = EM_FIELD_DIG_IN[0];

      if (em_map_ensure(bus, ax->slave, EM_OID_TXPDO_ASSIGN, ax->tx_pdo,
                        need_tx, n_need_tx, allow_remap,
                        &bus->snap_tx[i].assign, &bus->snap_tx[i].pdo,
                        "TxPDO") != EM_R_OK)
         return EM_R_FAIL;

      if (bus->snap_rx[i].pdo.changed || bus->snap_tx[i].pdo.changed ||
          bus->snap_rx[i].assign.changed || bus->snap_tx[i].assign.changed)
         bus->mapped = 1;
   }

   /* ---- 3. 建过程数据映射 ---- */
   {
      int size;

      /* 自己走状态阶梯, 不让 SOEM 在 config_map_group 里自动请求 SAFE_OP */
      bus->prev_manualstatechange = bus->ctx.manualstatechange;
      bus->ctx.manualstatechange = 1;

      size = ecx_config_map_group(&bus->ctx, bus->iomap, 0);
      if (size <= 0)
      {
         em__err("ecx_config_map_group 返回 %d (没有有效的 PDO 映射) -> 拒绝", size);
         return EM_R_FAIL;
      }
      bus->mapped = 1;

      printf("  过程数据映射建成: IOmap %d 字节 (输出 %u + 输入 %u)\n",
             size, (unsigned)bus->ctx.grouplist[0].Obytes,
             (unsigned)bus->ctx.grouplist[0].Ibytes);

      bus->expected_wkc = (bus->ctx.grouplist[0].outputsWKC * 2)
                          + bus->ctx.grouplist[0].inputsWKC;
      printf("  期望 WKC = outputsWKC(%u)*2 + inputsWKC(%u) = %d\n",
             (unsigned)bus->ctx.grouplist[0].outputsWKC,
             (unsigned)bus->ctx.grouplist[0].inputsWKC, bus->expected_wkc);
   }

   /* ---- 4. 逐轴取镜像指针 + 证明偏移 ---- */
   for (i = 0; i < bus->naxis; i++)
   {
      em_axis_t *ax = bus->axis[i];
      int offs[3];

      ax->out    = bus->ctx.slavelist[ax->slave].outputs;
      ax->in     = bus->ctx.slavelist[ax->slave].inputs;
      ax->Obytes = bus->ctx.slavelist[ax->slave].Obytes;
      ax->Ibytes = bus->ctx.slavelist[ax->slave].Ibytes;

      printf("\n  %s: 输出镜像 %u 字节 (基址 %p) / 输入镜像 %u 字节 (基址 %p)\n",
             ax->label, (unsigned)ax->Obytes, (void *)ax->out,
             (unsigned)ax->Ibytes, (void *)ax->in);
      if (ax->out == NULL || ax->in == NULL)
      {
         em__err("%s: SOEM 没给出镜像指针 (该轴没有过程数据) -> 拒绝", ax->label);
         return EM_R_FAIL;
      }

      /* RxPDO: 6040h / 607Ah / 60FFh —— 用实读出的那张表, 不是写死的 1600h */
      if (em_axis_bind(ax, ax->rx_pdo, EM_NEED_RX, EM_NEED_RX_N, offs,
                       ax->label) != EM_R_OK)
         return EM_R_FAIL;
      ax->off_cw         = offs[0];
      ax->off_target_pos = offs[1];
      ax->off_target_vel = offs[2];

      /* TxPDO: 6041h / 6064h / 606Ch */
      if (em_axis_bind(ax, ax->tx_pdo, EM_NEED_TX, EM_NEED_TX_N, offs,
                       ax->label) != EM_R_OK)
         return EM_R_FAIL;
      ax->off_sw      = offs[0];
      ax->off_act_pos = offs[1];
      ax->off_act_vel = offs[2];

      /* 60FDh 数字输入 (U32 RO) —— 只绑不补, 与 6060h/6083h/6084h 同一类: 不在 EM_NEED_TX 里,
       * 所以不会为它改写 1A00h; 绑不上就是 -1 (一个监视项不该把 CSP 连坐掉) */
      ax->off_dig_in = em__find_field(bus, ax->slave, ax->tx_pdo,
                                      EM_OID_DIG_IN, 0, 32);
      if (ax->off_dig_in >= 0 && (uint32_t)ax->off_dig_in + 4 > ax->Ibytes)
      {
         /* 偏移落在本轴输入镜像之外 = 会读到别的从站的数据 -> 降级, 不拒绝 */
         em__warn("%s: 60FDh 算出的偏移 +%d 超出本轴输入镜像 (%u 字节) -> "
                  "当作不在映射里 (限位开关那三个灯会显示 --)",
                  ax->label, ax->off_dig_in, (unsigned)ax->Ibytes);
         ax->off_dig_in = -1;
      }
      printf("  %s: 60FDh 数字输入 %s\n", ax->label,
             (ax->off_dig_in >= 0)
                ? "在生效 TxPDO 里 (原点/正限位/负限位三个开关每周期可读)"
                : "**不在生效 TxPDO 里** -> 三个开关的灯会显示灰/-- "
                  "(本函数按设计不去补映射; 要补见 em_require_dig_in)");

      /* 6040h 与 6041h 是硬要求 (没有它们连状态机都推不动); 其余四项缺了只是用它们的
       * 模式不能用, 由 em_csp_available()/em_pv_available() 说清楚, 不在这里整体拒绝。 */
      if (ax->off_cw < 0 || ax->off_sw < 0)
      {
         em__err("%s: 6040h 或 6041h 不在映射里, 连状态机都推不动 -> 拒绝",
                 ax->label);
         return EM_R_FAIL;
      }
      if ((uint32_t)ax->off_cw + 2 > ax->Obytes ||
          (uint32_t)ax->off_sw + 2 > ax->Ibytes)
      {
         em__err("%s: 算出的偏移超出本从站的镜像区间 (Obytes=%u, Ibytes=%u) -> 拒绝",
                 ax->label, (unsigned)ax->Obytes, (unsigned)ax->Ibytes);
         return EM_R_FAIL;
      }

      /* 6060h 单独查 (它不在 EM_NEED_RX 里, 不走"缺了就补"那条路): 在生效映射里就必须
       * 经过程数据驱动, 不在里面才走 SDO。 */
      ax->off_modes = em__find_field(bus, ax->slave, ax->rx_pdo,
                                     EM_OID_MODES, 0, 8);
      if (ax->off_modes >= 0)
      {
         if ((uint32_t)ax->off_modes + 1 > ax->Obytes)
         {
            em__err("%s: 6060h 算出的偏移 +%d 超出本从站输出镜像 (%u 字节) -> 拒绝",
                    ax->label, ax->off_modes, (unsigned)ax->Obytes);
            return EM_R_FAIL;
         }
         printf("  %s: 6060h 在生效 RxPDO 的 +%d -> 运行模式经过程数据驱动 "
                "(SDO 写会被下一帧撤销)\n", ax->label, ax->off_modes);
      }
      else
      {
         printf("  %s: 6060h 不在生效 RxPDO 里 -> 运行模式走 SDO\n", ax->label);
      }

      /* 6083h / 6084h 轮廓加减速度 —— 在生效映射里就是主站拥有的, 不写就是下发 0, 而 6083h = 0
       * 会让 PV 的斜坡起不来 (606Ch 恒为 0, 6064h 不动)。默认值采信驱动器自己的实读值。 */
      ax->off_prof_acc = em__find_field(bus, ax->slave, ax->rx_pdo,
                                        EM_OID_PROF_ACC, 0, 32);
      ax->off_prof_dec = em__find_field(bus, ax->slave, ax->rx_pdo,
                                        EM_OID_PROF_DEC, 0, 32);
      if (ax->off_prof_acc >= 0 && (uint32_t)ax->off_prof_acc + 4 > ax->Obytes)
      {
         em__err("%s: 6083h 算出的偏移 +%d 超出本从站输出镜像 (%u 字节) -> 拒绝",
                 ax->label, ax->off_prof_acc, (unsigned)ax->Obytes);
         return EM_R_FAIL;
      }
      if (ax->off_prof_dec >= 0 && (uint32_t)ax->off_prof_dec + 4 > ax->Obytes)
      {
         em__err("%s: 6084h 算出的偏移 +%d 超出本从站输出镜像 (%u 字节) -> 拒绝",
                 ax->label, ax->off_prof_dec, (unsigned)ax->Obytes);
         return EM_R_FAIL;
      }

      ax->prof_acc = em__pick_ramp(bus, ax->slave, EM_OID_PROF_ACC,
                                   EM_RAMP_ACC_DEF, "6083h 加速度");
      ax->prof_dec = em__pick_ramp(bus, ax->slave, EM_OID_PROF_DEC,
                                   EM_RAMP_DEC_DEF, "6084h 减速度");

      if (ax->off_prof_acc < 0 && ax->off_prof_dec < 0)
         printf("  %s: 6083h/6084h 都不在生效 RxPDO 里 -> 主站不覆盖它们, 驱动器自己"
                "的值生效 (本接口记录的是 %u/%u pul/s²)\n", ax->label,
                (unsigned)ax->prof_acc, (unsigned)ax->prof_dec);
      else
         printf("  %s: 6083h/6084h 在生效 RxPDO 的偏移 +%d/+%d, 主站每周期下发 "
                "%u/%u pul/s² (-1 = 该项不在映射里, 不会被覆盖)\n", ax->label,
                ax->off_prof_acc, ax->off_prof_dec,
                (unsigned)ax->prof_acc, (unsigned)ax->prof_dec);

      /* 进 OP 之前就要把这些常量写进镜像 —— 一进 OP 主站就开始发帧了 */
      em__pin_ramp(ax);
   }

   /* ---- 5. 上 SAFE_OP ---- */
   em__log(bus, "---- 请求 SAFE_OP ----");
   for (i = 0; i < bus->naxis; i++)
   {
      if (em_wait_state(bus, bus->axis[i]->slave, EC_STATE_SAFE_OP,
                        EC_TIMEOUTSTATE) != EM_R_OK)
      {
         em__err("%s: 上 SAFE_OP 失败。若 AL 状态码是 0x001E "
                 "(Invalid input configuration), 说明 TxPDO 里没有有效映射 "
                 "(SM3 长度为 0)", bus->axis[i]->label);
         return EM_R_FAIL;
      }
      em__log(bus, "%s: SAFE_OP [PASS]", bus->axis[i]->label);
   }

   return EM_R_OK;
}

int em_axis_count(const em_bus_t *bus) { return bus ? bus->naxis : 0; }

em_axis_t *em_axis(em_bus_t *bus, int i)
{
   if (bus == NULL || i < 0 || i >= bus->naxis)
      return NULL;
   return bus->axis[i];
}

em_axis_t *em_axis_by_pos(em_bus_t *bus, int bus_pos)
{
   int i;

   if (bus == NULL)
      return NULL;
   for (i = 0; i < bus->naxis; i++)
   {
      if (bus->axis[i]->bus_pos == bus_pos)
         return bus->axis[i];
   }
   return NULL;
}

int em_expected_wkc(const em_bus_t *bus) { return bus ? bus->expected_wkc : 0; }

int em_enter_op(em_bus_t *bus, int use_dc, uint32_t cycle_us)
{
   int i;

   if (bus == NULL || !bus->mapped)
      return EM_R_FAIL;

   if (use_dc)
   {
      printf("\n---- 配置 DC (周期 %u us) ----\n", (unsigned)cycle_us);
      if (!ecx_configdc(&bus->ctx))
         em__warn("ecx_configdc 返回 false —— 继续, 但 DC 可能没配上");
      for (i = 0; i < bus->naxis; i++)
      {
         /* 光 ecx_configdc 不够, 还得 ecx_dcsync0 把 SYNC0 真的打开 */
         ecx_dcsync0(&bus->ctx, (uint16_t)bus->axis[i]->slave, TRUE,
                     (uint32_t)cycle_us * 1000u, 0);
         em__log(bus, "%s: SYNC0 已启用", bus->axis[i]->label);
      }
   }

   printf("\n---- 进 OP ----\n");
   (void)em__cycle(bus);   /* 先进一帧, 把从站的 SM 数据通路带起来 */

   for (i = 0; i < bus->naxis; i++)
   {
      if (em_wait_state(bus, bus->axis[i]->slave, EC_STATE_OPERATIONAL,
                        EC_TIMEOUTSTATE) != EM_R_OK)
      {
         em__err("%s: 进不去 OP。若 AL 状态码是 0x001B 一类看门狗/同步相关, "
                 "检查 DC 周期 (Windows 不是实时系统, 周期太小会丢帧)",
                 bus->axis[i]->label);
         return EM_R_FAIL;
      }
      em__log(bus, "%s: OP [PASS]", bus->axis[i]->label);
   }

   bus->in_op = 1;
   return EM_R_OK;
}

void em_shutdown(em_bus_t *bus, int restore_mapping, int *motor_maybe_live)
{
   int i;
   int unconfirmed = 0;

   if (motor_maybe_live != NULL)
      *motor_maybe_live = 0;
   if (bus == NULL)
      return;

   printf("\n---- 收尾 ----\n");

   /* ---- 1. 只要动过控制字, 就写 0x0000 并继续打一段过程数据 ---- */
   if (bus->mapped && bus->in_op)
   {
      int wrote = 0;

      for (i = 0; i < bus->naxis; i++)
      {
         em_axis_t *ax = bus->axis[i];

         if (ax->mirror_ok && (ax->sw & EM_SW_OP_ENABLED) != 0)
         {
            em__set_cw(ax, EM_CW_DISABLE_V);
            wrote = 1;
         }
      }
      if (wrote)
      {
         int k;

         printf("  所有使能中的轴写 RxPDO[6040h]=0x0000 (Disable voltage), "
                "继续打 250ms 过程数据\n");
         for (k = 0; k < 50; k++)
         {
            (void)em__cycle(bus);
            em__sleep_ms(5);
         }
      }
   }

   /* ---- 2. 用 TxPDO 的 6041h 确认 bit2 已清 (这是"电机不带电"的正面证据) ---- */
   for (i = 0; i < bus->naxis; i++)
   {
      em_axis_t *ax = bus->axis[i];
      int reads = 0, k;
      uint16_t sw = 0;

      if (!bus->in_op || !ax->mirror_ok)
         continue;

      for (k = 0; k < 200; k++)
      {
         (void)em__cycle(bus);
         sw = ax->sw;
         reads++;
         if ((sw & EM_SW_OP_ENABLED) == 0)
            break;
         em__sleep_ms(5);
      }
      if (reads == 0 || (sw & EM_SW_OP_ENABLED) != 0)
      {
         em__err("%s: 写了 0x0000 但 6041h 仍报 Operation enabled (0x%04X) —— "
                 "失能无法确认", ax->label, (unsigned)sw);
         unconfirmed = 1;
      }
      else
      {
         printf("  %s: [PASS] 已确认失能 (%s)\n", ax->label, em_sw_state_str(sw));
      }
   }

   /* ---- 2.5 还原 2300h (只还原**真改过**的轴; 这一步不查授权) ----
    * 授权管"改"; 还原是把这一趟改掉的撤掉 —— 再要一次授权会落成"没授权时收尾反而留下改动"。
    * 不绑在 bus->mapped 上: 改 2300h 与有没有补 PDO 映射是两件事。 */
   {
      int any_di = 0;

      for (i = 0; i < bus->naxis; i++)
         if (bus->di_logic_changed[i])
            any_di = 1;

      if (any_di)
      {
         if (restore_mapping)
         {
            for (i = 0; i < bus->naxis; i++)
            {
               em_axis_t *ax = bus->axis[i];

               if (!bus->di_logic_changed[i])
                  continue;

               if (em__di_write(ax, bus->di_logic_orig[i], bus->di_logic_sz[i],
                                "收尾还原 2300h") != EM_R_OK)
                  em__warn("%s: 2300h 还原失败 —— 驱动器里仍是这一趟写进去的值", ax->label);
               else
               {
                  bus->di_logic_changed[i] = 0;
                  printf("  %s: 2300h 已还原 0x%04X\n",
                         ax->label, (unsigned)bus->di_logic_orig[i]);
               }
            }
         }
         else
         {
            printf("  按 --keep-mapping: 保留改过的 2300h (顺带也保留 PDO 映射)\n");
         }
      }
   }

   /* ---- 3. 还原 PDO 映射 (默认; 这些对象只写 RAM, 失败只记 WARN) ---- */
   if (bus->mapped)
   {
      if (restore_mapping)
      {
         for (i = 0; i < bus->naxis; i++)
         {
            em_axis_t *ax = bus->axis[i];

            if (bus->snap_rx[i].pdo.changed || bus->snap_tx[i].pdo.changed ||
                bus->snap_rx[i].assign.changed || bus->snap_tx[i].assign.changed)
            {
               printf("  %s: 还原 PDO 映射 ...\n", ax->label);
               /* 先映射对象, 后分配对象 —— **这是写入的逆序**, 不是随手排的:
                * 写入时是先写分配表 (1C12h/1C13h, 见上面第 7 步) 再写映射对象 (第 8 步),
                * 还原就反过来。撤东西按放的逆序, 中途每一步都是驱动器认得的组合 */
               (void)em_snap_restore(bus, ax->slave, &bus->snap_tx[i].pdo, 4, "TxPDO 1A00h");
               (void)em_snap_restore(bus, ax->slave, &bus->snap_tx[i].assign, 2, "TxPDO 分配 1C13h");
               (void)em_snap_restore(bus, ax->slave, &bus->snap_rx[i].pdo, 4, "RxPDO 1600h");
               (void)em_snap_restore(bus, ax->slave, &bus->snap_rx[i].assign, 2, "RxPDO 分配 1C12h");
            }
         }
      }
      else
      {
         printf("  按 --keep-mapping: 保留改过的 PDO 映射 "
                "(仅 RAM, 掉电即回出厂值)\n");
      }
   }

   /* ---- 4. 降回 PRE_OP ---- */
   if (bus->mapped)
   {
      printf("  降回 PRE_OP ...\n");
      for (i = 0; i < bus->naxis; i++)
      {
         if (em_wait_state(bus, bus->axis[i]->slave, EC_STATE_PRE_OP, 1000) != EM_R_OK)
            em__warn("%s: 未能确认回到 PRE_OP", bus->axis[i]->label);
      }
   }

   if (unconfirmed)
      printf("  >>> 电机可能仍然带电。立即断开驱动器供电, 不要用手去推滑台。\n");

   if (motor_maybe_live != NULL)
      *motor_maybe_live = unconfirmed;

   /* ---- 5. 关网卡 ---- */
   if (bus->opened)
   {
      bus->ctx.manualstatechange = bus->prev_manualstatechange;
      ecx_close(&bus->ctx);
      bus->opened = 0;
   }
   bus->mapped = 0;
   bus->in_op = 0;
}

/* 轴: 访问器与只读查询 */

int         em_axis_slave(const em_axis_t *ax)  { return ax ? ax->slave : 0; }
int         em_axis_buspos(const em_axis_t *ax) { return ax ? ax->bus_pos : -1; }
int         em_axis_index(const em_axis_t *ax)  { return ax ? ax->idx : -1; }
const char *em_axis_label(const em_axis_t *ax)  { return ax ? ax->label : "(null)"; }

int em_axis_set_move_limit(em_axis_t *ax, uint32_t max_delta_pul)
{
   if (ax == NULL || max_delta_pul == 0)
      return EM_R_FAIL;
   ax->move_limit = max_delta_pul;
   return EM_R_OK;
}

uint32_t em_axis_move_limit(const em_axis_t *ax)
{
   return ax ? ax->move_limit : 0;
}

uint16_t em_sw (const em_axis_t *ax) { return ax ? ax->sw : 0; }
int32_t  em_pos(const em_axis_t *ax) { return ax ? ax->pos : 0; }
int32_t  em_vel(const em_axis_t *ax) { return ax ? ax->vel : 0; }
int      em_mirror_ok(const em_axis_t *ax) { return ax ? ax->mirror_ok : 0; }
uint32_t em_mirror_frames(const em_axis_t *ax) { return ax ? ax->frames : 0; }

uint16_t em_rx_pdo(const em_axis_t *ax) { return ax ? ax->rx_pdo : 0; }
uint16_t em_tx_pdo(const em_axis_t *ax) { return ax ? ax->tx_pdo : 0; }

/* 60FDh 的三个开关。位运算集中在这里, 与状态字判读集中在 ec_motor.c 同一条规矩:
 * 调用方各写一份 (x & 0x2), 就会出现某处把 0x2 写成 0x4 */
int em_dig_in_known(const em_axis_t *ax)
{
   return (ax != NULL && ax->off_dig_in >= 0 && ax->mirror_ok) ? 1 : 0;
}

int em_di_home  (const em_axis_t *ax)
{
   return (ax != NULL && (ax->dig_in & EM_DI_HOME) != 0) ? 1 : 0;
}

int em_di_poslim(const em_axis_t *ax)
{
   return (ax != NULL && (ax->dig_in & EM_DI_POS_LIMIT) != 0) ? 1 : 0;
}

int em_di_neglim(const em_axis_t *ax)
{
   return (ax != NULL && (ax->dig_in & EM_DI_NEG_LIMIT) != 0) ? 1 : 0;
}

uint32_t em_dig_in_raw   (const em_axis_t *ax) { return ax ? ax->dig_in : 0; }
int      em_dig_in_offset(const em_axis_t *ax) { return ax ? ax->off_dig_in : -1; }

void em_require_dig_in(em_bus_t *bus, int on)
{
   if (bus != NULL)
      bus->want_dig_in = on ? 1 : 0;
}

void em_allow_param_write(em_bus_t *bus, int on)
{
   if (bus != NULL)
      bus->allow_param = on ? 1 : 0;
}

int em_di_set_logic(em_bus_t *bus, uint16_t want)
{
   int i, any_fail = 0, nwrite = 0;

   if (bus == NULL || !bus->opened || bus->naxis <= 0)
      return EM_R_FAIL;

   if (!bus->allow_param)
   {
      em__err("改 2300h 需要授权 (em_allow_param_write / --allow-param)"
              " -> 拒绝, 一个字节都没写");
      return EM_R_FAIL;
   }

   /* ---- 1. 先把每根轴的原值读齐。任一根读不到就整体拒绝: 没读到原值就写, 收尾无从还原
    *         (这才是"一个字节都不写"的意义) ---- */
   for (i = 0; i < bus->naxis; i++)
   {
      em_axis_t *ax = bus->axis[i];

      if (em__di_read(bus, ax->slave, &bus->di_logic_orig[i], &bus->di_logic_sz[i]) != EM_R_OK)
      {
         em__err("%s: 读不到 2300h 原值 -> 整体拒绝, 一个字节都不写", ax->label);
         return EM_R_FAIL;
      }
      bus->di_logic_have[i] = 1;
   }

   /* ---- 2. 逐轴写; 已经等于 want 的跳过 (写 = 改驱动器, 能不写就不写) ---- */
   for (i = 0; i < bus->naxis; i++)
   {
      em_axis_t *ax = bus->axis[i];
      uint16_t   v;

      if (EM_DI_LOGIC_EQ(bus->di_logic_orig[i], want))
      {
         printf("  %s: 2300h = 0x%04X, 已经是想要的极性 -> 不写\n",
                ax->label, (unsigned)bus->di_logic_orig[i]);
         continue;
      }

      v = (uint16_t)((bus->di_logic_orig[i] & (uint16_t)~EM_DI_LOGIC_MASK) |
                     (want & (uint16_t)EM_DI_LOGIC_MASK));

      if (em__di_write(ax, v, bus->di_logic_sz[i], "2300h 输入有效电平逻辑") != EM_R_OK)
      {
         em__err("%s: 写 2300h = 0x%04X 失败 -> 该轴仍是原极性 0x%04X",
                 ax->label, (unsigned)v, (unsigned)bus->di_logic_orig[i]);
         any_fail = 1;
         continue;
      }

      /* 写成功就立刻记下来: 后面某根轴失败也必须能把这根还原回去 */
      bus->di_logic_changed[i] = 1;
      nwrite++;
      printf("  %s: 2300h 0x%04X -> 0x%04X (%d 字节, 收尾写回原值)\n",
             ax->label, (unsigned)bus->di_logic_orig[i], (unsigned)v, bus->di_logic_sz[i]);
   }

   if (nwrite > 0)
      printf("  2300h 改了 %d 根轴: 60FDh 的三个开关与 6041h bit11 一起跟着变。\n", nwrite);
   if (any_fail)
   {
      printf("  >>> 有轴没改成: 那一根仍按原极性判限位 -> 扫描可能开不了。"
             "退路是改用上位机侧取反 (scan 界面的「高级选项」)。\n");
      return EM_R_FAIL;
   }
   return EM_R_OK;
}

int em_modes_via_pdo(const em_axis_t *ax)
{
   return (ax != NULL && ax->off_modes >= 0);
}

int em_modes_offset(const em_axis_t *ax)
{
   return ax ? ax->off_modes : -1;
}

void em_set_ramp(em_axis_t *ax, uint32_t acc, uint32_t dec)
{
   if (ax == NULL)
      return;

   /* 0 不拒绝 —— 有的驱动器把 6083h = 0 解释成"瞬时"; 但本机的 0 正是"斜坡起不来"的那个值 */
   if (acc == 0 || dec == 0)
      em__warn("%s: 把 6083h/6084h 设成 0 (acc=%u dec=%u) —— 本机驱动器在 PV 下会因此"
               "停在 0 转速: 它会收下速度指令 (6041h bit12 清零) 但一步不走",
               ax->label, (unsigned)acc, (unsigned)dec);

   ax->prof_acc = acc;
   ax->prof_dec = dec;
   /* 立刻写进镜像: 它是主站每周期下发的一项, 光记在结构体里不会到达驱动器 */
   em__pin_ramp(ax);
}

uint32_t em_ramp_acc(const em_axis_t *ax) { return ax ? ax->prof_acc : 0; }
uint32_t em_ramp_dec(const em_axis_t *ax) { return ax ? ax->prof_dec : 0; }
int      em_ramp_offset(const em_axis_t *ax) { return ax ? ax->off_prof_acc : -1; }

int em_is_enabled(const em_axis_t *ax)
{
   if (ax == NULL || !ax->mirror_ok)
      return 0;
   return (ax->sw & EM_SW_OP_ENABLED) != 0;
}

int em_pos_sdo(em_axis_t *ax, int32_t *pos)
{
   return (ax == NULL) ? EM_R_FAIL : em_rd_i32(ax->bus, ax->slave, EM_OID_ACT_POS, 0, pos);
}

int em_vel_sdo(em_axis_t *ax, int32_t *vel)
{
   return (ax == NULL) ? EM_R_FAIL : em_rd_i32(ax->bus, ax->slave, EM_OID_ACT_VEL, 0, vel);
}

int em_sw_sdo(em_axis_t *ax, uint16_t *sw)
{
   return (ax == NULL) ? EM_R_FAIL : em_rd_u16(ax->bus, ax->slave, EM_OID_STATUSWORD, 0, sw);
}

int em_csp_available(const em_axis_t *ax)
{
   if (ax == NULL)
      return 0;
   return ax->off_cw >= 0 && ax->off_target_pos >= 0 && ax->off_act_pos >= 0;
}

int em_pv_available(const em_axis_t *ax)
{
   if (ax == NULL)
      return 0;
   return ax->off_cw >= 0 && ax->off_target_vel >= 0 && ax->off_sw >= 0;
}

int em_get_mode(em_axis_t *ax)
{
   int8_t m = 0;

   if (ax == NULL)
      return -1;
   if (em_rd_i8(ax->bus, ax->slave, EM_OID_MODES_DISP, 0, &m) != EM_R_OK)
      return -1;
   return (int)m;
}

int em_set_mode(em_axis_t *ax, int mode)
{
   int8_t want;
   int    got;

   if (ax == NULL)
      return EM_R_FAIL;

   /* CiA402 规定运行模式只能在未使能时改: 已使能时改, 驱动器要么拒绝,
    * 要么在运动中换掉解释 607Ah 的方式 */
   if (em_is_enabled(ax))
   {
      em__err("%s: 已使能 (6041h=0x%04X), 拒绝改运行模式。先 em_disable()",
              ax->label, (unsigned)ax->sw);
      return EM_R_FAIL;
   }
   if (mode != EM_MODE_PP && mode != EM_MODE_PV && mode != EM_MODE_HM &&
       mode != EM_MODE_CSP)
   {
      em__err("%s: 运行模式 %d 不是本驱动器支持的值 "
              "(1 PP / 3 PV / 6 HM / 8 CSP —— **没有 9 CSV**)",
              ax->label, mode);
      return EM_R_FAIL;
   }

   want = (int8_t)mode;

   /* 两条传输路径, 由 setup 实读的 ax->off_modes 选: 在映射里 -> 写镜像 + 打帧; 不在 -> SDO 写。
    * 6060h 在生效 RxPDO 里时 SDO 写下一帧就被输出镜像盖回去 (真机: 6060h 回读 8, 6061h 读回 0) */
   if (ax->off_modes >= 0)
   {
      uint32_t t0    = em__now_ms();
      uint32_t got_f = 0;      /* 期间收到过多少完整帧 */

      em__log(ax->bus, "%s: 6060h 在生效 RxPDO 的 +%d -> 经过程数据下发 %d",
              ax->label, ax->off_modes, mode);

      for (;;)
      {
         /* 每周期都重写这一字节: 确认期间只要它被任何路径改掉, 驱动器就会按别的模式理解 607Ah */
         em__put_u8(ax->out, ax->off_modes, (uint8_t)want);
         (void)em__cycle(ax->bus);
         got_f = ax->frames;

         /* 读 6061h 确认驱动器认了。是 SDO 读不是写 —— 读不跟过程数据打架, 且 6061h 不在 TxPDO 里 */
         got = em_get_mode(ax);
         if (got == mode)
            break;

         if (em_stop_requested())
         {
            em__err("%s: 改运行模式期间收到停止请求 -> 中止", ax->label);
            return EM_R_FAIL;
         }
         if ((int32_t)(em__now_ms() - t0) >= (int32_t)EM_STEP_TMO_MS)
         {
            /* 分清两种失败: 一帧完整过程数据都没收到的话 6060h 根本没送到驱动器, 那是总线问题 */
            if (got_f == 0)
            {
               em__err("%s: %ums 内一帧完整过程数据都没收到 (short_frames=%u) "
                       "-> 6060h 根本没送到驱动器, 这是总线问题不是模式问题",
                       ax->label, (unsigned)EM_STEP_TMO_MS,
                       (unsigned)ax->short_frames);
            }
            else
            {
               em__err("%s: 写了 6060h=%d (经过程数据 +%d, 期间收到 %u 帧) "
                       "但 6061h 读回 %d —— 驱动器没接受这个模式",
                       ax->label, mode, ax->off_modes, (unsigned)got_f, got);
            }
            return EM_R_FAIL;
         }
         em__sleep_ms(EM_POLL_MS);
      }
   }
   else
   {
      if (em__wr_i8(ax, EM_OID_MODES, 0, want, "运行模式 6060h") != EM_R_OK)
         return EM_R_FAIL;

      /* 读 6061h 确认驱动器**接受了**: 6060h 写进去不等于它认这个模式 */
      got = em_get_mode(ax);
      if (got != mode)
      {
         em__err("%s: 写了 6060h=%d (经 SDO) 但 6061h 读回 %d —— 驱动器没接受这个模式",
                 ax->label, mode, got);
         return EM_R_FAIL;
      }
   }

   em__log(ax->bus, "%s: 运行模式 = %d (%s)", ax->label, mode,
           (mode == EM_MODE_CSP) ? "CSP 位置同步" :
           (mode == EM_MODE_PV)  ? "PV 速度" :
           (mode == EM_MODE_HM)  ? "HM 回零" : "PP 轮廓位置");
   return EM_R_OK;
}

/* 控制字步骤 —— 写镜像 -> 每周期打过程数据 -> 等状态字满足 */

int em__cw_step(em_axis_t *ax, const char *name, uint16_t cw,
                uint16_t mask, uint16_t want, uint32_t tmo_ms)
{
   uint32_t t0;
   int      reads = 0;
   int      got = 0;

   if (ax == NULL)
      return EM_R_FAIL;
   if (ax->off_cw < 0)
   {
      em__err("%s: 6040h 不在 RxPDO 映射里, 无法推送控制字", ax->label);
      return EM_R_FAIL;
   }

   printf("  %s: 写 RxPDO[6040h]=0x%04X ...", name, (unsigned)cw);
   fflush(stdout);

   em__set_cw(ax, cw);
   t0 = em__now_ms();

   while ((int32_t)(em__now_ms() - t0) < (int32_t)tmo_ms)
   {
      int wkc;

      if (em_stop_requested())
      {
         printf(" [中止]\n");
         return EM_R_STOP;
      }

      wkc = em__cycle(ax->bus);
      if (ax->bus->expected_wkc > 0 && wkc < ax->bus->expected_wkc)
      {
         if (ax->short_frames < 20)
         {
            em__sleep_ms(EM_POLL_MS);
            continue;
         }
         printf(" [FAIL] 过程数据 WKC=%d < 期望 %d (从站可能不在 OP / 掉线)\n",
                wkc, ax->bus->expected_wkc);
         return EM_R_FAIL;
      }

      if (ax->mirror_ok)
      {
         reads++;
         if ((ax->sw & mask) == want)
         {
            got = 1;
            break;
         }
      }
      em__sleep_ms(EM_POLL_MS);
   }

   if (!got)
   {
      printf(" [FAIL] 断言未成立\n");
      if (reads == 0)
      {
         /* 一笔都没取到。不能拿零初始化的 sw 冒充"实测 0x0000" —— 0x0000 在 CiA402 里
          * 是合法的"未使能", 那会把"不知道"说成"驱动器报了个状态" */
         printf("        一笔 6041h 都没从 TxPDO 取到: 状态未知\n");
      }
      else
      {
         printf("        期望 6041h & 0x%04X == 0x%04X, 实测 %s\n",
                (unsigned)mask, (unsigned)want, em_sw_describe(ax->sw));
      }
      printf("        AL 状态=0x%02X, AL 状态码=0x%04X %s\n",
             (unsigned)ax->bus->ctx.slavelist[ax->slave].state,
             (unsigned)ax->bus->ctx.slavelist[ax->slave].ALstatuscode,
             ec_ALstatuscode2string(
                ax->bus->ctx.slavelist[ax->slave].ALstatuscode));

      /* 用 SDO 回读 6040h, 把两种失败分开: 回读一致 = 控制字到了驱动器, 不受理在驱动器侧;
       * 回读是别的值 = 过程数据根本没落到控制字对象上 (主站侧: 偏移算错 / 从站不在 OP / WKC 短) */
      {
         uint16_t back = 0;

         if (em_rd_u16(ax->bus, ax->slave, EM_OID_CONTROLWORD, 0, &back) == EM_R_OK)
            printf("       6040h 回读 = 0x%04X (经 RxPDO 写的是 0x%04X) %s\n",
                   (unsigned)back, (unsigned)cw,
                   (back == cw) ? "一致 -> 控制字确实到了驱动器"
                                : "<<< 不一致 -> 过程数据没落到控制字对象上");
      }
      return EM_R_FAIL;
   }

   printf(" [PASS] 6041h=%s\n", em_sw_describe(ax->sw));
   return EM_R_OK;
}

int em__wait_sw(em_axis_t *ax, uint16_t mask, uint16_t want,
                uint32_t tmo_ms, const char *what)
{
   uint32_t t0;
   int      reads = 0;

   if (ax == NULL)
      return EM_R_FAIL;

   t0 = em__now_ms();
   for (;;)
   {
      if (em_stop_requested())
         return EM_R_STOP;

      (void)em__cycle(ax->bus);
      if (ax->mirror_ok)
      {
         reads++;
         if ((ax->sw & mask) == want)
            return EM_R_OK;
      }
      if ((int32_t)(em__now_ms() - t0) >= (int32_t)tmo_ms)
      {
         if (reads == 0)
            em__err("%s: 等 %s 超时 (%ums), 且一笔 6041h 都没取到: 状态未知",
                    ax->label, what, (unsigned)tmo_ms);
         else
            em__err("%s: 等 %s 超时 (%ums), 实测 %s", ax->label, what,
                    (unsigned)tmo_ms, em_sw_describe(ax->sw));
         return EM_R_FAIL;
      }
      em__sleep_ms(EM_POLL_MS);
   }
}

int em__check_motion_ready(const em_axis_t *ax, const char *stage)
{
   if (ax == NULL)
      return EM_R_FAIL;

   if (!ax->mirror_ok)
   {
      em__err("%s: [%s] 一笔完整的 6041h 都没取到, 状态未知 -> 拒绝动作",
              ax->label, stage);
      return EM_R_FAIL;
   }
   if ((ax->sw & EM_SW_FAULT) != 0)
   {
      em__err("%s: [%s] 6041h bit3 = Fault (%s) -> 拒绝动作",
              ax->label, stage, em_sw_state_str(ax->sw));
      return EM_R_FAIL;
   }
   if ((ax->sw & EM_SW_INTLIMIT) != 0)
   {
      em__err("%s: [%s] 6041h bit11 = 硬件限位有效 -> 拒绝动作",
              ax->label, stage);
      return EM_R_FAIL;
   }
   if ((ax->sw & EM_SW_OP_ENABLED) == 0)
   {
      em__err("%s: [%s] 6041h bit2 = 0, 电机未使能 (当前 %s) -> 拒绝动作",
              ax->label, stage, em_sw_state_str(ax->sw));
      return EM_R_FAIL;
   }
   return EM_R_OK;
}

static void em_dump_one_map(em_bus_t *bus, int slave, uint16_t assign_index,
                            uint16_t fallback, int sm, const em_field_t *need,
                            int nneed, const char *label)
{
   uint32_t ae[EM_MAP_MAX];
   uint32_t e[EM_MAP_MAX];
   int      an = 0, n = 0, i, k;
   uint16_t pdo_index;
   int      assigned = 1;

   printf("\n  ---- %s ----\n", label);

   /* 分配对象的项是 U16 (实测自报 2 字节), 映射对象的项是 U32 —— 见 em_map_read */
   if (em_map_read(bus, slave, assign_index, 2, &an, ae) != EM_R_OK)
   {
      printf("    %04Xh (分配): 读失败\n", (unsigned)assign_index);
      return;
   }
   printf("    %04Xh (分配): %d 项", (unsigned)assign_index, an);
   for (i = 0; i < an; i++)
      printf("  0x%04X", (unsigned)(ae[i] & 0xFFFFu));
   printf("\n");

   /* 映射对象由分配对象指定, 不是写死的 1600h / 1A00h。真机 1C12h 指的就是 1601h;
    * 去 dump 1600h 打出来的是一张与过程数据无关的表, 且它长得"很对" */
   if (an == 0)
   {
      pdo_index = fallback;
      assigned  = 0;
      printf("    >>> 分配为空: 该方向没有生效的 PDO, SM 长度会是 0。\n"
             "        下面按默认表 %04Xh 打印, 但注意它**没有生效**。\n",
             (unsigned)fallback);
   }
   else if (an > 1)
   {
      pdo_index = (uint16_t)(ae[0] & 0xFFFFu);
      printf("    >>> 多个 PDO 拼接同一 SM: 按规范映射要按顺序首尾相接, 只打印首项 %04Xh\n",
             (unsigned)pdo_index);
   }
   else
   {
      pdo_index = (uint16_t)(ae[0] & 0xFFFFu);
      printf("    >>> 本方向生效的映射对象是 %04Xh%s\n", (unsigned)pdo_index,
             (pdo_index != fallback)
                ? "  (不是通常的默认值 —— 所以偏移必须按这张表推)" : "");
   }

   if (em_map_read(bus, slave, pdo_index, 4, &n, e) != EM_R_OK)
   {
      printf("    %04Xh (映射): 读失败\n", (unsigned)pdo_index);
      return;
   }
   printf("    %04Xh (映射): %d 项\n", (unsigned)pdo_index, n);

   {
      int bit = 0;

      for (i = 0; i < n; i++)
      {
         uint16_t ei = (uint16_t)((e[i] >> 16) & 0xFFFFu);
         uint8_t  es = (uint8_t)((e[i] >> 8) & 0xFFu);
         int      eb = (int)(e[i] & 0xFFu);

         printf("      [%d] %04Xh:%02X  %2d bit  @ +%d.%d 字节%s\n",
                i + 1, (unsigned)ei, (unsigned)es, eb, bit / 8, bit % 8,
                (eb <= 0) ? "   <<< 位宽 0, 整张表不可解释" :
                ((bit % 8) != 0) ? "   <<< 非整字节, 后面的字段算不出字节偏移" : "");
         bit += eb;
      }
      printf("      合计 %d bit = %d 字节", bit, (bit + 7) / 8);
      printf("   (SOEM 当前记录的 SM%d 长度 = %d 字节",
             sm, (int)etohs(bus->ctx.slavelist[slave].SM[sm].SMlength));
      printf("%s)\n", bus->mapped ? "" : ", config_map 之前可能是 SII 值");
   }

   /* 本接口需要的字段在这张表里的偏移 */
   printf("    本接口需要的字段:");
   if (!assigned)
      printf(" (这张表没有生效, 下面的偏移仅供参考)");
   printf("\n");
   for (k = 0; k < nneed; k++)
   {
      int off = em_map_offset(e, n, need[k].index, need[k].sub, need[k].bits);

      if (off >= 0)
         printf("      %-24s -> +%d 字节\n", need[k].name, off);
      else
         printf("      %-24s -> **不在映射里** (缺项, 或位宽/对齐不符)\n",
                need[k].name);
   }
}

/* 打印生效 RxPDO 里某一项的处置: 在映射里给出偏移与"驱动/不驱动"及理由,
 * 不在映射里的说明主站不会覆盖它 */
static void em_dump_disposition(em_bus_t *bus, int slave, uint16_t rx_pdo,
                                uint16_t index, uint8_t sub, int bits,
                                const char *name, const char *disposition)
{
   int off = em__find_field(bus, slave, rx_pdo, index, sub, bits);

   if (off < 0)
      printf("      %s 不在映射里 -> 主站不覆盖它, 驱动器自己的值生效\n", name);
   else
      printf("      %s +%-4d %s\n", name, off, disposition);
}

void em_dump_pdo(em_bus_t *bus, int slave)
{
   struct ec_slave *s;

   if (bus == NULL || slave < 1 || slave > bus->nslaves)
      return;

   s = &bus->ctx.slavelist[slave];

   printf("\n  ==== 从站 %d (总线位置 %d) 的 PDO 映射实读 ====\n", slave, slave - 1);
   printf("    AL 状态 0x%02X\n", (unsigned)s->state);
   if (bus->mapped)
      printf("    过程数据镜像: 输出 %u 字节 / 输入 %u 字节\n",
             (unsigned)s->Obytes, (unsigned)s->Ibytes);
   else
      /* config_map_group 之前 SOEM 还没从 CoE 读到映射, slavelist[].Obytes/Ibytes 就是 0;
       * 把它打印成"输出 0 字节"是"还没读"说成"读到了 0" */
      printf("    过程数据镜像: (尚未建立 —— ecx_config_map_group 之前这两个字段还是 0,\n"
             "                   不代表设备没有过程数据; 下面那张实读的映射表才是依据)\n");
   printf("    >>> 偏移一律从下面这张**实读**的表推; 手册值 / ESI 声明值 / ESI 字典\n"
          "        默认值三者互不相同, 真机上量到的还是第四种, 都不能当依据。\n");

   em_dump_one_map(bus, slave, EM_OID_RXPDO_ASSIGN, EM_OID_RXPDO0, EM_SM_RXPDO,
                   EM_NEED_RX, EM_NEED_RX_N, "RxPDO (主站 -> 驱动器, 输出镜像)");
   em_dump_one_map(bus, slave, EM_OID_TXPDO_ASSIGN, EM_OID_TXPDO0, EM_SM_TXPDO,
                   EM_NEED_TX, EM_NEED_TX_N, "TxPDO (驱动器 -> 主站, 输入镜像)");

   /* 6060h 单独打印 —— 它不在上面那张表里, 但它最容易出事: 在生效 RxPDO 里就意味着
    * 主站每周期都在下发它, SDO 写一律被下一帧撤销, 所以偏移与传输方式必须看得见。
    */
   {
      uint32_t ae[EM_MAP_MAX];
      int      an = 0;

      if (em_map_read(bus, slave, EM_OID_RXPDO_ASSIGN, 2, &an, ae) == EM_R_OK &&
          an > 0)
      {
         uint16_t rx  = (uint16_t)(ae[0] & 0xFFFFu);
         int      off = em__find_field(bus, slave, rx, EM_OID_MODES, 0, 8);

         if (off >= 0)
            printf("    6060h 运行模式: 在生效 RxPDO %04Xh 的 +%d 字节 "
                   "-> **经过程数据驱动**\n"
                   "        (对它做 SDO 写会被下一帧撤销: 主站每周期都在下发它)\n",
                   (unsigned)rx, off);
         else
            printf("    6060h 运行模式: 不在生效 RxPDO %04Xh 里 -> 经 SDO 写\n",
                   (unsigned)rx);

         /* 表里每一项的处置都列出来。生效 RxPDO 里的每一项都是主站拥有的, 每周期都在下发 ——
          * 不驱动它就是下发 0, 且不会有任何报错 (6060h 曾 SDO 写进 8 而 6061h 读回 0;
          * 6083h 被下发成 0 之后 PV 斜坡起不来)。所以逐项交代"驱动 / 不驱动"及理由。 */
         printf("    本接口对生效 RxPDO 每一项的处置:\n");
         em_dump_disposition(bus, slave, rx, EM_OID_CONTROLWORD, 0, 16,
                             "6040h 控制字    ", "驱动 (使能状态机)");
         em_dump_disposition(bus, slave, rx, EM_OID_MODES, 0, 8,
                             "6060h 运行模式  ", "驱动 (em_set_mode)");
         em_dump_disposition(bus, slave, rx, EM_OID_TARGET_POS, 0, 32,
                             "607Ah 目标位置  ", "驱动 (CSP)");
         em_dump_disposition(bus, slave, rx, EM_OID_TARGET_VEL, 0, 32,
                             "60FFh 目标速度  ", "驱动 (PV)");
         em_dump_disposition(bus, slave, rx, EM_OID_PROF_VEL, 0, 32,
                             "6081h 轮廓速度  ",
                             "**不驱动** (本接口不做 PP) -> 主站下发 0, "
                             "与驱动器基线一致");
         {
            /* 这两项的处置文字要带上实际会下发的值 —— 它是不是 0 就是"PV 会不会动"的分水岭 */
            em_axis_t *ax = em_axis_by_pos(bus, slave - 1);
            char       b1[128], b2[128];

            if (ax == NULL && bus->naxis == 0)
            {
               /* em_dump_pdo 在 S2 就会被调用, 那时 em_setup() 还没跑、一根轴都没建:
                * 既不能说成"不驱动"也不说成"要下发 0", 如实说"还不知道" */
               snprintf(b1, sizeof(b1), "**未定** (em_setup 还没跑, 尚未建轴)");
               snprintf(b2, sizeof(b2), "**未定** (em_setup 还没跑, 尚未建轴)");
            }
            else if (ax == NULL)
            {
               /* 这个从站确实不是本接口管的轴 —— 没人给它定过值, 别说成"要下发 0" */
               snprintf(b1, sizeof(b1), "**不驱动** (本从站不是本接口的轴)");
               snprintf(b2, sizeof(b2), "**不驱动** (本从站不是本接口的轴)");
            }
            else
            {
               snprintf(b1, sizeof(b1), "驱动 (em_set_ramp, 每周期下发 %u pul/s²)%s",
                        (unsigned)em_ramp_acc(ax),
                        em_ramp_acc(ax) == 0 ? "  <<< 0 会让 PV 的斜坡起不来!" : "");
               snprintf(b2, sizeof(b2), "驱动 (em_set_ramp, 每周期下发 %u pul/s²)%s",
                        (unsigned)em_ramp_dec(ax),
                        em_ramp_dec(ax) == 0 ? "  <<< 0 会让 PV 的斜坡停不住!" : "");
            }
            em_dump_disposition(bus, slave, rx, EM_OID_PROF_ACC, 0, 32,
                                "6083h 轮廓加速度", b1);
            em_dump_disposition(bus, slave, rx, EM_OID_PROF_DEC, 0, 32,
                                "6084h 轮廓减速度", b2);
         }
      }
   }
}
