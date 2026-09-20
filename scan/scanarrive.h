/*
 * scan/scanarrive.h —— 到位 + 稳定判据 (纯逻辑, 不依赖 Qt 与总线): in_op && valid &&
 * mirror_ok && enabled && !fault && at_target && |pos - target| <= tol, 连续成立 >= settle_ms。
 * 不能只看 AxisTelem::at_target: 它是 `want == tgt`, 只说明插值器走完了, 而 doStop() 会把
 * m_want[i] = m_tgt[i], 任何一次「停止」之后它立刻为真。开窗前必须先看到一次
 * at_target == false (下发与下一帧遥测隔 2ms); 前一点离得极近时永远看不到 false,
 * 由 500ms 兜底放行。
 */
#pragma once

#include <cstdint>
#include <string>

namespace scan {

/* 一个 tick 从 AxisTelem 取出来的、判据关心的那几个字段 */
struct ArrivalObs
{
   bool     in_op     = false;
   bool     valid     = false;   /* AxisTelem::valid */
   bool     mirror_ok = false;   /* 短帧时 pos 是陈值, 不能信 */
   bool     enabled   = false;
   bool     fault     = false;
   bool     at_target = false;
   int32_t  pos       = 0;       /* 显示坐标 */
};

class ArrivalJudge
{
public:
   enum class Verdict
   {
      Waiting,    /* 还在走 / 窗口还没够长 */
      Arrived,    /* 到了, 且稳定了 */
      Faulted     /* 不该再等了: 掉出 OP / 故障 / 被失能。why 里写了原因 */
   };

   void begin(int32_t target_pul, int32_t tol_pul, int settle_ms, int64_t now_ms);

   /* 每 tick 喂一次。now_ms 必须是**单调钟** (QElapsedTimer), 不是墙上时间 */
   Verdict feed(const ArrivalObs &o, int64_t now_ms, std::string *why);

   /* 稳定窗口内 pos 的极差 (脉冲), 会落进 CSV */
   int32_t spread() const { return m_have_spread ? (m_max - m_min) : 0; }

   /* 窗口是否已经开 (调试/界面用) */
   bool windowOpen() const { return m_win_start >= 0; }

private:
   void feed_reset_window();

   int32_t m_target   = 0;
   int32_t m_tol      = 50;
   int     m_settle   = 100;
   int64_t m_begin_ms = 0;
   int64_t m_win_start = -1;
   bool    m_seen_moving = false;
   int32_t m_min = 0;
   int32_t m_max = 0;
   bool    m_have_spread = false;
};

}   /* namespace scan */
