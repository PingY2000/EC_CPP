/*
 * sm_motion.c - CiA402 使能状态机 (S3) 与微动反馈闭环 (S4)
 *
 * 本文件**不含任何 ecx_SDOwrite** —— 所有写都经由 sm_guard.c 的
 * sm_wr_*() / sm_set_cw(), 未授权时它们直接拒绝。这里只负责"写什么、按什么顺序写、
 * 写完等什么位、等超时了怎么办"。
 *
 * 为什么用 SDO 而不是 PDO (见设计决策 D1):
 *   YKD2205PE 出厂默认 TxPDO (1A00h) 是空的, 要让 PDO 通路可用必须写
 *   1C12h/1C13h/1600h/1A00h —— 而 PP 模式的速度规划器本来就在驱动器内部,
 *   走 SDO 写 607Ah/6081h/6040h 一样能跑完整动作, 且零持久配置变更、
 *   不暴露 SM 看门狗 (SAFE_OP/PRE_OP 下不发过程数据, 不会被看门狗判故障)。
 *
 * 使能序列用**位判断**, 不用数值比较 —— 手册明确该驱动器 6041h 低 4 位是
 * 0000/0001/0011/0111 的非标准编码。
 *
 * 微动完成判据用**位置静止**而不是 bit10/bit12 握手: SDO 轮询 (5ms 节拍 + 邮箱
 * 往返) 大概率抓不到 PP 的瞬态握手位, 靠位会误判。位置静止 + 行程断言
 * 把"动作结束了吗"和"动得对不对"干净地分成两件事。
 */

#include <stdio.h>
#include <string.h>

#include "sm.h"

/* ======================================================================
 * 等待状态字某组位
 *
 * fault_aborts = 1 时, 等到 Fault 位置起就立即中止 (运动期应该这样 —— 有故障就停)。
 * fault_aborts = 0 时忽略 Fault 位, 只按 mask/want 判 (故障复位等待要用这个:
 * 复位过程中 Fault 位本来就是 1, 不能因此中止)。
 *
 * 返回: 1 = 满足; 0 = 超时; -1 = 已中止 (Fault / Ctrl-C / 掉线 / 读失败)
 * ====================================================================== */
static int wait_sw(sm_axis_t *ax, uint16_t mask, uint16_t want,
                   uint32_t tmo_ms, uint16_t *last_sw, int fault_aborts)
{
   uint32_t deadline = sm_now_ms() + tmo_ms;

   for (;;)
   {
      uint16_t sw = 0;
      int32_t  ab = 0;
      int      rc;

      if (sm_guard_should_abort())
         return -1;

      rc = sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &sw,
                     SM_SDO_TMO_MOTION, &ab);
      if (rc == SM_RD_OK)
      {
         if (last_sw != NULL)
            *last_sw = sw;
         sm_guard_feed();
         if ((sw & mask) == want)
            return 1;
         if (fault_aborts && (sw & SM_SW_FAULT) != 0)
         {
            sm_guard_abort(SM_ABORT_FAULT);
            return -1;
         }
      }
      else if (rc == SM_RD_TIMEOUT)
      {
         sm_guard_abort(SM_ABORT_IO);
         return -1;
      }

      if (sm_now_ms() >= deadline)
         return 0;
   }
}

/* 读一个 int32 对象, 读不到就中止整轮动作 */
static int read_i32_or_abort(sm_axis_t *ax, uint16_t index, int32_t *v)
{
   int32_t ab = 0;
   int     rc = sm_rd_i32(ax->slave, index, 0, v, SM_SDO_TMO_MOTION, &ab);

   if (rc == SM_RD_OK)
   {
      sm_guard_feed();
      return 0;
   }
   sm_guard_abort(SM_ABORT_IO);
   return -1;
}

/* ======================================================================
 * 只读快照: 6081h / 6083h / 6084h / 6060h
 *
 * 微动会临时改这四个对象, 收尾必须恢复原值。预演模式也要用 —— 好让操作员
 * 在真正动手前就看到"收尾会把它们恢复成什么"。全程只读。
 * ====================================================================== */
void sm_snapshot_pp(sm_axis_t *ax)
{
   static const uint16_t idx[3] = { SM_OID_PROF_VEL, SM_OID_PROF_ACC,
                                    SM_OID_PROF_DEC };
   int k;

   /*
    * 只补空缺, 不覆盖已有快照 —— 这点很关键:
    *   S3 会先把 6060h 写成 1 (PP)。如果 S4 再调一次本函数并覆盖 6060h,
    *   快照记录的就成了我们**自己写进去的 1**, 收尾会把它"恢复"成 1,
    *   而真正的原值 (0) 就丢了。同理 6081h/6083h/6084h 也一样。
    *   "谁先读到谁说了算"保证了无论调用顺序如何, 记下的都是原始值。
    */
   for (k = 0; k < 3; k++)
   {
      uint8_t buf[4];
      int     sz = 0;
      int32_t ab = 0;
      uint32_t v;

      if ((k == 0 && ax->have_prof_vel) ||
          (k == 1 && ax->have_prof_acc) ||
          (k == 2 && ax->have_prof_dec))
         continue;

      if (sm_rd_raw(ax->slave, idx[k], 0, SM_SDO_TMO_IDLE, buf, &sz,
                    &ab) != SM_RD_OK)
         continue;

      v = (uint32_t)sm_bytes_to_i64(buf, sz, SM_DT_U32);
      if (k == 0)      { ax->prof_vel_snap = v; ax->have_prof_vel = 1; }
      else if (k == 1) { ax->prof_acc_snap = v; ax->have_prof_acc = 1; }
      else             { ax->prof_dec_snap = v; ax->have_prof_dec = 1; }
   }

   if (!ax->have_mode)
   {
      int8_t  m  = 0;
      int32_t ab = 0;

      if (sm_rd_i8(ax->slave, SM_OID_MODES, 0, &m, SM_SDO_TMO_IDLE,
                   &ab) == SM_RD_OK)
      {
         ax->mode_snap = m;
         ax->have_mode = 1;
      }
   }
}

/* ======================================================================
 * 前置检查 (全程只读, 在第一次写之前)
 *
 * 任一条件命中就拒绝执行, 退出码 SM_EXIT_REFUSED —— 此时**一个字节都没写**,
 * 操作员可以把"拒绝"和"动了但失败"清楚地区分开。
 * ====================================================================== */
int sm_preflight(sm_axis_t *ax, int nslaves, int32_t delta)
{
   int      i;
   int      refused = 0;
   int      digin_ok;
   uint16_t sw = 0;
   uint32_t dig_in = 0;
   int32_t  pos[3] = { 0, 0, 0 };
   int32_t  ab = 0;

   (void)nslaves;   /* 单轴检查; 台数由调用者掌握 */

   printf("\n  ---- 位置 %d 动作前置检查 (只读) ----\n", ax->pos);

   /* P1: 状态字 —— 故障位、电压位、运转中 */
   if (sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &sw, SM_SDO_TMO_IDLE,
                 &ab) != SM_RD_OK)
   {
      printf("      [拒绝] 6041h 读不到 (驱动离线?), 不动作。\n");
      return SM_EXIT_REFUSED;
   }
   ax->sw_initial = sw;
   printf("      6041h 初值 = 0x%04X (bit0=%d bit1=%d bit2=%d bit3=%d bit4=%d)\n",
          (unsigned)sw, (sw & 1) ? 1 : 0, (sw & 2) ? 1 : 0, (sw & 4) ? 1 : 0,
          (sw & 8) ? 1 : 0, (sw & 0x10) ? 1 : 0);

   if ((sw & SM_SW_FAULT) != 0)
   {
      printf("      [拒绝] 驱动器处于故障态 (bit3=1)");
      if (g_guard.reset_fault)
         printf(", 将从 S3 第 0 步尝试故障复位 (--reset-fault)。\n");
      else
      {
         printf("; 要尝试复位请加 --reset-fault。\n");
         return SM_EXIT_REFUSED;
      }
   }
   if ((sw & SM_SW_OP_ENABLED) != 0)
   {
      printf("      [拒绝] 驱动器已经处于 Operation enabled (bit2=1), "
             "说明有别的程序在控制它, 不接手。\n");
      return SM_EXIT_REFUSED;
   }
   if ((sw & SM_SW_VOLTAGE) == 0)
   {
      /* 文档没列 bit4, 只当启发式提示 */
      printf("      [WARN] 6041h bit4 (主电已上) = 0。可能是主电未上或该位定义不同, "
             "继续但请留意。\n");
      ax->warn = 1;
   }

   /* P2: 位置与速度读数。前置阶段一律不用 read_i32_or_abort ——
      它会把失败升级成全局中止, 而这里只想干净地"拒绝", 不动全局状态。 */
   for (i = 0; i < 3; i++)
   {
      if (sm_rd_i32(ax->slave, SM_OID_ACT_POS, 0, &pos[i], SM_SDO_TMO_IDLE,
                    &ab) != SM_RD_OK)
      {
         printf("      [拒绝] 6064h 实际位置读不到 (abort 0x%08X), "
                "反馈不可信, 不动作。\n", (unsigned)ab);
         return SM_EXIT_REFUSED;
      }
   }

   printf("      6064h 三次读数 = %d / %d / %d\n",
          (int)pos[0], (int)pos[1], (int)pos[2]);

   if (pos[0] > SM_POS_ABS_MAX || pos[0] < -SM_POS_ABS_MAX)
   {
      printf("      [拒绝] 位置读数 %d 超出合理范围 (|pos| > %ld), "
             "疑似换算/计数异常。\n", (int)pos[0], (long)SM_POS_ABS_MAX);
      return SM_EXIT_REFUSED;
   }

   {
      int32_t dmax = 0;
      for (i = 1; i < 3; i++)
      {
         int32_t d = pos[i] - pos[0];
         if (d < 0) d = -d;
         if (d > dmax) dmax = d;
      }
      if (dmax > SM_POS_NOISE_PULSES)
      {
         printf("      [拒绝] 未使能状态下位置读数跳动 %d 脉冲 (阈值 %d), "
                "反馈不可信, 不动作。\n", (int)dmax, SM_POS_NOISE_PULSES);
         return SM_EXIT_REFUSED;
      }
   }

   {
      int32_t vel = 0;

      /*
       * 读不到 606Ch 必须**拒绝**, 不能"跳过这一项继续"。
       * 这一项问的是"轴现在是不是正在动?"; 答不出来就意味着可能有另一套
       * 程序(或上一次中止残留)正在驱动这根轴。此时再使能并叠加一次微动,
       * 是与未知运动抢控制权 —— 正是本检查存在的原因。6041h/6064h 读不到
       * 都判拒绝, 这里没有理由例外。
       */
      if (sm_rd_i32(ax->slave, SM_OID_ACT_VEL, 0, &vel, SM_SDO_TMO_IDLE,
                    &ab) != SM_RD_OK)
      {
         printf("      [拒绝] 606Ch 实际速度读不到 (abort 0x%08X) —— "
                "无法确认轴是否正在运动, 不动作。\n", (unsigned)ab);
         return SM_EXIT_REFUSED;
      }
      printf("      606Ch 实际速度 = %d\n", (int)vel);
      if (vel != 0)
      {
         printf("      [拒绝] 未使能状态下仍有速度 %d —— 轴正在动! "
                "不动作。\n", (int)vel);
         return SM_EXIT_REFUSED;
      }
   }

   /* P3: 相对微动的原点保护。相对模式下 607Ah 是小偏移量; 万一驱动器忽略了
      bit6 当成绝对位置, 从很远处出发就会是一次不受控的长距离运动。 */
   if (pos[0] > SM_ORIGIN_GUARD || pos[0] < -SM_ORIGIN_GUARD)
   {
      printf("      [拒绝] 当前位置 %d 距原点超过保护距离 %ld, "
             "拒绝相对微动 (防止 bit6 被忽略时变成 |%.0f| 脉冲的长距离绝对运动)。\n",
             (int)pos[0], (long)SM_ORIGIN_GUARD, (double)pos[0]);
      return SM_EXIT_REFUSED;
   }

   /* P4: 限位 (60FDh)。只在**运动方向**上的限位已触发才拒绝 ——
      反方向的限位压着不影响往另一侧走。 */
   {
      uint8_t dbuf[4];
      int     dsz = 0;

      digin_ok = (sm_rd_raw(ax->slave, SM_OID_DIG_IN, 0, SM_SDO_TMO_IDLE,
                            dbuf, &dsz, &ab) == SM_RD_OK);
      if (digin_ok)
         dig_in = (uint32_t)sm_bytes_to_i64(dbuf, dsz, SM_DT_U32);
   }
   if (digin_ok)
   {
      printf("      60FDh 数字输入 = 0x%08X (bit0 负限位=%d, bit1 正限位=%d, "
             "bit2 原点=%d)\n",
             (unsigned)dig_in, (dig_in & 1) ? 1 : 0, (dig_in & 2) ? 1 : 0,
             (dig_in & 4) ? 1 : 0);

      if ((dig_in & 0x0001u) != 0 && delta < 0)
      {
         printf("      [拒绝] 负限位已触发 (bit0=1), 而本次要往负方向走。\n");
         refused = 1;
      }
      if ((dig_in & 0x0002u) != 0 && delta > 0)
      {
         printf("      [拒绝] 正限位已触发 (bit1=1), 而本次要往正方向走。\n");
         refused = 1;
      }
      if ((dig_in & (0x0001u | 0x0002u)) != 0)
         ax->warn = 1;   /* 有一条限位压着, 记 WARN */
   }
   else
   {
      printf("      [WARN] 60FDh 读不到, 无法预检限位 —— 请自行确认限位未压住。\n");
      ax->warn = 1;
   }

   /* P5: 软限位 (607Dh)。文档没列这个对象, 很可能不存在 —— 读得到就是硬约束,
      读不到就把微动行程收紧到 SM_NO_SOFTLIMIT_CAP。 */
   {
      int32_t lo = 0, hi = 0;
      int32_t ab1 = 0, ab2 = 0;
      int     r1 = sm_rd_i32(ax->slave, SM_OID_SOFTLIM, 1, &lo,
                             SM_SDO_TMO_IDLE, &ab1);
      int     r2 = sm_rd_i32(ax->slave, SM_OID_SOFTLIM, 2, &hi,
                             SM_SDO_TMO_IDLE, &ab2);

      if (r1 == SM_RD_OK && r2 == SM_RD_OK)
      {
         /* 用 int64 求和: delta 在别处已被收紧, 但这里不该依赖那个假设 */
         int64_t tgt = (int64_t)pos[0] + (int64_t)delta;
         printf("      607Dh 软限位 = [%d, %d], 目标位置 %d\n",
                (int)lo, (int)hi, (int)tgt);
         if (tgt < (int64_t)lo || tgt > (int64_t)hi)
         {
            printf("      [拒绝] 目标位置 %d 越出软限位 [%d, %d]。\n",
                   (int)tgt, (int)lo, (int)hi);
            refused = 1;
         }
      }
      else
      {
         /*
          * 没有软限位可依据, 就**真的**把行程卡死在这里 —— 只是打印一句
          * "将被限制在 ±N" 而代码里没有对应约束, 是对操作者的误导: 一旦
          * 有人用 --force-caps 放大 --jog, 提示与实际就分道扬镳了。
          */
         printf("      607Dh 软限位不可读 (abort 0x%08X/0x%08X), 该驱动器可能不支持; "
                "微动行程将被限制在 ±%d 脉冲内。\n",
                (unsigned)ab1, (unsigned)ab2, SM_NO_SOFTLIMIT_CAP);
         if (delta > SM_NO_SOFTLIMIT_CAP || delta < -SM_NO_SOFTLIMIT_CAP)
         {
            printf("      [拒绝] 本次行程 %d 脉冲超过无软限位时的上限 ±%d "
                   "—— 没有任何限位兜底, 不动作。\n",
                   (int)delta, SM_NO_SOFTLIMIT_CAP);
            refused = 1;
         }
      }
   }

   if (refused)
      return SM_EXIT_REFUSED;

   printf("      [通过] 前置检查全部通过。\n");
   return 0;
}

/* ======================================================================
 * S3: 使能状态机
 *
 * 步骤 (位判断, 数值不作为判据):
 *   0  故障复位 (仅在 --reset-fault 时)
 *   1  6040h = 0x0006 Shutdown        -> 等 bit0 & bit1
 *   2  6040h = 0x0007 Switch On       -> 等 bit1 且无 bit3
 *   3  6040h = 0x000F Enable Op       -> 等 bit2
 *   4  保持 enable_hold_ms, 期间 bit2 不得掉、bit3 不得起
 *   5  写 6060h=1 (PP), 回读 6061h 验证写入生效
 * ====================================================================== */
int sm_stage_enable(sm_axis_t *ax, uint32_t enable_hold_ms)
{
   uint16_t last = 0;
   int      r = 0;

   printf("\n  === 位置 %d: 使能状态机 (S3) ===\n", ax->pos);
   g_guard.t_start_ms = sm_now_ms();

   /*
    * 从这里起这根轴归本次运行接管。收尾与急停只处理 engaged 的轴, 所以
    * "接管"这一步的时机决定了我们会去动哪些轴 —— 放在前置检查已经通过、
    * 马上要写第一个字节的位置。置位之后无论从哪条路径退出, 收尾都会覆盖它。
    */
   ax->engaged = 1;

   /* 看门狗基准也在这里落一次, 免得一个过期的 last_io_ms 让第一条轮询就
     误判成掉线。 */
   sm_guard_feed();

   /* 在**任何写之前**抓原始快照 (6081h/6083h/6084h/6060h), 收尾按它恢复。
     放在这里而不是 S4, 是因为 S3 第 5 步就会把 6060h 写成 1。 */
   sm_snapshot_pp(ax);

   /* 0. 故障复位 (可选) */
   if ((ax->sw_initial & SM_SW_FAULT) != 0)
   {
      if (!g_guard.reset_fault)
      {
         printf("      [FAIL] 驱动器处于故障态且未给 --reset-fault。\n");
         return SM_V_FAIL;
      }
      printf("      [复位] 写 6040h = 0x%04X (Fault reset), 随后回 0x0000\n",
             SM_CW_FAULT_RST);
      if (sm_set_cw(ax, SM_CW_FAULT_RST, "故障复位") != 0)
         return SM_V_FAIL;
      if (sm_set_cw(ax, SM_CW_DISABLE_V, "复位后回 0") != 0)
         return SM_V_FAIL;
      /* 复位过程中 Fault 位本来就是 1, 所以这一等不把 Fault 当致命 */
      if (wait_sw(ax, SM_SW_FAULT, 0, SM_ENABLE_TMO_MS, &last, 0) != 1)
      {
         printf("      [FAIL] 故障复位无效, bit3 仍为 1 (6041h=0x%04X)。\n",
                (unsigned)last);
         return SM_V_FAIL;
      }
      printf("      [PASS] 故障已清除 (6041h=0x%04X)\n", (unsigned)last);
   }

   /* 1. Shutdown */
   if (sm_set_cw(ax, SM_CW_SHUTDOWN, "S3-1 Shutdown") != 0)
      return SM_V_FAIL;
   r = wait_sw(ax, SM_SW_RTSO | SM_SW_SWITCHED, SM_SW_RTSO | SM_SW_SWITCHED,
               SM_ENABLE_TMO_MS, &last, 1);
   sm_trace_fill_sw(ax, last);
   if (r != 1)
   {
      printf("      [FAIL] 写 0x0006 后未等到 Ready to switch on + Switched on "
             "(6041h=0x%04X)%s\n", (unsigned)last,
             (r < 0) ? " —— 被故障/中止打断" : " —— 超时");
      return SM_V_FAIL;
   }
   printf("      [PASS] Ready to switch on + Switched on (6041h=0x%04X)\n",
          (unsigned)last);

   /* 2. Switch On */
   if (sm_set_cw(ax, SM_CW_SWITCHON, "S3-2 Switch On") != 0)
      return SM_V_FAIL;
   r = wait_sw(ax, SM_SW_SWITCHED | SM_SW_FAULT, SM_SW_SWITCHED,
               SM_ENABLE_TMO_MS, &last, 1);
   sm_trace_fill_sw(ax, last);
   if (r != 1)
   {
      printf("      [FAIL] 写 0x0007 后未保持 Switched on 且无故障 "
             "(6041h=0x%04X)\n", (unsigned)last);
      return SM_V_FAIL;
   }
   printf("      [PASS] Switched on (6041h=0x%04X)\n", (unsigned)last);

   /* 3. Enable Operation —— 这是本工具最关键的一步, 也是"PRE_OP/SAFE_OP 下
      SDO 写 6040h 到底能不能真的把功率级打开"这个未知问题的答案所在。 */
   if (sm_set_cw(ax, SM_CW_ENABLE_OP, "S3-3 Enable Operation") != 0)
      return SM_V_FAIL;
   r = wait_sw(ax, SM_SW_OP_ENABLED, SM_SW_OP_ENABLED,
               SM_ENABLE_OP_TMO_MS, &last, 1);
   sm_trace_fill_sw(ax, last);
   if (r != 1)
   {
      struct ec_slave *s = &g_ctx.slavelist[ax->slave];
      printf("      [FAIL] 未能进入 Operation enabled (6041h=0x%04X, "
             "AL 状态 0x%02X)\n", (unsigned)last, (unsigned)s->state);
      printf("      >>> 这通常意味着该驱动器不允许在 %s 下用 SDO 打开功率级。\n",
             (s->state == EC_STATE_PRE_OP) ? "PRE_OP" : "SAFE_OP");
      printf("      >>> 可尝试: --state safe-op / --state pre-op 换一个状态再试。\n");
      ax->abort_code = sm_take_abort(ax->slave, SM_OID_CONTROLWORD, 0);
      if (ax->abort_code != 0)
         printf("      >>> 最后一次 SDO abort 码: 0x%08X\n",
                (unsigned)ax->abort_code);
      return SM_V_FAIL;
   }
   ax->enable_ok = 1;
   printf("      [PASS] Operation enabled (6041h=0x%04X) —— 功率级已打开\n",
          (unsigned)last);

   /* 4. 保持观察 */
   {
      /* 差值法比较, 回绕安全; 并轮询中止标志, 免得 Ctrl-C 要等满这一整段 */
      uint32_t t_h = sm_now_ms();
      while ((int32_t)(sm_now_ms() - t_h) < (int32_t)enable_hold_ms)
      {
         int32_t ab = 0;
         if (sm_guard_should_abort())
         {
            printf("      [中止] 保持观察期间收到中止请求。\n");
            return SM_V_FAIL;
         }
         if (sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &last,
                       SM_SDO_TMO_MOTION, &ab) == SM_RD_OK)
         {
            sm_guard_feed();
            if ((last & SM_SW_FAULT) != 0)
            {
               printf("      [FAIL] 保持期间出现故障 (6041h=0x%04X)\n",
                      (unsigned)last);
               sm_guard_abort(SM_ABORT_FAULT);
               return SM_V_FAIL;
            }
            if ((last & SM_SW_OP_ENABLED) == 0)
            {
               printf("      [FAIL] 保持期间掉出 Operation enabled "
                      "(6041h=0x%04X)\n", (unsigned)last);
               return SM_V_FAIL;
            }
         }
      }
      printf("      [PASS] 保持 %ums 期间稳定在使能态\n",
             (unsigned)enable_hold_ms);
   }

   /* 5. 写操作模式并回读 —— 真正的"写进去了吗"验证 */
   {
      int8_t  disp = 0;
      int32_t ab = 0;
      uint32_t t0 = sm_now_ms();

      if (sm_wr_u8(ax->slave, SM_OID_MODES, 0, (uint8_t)SM_MODE_PP,
                   "S3-5 设 PP 模式") != 0)
      {
         printf("      [WARN] 6060h 写入失败, PP 模式未能设定。\n");
         return SM_V_WARN;
      }

      /* 回读 6061h, 给驱动器一点时间同步 */
      for (;;)
      {
         if (sm_guard_should_abort())
            break;
         if (sm_rd_i8(ax->slave, SM_OID_MODES_DISP, 0, &disp,
                      SM_SDO_TMO_MOTION, &ab) == SM_RD_OK &&
             disp == SM_MODE_PP)
            break;
         if (sm_now_ms() - t0 > SM_STEP_TMO_MS)
            break;
      }

      if (disp == SM_MODE_PP)
      {
         printf("      [PASS] 写入生效: 6060h=1 -> 6061h 回读 %d (PP 轮廓位置模式)\n",
                (int)disp);
      }
      else
      {
         printf("      [WARN] 6060h 写了 1 但 6061h 回读 %d, 写入未生效。\n",
                (int)disp);
         ax->warn = 1;
         return SM_V_WARN;
      }
   }

   return SM_V_PASS;
}

/* ======================================================================
 * S4: 单腿微动
 *
 * 完成判据是"位置静止", 不是 bit10/bit12:
 *   - 已经动过 (|pos-p0| > 噪声) 且位置连续 SM_SETTLE_MS 不变 -> 结束
 *   - 一直没动过且超过 SM_STALL_MS -> 失速中止
 *   - 超过单腿 deadline -> 超时中止
 * 动得对不对由调用者的断言负责, 这里只回答"停了吗"。
 * ====================================================================== */
static int jog_leg(sm_axis_t *ax, int32_t delta, uint32_t vel, uint32_t acc,
                   uint32_t dec, uint32_t move_tmo_ms, int32_t *p0_out,
                   int32_t *p1_out, int32_t *peak_out, uint32_t *ms_out,
                   int *dir_viol)
{
   int32_t  p0 = 0, pos = 0, last_pos = 0, vel_now = 0;
   int32_t  peak = 0;
   uint32_t t0;            /* 在**触发之后**赋值, 见下方说明 */
   uint32_t deadline;
   uint32_t last_change_ms;
   int      moved = 0;
   int      violations = 0;
   uint16_t sw = 0;
   int32_t  ab = 0;
   int      rc;

   /* 读起点 */
   if (read_i32_or_abort(ax, SM_OID_ACT_POS, &p0) != 0)
      return -1;
   last_pos = p0;
   last_change_ms = sm_now_ms();

   /* 写 PP 参数 (调用者已在外面做过快照) */
   printf("      [WRITE] 6081h PP速度 = %u\n", (unsigned)vel);
   if (sm_wr_u32(ax->slave, SM_OID_PROF_VEL, 0, vel, "微动速度") != 0)
      return -1;
   printf("      [WRITE] 6083h 加速度 = %u\n", (unsigned)acc);
   if (sm_wr_u32(ax->slave, SM_OID_PROF_ACC, 0, acc, "微动加速度") != 0)
      return -1;
   printf("      [WRITE] 6084h 减速度 = %u\n", (unsigned)dec);
   if (sm_wr_u32(ax->slave, SM_OID_PROF_DEC, 0, dec, "微动减速度") != 0)
      return -1;

   /* 写目标位置 (相对偏移量)。按 4 字节原样写, 负值也保持符号。 */
   printf("      [WRITE] 607Ah 相对目标位置 = %d\n", (int)delta);
   if (sm_wr_i32(ax->slave, SM_OID_TARGET_POS, 0, delta, "微动目标位置") != 0)
      return -1;
   {
      int32_t rb = 0;

      /*
       * 回读失败也必须放弃本腿。607Ah 的回读是"驱动器到底接受了什么目标"
       * 的唯一证据 —— 尤其在加急写 (4 字节) 上, 这份 SOEM 会把从站的 SDO
       * abort 当成写成功(见 sm_guard.c 里 guard_force_disable 的说明), 所以
       * 写返回 0 并不能说明目标被接受。读不回来就当作没写进去。
       */
      if (sm_rd_i32(ax->slave, SM_OID_TARGET_POS, 0, &rb, SM_SDO_TMO_MOTION,
                    &ab) != SM_RD_OK)
      {
         printf("      [FAIL] 607Ah 回读失败 (abort 0x%08X) —— 无法确认驱动器"
                "接受了什么目标, 放弃本腿 (不触发)。\n", (unsigned)ab);
         return -1;
      }
      printf("      [CHECK] 607Ah 回读 = %d %s\n", (int)rb,
             (rb == delta) ? "(一致)" : "(!! 与写入不一致, 驱动器可能不接受该值)");
      if (rb != delta)
      {
         printf("      [FAIL] 目标位置写入未生效, 放弃本腿。\n");
         return -1;
      }
   }

   /* 设定点边沿: 先无 bit4 (0x006F), 再带 bit4 (0x007F) 触发 */
   if (sm_set_cw(ax, SM_CW_PP_IDLE, "微动 待触发") != 0)
      return -1;
   if (sm_set_cw(ax, SM_CW_PP_TRIGGER, "微动 触发新设定点") != 0)
      return -1;
   /* bit4 必须落下去, 否则下一腿的上升沿不存在, 那一腿会直接失速。
      落沿失败没有别的补救办法, 只能放弃本腿。 */
   if (sm_set_cw(ax, SM_CW_PP_IDLE, "微动 落沿") != 0)
   {
      printf("      [FAIL] bit4 落沿写失败 —— 无法为下一腿准备上升沿。\n");
      return -1;
   }

   /*
    * 计时起点放在**触发之后**。写在前面的话, 几次邮箱往返 (几个 200ms 的
    * SDO 超时窗口) 会被算进"命令发出后位置仍无变化"的窗口里, 一条刚起步的
    * 健康运动可能在触发瞬间就被判失速。
    */
   t0 = sm_now_ms();

   /* 单腿时间预算: 梯形速度曲线下的理论时间 × 3 + 余量 */
   {
      uint32_t ad = (delta < 0) ? (uint32_t)(-delta) : (uint32_t)delta;
      uint32_t ramp_ms = (acc > 0) ? (uint32_t)((uint64_t)vel * 1000u / acc) : 0;
      uint32_t cruise_ms = (vel > 0) ? (uint32_t)((uint64_t)ad * 1000u / vel) : 0;
      uint64_t need = (uint64_t)ramp_ms * 2u + cruise_ms;

      need = need * 3u + 500u;
      if (need > move_tmo_ms)
         need = move_tmo_ms;
      deadline = t0 + (uint32_t)need;
      printf("      [预算] 理论耗时约 %u ms, 本腿超时 %u ms\n",
             (unsigned)((uint64_t)ramp_ms * 2u + cruise_ms), (unsigned)need);
   }

   /* 运动轮询 */
   for (;;)
   {
      uint32_t now = sm_now_ms();

      if (sm_guard_should_abort())
         return -1;

      /*
       * dead-man 检查。掉线不会自己举手 —— 这份 SOEM 里 islost 从不被置位,
       * 从站静静地不来只表现为 SDO 一直超时。所以用"距上次成功交互多久"
       * 判断总线是不是还活着。电机正带电时这是必须的。
       */
      if (sm_guard_watchdog_expired())
      {
         printf("      [失联] 已 %ums 没有一次成功的总线交互 "
                "(看门狗 %ums) —— 视为掉线, 立即中止。\n",
                (unsigned)g_guard.watchdog_ms, (unsigned)g_guard.watchdog_ms);
         sm_guard_abort(SM_ABORT_LOST);
         return -1;
      }

      if (sm_rd_u16(ax->slave, SM_OID_STATUSWORD, 0, &sw,
                    SM_SDO_TMO_MOTION, &ab) != SM_RD_OK)
      {
         sm_guard_abort(SM_ABORT_IO);
         return -1;
      }
      if ((sw & SM_SW_FAULT) != 0)
      {
         printf("      [FAULT] 6041h=0x%04X 故障位 bit3=1\n", (unsigned)sw);
         sm_guard_abort(SM_ABORT_FAULT);
         return -1;
      }
      sm_guard_feed();
      sm_drain_errors(ax->slave, 1);

      if (read_i32_or_abort(ax, SM_OID_ACT_POS, &pos) != 0)
         return -1;
      if (sm_rd_i32(ax->slave, SM_OID_ACT_VEL, 0, &vel_now,
                    SM_SDO_TMO_MOTION, &ab) == SM_RD_OK)
      {
         int32_t av = (vel_now < 0) ? -vel_now : vel_now;
         if (av > peak)
            peak = av;
      }

      /* 方向校验: 动起来之后位置只能朝 delta 的符号走 */
      if (pos != last_pos)
      {
         int32_t d = pos - p0;
         if ((d > SM_POS_NOISE_PULSES && delta < 0) ||
             (d < -SM_POS_NOISE_PULSES && delta > 0))
            violations++;
         last_change_ms = now;
         last_pos = pos;
         if (d > SM_POS_NOISE_PULSES || d < -SM_POS_NOISE_PULSES)
            moved = 1;
      }

      /* 结束/失速/超时 */
      if (moved && (now - last_change_ms) >= SM_SETTLE_MS)
         break;
      if (!moved && (now - t0) >= SM_STALL_MS)
      {
         printf("      [失速] 命令发出 %ums 后位置仍无变化 (pos=%d)\n",
                SM_STALL_MS, (int)pos);
         sm_guard_abort(SM_ABORT_STALL);
         return -1;
      }
      if (now >= deadline)
      {
         printf("      [超时] 超过本腿时间预算, 位置 %d (起点 %d)\n",
                (int)pos, (int)p0);
         sm_guard_abort(SM_ABORT_TIMEOUT);
         return -1;
      }

      /* 掉线检测 */
      if (g_ctx.slavelist[ax->slave].islost)
      {
         sm_guard_abort(SM_ABORT_LOST);
         return -1;
      }
   }

   /* 读终点 */
   if (read_i32_or_abort(ax, SM_OID_ACT_POS, &pos) != 0)
      return -1;

   rc = 0;
   g_guard.total_motion_ms += (sm_now_ms() - t0);
   if (g_guard.total_motion_ms > SM_TOTAL_MOTION_MAX)
   {
      printf("      [中止] 累计动作时间 %ums 超过预算 %dms\n",
             (unsigned)g_guard.total_motion_ms, SM_TOTAL_MOTION_MAX);
      sm_guard_abort(SM_ABORT_DEADLINE);
      rc = -1;
   }

   if (p0_out != NULL)    *p0_out = p0;
   if (p1_out != NULL)    *p1_out = pos;
   if (peak_out != NULL)  *peak_out = peak;
   if (ms_out != NULL)    *ms_out = sm_now_ms() - t0;
   if (dir_viol != NULL)  *dir_viol = violations;
   return rc;
}

/*
 * S4: 微动 + 反馈闭环 (去一趟 + 回一趟, 可重复 N 次)
 *
 * 断言:
 *   A1 行程  |实测Δ - 命令Δ| <= max(SM_POS_NOISE_PULSES, 2% × |命令Δ|)
 *   A2 方向  sign(实测Δ) == sign(命令Δ)
 *   A3 速度  峰值速度 > 0           (驱动器真的动了, 反馈不是死的)
 *   A4 限速  峰值速度 <= 设定速度 × 1.2                      (超了记 WARN)
 *   A5 回程  反向腿方向正确
 *   A6 归位  |终点 - 起点| <= 容差    (往返闭环能回到原点)
 *   A7 单调  方向违例次数 == 0                               (有则记 WARN)
 */
int sm_stage_jog(sm_axis_t *ax, int32_t delta, uint32_t vel, uint32_t acc,
                 uint32_t dec, uint32_t move_tmo_ms, int repeats,
                 int32_t tol_pulses)
{
   int     leg;
   int32_t tol;
   int     fail = 0, warn = 0;

   printf("\n  === 位置 %d: 微动与反馈闭环 (S4) ===\n", ax->pos);
   printf("      命令行程 ±%d 脉冲, 速度 %u 脉冲/s, 加/减速 %u/%u\n",
          (int)delta, (unsigned)vel, (unsigned)acc, (unsigned)dec);

   tol = (delta < 0) ? -delta : delta;
   tol = (int32_t)((int64_t)tol * 2 / 100);
   if (tol < SM_POS_NOISE_PULSES)
      tol = SM_POS_NOISE_PULSES;
   if (tol_pulses > 0)
      tol = tol_pulses;

   /*
    * 再兜一次底。调用者已经夹过容差, 但这是个公开入口 —— 容差一旦大于行程
    * 本身, A1/A6 就恒成立, 日志会显示 PASS, 而"动得对不对"其实没被检查。
    * 这类"检查悄悄失效"的失效方式比直接报错危险得多, 所以在最后一道也夹住。
    */
   {
      int64_t a = (delta < 0) ? -(int64_t)delta : (int64_t)delta;
      int64_t cap = a / 4;

      if (cap < SM_POS_NOISE_PULSES)
         cap = SM_POS_NOISE_PULSES;
      if ((int64_t)tol > cap)
      {
         printf("      [上限] 容差 ±%d 超过行程的 1/4, 收紧为 ±%d。\n",
                (int)tol, (int)cap);
         tol = (int32_t)cap;
      }
   }
   printf("      行程容差 ±%d 脉冲\n", (int)tol);

   /* 快照 PP 参数, 收尾要恢复 */
   sm_snapshot_pp(ax);
   printf("      快照: 6081h=%s%u 6083h=%s%u 6084h=%s%u (收尾会恢复)\n",
          ax->have_prof_vel ? "" : "?", (unsigned)ax->prof_vel_snap,
          ax->have_prof_acc ? "" : "?", (unsigned)ax->prof_acc_snap,
          ax->have_prof_dec ? "" : "?", (unsigned)ax->prof_dec_snap);

   ax->p_start = 0;
   for (leg = 0; leg < repeats; leg++)
   {
      int32_t  f0 = 0, f1 = 0, r1 = 0;
      int32_t  peak_f = 0, peak_r = 0;
      uint32_t ms_f = 0, ms_r = 0;
      int      dv_f = 0, dv_r = 0;
      int32_t  d_f, d_r;

      if (repeats > 1)
         printf("\n      ---- 第 %d/%d 轮 ----\n", leg + 1, repeats);

      /* 正向腿 */
      printf("      >> 正向腿: +%d 脉冲\n", (int)delta);
      if (jog_leg(ax, delta, vel, acc, dec, move_tmo_ms, &f0, &f1,
                  &peak_f, &ms_f, &dv_f) != 0)
      {
         fail = 1;
         break;
      }
      d_f = f1 - f0;
      printf("      << 正向结果: %d -> %d (Δ=%d), 峰值速度 %d, 耗时 %ums\n",
             (int)f0, (int)f1, (int)d_f, (int)peak_f, (unsigned)ms_f);

      /* 反向腿 (走回起点) */
      printf("      >> 反向腿: -%d 脉冲\n", (int)delta);
      if (jog_leg(ax, -delta, vel, acc, dec, move_tmo_ms, &f1, &r1,
                  &peak_r, &ms_r, &dv_r) != 0)
      {
         ax->d_fwd = d_f;
         ax->p_start = f0;
         ax->p_fwd_end = f1;
         fail = 1;
         break;
      }
      d_r = r1 - f1;
      printf("      << 反向结果: %d -> %d (Δ=%d), 峰值速度 %d, 耗时 %ums\n",
             (int)f1, (int)r1, (int)d_r, (int)peak_r, (unsigned)ms_r);

      ax->p_start = f0;
      ax->p_fwd_end = f1;
      ax->p_rev_end = r1;
      ax->d_fwd = d_f;
      ax->d_rev = d_r;
      ax->peak_fwd = peak_f;
      ax->peak_rev = peak_r;
      ax->ms_fwd = ms_f;
      ax->ms_rev = ms_r;
      ax->dir_violations = dv_f + dv_r;
      ax->travel_err = d_f - delta;
      ax->home_err = r1 - f0;
   }

   if (fail)
   {
      printf("  === 位置 %d 微动结论: FAIL (动作未能完成, 详见上面的中止原因) ===\n",
             ax->pos);
      return SM_V_FAIL;
   }

   ax->jog_done = 1;

   /* ---- 断言 ---- */
   printf("\n      ---- 断言 ----\n");

   /* A1 行程 */
   {
      int32_t e = ax->travel_err;
      int32_t ae = (e < 0) ? -e : e;
      if (ae > tol)
      {
         fail = 1;
         printf("      [FAIL] A1 行程误差 %d 脉冲 (命令 %d, 实测 %d), 超过容差 %d。\n",
                (int)e, (int)delta, (int)ax->d_fwd, (int)tol);
      }
      else
      {
         printf("      [PASS] A1 行程误差 %d 脉冲 (容差 %d)\n", (int)e, (int)tol);
      }
   }

   /* A2 方向 */
   if (ax->d_fwd != 0 &&
       ((ax->d_fwd > 0 && delta > 0) || (ax->d_fwd < 0 && delta < 0)))
   {
      printf("      [PASS] A2 方向正确 (命令 %+d, 实测 %+d)\n",
             (int)delta, (int)ax->d_fwd);
   }
   else
   {
      fail = 1;
      printf("      [FAIL] A2 方向错误: 命令 %+d 却走了 %+d —— "
             "检查电机接线相序或编码器方向。\n", (int)delta, (int)ax->d_fwd);
   }

   /* A3 峰值速度非零 */
   if (ax->peak_fwd > 0 && ax->peak_rev > 0)
   {
      printf("      [PASS] A3 有速度反馈: 正向峰值 %d, 反向峰值 %d\n",
             (int)ax->peak_fwd, (int)ax->peak_rev);
   }
   else
   {
      fail = 1;
      printf("      [FAIL] A3 全程无速度反馈 (正向 %d, 反向 %d) —— "
             "位置虽变了但 606Ch 始终为 0, 反馈链路可疑。\n",
             (int)ax->peak_fwd, (int)ax->peak_rev);
   }

   /* A4 峰值速度不超过设定值太多 */
   {
      int32_t lim = (int32_t)((int64_t)vel * 12 / 10);
      if (ax->peak_fwd > lim || ax->peak_rev > lim)
      {
         warn = 1;
         printf("      [WARN] A4 峰值速度超过设定值的 120%% (设定 %u, 允许 %d, "
                "实测 %d/%d)\n", (unsigned)vel, (int)lim,
                (int)ax->peak_fwd, (int)ax->peak_rev);
      }
      else
      {
         printf("      [PASS] A4 峰值速度在设定范围内 (设定 %u, 实测 %d/%d)\n",
                (unsigned)vel, (int)ax->peak_fwd, (int)ax->peak_rev);
      }
   }

   /* A5 回程方向 */
   if (ax->d_rev != 0 &&
       ((ax->d_rev > 0 && delta < 0) || (ax->d_rev < 0 && delta > 0)))
   {
      printf("      [PASS] A5 回程方向正确 (实测 %+d)\n", (int)ax->d_rev);
   }
   else
   {
      fail = 1;
      printf("      [FAIL] A5 回程方向错误 (命令 %+d, 实测 %+d)\n",
             (int)-delta, (int)ax->d_rev);
   }

   /* A6 归位 */
   {
      int32_t e = ax->home_err;
      int32_t ae = (e < 0) ? -e : e;
      if (ae > tol)
      {
         fail = 1;
         printf("      [FAIL] A6 往返未回到起点: 起点 %d, 终点 %d, 偏差 %d (容差 %d) —— "
                "可能有累积丢步或反向间隙。\n",
                (int)ax->p_start, (int)ax->p_rev_end, (int)e, (int)tol);
      }
      else
      {
         printf("      [PASS] A6 往返回到起点 (偏差 %d, 容差 %d)\n", (int)e, (int)tol);
      }
   }

   /* A7 方向单调性 */
   if (ax->dir_violations == 0)
   {
      printf("      [PASS] A7 全程方向单调, 无反向抖动\n");
   }
   else
   {
      warn = 1;
      printf("      [WARN] A7 检测到 %d 次方向与命令相反的位置变化 "
             "(可能的过冲/回弹或噪声)\n", ax->dir_violations);
   }

   if (fail)
   {
      printf("  === 位置 %d 微动结论: FAIL ===\n", ax->pos);
      return SM_V_FAIL;
   }
   if (warn)
   {
      printf("  === 位置 %d 微动结论: WARN ===\n", ax->pos);
      return SM_V_WARN;
   }
   printf("  === 位置 %d 微动结论: PASS (行程、方向、闭环归位全部通过) ===\n",
          ax->pos);
   return SM_V_PASS;
}

/* ======================================================================
 * 预演: 打印将要写什么, 一个字节都不写
 *
 * 这是零风险的端到端自检路径 —— 参数解析、身份门、基线比对、S3/S4 的
 * 完整写入序列与断言逻辑都会跑到, 只是不落到总线上。
 * ====================================================================== */
void sm_stage_dry_run(const sm_axis_t *axes, int nslaves, int32_t delta,
                      uint32_t vel, uint32_t acc, uint32_t dec,
                      uint32_t move_tmo_ms, int repeats)
{
   int i;

   printf("\n==================== 预演 (--dry-run, 不写任何字节) ====================\n");
   printf("下面是 S3/S4 在真动模式下**将会**按顺序写入的内容。\n");

   for (i = 0; i < nslaves; i++)
   {
      const sm_axis_t *ax = &axes[i];
      int32_t ad, ramp_pulses;

      if (!ax->is_ykd)
         continue;

      ad = (delta < 0) ? -delta : delta;
      ramp_pulses = (acc > 0) ? (int32_t)((int64_t)vel * vel / (2 * (int64_t)acc))
                              : 0;

      printf("\n  ---- 位置 %d (从站序号 %d) ----\n", ax->pos, ax->slave);

      printf("  S1 参数快照 (只读): 6081h %s, 6083h %s, 6084h %s, 6060h %s\n",
             ax->have_prof_vel ? "已读" : "?",
             ax->have_prof_acc ? "已读" : "?",
             ax->have_prof_dec ? "已读" : "?",
             ax->have_mode ? "已读" : "?");

      printf("  S3 使能状态机:\n");
      printf("     [读] 6041h 初始状态字 (故障位预检)\n");
      if (g_guard.reset_fault)
         printf("     [写] 6040h = 0x%04X (Fault reset) 然后 0x%04X\n",
                SM_CW_FAULT_RST, SM_CW_DISABLE_V);
      printf("     [写] 6040h = 0x%04X (Shutdown)        -> 等 bit0&bit1\n",
             SM_CW_SHUTDOWN);
      printf("     [写] 6040h = 0x%04X (Switch On)       -> 等 bit1, 且 bit3=0\n",
             SM_CW_SWITCHON);
      printf("     [写] 6040h = 0x%04X (Enable Operation)-> 等 bit2  <== 功率级在此打开\n",
             SM_CW_ENABLE_OP);
      printf("     [等] 保持 %ums 确认使能稳定\n", SM_ENABLE_HOLD_MS);
      printf("     [写] 6060h = 1 (PP 模式), 回读 6061h 验证写入生效\n");

      printf("  S4 微动 (±%d 脉冲 × %d 轮, 往返各一次):\n",
             (int)delta, repeats);
      printf("     [写] 6081h = %u  (速度, 原值收尾恢复)\n", (unsigned)vel);
      printf("     [写] 6083h = %u  (加速度)\n", (unsigned)acc);
      printf("     [写] 6084h = %u  (减速度)\n", (unsigned)dec);
      printf("     [写] 607Ah = %+d (相对目标位置; 回读校验)\n", (int)delta);
      printf("     [写] 6040h = 0x%04X (0x0F|bit5 立即生效|bit6 相对, 不含 bit4)\n",
             SM_CW_PP_IDLE);
      printf("     [写] 6040h = 0x%04X (bit4 上升沿 = 触发新设定点)\n",
             SM_CW_PP_TRIGGER);
      printf("     [写] 6040h = 0x%04X (落沿)\n", SM_CW_PP_IDLE);
      printf("     [轮询 5ms] 6041h / 6064h / 606Ch 直到位置静止\n");
      printf("     [写] 607Ah = %+d, 重复上面的触发序列 (反向腿)\n", (int)-delta);

      printf("  S5 收尾 (无条件, 含错误路径):\n");
      printf("     [写] 6040h = 0x%04X (Disable operation)\n", SM_CW_SWITCHON);
      printf("     [写] 6040h = 0x%04X (Shutdown)\n", SM_CW_SHUTDOWN);
      printf("     [写] 6040h = 0x%04X (Disable voltage) <== 电机最终失能\n",
             SM_CW_DISABLE_V);
      printf("     [写] 恢复 6081h/6083h/6084h/6060h 为 S1 快照值\n");

      printf("  时间预算: 理论动作约 %u ms/腿, 单腿超时 %u ms\n",
             (unsigned)((acc > 0 && vel > 0)
                           ? ((uint64_t)vel * 1000u / acc +
                              (uint64_t)ad * 1000u / vel)
                           : 0u),
             (unsigned)move_tmo_ms);
      printf("  行程校验: 加速段占 %d 脉冲, 目标 %d 脉冲 %s\n",
             (int)ramp_pulses, (int)ad,
             (ramp_pulses * 2 >= ad) ? "!! 警告: 加/减速段占比过大, 行程可能不足"
                                     : "(梯形速度曲线, 行程充足)");
   }

   printf("\n  提示: 去掉 --dry-run 并加上 --allow-motion --allow-jog 才会真正执行。\n");
   printf("========================================================================\n");
}
