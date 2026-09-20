#include "scanarrive.h"

#include <cstdlib>

namespace scan {

/* 下发新目标后, 最多等这么久就允许开窗 —— 兜住"上一点离得极近、永远看不到 at_target=false" */
static const int64_t GATE_FALLBACK_MS = 500;

void ArrivalJudge::begin(int32_t target_pul, int32_t tol_pul, int settle_ms, int64_t now_ms)
{
   m_target   = target_pul;
   m_tol      = (tol_pul > 0) ? tol_pul : 50;
   m_settle   = (settle_ms > 0) ? settle_ms : 0;
   m_begin_ms = now_ms;

   m_win_start   = -1;
   m_seen_moving = false;
   m_have_spread = false;
   m_min = m_max = 0;
}

void ArrivalJudge::feed_reset_window()
{
   m_win_start   = -1;
   m_have_spread = false;
}

ArrivalJudge::Verdict ArrivalJudge::feed(const ArrivalObs &o, int64_t now_ms, std::string *why)
{
   /* ---- 1. 这三条会永久性地让窗口开不起来, 必须报 Faulted 而不是 Waiting:
    *   !in_op = 总线掉了 / fault = 6041h bit3 / !enabled = 插值目标不推进, want != tgt 恒成立 */
   if (!o.in_op)
   {
      if (why) *why = "总线已掉出 OP";
      return Verdict::Faulted;
   }
   if (o.fault)
   {
      if (why) *why = "驱动器报故障 (6041h bit3)";
      return Verdict::Faulted;
   }
   if (!o.enabled)
   {
      if (why) *why = "轴已被失能 —— 未使能时插值目标不会推进, 再等也不会到";
      return Verdict::Faulted;
   }

   if (!o.valid || !o.mirror_ok)
   {
      feed_reset_window();
      return Verdict::Waiting;
   }

   if (!m_seen_moving)
   {
      if (!o.at_target)
      {
         m_seen_moving = true;
      }
      else if (now_ms - m_begin_ms >= GATE_FALLBACK_MS)
      {
         m_seen_moving = true;      /* 目标本来就在容差内, 放行 */
      }
      else
      {
         /* 这一帧很可能是"写目标之前"的旧快照 */
         return Verdict::Waiting;
      }
   }

   int64_t d = std::llabs((int64_t)o.pos - (int64_t)m_target);
   bool in_window = o.at_target && (d <= (int64_t)m_tol);

   if (!in_window)
   {
      feed_reset_window();          /* 任何一次不满足都从头计时 */
      return Verdict::Waiting;
   }

   if (m_win_start < 0)
   {
      m_win_start   = now_ms;
      m_min = m_max = o.pos;
      m_have_spread = true;
   }
   else
   {
      if (o.pos < m_min) m_min = o.pos;
      if (o.pos > m_max) m_max = o.pos;
   }

   /* 走到这里说明条件此刻仍成立; 落进 CSV 的 spread 是这条判据的可观测证据 */
   if (now_ms - m_win_start >= (int64_t)m_settle)
      return Verdict::Arrived;

   return Verdict::Waiting;
}

}   /* namespace scan */
