/*
 * ec_motor_motion.c - 运动层: "写什么、按什么顺序写、写完等哪个位" (使能状态机 /
 * CSP 多轴轨迹 / PV 速度 / 回零); "怎么发出去"是 ec_motor.c 的 SDO 与过程数据层。
 * include 顺序同样是硬的: struct em_bus 含完整的 ecx_contextt, soem.h 必须先于内部头。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "soem/soem.h"

#include "ec_motor_internal.h"

/* 6041h 低位四位的状态掩码, 只用手册定义的 bit0/1/2/3。这四位不是标准 CiA402 编码 (标准是
 * 0x40/0x21/0x23/0x27/0x31...), 该驱动器手册走自己的 8 态迁移表, 所以全程按位判, 不用 bit6。
 * 真机实测: 刚上电 0x0210 (低四位 0000), 写 6040h=0x0000 后 0x0231 (低四位 0001 = Ready to switch on) */
#define EM_SW_MASK_STATE \
   (EM_SW_RTSO | EM_SW_SWITCHED | EM_SW_OP_ENABLED | EM_SW_FAULT)

/* 取该轴当前的实际位置: 优先用最新一帧过程数据, 镜像不可信时走 SDO 兜底; 绝不拿 0 当"当前位置" */
static int em__cur_pos(em_axis_t *ax, int32_t *out)
{
   if (ax == NULL || out == NULL)
      return EM_R_FAIL;

   if (ax->mirror_ok)
   {
      *out = ax->pos;
      return EM_R_OK;
   }
   em__log(ax->bus, "%s: 过程数据镜像还不可信, 当前实际位置改走 SDO 读 (慢, 但是真的)",
           ax->label);
   return em_pos_sdo(ax, out);
}

int em_arm(em_axis_t *ax)
{
   int     mode;
   int32_t cur;

   if (ax == NULL)
      return EM_R_FAIL;

   if (em_is_enabled(ax))
   {
      em__err("%s: 已使能 (6041h=0x%04X), 拒绝改目标预定值 —— 要防的正是使能那一刻, "
              "现在过去了", ax->label, (unsigned)ax->sw);
      return EM_R_FAIL;
   }

   mode = em_get_mode(ax);
   if (mode < 0)
   {
      em__err("%s: 读 6061h (实际运行模式) 失败, 不知道要钉哪个目标值 -> 不猜",
              ax->label);
      return EM_R_FAIL;
   }

   /* 6060h 在生效 RxPDO 里时, 把镜像那一字节对齐到驱动器实际认的模式 (6061h):
    * 使能那一帧若不一致, 驱动器会在刚钉好目标值之后换一种解释 607Ah 的方式 */
   if (ax->off_modes >= 0)
      em__put_u8(ax->out, ax->off_modes, (uint8_t)mode);

   /* 6083h / 6084h 也重钉一次: 在生效 RxPDO 里不写就是下发 0, 而 6083h = 0 会让 PV 的斜坡起不来 */
   em__pin_ramp(ax);

   switch (mode)
   {
      case EM_MODE_CSP:
         if (!em_csp_available(ax))
         {
            em__err("%s: 6061h 说当前是 CSP, 但 607Ah 或 6064h 不在本轴实读的映射里 "
                    "-> 钉不住目标位置, 拒绝", ax->label);
            return EM_R_FAIL;
         }
         if (em__cur_pos(ax, &cur) != EM_R_OK)
         {
            em__err("%s: 读不到当前实际位置 -> 无法把 CSP 目标钉在当前位置, 拒绝使能",
                    ax->label);
            return EM_R_FAIL;
         }
         em__put_i32(ax->out, ax->off_target_pos, cur);
         ax->csp_target = cur;
         printf("  %s: CSP 目标位置已钉在当前位置 %d pul (使能那一帧就是「原地不动」)\n",
                ax->label, cur);
         return EM_R_OK;

      case EM_MODE_PV:
         if (!em_pv_available(ax))
         {
            em__err("%s: 6061h 说当前是 PV, 但 60FFh 不在本轴实读的映射里 "
                    "-> 钉不住目标速度, 拒绝", ax->label);
            return EM_R_FAIL;
         }
         em__put_i32(ax->out, ax->off_target_vel, 0);
         printf("  %s: PV 目标速度已钉在 0 (使能那一帧不会转起来)\n", ax->label);
         return EM_R_OK;

      case EM_MODE_HM:
         /* 回零靠 6040h bit4 的上升沿启动, 没有"每周期跟的目标值"要先钉住 */
         em__log(ax->bus, "%s: 模式 HM (回零), 没有目标值需要在使能前钉住", ax->label);
         return EM_R_OK;

      case EM_MODE_PP:
         em__log(ax->bus, "%s: 模式 PP (轮廓位置) —— 本接口不驱动这个模式 "
                 "(那是 slide_motion 的活), 不写任何目标值", ax->label);
         return EM_R_OK;

      default:
         em__log(ax->bus, "%s: 模式 %d 不在本接口实现的范围内 (CSP/PV/HM/PP), "
                 "不写任何目标值", ax->label, mode);
         return EM_R_OK;
   }
}

int em_enable(em_axis_t *ax)
{
   int rc;

   if (ax == NULL)
      return EM_R_FAIL;

   if (ax->off_cw < 0)
   {
      em__err("%s: 6040h 不在 RxPDO 映射里, 推不动控制字", ax->label);
      return EM_R_FAIL;
   }
   if (!ax->mirror_ok)
   {
      em__err("%s: 一笔完整的 6041h 都没取到, 状态未知 -> 拒绝使能", ax->label);
      return EM_R_FAIL;
   }
   if ((ax->sw & EM_SW_FAULT) != 0)
   {
      em__err("%s: 6041h bit3 = Fault (0x%04X) -> 先 em_fault_reset(), 不要直接使能",
              ax->label, (unsigned)ax->sw);
      return EM_R_FAIL;
   }
   if (!em_sw_remote_ok(ax->sw))
   {
      /* bit9 = Remote。为 0 表示控制字不可操作, 整套 6040h 状态机无效 —— 推下去没反应, 只会白等超时 */
      em__err("%s: 6041h bit9 = 0 (Remote 未就绪), 控制字不可操作 -> 拒绝使能 "
              "(0x%04X)", ax->label, (unsigned)ax->sw);
      return EM_R_FAIL;
   }

   printf("  ---- %s 使能 ----\n", ax->label);

   /* 使能之前先把目标值钉在"原地不动"上 —— 理由见 em_arm() 的说明 */
   rc = em_arm(ax);
   if (rc != EM_R_OK)
      return rc;

   /* 上行阶梯 (真机跑通过的顺序与期望值): 0x0000 -> 低四位 0x1 (RTSO) 先归位;
    * 0x0006 -> 0x1 Shutdown; 0x0007 -> 0x3 (RTSO|SW) Switch on; 0x000F -> 0x7 (电机带电) */
   rc = em__cw_step(ax, "0. 归位 Disable voltage", EM_CW_DISABLE_V,
                    EM_SW_MASK_STATE, EM_SW_RTSO, EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
      return rc;

   rc = em__cw_step(ax, "1. Shutdown", EM_CW_SHUTDOWN,
                    EM_SW_MASK_STATE, EM_SW_RTSO, EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
      return rc;

   rc = em__cw_step(ax, "2. Switch on", EM_CW_SWITCHON,
                    EM_SW_MASK_STATE, EM_SW_RTSO | EM_SW_SWITCHED, EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
      return rc;

   rc = em__cw_step(ax, "3. Enable Operation", EM_CW_ENABLE_OP, EM_SW_MASK_STATE,
                    EM_SW_RTSO | EM_SW_SWITCHED | EM_SW_OP_ENABLED, EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
   {
      em__err("     控制字经 RxPDO 写进去了但状态字没跟上。看上面那行 6040h 回读: "
              "一致 = 问题在驱动器侧; 不一致 = 问题在主站侧 (偏移 / OP / WKC)");
      return rc;
   }

   printf("      >>> %s 已通电 (有保持力矩, 但未移动)\n", ax->label);
   return EM_R_OK;
}

int em_disable(em_axis_t *ax)
{
   int rc;

   if (ax == NULL)
      return EM_R_FAIL;
   if (ax->off_cw < 0)
   {
      em__err("%s: 6040h 不在 RxPDO 映射里, 推不动控制字", ax->label);
      return EM_R_FAIL;
   }

   printf("  ---- %s 失能 ----\n", ax->label);

   /* 下行逐级退回去。只测上行的话"能不能干净地停"恰恰没测到, 那在真机上比上行重要 */
   if (em_is_enabled(ax))
   {
      rc = em__cw_step(ax, "Disable operation", EM_CW_SWITCHON, EM_SW_MASK_STATE,
                       EM_SW_RTSO | EM_SW_SWITCHED, EM_STEP_TMO_MS);
      if (rc != EM_R_OK)
         return rc;

      rc = em__cw_step(ax, "Shutdown", EM_CW_SHUTDOWN, EM_SW_MASK_STATE,
                       EM_SW_RTSO, EM_STEP_TMO_MS);
      if (rc != EM_R_OK)
         return rc;
   }

   /* 最后一步的判据用 bit2 本身, 不用 (sw & 0x000F) == 0: 驱动器自检完成后合法地停在低四位
    * = 0001 (Ready to switch on), 要求读到 0000 必然失败; 掩码带 bit3 = "带故障的未使能"不算成功 */
   return em__cw_step(ax, "Disable voltage", EM_CW_DISABLE_V,
                      EM_SW_OP_ENABLED | EM_SW_FAULT, 0, EM_STEP_TMO_MS);
}

int em_fault_reset(em_axis_t *ax)
{
   uint32_t t0;
   int      k;
   int      was_fault, cleared = 0;

   if (ax == NULL)
      return EM_R_FAIL;
   if (ax->off_cw < 0)
   {
      em__err("%s: 6040h 不在 RxPDO 映射里, 推不动控制字", ax->label);
      return EM_R_FAIL;
   }
   if (!ax->mirror_ok)
   {
      em__err("%s: 一笔完整的 6041h 都没取到 -> 不知道有没有故障, 拒绝复位",
              ax->label);
      return EM_R_FAIL;
   }

   was_fault = (ax->sw & EM_SW_FAULT) != 0;
   printf("  ---- %s 故障复位 (6040h bit7 上升沿) ----\n", ax->label);

   /* bit7 是上升沿触发: 它本来就是 1 的话, 直接写 0x0080 不构成上升沿, 什么也不会发生
    * (而且会静默地"成功")。所以先把 bit7 压到 0, 打几帧确认发出去了再抬起来 */
   em__set_cw(ax, EM_CW_DISABLE_V);
   for (k = 0; k < 10; k++)
   {
      (void)em__cycle(ax->bus);
      em__sleep_ms(EM_POLL_MS);
   }

   em__set_cw(ax, EM_CW_FAULT_RST);
   printf("  写 RxPDO[6040h]=0x%04X ...", (unsigned)EM_CW_FAULT_RST);
   fflush(stdout);

   t0 = em__now_ms();
   while ((int32_t)(em__now_ms() - t0) < (int32_t)EM_STEP_TMO_MS)
   {
      if (em_stop_requested())
      {
         printf(" [中止]\n");
         em__set_cw(ax, EM_CW_DISABLE_V);
         return EM_R_STOP;
      }
      (void)em__cycle(ax->bus);
      if (ax->mirror_ok && (ax->sw & EM_SW_FAULT) == 0)
      {
         cleared = 1;
         break;
      }
      em__sleep_ms(EM_POLL_MS);
   }

   /* 收手: bit7 不能一直举着 */
   em__set_cw(ax, EM_CW_DISABLE_V);

   if (!cleared)
   {
      printf(" [FAIL]\n");
      em__err("%s: 写了 bit7 但 6041h bit3 仍是 Fault (%s)。故障原因要先排除 —— "
              "读 603Fh 看故障码, 不要靠反复复位硬顶", ax->label,
              em_sw_describe(ax->sw));
      return EM_R_FAIL;
   }

   if (!was_fault)
   {
      /* 本来就没故障。上面那个循环看到 bit3 = 0 就"成功"了, 但那不代表复位动作起了作用 ——
       * 如实说清楚, 不让调用者以为清掉了一个不存在的故障 */
      printf(" [PASS] 但本来就没有故障 (6041h bit3 未曾置位) —— 这次复位是空操作\n");
      return EM_R_OK;
   }

   printf(" [PASS] 故障已清, 现在 %s\n", em_sw_describe(ax->sw));
   return EM_R_OK;
}

int em_enable_all(em_bus_t *bus)
{
   int i, j, rc;

   if (bus == NULL || bus->naxis < 1)
      return EM_R_FAIL;

   for (i = 0; i < bus->naxis; i++)
   {
      rc = em_enable(bus->axis[i]);
      if (rc != EM_R_OK)
      {
         /* 一根轴没使能上就把已经使能的退回去 —— 停在"一半带电一半不带电"上最难收拾 */
         em__err("%s 使能失败 -> 把已使能的轴退回去", bus->axis[i]->label);
         for (j = 0; j < i; j++)
         {
            if (em_is_enabled(bus->axis[j]))
               (void)em_disable(bus->axis[j]);
         }
         return rc;
      }
   }
   return EM_R_OK;
}

int em_disable_all(em_bus_t *bus)
{
   int i, bad = 0;

   if (bus == NULL)
      return EM_R_FAIL;

   for (i = 0; i < bus->naxis; i++)
   {
      /* 一根轴失能失败不妨碍继续退其它的: 目标是"尽可能多地把电撤掉", 哪根没退掉由 em_disable 打印 */
      if (em_disable(bus->axis[i]) != EM_R_OK)
         bad = 1;
   }
   return bad ? EM_R_FAIL : EM_R_OK;
}

int em_csp_move_multi(em_axis_t **axes, const int32_t *target, const uint32_t *vel,
                      int n, uint32_t tmo_ms)
{
   int32_t   cur[EM_MAX_AXES];
   int32_t   end[EM_MAX_AXES];
   uint32_t  v[EM_MAX_AXES];
   int       done[EM_MAX_AXES];
   em_bus_t *bus;
   uint32_t  t0, last;
   int       i, j, left;

   if (axes == NULL || target == NULL || vel == NULL || n < 1 || n > EM_MAX_AXES)
   {
      em__err("em_csp_move_multi: 轴数必须在 1..%d 之间", EM_MAX_AXES);
      return EM_R_FAIL;
   }
   if (axes[0] == NULL)
      return EM_R_FAIL;
   bus = axes[0]->bus;

   for (i = 0; i < n; i++)
   {
      em_axis_t *ax = axes[i];
      int64_t    d;

      if (ax == NULL)
      {
         em__err("em_csp_move_multi: 第 %d 根轴是空指针", i);
         return EM_R_FAIL;
      }
      if (ax->bus != bus)
      {
         em__err("%s: 与第 0 根轴不在同一条总线上 —— 多轴运动必须同一条总线 "
                 "(一帧喂所有轴)", ax->label);
         return EM_R_FAIL;
      }
      for (j = 0; j < i; j++)
      {
         if (axes[j] == ax)
         {
            /* 同一根轴出现两次: 后一次会直接覆盖前一次的 607Ah, 于是"走到了两个不同的终点"
             * 谁也不知道, 两条命令都像成功 */
            em__err("%s: 在同一批里出现了两次 -> 拒绝 (后一次会静默覆盖前一次)",
                    ax->label);
            return EM_R_FAIL;
         }
      }

      if (!em_csp_available(ax))
      {
         em__err("%s: 607Ah (目标位置) 或 6064h (实际位置) 不在本轴实读的映射里 "
                 "-> CSP 做不了", ax->label);
         return EM_R_FAIL;
      }
      if (em__check_motion_ready(ax, "CSP") != EM_R_OK)
         return EM_R_FAIL;
      if (em_get_mode(ax) != EM_MODE_CSP)
      {
         em__err("%s: 6061h 报当前模式 %d, 不是 CSP(8)。先 em_disable() 再 "
                 "em_set_mode(ax, EM_MODE_CSP)", ax->label, em_get_mode(ax));
         return EM_R_FAIL;
      }
      if (em__cur_pos(ax, &cur[i]) != EM_R_OK)
      {
         em__err("%s: 读不到当前实际位置 -> 起点未知, 拒绝运动", ax->label);
         return EM_R_FAIL;
      }

      end[i] = target[i];
      v[i]   = vel[i];

      if (v[i] == 0)
      {
         /* 速度为 0 就是不动: 走"运动"这条路会得到一个必然超时的等待, 期间电机还带电 */
         em__err("%s: 速度是 0 -> 拒绝 (速度 0 是「不动」, 不是「尽快」)", ax->label);
         return EM_R_FAIL;
      }

      /* 位移上限: 放宽它是调用方**有意**的动作, 所以没有"传 0 表示不限"这种写法 */
      d = (int64_t)end[i] - (int64_t)cur[i];
      if (d < 0)
         d = -d;
      if ((uint64_t)d > (uint64_t)ax->move_limit)
      {
         em__err("%s: 位移 %lld pul 超过本轴上限 %u pul -> 拒绝 "
                 "(放宽要用 em_axis_set_move_limit, 且应当是有意的)",
                 ax->label, (long long)d, (unsigned)ax->move_limit);
         return EM_R_FAIL;
      }

      done[i] = (cur[i] == end[i]);
   }

   printf("  ---- CSP 多轴运动 (%d 轴, 同周期下发) ----\n", n);
   for (i = 0; i < n; i++)
   {
      printf("    %s: %d -> %d pul (位移 %lld), 速度 %u pul/s, 容差 %d pul%s\n",
             axes[i]->label, cur[i], end[i],
             (long long)((int64_t)end[i] - (int64_t)cur[i]),
             (unsigned)v[i], (int)axes[i]->pos_tol,
             done[i] ? "  [已在终点]" : "");
   }

   t0   = em__now_ms();
   last = t0;

   for (;;)
   {
      uint32_t now, dt;

      if (em_stop_requested())
      {
         /* 收到停止请求: 把目标冻结在当前插值点上, 再打几帧让"停在这"确实发出去。
          * 不写 0x0000 —— 那会让电机瞬间卸力, 垂直轴会自由下落; 要不要撤电由调用方决定 */
         printf("  [中止] 目标冻结在当前插值点, 保持使能\n");
         for (i = 0; i < n; i++)
         {
            em__set_cw(axes[i], EM_CW_ENABLE_OP);
            em__put_i32(axes[i]->out, axes[i]->off_target_pos, cur[i]);
            em__put_i32(axes[i]->out, axes[i]->off_target_vel, 0);
         }
         for (j = 0; j < 20; j++)
         {
            (void)em__cycle(bus);
            em__sleep_ms(EM_POLL_MS);
         }
         return EM_R_STOP;
      }

      now  = em__now_ms();
      dt   = now - last;
      last = now;

      /* 用实测 dt 推进, 不用假设的周期: Windows 不是实时系统, 假设 2ms 而实际卡了 30ms
       * 会让插值目标落后于真实时间 —— CSP 下驱动器跟着目标走, 落后就是实际速度比命令值低 */
      if (dt == 0)
         dt = 1;
      if (dt > 100)
      {
         /* 卡了一下。一次最多按 100ms 推进: 让插值慢慢补回来, 好过让目标一次跳一大步
          * (CSP 下跳一大步就是一次高速冲刺, 而卡顿本身往往意味着总线状态不稳) */
         em__log(bus, "过程数据间隔 %ums > 100ms, 本次按 100ms 推进 (不让目标跳步)", dt);
         dt = 100;
      }

      left = 0;
      for (i = 0; i < n; i++)
      {
         em_axis_t *ax = axes[i];

         if (!done[i])
         {
            int64_t step = (int64_t)v[i] * (int64_t)dt / 1000;

            if (step <= 0)
               step = 1;    /* dt 很小时也得往前挪, 否则永远到不了 */

            if (cur[i] < end[i])
               cur[i] = (cur[i] + step > end[i]) ? end[i] : (int32_t)(cur[i] + step);
            else
               cur[i] = (cur[i] - step < end[i]) ? end[i] : (int32_t)(cur[i] - step);

            if (cur[i] == end[i])
               done[i] = 1;
         }

         /* 每个周期都要重发控制字: 6040h 是过程数据, 停发或发成别的值下一周期就不再 Operation enabled */
         em__set_cw(ax, EM_CW_ENABLE_OP);
         em__put_i32(ax->out, ax->off_target_pos, cur[i]);
         ax->csp_target = cur[i];

         if (!done[i])
            left++;
      }

      /* >>> 一帧喂所有轴 —— "同时调用两个驱动器"就发生在这一行 <<< */
      (void)em__cycle(bus);

      if (left == 0)
      {
         int all_in = 1;

         /* 插值目标到了终点还不够: 目标是发出去的, 位置是驱动器报回来的, 到位要两个都成立 */
         for (i = 0; i < n; i++)
         {
            if (!axes[i]->mirror_ok)
            {
               all_in = 0;
               break;
            }
            {
               int64_t err = (int64_t)axes[i]->pos - (int64_t)end[i];

               if (err < 0)
                  err = -err;
               if (err > (int64_t)axes[i]->pos_tol)
               {
                  all_in = 0;
                  break;
               }
            }
         }
         if (all_in)
         {
            printf("  [PASS] %d 轴全部到位 (耗时 %ums)\n",
                   n, (unsigned)(em__now_ms() - t0));
            for (i = 0; i < n; i++)
               printf("    %s: 命令 %d, 实测 6064h = %d, 差 %lld pul\n",
                      axes[i]->label, end[i], axes[i]->pos,
                      (long long)((int64_t)axes[i]->pos - (int64_t)end[i]));
            return EM_R_OK;
         }
      }

      if ((int32_t)(em__now_ms() - t0) >= (int32_t)tmo_ms)
      {
         printf("  [FAIL] %ums 内未全部到位\n", (unsigned)tmo_ms);
         for (i = 0; i < n; i++)
         {
            em_axis_t *ax = axes[i];

            printf("    %s: 命令 %d, 插值 %d%s, ", ax->label, end[i], cur[i],
                   done[i] ? "(已到终点)" : "(仍在推进)");
            if (ax->mirror_ok)
               printf("实测 6064h = %d, 差 %lld pul, 6041h=%s\n", ax->pos,
                      (long long)((int64_t)ax->pos - (int64_t)end[i]),
                      em_sw_describe(ax->sw));
            else
               printf("6041h 镜像不可信 (一笔完整帧都没收到)\n");
         }
         for (i = 0; i < n; i++)
         {
            /* 「信号有效」, 不是「撞到限位了」: 手册对这一位的定义就是
             * 硬件限位信号有效时置 1 —— 它是电平不是闩锁, 回零时本来就该置起 */
            if ((axes[i]->sw & EM_SW_INTLIMIT) != 0)
               printf("    >>> %s 6041h bit11 硬件限位信号有效 (未必真压着 —— "
                      "极性配反会让它一直亮着)\n", axes[i]->label);
         }
         return EM_R_FAIL;
      }

      em__sleep_ms(EM_POLL_MS);
   }
}

int em_csp_set_target(em_axis_t *ax, int32_t target)
{
   if (ax == NULL || ax->out == NULL)
      return EM_R_FAIL;
   if (ax->off_target_pos < 0)
   {
      /* 607Ah 不在这一轴实读的映射里。不退回 SDO: 这条路径是每周期调的, 一次 SDO 往返
       * 700ms, 且 OP 下的 SDO 往返本身就可能把驱动器踢出 OP。写不进去就直说写不进去 */
      em__err("%s: 607Ah 不在本轴实读的映射里 -> 目标位置下发不出去", ax->label);
      return EM_R_FAIL;
   }

   /* 只写镜像。使能状态与 6061h==CSP 由调用方保证 —— 这是每周期路径, 上面不能有 SDO */
   em__put_i32(ax->out, ax->off_target_pos, target);
   return EM_R_OK;
}

int em_csp_move_abs(em_axis_t *ax, int32_t target, uint32_t vel, uint32_t tmo_ms)
{
   em_axis_t *one[1];
   int32_t    t[1];
   uint32_t   v[1];

   if (ax == NULL)
      return EM_R_FAIL;

   one[0] = ax;
   t[0]   = target;
   v[0]   = vel;
   return em_csp_move_multi(one, t, v, 1, tmo_ms);
}

int em_csp_move_rel(em_axis_t *ax, int32_t delta, uint32_t vel, uint32_t tmo_ms)
{
   em_axis_t *one[1];
   int32_t    d[1];
   uint32_t   v[1];

   if (ax == NULL)
      return EM_R_FAIL;

   one[0] = ax;
   d[0]   = delta;
   v[0]   = vel;
   return em_csp_move_rel_multi(one, d, v, 1, tmo_ms);
}

int em_csp_move_rel_multi(em_axis_t **axes, const int32_t *delta,
                          const uint32_t *vel, int n, uint32_t tmo_ms)
{
   int32_t tgt[EM_MAX_AXES];
   int32_t cur[EM_MAX_AXES];
   int     i;

   if (axes == NULL || delta == NULL || vel == NULL || n <= 0 || n > EM_MAX_AXES)
      return EM_R_FAIL;

   /* 第一步: 把所有轴的起点取完, 一个字节都还没下发 —— 取一根下发一根的话各轴起点就落在不同帧上 */
   for (i = 0; i < n; i++)
   {
      if (axes[i] == NULL)
         return EM_R_FAIL;
      if (em__cur_pos(axes[i], &cur[i]) != EM_R_OK)
      {
         em__err("%s: 读不到当前实际位置 -> 相对运动没有起点, 拒绝 "
                 "(一个轴取不到就不下发任何轴)",
                 axes[i]->label);
         return EM_R_FAIL;
      }
   }

   /* 第二步: 逐轴算绝对目标。越 32 位就整体拒绝 —— 不静默回绕。 */
   for (i = 0; i < n; i++)
   {
      int64_t t = (int64_t)cur[i] + (int64_t)delta[i];

      if (t < -2147483647LL || t > 2147483647LL)
      {
         em__err("%s: 当前位置 %d 加位移 %d 超出 32 位范围 -> 拒绝 (不静默回绕)",
                 axes[i]->label, cur[i], delta[i]);
         return EM_R_FAIL;
      }
      tgt[i] = (int32_t)t;
   }

   /* 把基准显式打出来: 要确认的就是"607Ah 相对于开始这一刻的实际位置" */
   printf("  相对运动的起点 (调用这一刻各轴的实际位置):\n");
   for (i = 0; i < n; i++)
      printf("    %s: %d -> %d pul (位移 %d)\n", axes[i]->label, cur[i], tgt[i],
             delta[i]);

   /* 第三步: 一次下发 —— 所有轴共用同一个基准时刻、同一个周期帧 */
   return em_csp_move_multi(axes, tgt, vel, n, tmo_ms);
}

/* PV —— 速度模式。这台驱动器没有 CSV (6060h=9): 手册 6502h = 0x00A5 = PP + PV + HM + CSP,
 * 所以"速度模式"只有 PV 一种 */

int em_pv_stop(em_axis_t *ax)
{
   if (ax == NULL)
      return EM_R_FAIL;
   if (!em_pv_available(ax))
   {
      em__err("%s: 60FFh (目标速度) 不在本轴实读的映射里 -> PV 做不了", ax->label);
      return EM_R_FAIL;
   }

   em__set_cw(ax, EM_CW_ENABLE_OP);
   em__put_i32(ax->out, ax->off_target_vel, 0);

   /* 写 0 之后要看驱动器真的停了: 6041h bit12 在 PV 下是 Speed (1 = 速度为 0)。
    * "我发了 0"和"它停了"是两件事 */
   return em__wait_sw(ax, EM_SW_PV_SPEED_ZERO, EM_SW_PV_SPEED_ZERO,
                      EM_STEP_TMO_MS, "PV 速度归零 (6041h bit12)");
}

/* 把所有轴的速度写 0 并打若干帧 —— 出事时的统一收尾; 不在这里断言 bit12, 一根没停住不该拖住其余轴 */
static void em__pv_stop_all(em_axis_t **axes, int n)
{
   int i, k;

   for (i = 0; i < n; i++)
   {
      if (axes[i] != NULL && axes[i]->off_target_vel >= 0)
      {
         em__set_cw(axes[i], EM_CW_ENABLE_OP);
         em__put_i32(axes[i]->out, axes[i]->off_target_vel, 0);
      }
   }
   for (k = 0; k < 20; k++)
   {
      if (axes[0] != NULL)
         (void)em__cycle(axes[0]->bus);
      em__sleep_ms(EM_POLL_MS);
   }
}

int em_pv_run_multi(em_axis_t **axes, const int32_t *vel, int n, uint32_t hold_ms)
{
   uint32_t t0;
   int      reads[EM_MAX_AXES];
   int32_t  pos0[EM_MAX_AXES];   /* 起跑时的实际位置, 用来判"到底转没转" */
   int32_t  pos1[EM_MAX_AXES];
   int32_t  vpeak[EM_MAX_AXES];  /* 运行期间 606Ch 的峰值 */
   int      i;

   if (axes == NULL || vel == NULL || n <= 0 || n > EM_MAX_AXES)
      return EM_R_FAIL;

   /* ---- 全部校验, 一根都不动 ---- 多轴下"跑到一半才发现第 2 根不行"意味着一根在动一根
    * 没动, 所以全部查完才写第一个字节 */
   for (i = 0; i < n; i++)
   {
      em_axis_t *ax = axes[i];

      if (ax == NULL)
         return EM_R_FAIL;
      if (!em_pv_available(ax))
      {
         em__err("%s: 60FFh (目标速度) 不在本轴实读的映射里 -> PV 做不了。"
                 "看 setup 时那条 [WARN] 与 em_dump_pdo 的输出", ax->label);
         return EM_R_FAIL;
      }
      if (vel[i] == 0)
      {
         em__err("%s: 目标速度是 0 -> 拒绝 (速度 0 是「不动」, 不是「尽快」)",
                 ax->label);
         return EM_R_FAIL;
      }
      if (em__check_motion_ready(ax, "PV") != EM_R_OK)
         return EM_R_FAIL;
      if (em_get_mode(ax) != EM_MODE_PV)
      {
         em__err("%s: 6061h 报当前模式 %d, 不是 PV(3)。先 em_disable() 再 "
                 "em_set_mode(ax, EM_MODE_PV)", ax->label, em_get_mode(ax));
         return EM_R_FAIL;
      }
      /* 取起跑位置 —— "转没转"唯一的客观依据 (6064h), 取不到就整体拒绝。光断言 6041h bit12
       * 不够: 那位说的是"收下了速度指令"不是"电机动了" (真机: bit12 清零而 6064h 一个计数不动) */
      if (em__cur_pos(ax, &pos0[i]) != EM_R_OK)
      {
         em__err("%s: 取不到起跑时的实际位置 (6064h) -> 无法判断这趟到底转没转, "
                 "拒绝下发速度", ax->label);
         return EM_R_FAIL;
      }
      reads[i] = 0;
      vpeak[i] = 0;
   }

   printf("  ---- PV: %d 根轴各跑自己的速度, 同周期下发, 跑 %ums ----\n",
          n, (unsigned)hold_ms);
   for (i = 0; i < n; i++)
      printf("       %s: 目标速度 %d pul/s\n", axes[i]->label, vel[i]);

   t0 = em__now_ms();
   while ((int32_t)(em__now_ms() - t0) < (int32_t)hold_ms)
   {
      if (em_stop_requested())
      {
         printf("  [中止] 停止请求 -> 所有轴速度写 0\n");
         em__pv_stop_all(axes, n);
         return EM_R_STOP;
      }

      /* 6040h 与 60FFh 都是过程数据, 每周期都得重发 */
      for (i = 0; i < n; i++)
      {
         em__set_cw(axes[i], EM_CW_ENABLE_OP);
         em__put_i32(axes[i]->out, axes[i]->off_target_vel, vel[i]);
      }

      (void)em__cycle(axes[0]->bus);

      /* 用同一帧的镜像逐轴检查: PV 期间出故障或撞限位, 驱动器会自己停, 主站若只顾跑满
       * hold_ms 就会把"它已经停了"当成"跑完了" —— 那等于把一次异常说成正常 */
      for (i = 0; i < n; i++)
      {
         em_axis_t *ax = axes[i];

         if (!ax->mirror_ok)
            continue;
         reads[i]++;

         /* 606Ch 的峰值 —— "确实在转"的另一个客观证据 (取绝对值, 反向跑也算) */
         {
            int32_t av = ax->vel < 0 ? -ax->vel : ax->vel;

            if (av > vpeak[i])
               vpeak[i] = av;
         }

         if ((ax->sw & EM_SW_FAULT) != 0)
         {
            printf("  [FAIL] %s: 运行中 6041h bit3 = Fault (%s)\n",
                   ax->label, em_sw_describe(ax->sw));
            em__pv_stop_all(axes, n);
            return EM_R_FAIL;
         }
         if ((ax->sw & EM_SW_OP_ENABLED) == 0)
         {
            printf("  [FAIL] %s: 运行中掉出 Operation enabled (%s)\n",
                   ax->label, em_sw_describe(ax->sw));
            em__pv_stop_all(axes, n);
            return EM_R_FAIL;
         }
         if ((ax->sw & EM_SW_INTLIMIT) != 0)
         {
            printf("  [FAIL] %s: 运行中 6041h bit11 硬件限位信号有效, "
                   "所有轴速度写 0\n", ax->label);
            em__pv_stop_all(axes, n);
            return EM_R_FAIL;
         }
      }
      em__sleep_ms(EM_POLL_MS);
   }

   /* 逐轴判"这趟读数到底有没有发生过": 一笔 6041h 都没取到, 上面那些"运行中检查"就是一次都没做过 */
   for (i = 0; i < n; i++)
   {
      if (reads[i] == 0)
      {
         em__err("%s: 运行期间一笔完整的 6041h 都没取到, 那几条运行中检查一次都没生效 "
                 "-> 拒绝把这次当成成功", axes[i]->label);
         em__pv_stop_all(axes, n);
         return EM_R_FAIL;
      }
   }

   for (i = 0; i < n; i++)
      printf("  %s: 跑满 %ums (期间 %d 次读数, 6041h=%s)\n", axes[i]->label,
             (unsigned)hold_ms, reads[i], em_sw_describe(axes[i]->sw));

   /* 判"到底转没转": 6041h 正常、退出码 0 而 6064h 一个计数没动是出现过的 (上述检查都在问
    * "驱动器收下指令了吗"); 位置不会骗人, 走没走 6064h 说了算 (606Ch 是驱动器按自己斜坡算
    * 的瞬时值, 有些驱动器直接回报指令值) */
   for (i = 0; i < n; i++)
   {
      int32_t moved;

      if (em__cur_pos(axes[i], &pos1[i]) != EM_R_OK)
      {
         em__err("%s: 取不到停后的实际位置 (6064h) -> 无法判断这趟到底转没转, "
                 "拒绝把这次当成成功", axes[i]->label);
         em__pv_stop_all(axes, n);
         return EM_R_FAIL;
      }
      moved = pos1[i] - pos0[i];
      if (moved < 0)
         moved = -moved;

      printf("      %s: 6064h %d -> %d (走了 %d pul), 期间 606Ch 峰值 %d pul/s\n",
             axes[i]->label, pos0[i], pos1[i], moved, vpeak[i]);

      if (moved == 0)
      {
         em__err("%s: 整段 %ums 里 6064h 一个计数都没变 —— 驱动器收下了速度指令 "
                 "(6041h bit12 已清零), 但电机没动。\n"
                 "        先查 6083h/6084h: 它们在生效 RxPDO 里, 主站每周期都在下发, "
                 "被下发成 0 就会让斜坡永远起不来\n"
                 "        (跑一次 em_dump_pdo 看这两项的处置; 或用 --ramp-acc 指定一个非 0 值)",
                 axes[i]->label, (unsigned)hold_ms);
         em__pv_stop_all(axes, n);
         return EM_R_FAIL;
      }
   }

   /* 停机: 先把所有轴的速度一起写 0, 再逐轴断言 bit12 (Speed = 0)。不逐轴调 em_pv_stop():
    * 那个函数写完 0 就等在自己那根轴上, 多轴下会变成"先后停", 这里要的是"一起停" */
   for (i = 0; i < n; i++)
   {
      em__set_cw(axes[i], EM_CW_ENABLE_OP);
      em__put_i32(axes[i]->out, axes[i]->off_target_vel, 0);
   }
   for (i = 0; i < n; i++)
   {
      int rc = em__wait_sw(axes[i], EM_SW_PV_SPEED_ZERO, EM_SW_PV_SPEED_ZERO,
                           EM_STEP_TMO_MS, "PV 速度归零 (6041h bit12)");

      if (rc != EM_R_OK)
         return rc;
   }
   return EM_R_OK;
}

int em_pv_run_for(em_axis_t *ax, int32_t vel, uint32_t hold_ms)
{
   em_axis_t *one[1];
   int32_t    v[1];

   if (ax == NULL)
      return EM_R_FAIL;

   one[0] = ax;
   v[0]   = vel;
   return em_pv_run_multi(one, v, 1, hold_ms);
}

/* 手册 V2.3 §3.7: 驱动器支持 1~14、17~30、33、34、35 回原点方法, 其中 1~14、33、34
 * 需配套带 Z 信号的闭环步进电机。这道闸就限死这个集合, 15/16/31/32 一律拒绝 */
static int em__home_method_ok(int m)
{
   return (m >= 1 && m <= 14) || (m >= 17 && m <= 30) || m == 33 || m == 34 || m == 35;
}

/* 回零等待里 bit11 那一行的最小打印间隔 (ms): 只在变化时打, 这道缝是给抖动/接触不良的开关留的 */
#define EM_HOME_LIM_PRINT_MIN_MS  500u

void em_home_cfg_default(em_home_cfg_t *c)
{
   if (c == NULL)
      return;

   /* 方式 24 = 原点开关 (X0) 为原点, 正向先找。选 24 而不是出厂默认的 17/18, 依据是真机量到的
    * 端子功能 (2310h): X0 = 原点, X1 = 正限位, X2 = 负限位。首次回零务必把速度收得很低, 人在
    * 急停旁, 方向不对就换 29 或 35。默认速度按 2400h 细分 = 50000 pul/圈 算 (2000 pul/s ≈ 0.04 圈/s) */
   c->method   = 24;
   c->vel_fast = 2000;
   c->vel_slow = 500;
   c->acc      = 5000;
   c->offset   = 0;
}

int em_home(em_axis_t *ax, const em_home_cfg_t *cfg, uint32_t tmo_ms)
{
   em_home_cfg_t def;
   uint32_t      t0;
   int           rc, reads = 0;

   if (ax == NULL)
      return EM_R_FAIL;

   if (cfg == NULL)
   {
      em_home_cfg_default(&def);
      cfg = &def;
   }

   if (em_is_enabled(ax))
   {
      em__err("%s: 已使能, 拒绝写回零参数 (6098h/6099h/609Ah/607Ch 只能在本使能时写)。"
              "先 em_disable()", ax->label);
      return EM_R_FAIL;
   }
   if (!ax->mirror_ok)
   {
      em__err("%s: 一笔完整的 6041h 都没取到 -> 状态未知, 拒绝回零", ax->label);
      return EM_R_FAIL;
   }
   if ((ax->sw & EM_SW_FAULT) != 0)
   {
      em__err("%s: 6041h bit3 = Fault (0x%04X) -> 先 em_fault_reset()", ax->label,
              (unsigned)ax->sw);
      return EM_R_FAIL;
   }
   if (!em__home_method_ok(cfg->method))
   {
      em__err("%s: 回零方式 %d 不在本驱动器支持的范围里 (手册 §3.7: 1~14, 17~30, "
              "33, 34, 35) -> 一个字节都没写", ax->label, cfg->method);
      return EM_R_FAIL;
   }

   printf("\n  ---- %s 回零 (方式 %d, 找原点速度 %u, 返回速度 %u, 加减速 %u, "
          "原点偏移 %d) ----\n", ax->label, cfg->method, (unsigned)cfg->vel_fast,
          (unsigned)cfg->vel_slow, (unsigned)cfg->acc, cfg->offset);

   if (em__wr_i8(ax, EM_OID_HOMING_MODE, 0, (int8_t)cfg->method,
                 "回零方式 6098h") != EM_R_OK)
      return EM_R_FAIL;
   if (em__wr_u32(ax, EM_OID_HOMING_VEL, 1, cfg->vel_fast,
                  "找原点速度 6099h:01") != EM_R_OK)
      return EM_R_FAIL;
   if (em__wr_u32(ax, EM_OID_HOMING_VEL, 2, cfg->vel_slow,
                  "返回速度 6099h:02") != EM_R_OK)
      return EM_R_FAIL;
   if (em__wr_u32(ax, EM_OID_HOMING_ACC, 0, cfg->acc,
                  "回零加减速 609Ah") != EM_R_OK)
      return EM_R_FAIL;
   if (em__wr_i32(ax, EM_OID_HOMING_OFF, 0, cfg->offset,
                  "原点偏移 607Ch") != EM_R_OK)
      return EM_R_FAIL;

   /* 2. 读 2214h (回零辅助): 它决定回零完成后 6064h 显示什么, 所以只读出来打印, 不断言位置为 0 */
   {
      uint32_t aux = 0;

      if (em_rd_u32(ax->bus, ax->slave, EM_OID_HOMING_AUX, 0, &aux) == EM_R_OK)
         printf("  2214h 回零辅助 = 0x%08X (决定回零完成后 6064h 显示什么; "
                "本函数不断言位置为 0)\n", (unsigned)aux);
      else
         printf("  2214h 读失败 —— 不影响回零本身, 但回零后 6064h 会是什么值无法预判\n");
   }

   /* ---- 3. 切到 HM 模式 (未使能时才能改) ---- */
   rc = em_set_mode(ax, EM_MODE_HM);
   if (rc != EM_R_OK)
      return rc;

   /* ---- 4. 使能 (em_enable 内部的 em_arm 对 HM 不做任何事) ---- */
   rc = em_enable(ax);
   if (rc != EM_R_OK)
      return rc;

   /* ---- 5. 抬 bit4: 6040h 0x000F -> 0x001F, 上升沿启动回零 ---- */
   rc = em__cw_step(ax, "回零启动", EM_CW_HOMING_GO, EM_SW_FAULT, 0,
                    EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
      return rc;

   /* ---- 6. 等 bit12 Homing attained; bit13 Homing error 一置就失败 ---- */
   printf("  等待回零完成 (6041h bit12 Homing attained, 上限 %ums) ...",
          (unsigned)tmo_ms);
   fflush(stdout);

   /* 这几个观测量只为本段日志服务, 理由都是"轴一步不动"这一个问题: pos0/pos_ok = 抬 bit4 那
    * 一刻的位置 —— 超时时回答"到底动没动", 这是区分「在走、只是没找到开关」与「一步都没走」唯一
    * 的事实来源; lim_shown = bit11 上次打印过的值 (-1 = 还没打过); lim_all = bit11 是否全程有效 */
   int32_t  pos0    = 0;
   int      pos_ok  = (em__cur_pos(ax, &pos0) == EM_R_OK);
   int      lim_shown    = -1;
   int      lim_all      = 1;
   uint32_t t_lim_print  = 0;

   t0 = em__now_ms();
   for (;;)
   {
      if (em_stop_requested())
      {
         /* 撤掉 bit4 = 放弃这次回零。保持使能 —— 回零中途停机位置不明, 卸力可能让滑台自由下滑 */
         printf(" [中止] 撤掉 6040h bit4, 保持使能\n");
         em__set_cw(ax, EM_CW_ENABLE_OP);
         for (rc = 0; rc < 20; rc++)
         {
            (void)em__cycle(ax->bus);
            em__sleep_ms(EM_POLL_MS);
         }
         return EM_R_STOP;
      }

      /* 期间 6040h bit4 必须一直举着 —— 中途掉回 0x000F 会让回零中止 */
      em__set_cw(ax, EM_CW_HOMING_GO);
      (void)em__cycle(ax->bus);

      if (ax->mirror_ok)
      {
         reads++;

         if ((ax->sw & EM_SW_FAULT) != 0)
         {
            printf(" [FAIL]\n");
            em__err("%s: 回零中 6041h bit3 = Fault (%s)", ax->label,
                    em_sw_describe(ax->sw));
            return EM_R_FAIL;
         }
         if ((ax->sw & EM_SW_HM_ERROR) != 0)
         {
            printf(" [FAIL]\n");
            em__err("%s: 6041h bit13 = Homing error —— 回零失败 (%s)。"
                    "常见原因: 该方向找不到原点开关 / 方式与接线不符 (试 29 或 35) / "
                    "限位信号一直是有效的 (极性配反时 X1/X2 恒报压着, 回零找不到跳变) / "
                    "真的压上了限位", ax->label, em_sw_describe(ax->sw));
            return EM_R_FAIL;
         }
         if ((ax->sw & EM_SW_HM_ATTAINED) != 0)
         {
            printf(" [PASS] 6041h=%s (耗时 %ums)\n", em_sw_describe(ax->sw),
                   (unsigned)(em__now_ms() - t0));
            break;
         }
         /* bit11 只在变化时打一行 (外加 EM_HOME_LIM_PRINT_MIN_MS 那道缝, 给抖动/接触不良的
          * 开关留的)。lim_all 决定超时那条消息说"驱动器自己不许动"还是"接着找" */
         {
            const int lim = ((ax->sw & EM_SW_INTLIMIT) != 0);

            if (!lim)
               lim_all = 0;

            /* 第一次观察就是"没压着" —— 那是常态, 不值得一行 */
            if (lim_shown < 0 && !lim)
               lim_shown = 0;

            if (lim != lim_shown)
            {
               const uint32_t now = em__now_ms();

               if (t_lim_print == 0 ||
                   (int32_t)(now - t_lim_print) >= (int32_t)EM_HOME_LIM_PRINT_MIN_MS)
               {
                  t_lim_print = now;
                  lim_shown   = lim;
                  printf("\n  [..] 6041h bit11 %s\n",
                         lim ? "硬件限位有效 (仍在找原点) —— 见 2204h 超程停车方式, "
                               "以及 2300h 输入逻辑与接线是否配反"
                             : "已清 (驱动器不再报硬件限位)");
                  fflush(stdout);
               }
            }
         }
      }

      if ((int32_t)(em__now_ms() - t0) >= (int32_t)tmo_ms)
      {
         printf(" [FAIL]\n");
         if (reads == 0)
         {
            em__err("%s: 等 bit12 超时 (%ums), 且一笔完整的 6041h 都没取到: 状态未知",
                    ax->label, (unsigned)tmo_ms);
            return EM_R_FAIL;
         }

         {
            int32_t   pos1    = 0;
            const int pos_ok1 = (em__cur_pos(ax, &pos1) == EM_R_OK);
            /* -1 = 不知道 (两次里有一次没读出来), 0 = 一步都没动, 1 = 动了 */
            const int moved   = (pos_ok && pos_ok1) ? (pos1 != pos0) : -1;

            em__err("%s: 等 bit12 超时 (%ums), 实测 %s", ax->label, (unsigned)tmo_ms,
                    em_sw_describe(ax->sw));

            if (lim_all)
            {
               /* bit11 = 硬件限位有效 (手册 §3.3.2: 限位信号有效时该位置 1)。2204h 超程停车
                * 方式 = 0 (停止) 时驱动器按"已经压着限位"处理, 一个方向都不许走 —— 现象是
                * "模式对、使能对、bit4 也抬了, 轴却一步不动, 也不报 bit3/bit13" */
               if (moved == 0)
               {
                  printf("        6041h bit11 = 硬件限位有效**全程举着** (一次都没清), "
                         "而位置一步没变 (%d pul)。\n"
                         "        这多半不是「没找到开关」, 而是**驱动器自己不许动** —— "
                         "按顺序查这三处:\n"
                         "        1) 2300h 输入逻辑与现场接线是否一致: 常开的驱动器配 NPN "
                         "传感器 (高电平 = 未触发) 时 X0/X1/X2 **一起反相**, bit11 因此\n"
                         "           恒置 1。改法是把 2300h 的 bit0~bit2 都置 1 (= 0x0007) —— "
                         "motor_test 用 --allow-param --npn 就能做, scan 界面\n"
                         "           「高级选项」里那一项默认开着; 要断电也在, 再用厂家工具"
                         "按 2102h 存一次 EEPROM;\n"
                         "        2) 2204h 超程停车方式 (= 0 停止 / 1 急停 / 2 无效);\n"
                         "        3) 2310h~2312h 的端子分配 (X0 原点 / X1 正限位 / X2 负限位)。\n"
                         "        界面上的「上位机侧取反」勾**帮不了这里**: 它只改主站的"
                         "判据, 开关是驱动器自己读的。\n", pos1);
                  fflush(stdout);
               }
               else if (moved > 0)
               {
                  printf("        6041h bit11 全程有效, 但位置**确实变了** (现在是 %d pul, "
                         "抬 bit4 时是 %d pul):\n"
                         "        驱动器在限位有效下仍然走了, 那 bit11 就不是拦住它的原因。"
                         "回零仍在进行或已卡住 ——\n"
                         "        先确认滑台在哪、原点开关是否触发。\n", pos1, pos0);
                  fflush(stdout);
               }
               else
               {
                  printf("        6041h bit11 全程有效, 而位置读不出来 —— 先查 2300h 输入"
                         "逻辑与现场接线\n"
                         "        (常开配 NPN 时三个信号一起反相, bit11 恒置 1), 再查 "
                         "2204h 与 2310h~2312h。\n");
                  fflush(stdout);
               }
            }
            else
            {
               printf("        回零仍在进行或已卡住 —— 先确认滑台在哪、限位/原点开关是否"
                      "触发\n"
                      "        (别硬顶: 够不着就把滑台先挪近, 而不是把速度调快)。\n");
               fflush(stdout);
            }
            return EM_R_FAIL;
         }
      }

      em__sleep_ms(EM_POLL_MS);
   }

   rc = em__cw_step(ax, "回零结束 (6040h bit4 -> 0)", EM_CW_ENABLE_OP,
                    EM_SW_HM_ATTAINED, EM_SW_HM_ATTAINED, EM_STEP_TMO_MS);
   if (rc != EM_R_OK)
      return rc;

   /* 只打印不判定: 回零完成后 6064h 是"原点偏移之后的坐标系里的位置", 等不等于 0 由 2214h 与 607Ch 定 */
   {
      int32_t pos_m = ax->pos, pos_s = 0;

      if (em_pos_sdo(ax, &pos_s) == EM_R_OK)
         printf("  回零后位置: TxPDO[6064h] = %d pul, SDO 读 6064h = %d pul%s\n",
                pos_m, pos_s, (pos_m == pos_s) ? " (一致)" : " <<< 不一致");
      else
         printf("  回零后位置: TxPDO[6064h] = %d pul (SDO 读失败)\n", pos_m);
   }
   return EM_R_OK;
}
