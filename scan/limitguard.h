/*
 * scan/limitguard.h —— 限位线 (纯逻辑, 不依赖 Qt)。
 *
 * 「撞到哪一侧的限位, 就把当时的位置记下来; 从此那一侧不许再越过」。三条规则:
 *   1. 记下来的位置**只许往外** (limitNote): 反复贴线试不许把线一寸寸啃进来;
 *   2. 手动目标被夹到界上, 而界**永远不比当前位置更靠里** (limitBound): 一次点击绝不会
 *      产生"往回走"这种没人点过的运动;
 *   3. 界面与控制器共用这一份数学 —— 手动点击那一路与起扫预检那一路说的是同一件事。
 *
 * 记录本身 (每根轴每一侧一个位置 + 零点世代) 在 ScanWindow 里, 落盘在 scanprefs 里;
 * 这个文件只有判据与算术, 所以自检钉得住 (scanwindow.cpp 不在 SCAN_COMMON_SRC 里)。
 *
 * 还有一条**只管显示**的 (2026-09-30, `limitTextNear`): 画布上那两处限位**文字**离得远就
 * 不写。它不夹任何东西、不发任何话, 只是"什么时候值得写" —— 但它同样住在这里, 同样为了自检。
 *
 * **不叫「软限位」**: 软限位是驱动器的 607Dh, 本程序一个字节都不写它
 * (docs/scan_sweep.md §10 那张「全程不写」的表)。这是**上位机侧**的一道护栏。
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace limitguard {

/* 一根轴的**一侧**限位现状。前两个是每拍从遥测来的 (存不住也不必存), 后两个是记下来的。
 *
 * 位置一律是**显示脉冲** (与 AxisTelem::pos 同一个坐标系), 不是 6064h 的原始值
 * —— 与界面、与画布、与那三条限位显示用的是同一套坐标。 */
struct LimitSide
{
   bool    known   = false;   /* 60FDh 读得到 —— 读不到就什么都不知道, 一律不夹 */
   bool    pressed = false;   /* 这一侧的开关此刻触发着 (值已含「上位机侧取反」) */
   bool    has_pos = false;   /* 触发过, 记下过位置 */
   int32_t pos     = 0;       /* 记下的位置; has_pos == false 时无意义 */
};

/* 一根轴的两侧。`pos` = 显示坐标朝正的那一侧, `neg` = 朝负的那一侧 */
struct LimitAxis
{
   LimitSide pos;
   LimitSide neg;
};

/* 记一笔。`side` = +1 正 / -1 负, 0 = 说不出是哪一侧 (调用方不许记)。
 *
 * **只许往外**: 贴着线再点一次, 开关会在同一个点附近再响一次, 每一响都把界往里收一点
 * —— 屏幕上只看到限位区在长大, 谁也不会想到是这个原因。 */
inline void limitNote(int side, LimitSide &s, int32_t at)
{
   if (side == 0)
      return;

   if (!s.has_pos)
   {
      s.has_pos = true;
      s.pos     = at;
      return;
   }

   if ((side > 0) ? (at > s.pos) : (at < s.pos))
      s.pos = at;
}

/* 这一侧的界 (显示脉冲)。**两个来源取更紧的那个**, 并且永远不比当前位置更靠里:
 *   开关此刻还压着 -> 界就是当前位置 (一刻都不许再往外);
 *   记过位置       -> 界是那个位置, 但与当前位置取**更靠外**的那一个 —— 老 ini 里那条线
 *                     可能落在当前位置的内侧, 那时界就成了当前位置;
 *   两样都没有     -> 这一侧的极值 (= 不夹)。
 * `side` 说的是**界在哪一边**: +1 = 正方向那一边, -1 = 负方向那一边。 */
inline int32_t limitBound(int side, bool has_pos, int32_t pos, bool pressed, int32_t cur)
{
   if (pressed)
      return cur;
   if (has_pos)
      return (side > 0) ? std::max(pos, cur) : std::min(pos, cur);
   return (side > 0) ? INT32_MAX : INT32_MIN;
}

/* 手动目标按一侧的界夹。`known == false` (60FDh 读不到) 一律不夹 —— 什么都不知道时,
 * 敢说的唯一一句话是"停"(那由 limit_active 管, 见 ScanWindow::updateLimitGuard),
 * 不是"往那边不许走"。 */
inline int32_t limitClampSide(int side, const LimitSide &s, int32_t cur, int32_t want)
{
   if (!s.known)
      return want;

   const int32_t b = limitBound(side, s.has_pos, s.pos, s.pressed, cur);
   return (side > 0) ? std::min(want, b) : std::max(want, b);
}

/* 一根轴的两侧各夹一次 (先正后负, 顺序不影响结果) */
inline int32_t limitClampManual(const LimitAxis &a, int32_t cur, int32_t want)
{
   want = limitClampSide(+1, a.pos, cur, want);
   want = limitClampSide(-1, a.neg, cur, want);
   return want;
}

/* ---- 「离得够近才写那两句字」 (2026-09-30) ----
 *
 * 画布上有两处跟着限位线走的**文字**: 贴着线那个「X 轴正限位区」标签, 与左上角 HUD 那行
 * 「…限位线在面板外 (… mm) —— 那一侧已锁住」。两处都只在**滑台离那条线 `kLimitTextNearUnit`
 * 以内**时才写 —— 隔得远的时候它们只是噪声 (用户原话: 「只要在接近限位的时候提示就好
 * 距离超过 2mm 时候不要有文字提示」)。
 *
 * **线与线外那片阴影照旧画**: 那是"以后不许越过"的界, 它存不存在与滑台在哪无关
 * (与 drawLimitZones 顶上那段"画的是界, 不是此刻压着"同一个理由)。
 *
 * 单位是**显示单位 (mm)**: 与画布、与界面同一套坐标, 判据不碰脉冲 —— 换算在调用方做,
 * 这一步只有减法、绝对值与一个比较 (所以自检钉得住: mapcanvas.cpp 不在 SCAN_COMMON_SRC 里)。
 *
 * `known` = 位置可信。**不知道位置时一律 false**: 说不出滑台此刻在哪, 就说不出一句"接近"
 * —— 那一句要是照旧写出来, 它说的是上一次断线前的位置。 */
inline constexpr double kLimitTextNearUnit = 2.0;

inline bool limitTextNear(double pos_unit, double line_unit, bool known)
{
   return known && std::fabs(pos_unit - line_unit) <= kLimitTextNearUnit;
}

/* 网格逐轴的 [lo, hi] (显示脉冲) 有没有越过记下的线。越了就写出那一句拒绝理由, 没有就给
 * 空串 —— 与 fitsRange 同型 (纯函数 + 一句给操作员看的中文, 界面与控制器共用同一份)。
 *
 * 只说**现象 + 出路**: 越过的点为什么采不到、该去查什么, 在 docs/scan_messages.md 里。
 * 只报第一条越过的 —— 拒绝理由是一句话, 不是一份清单。
 * 数组都是**两根轴**, 下标 0 = X / 1 = Y (与 armRun 那句"扫描需要正好两根"同一条)。 */
inline std::string limitPlanWhy(const LimitAxis *ax, const int32_t *lo, const int32_t *hi)
{
   if (ax == nullptr || lo == nullptr || hi == nullptr)
      return std::string();

   static const char *kAxisName[2] = {"X", "Y"};

   for (int i = 0; i < 2; i++)
   {
      for (int k = 0; k < 2; k++)
      {
         const int        side = (k == 0) ? +1 : -1;
         const LimitSide &s    = (k == 0) ? ax[i].pos : ax[i].neg;
         if (!s.has_pos)
            continue;

         const int32_t far_pul = (side > 0) ? hi[i] : lo[i];
         if (!((side > 0) ? (far_pul > s.pos) : (far_pul < s.pos)))
            continue;

         char buf[320];
         std::snprintf(buf, sizeof(buf),
            "网格越过了限位线: %s 轴%s限位记在 %lld pul, 网格最远要到 %lld pul。"
            "本次扫描未启动。请把区域改小, 或先回零。",
            kAxisName[i], (side > 0) ? "正" : "负",
            (long long)s.pos, (long long)far_pul);
         return std::string(buf);
      }
   }

   return std::string();
}

}   /* namespace limitguard */
