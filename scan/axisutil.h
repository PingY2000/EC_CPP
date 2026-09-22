/*
 * scan/axisutil.h —— 刻度步长这一个算法, 全仓库只此一份。
 *
 * 原先它是 mapcanvas.cpp 里的一个文件内 static。2026-09-22 加那条时间-功率曲线时它也要挑
 * 纵轴步长, 于是抽到这里 —— 抽出来的理由不是"复用", 是**只留一份实现**: 这种 1/2/5 挑步长
 * 的东西抄第二遍, 两处迟早会挑得不一样, 而"为什么有两个不一样的刻度"没人答得上来。
 *
 * header-only 且只要 <cmath>: mapcanvas.cpp 与 metercurve.cpp 都直接 include, 不新增编译单元。
 */
#pragma once

#include <cmath>

namespace scan {

/*
 * 从 1 / 2 / 5 × 10^k 里挑一个步长, 让 span 大约分成 want 段。
 * 挑"整数"是为了让人一眼读到 0.2 / 0.5 / 100 这种数 —— 按 span/want 直接切会得出
 * 0.037 之类读不出来的值, 数字一多反而更看不懂。
 *
 * span <= 0 或 want < 1 时返回 0.0 (调用方据此判"这轴画不了刻度")。
 */
inline double niceStep(double span, int want)
{
   if (!(span > 0.0) || want < 1)
      return 0.0;

   const double raw = span / (double)want;
   const double p   = std::pow(10.0, std::floor(std::log10(raw)));
   const double m   = raw / p;             /* 落在 [1, 10) */

   const double f = (m <= 1.0) ? 1.0 : (m <= 2.0) ? 2.0 : (m <= 5.0) ? 5.0 : 10.0;
   return f * p;
}

}   /* namespace scan */
