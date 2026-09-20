/*
 * aliasinfo - 打印每个从站 ESC 寄存器 0x0012h-0x0013h (Configured Station Alias) 拨码站号。
 * 值取自已由 ecx_config_init() 填好的 ctx.slavelist[i].aliasadr，只读，不进 OP。
 * 别名=0 表示未设拨码站号，多台同型号时须按总线线序区分。
 * 用法: aliasinfo [ifname]  (Windows Npcap 网卡名形如 \Device\NPF_{GUID}；不带则仅列出网卡)
 * 构建: cmake --build build --config Release --target aliasinfo
 *       -> build/aliasinfo/Release/aliasinfo.exe
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
             (unsigned)s->aliasadr, (unsigned)s->aliasadr, /* 别名寄存器 0012h-0013h */
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
