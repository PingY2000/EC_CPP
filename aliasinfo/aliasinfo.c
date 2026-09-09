/*
 * aliasinfo - 打印每个从站 ESC "Configured Station Alias" 寄存器
 * (0x0012h-0x0013h) 的内容，即拨码设定的站号。
 *
 * 背景:
 *   EtherCAT 标准里普通主站靠读每个从站的 ESC 寄存器 0x0012-0x0013
 *   (Configured Station Alias) 来识别"拨码/旋码站号"。例如研控 YKD2205PE
 *   的 SW1~6 就把站号 (0~63) 写进 0012h-0013h 供主站使用。
 *
 *   本仓库 python 框架 (pysoem) 封装的旧版 SOEM 不暴露该 ESC 寄存器，
 *   因此拨码站号对 pysoem 不可见；本工具用原生 SOEM 直接读。SOEM 在
 *   ecx_config_init() 中已对每台从站 FPRD 0x0012，结果存于
 *   ctx.slavelist[i].aliasadr，这里只把它打印出来即可 (无需进 OP，只读)。
 *
 *   注意: 别名 = 0 通常表示"未设置站号/拨码=0"，此时无法靠别名区分多台
 *   同型号驱动器，仍需按总线线序 (位置) 识别。
 *
 * 用法:
 *   aliasinfo [ifname]
 *     ifname   网卡名。Windows Npcap 形如  \Device\NPF_{GUID} ，
 *              例如: aliasinfo '\Device\NPF_{7C64E0FA-D69A-4C92-A821-E5D341E63575}'
 *              不带 ifname 时仅列出可用网卡。
 *
 * 位置/构建:
 *   本文件在本仓库自有顶层工程 (aliasinfo/)，引用根目录下载的 SOEM 库
 *   (add_subdirectory(SOEM))，不改动 SOEM 源码。
 *     cmake -S . -B build                # 仓库根 (含顶层 CMakeLists)
 *     cmake --build build --config Release --target aliasinfo
 *   产物: build/aliasinfo/Release/aliasinfo.exe
 *   MinGW/Ninja 示例:
 *     cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
 *           -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
 *     cmake --build build-mingw --target aliasinfo
 */

#include <stdio.h>

#include "soem/soem.h"

static ecx_contextt ctx;

static void list_adapters(void)
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

int main(int argc, char *argv[])
{
   int slave, cnt;

   printf("aliasinfo - 读取 ESC 站点别名寄存器 0012h-0013h (拨码站号)\n");

   if (argc < 2)
   {
      printf("用法: aliasinfo ifname\nifname = 网卡名，如 \\Device\\NPF_{GUID}\n\n可用网卡:\n");
      list_adapters();
      return 1;
   }

   if (!ecx_init(&ctx, argv[1]))
   {
      printf("无法打开网卡 %s\n", argv[1]);
      return 1;
   }

   cnt = ecx_config_init(&ctx);
   if (cnt <= 0)
   {
      printf("总线上未发现从站 (config_init=%d)，检查网卡/网线/供电。\n", cnt);
      ecx_close(&ctx);
      return 1;
   }

   printf("%d 台从站。位置为 0-based 总线线序 (主站起数)。\n", cnt);
   printf("%5s  %5s  %6s  %14s  %10s  %10s  %s\n",
          "位置", "序号", "配置地址", "别名0012h", "厂商ID", "产品码", "名称");
   printf("%5s  %5s  %6s  %14s  %10s  %10s  %s\n",
          "-----", "-----", "------", "------------", "----------", "----------", "----");

   for (slave = 1; slave <= cnt; slave++)
   {
      const struct ec_slave *s = &ctx.slavelist[slave];
      printf("%5d  %5d  0x%04X  %5u (0x%04X)  0x%08X  0x%08X  %s\n",
             slave - 1,                /* 总线位置 0-based */
             slave,                    /* SOEM 从站序号 1-based */
             (unsigned)s->configadr,
             (unsigned)s->aliasadr, (unsigned)s->aliasadr, /* 0012h-0013h 别名寄存器 */
             (unsigned)s->eep_man,
             (unsigned)s->eep_id,
             s->name);
   }

   printf("\n说明: '别名0012h' 即 ESC 寄存器 0012h-0013h (Configured Station Alias)。\n");
   for (slave = 1; slave <= cnt; slave++)
   {
      if (ctx.slavelist[slave].aliasadr == 0)
      {
         printf("  位置 %d 别名=0 (未设拨码站号或拨码=0)，同型号多台时请按位置区分。\n",
                slave - 1);
      }
   }

   ecx_close(&ctx);
   return 0;
}
