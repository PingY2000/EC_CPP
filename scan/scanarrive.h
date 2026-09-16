/*
 * scan/scanarrive.h —— 到位 + 稳定判据
 *
 * **纯逻辑, 不依赖 Qt 也不依赖总线。** 它是整个程序里最该单独验的一段: 它决定
 * "这一点的功率算不算数", 判错了不会报错, 只会让整片数据在错的位置上。
 *
 * 为什么不能只用 AxisTelem::at_target:
 *
 *   1. at_target 是 `want == tgt` (hmi/ecatworker.cpp:585), 意思是**插值器自己走完了**,
 *      不是滑台到了。插值目标到位之后驱动器还要追一段。
 *   2. 更要命的是: `doStop()` 会把 `m_want[i] = m_tgt[i]` —— 于是**任何一次「停止」
 *      之后 at_target 立刻为真, 哪怕滑台还在滑**。单靠它会在停止后的惯性段误判到位。
 *
 * 所以判据是"两个都要, 且要连续成立一段时间":
 *
 *   in_op && valid && mirror_ok && enabled && !fault
 *     && at_target                                  // 指令发完了
 *     && |pos - target| <= tol                      // 实读位置也在容差内
 *   连续成立 >= settle_ms
 *
 * 再加一道门: 下发新目标之后, **必须先看到一次 at_target == false** 才允许开窗。
 * 因为下发与下一帧遥测之间隔着 2ms 的周期, 30Hz 取到的头一个快照很可能还是**写之前**的
 * 旧值 —— 那时 at_target 是上一轮的 true, 窗口会立刻开在错的位置上。
 * 前一点离得很近(甚至就是同一个点)时可能永远看不到 false, 所以配一个 500ms 的兜底。
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

   /* 稳定窗口内 pos 的极差 (脉冲)。振没振在这儿看得见, 会落进 CSV */
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
