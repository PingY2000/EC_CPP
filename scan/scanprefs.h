/*
 * scan/scanprefs.h —— 参数的记忆 (exe 旁边那个 scan.ini): 扫描参数 / 上次用的网卡 (Npcap 名) /
 * 手动速度 (pul/s) / 回零速度 / 回零超时 / **回零偏移 (逐轴一个)** / 「高级选项」三项 /
 * 色标是不是自动跟随 / **色标的单位 (2026-09-29)** / **软件零点 (逐轴一个 + 世代)** /
 * **限位记录 (逐轴逐侧一个位置 + 世代, 2026-09-29)**。
 * 其余一概不记: CSV 路径带时间戳, 色标**上下限**只属于当次显示, 功率计的波长/量程/模式是
 * 设备自己的状态 (打开时向设备读)。
 *
 * 「色标上下限不记」与「色标自动跟随要记」「色标单位要记」不矛盾: 前者是一个数, 套到下一趟
 * 的数据上就是错的; 后两者是**模式** —— 记不住的话每次开程序都要重新勾 / 重新选一遍。
 *
 * 软件零点记进 ini 是 **2026-09-29 改的**, 从前刻意不记 (老理由是"它只是本次运行的东西")。
 * 取舍、代价与那道代价为什么可以接受: docs/scan_sweep.md §39 与 §29.5 的补注。
 */
#pragma once

#include <cstdint>

#include <QString>

#include "ec_motor.h"     /* 只为 EM_MAX_AXES: 下面那个数组按它定长 */
#include "powermeter.h"   /* 只为 SHADE_UNIT_* 那四个模式常量 (下面 shade_unit 的缺省与判据) */
#include "scanplan.h"

namespace scan {

struct Prefs
{
   Params  params;                  /* 缺省见 scanplan.h */
   QString nic;                     /* 上次连接用的网卡 (Npcap 名), 空 = 没记过 */
   int     manual_speed = -1;       /* 手动速度 pul/s; -1 = 没记过 (窗口用 HMI_VEL_DEF) */
   int     home_vel     = -1;       /* 回零速度 6099h:01 (pul/s); -1 = 没记过 (HMI_HOME_VEL_DEF) */
   int     home_tmo_s   = -1;       /* 回零超时 (s); -1 = 没记过 (HMI_HOME_TMO_DEF_S)。
                                     * **单位是秒**: ini 键 / 界面 / Cmd 一律秒, 只在 doHome() 里
                                     * 乘一次 1000 (见 hmi/ecatworker.h 里那三个宏的注释)。 */
   /* 「回零偏移」逐轴一个 (pul): 找原点收尾时把显示坐标 0 放在落点正方向多少个脉冲处。
    * -1 = 没记过 (用 HMI_HOME_OFF_DEF)。 ★ **0 是合法值** (0 = 零点就放在落点上), 所以
    * "没记过"的哨兵只能落在负数那一侧 —— 读它必须过 ecatcmd::home_off_from_pref,
    * 照 home_vel / home_tmo_s 的 `<= 0` 抄会把"操作员存下的 0"读成 75000。 */
   int     home_off_x   = -1;
   int     home_off_y   = -1;

   /* ---- 「高级选项」(只有 scan 界面有这三项; hmi 不读不写)。
    * 下面的初值就是"默认值"的唯一定义处: 界面初值 / ini 缺项回落 / 恢复默认都读它 ---- */
   bool    want_dig_in     = true;  /* 让 60FDh 进 TxPDO: 连接时补写 1A00h, 仅 RAM, 收尾还原 */
   bool    npn_write_drive = true;  /* 写驱动器 2300h bit0~bit2 = 1 (常闭); NPN 传感器要的极性, 仅 RAM */
   bool    npn_sw_invert   = false; /* 上位机侧取反: 只换本程序的限位判据, 不写驱动器 */

   /* 色阶自动跟随数据 (见 MapCanvas::setAutoFit)。缺省关 —— 锁定的色阶是能对比两张图的那一种 */
   bool    shade_auto      = false;

   /* 色标那几个数字用哪个单位 (SHADE_UNIT_*, 见 powermeter.h)。缺省 mW —— 它同时决定
    * **缺省那对上下限**是什么: 界面上那对是"0 … 1 个当前单位" (见 ScanWindow::buildShadePanel)。
    * 老 ini 里没这个键时也落到 mW (缺省就是"缺项的待遇", 不做迁移)。
    * 它是"这台机器怎么看数", 与 shade_auto 同型 —— 不是一个只会属于当趟数据的数。 */
   int     shade_unit      = SHADE_UNIT_MW;

   /* ---- 功率计那一族。**不再是"独立窗口"的**: 那个窗口已经不在了, 功率计现在是参数栏里
    * 一直露在外面的那一块, 这几个键由 ScanWindow 自己读写。
    * 记的是**模式与去处**, 不是数据: 采样间隔是操作习惯, 上一次写到哪个文件是接着写的依据。
    * 波长/量程/模式照旧不记 —— 那是设备自己的状态, 打开时向设备读。 ---- */
   int     meter_interval_ms = 200;   /* 「连续读数」的间隔; 超出 MeterLog 的上下限会被它夹回去 */
   /* 曲线与统计只保留最近多少分钟 (2026-09-29)。**与 meter_interval_ms 同一类**: 窗口开多大
    * 是"这台机器怎么看数" (看变化快慢的活要一小段, 看漂移的活要一大段), 不是一个只属于当趟
    * 数据的数 —— 越界或读坏了由 MeterLog 夹回 1..120, 界面那个旋钮自己也夹一道 */
   int     meter_window_min = 5;
   QString meter_csv;                 /* 上一次的 CSV 路径; 空 = 开始时按时间戳生成一个 */

   /* 上次用的那台功率计 (表头序列号, 空 = 没记过 -> 用枚举到的第一台)。
    * **与"波长/量程/模式不记"不矛盾**: 那三项是设备内部的状态, 换一台表就作废;
    * "台面上插着好几台时, 我用的总是这一台"是**操作习惯** —— 与网卡那一条同理。
    * 记的这一台若没插会**打开失败并明说**, 不悄悄退到第一台 (见 ophirmeter.cpp)。 ---- */
   QString meter_serial;

   /* ---- 软件零点: 断开重连与**重启程序**之后仍要沿用的那个零点 (见 ecatworker.h 的
    * setRememberedOrigin / tryInitOrigin)。
    * 记的是工作线程手里那个 m_origin[] —— **6064h 那套原始坐标**, 不是显示坐标。
    * ★ **"有没有"只能由 zero_naxis 说**: 零点值本身是一个任意 int32, 0 与负数都是合法坐标,
    *   拿某个值当哨兵就会把它误判成"没记过" (与上面 home_off 那条同一个坑, 但那边的哨兵
    *   至少只落在负数一侧, 这边连负数都是合法的, 所以只能另开一个字段)。
    *   半份记录 (缺一项 / 轴数越界) 一律不采纳, 见 prefsLoad —— 缺的那项会按 0 进来。
    * ★ zero_epoch 与那两个**独立**读: 它是一个计数器, 不是那份值的属性。一份零点值读坏了
    *   不该让世代也跟着丢 —— 丢了之后落盘那道"只许往前"的闸会让零点再也写不回来。 */
   int     zero_naxis = 0;                        /* 0 = 没记过; 否则 1..EM_MAX_AXES */
   int32_t zero_origin[EM_MAX_AXES] = {0};
   int     zero_epoch = 0;                        /* 0 = 第一代 (负数才是"没记过") */

   /* ---- 限位记录 (2026-09-29): 每根轴每一侧各一条 —— 撞到哪一侧就把当时的位置记下来,
    * 从此那一侧不许再越过 (见 scan/limitguard.h)。
    * 下标是 [轴][侧], 侧 0 = 正 / 1 = 负 (与显示坐标同向)。
    * 记的是**显示脉冲** (与 AxisTelem::pos 同一个坐标系)。
    * ★ 与 zero_origin 同一条坑: "有没有"只能由 lim_have 说 —— 位置是个任意 int32,
    *   0 与负数都是合法坐标, 拿某个值当哨兵就会把它误判成"没记过"。
    * ★ 但**读的规矩与那份零点值相反**: 零点缺一项要整份丢 (缺的那项会按 0 进来, 而那根轴
    *   会悄悄跑到 6064h 的 0 点上); 这一族是**疏的**, 只记撞过的那几侧, 而"没有记录"就是
    *   它的空值 —— 所以一条读不出来只作废它自己 (见 prefsLoad)。
    * ★ 它与 zero_* 是**一族**: 都**随零点世代作废** (位置记的是坐标系里的一个点, 零点一换
    *   那个数就没有意义了)。与 shade_unit / meter_interval_ms 那种"这台机器怎么看数"
    *   的习惯项不是一类。 */
   bool    lim_have[EM_MAX_AXES][2] = {{false, false}, {false, false}};
   int32_t lim_pos[EM_MAX_AXES][2]  = {{0, 0}, {0, 0}};
   int     lim_epoch = 0;         /* 记下它们时的零点世代; 与 zero_epoch 不同 = 陈的, 整份不采纳 */
};

/* ini 在 exe 旁边 (bin/scan.ini) */
QString prefsPath();

/* 文件不存在 / 某项缺失都不算错 —— 缺的项保持缺省 */
Prefs prefsLoad(const QString &path);

/* 整个文件重写。写不进去不算错 (目录只读 / 盘满 / 被占用) */
void prefsSave(const QString &path, const Prefs &p);

/* 把当前参数并进已读回来的记忆里。过不了 validate() 的当前参数不覆盖旧的;
 * 量程 (range_pul) 一律归零 —— 它由区域算出。 */
void prefsMergeParams(Prefs *store, const Params &cur);

/* ini 里那一串零点文本 -> 一个整数。**不许用 QVariant::toInt()**: 它对 "abc" 给 0, 而 0 是
 * **合法**的坐标值 —— "读成了 0"与"本来就是 0" 必须分得开, 否则一份手改坏的 ini 会静默变成
 * 一份看起来完全正常的零点 (滑台一根轴跑到 6064h 的 0 点上, 屏幕上一点异常都看不出来)。
 * 只认整数字面量 (首尾空白允许), 别的都返回 false。 */
bool originFromText(const QString &s, int32_t *out);

}   /* namespace scan */
