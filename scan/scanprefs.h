/*
 * scan/scanprefs.h —— 参数的记忆 (exe 旁边那个 scan.ini): 扫描参数 / 上次用的网卡 (Npcap 名) /
 * 手动速度 (pul/s) / 回零速度 / 回零超时 / **回零偏移 (逐轴一个)** / 「高级选项」三项 /
 * 色标是不是自动跟随。其余一概不记: CSV 路径
 * 带时间戳, 色标**上下限**只属于当次显示, 功率计的波长/量程/模式是设备自己的状态 (打开时向设备读)。
 *
 * 「色标上下限不记」与「色标自动跟随要记」不矛盾: 前者是一个数, 套到下一趟的数据上就是错的;
 * 后者是一个**模式**, 记不住的话每次开程序都要重新勾一遍。
 */
#pragma once

#include <QString>

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

   /* ---- 功率计那一族。**不再是"独立窗口"的**: 那个窗口已经不在了, 功率计现在是参数栏里
    * 一直露在外面的那一块, 这几个键由 ScanWindow 自己读写。
    * 记的是**模式与去处**, 不是数据: 采样间隔是操作习惯, 上一次写到哪个文件是接着写的依据。
    * 波长/量程/模式照旧不记 —— 那是设备自己的状态, 打开时向设备读。 ---- */
   int     meter_interval_ms = 200;   /* 「连续读数」的间隔; 超出 MeterLog 的上下限会被它夹回去 */
   QString meter_csv;                 /* 上一次的 CSV 路径; 空 = 开始时按时间戳生成一个 */

   /* 上次用的那台功率计 (表头序列号, 空 = 没记过 -> 用枚举到的第一台)。
    * **与"波长/量程/模式不记"不矛盾**: 那三项是设备内部的状态, 换一台表就作废;
    * "台面上插着好几台时, 我用的总是这一台"是**操作习惯** —— 与网卡那一条同理。
    * 记的这一台若没插会**打开失败并明说**, 不悄悄退到第一台 (见 ophirmeter.cpp)。 ---- */
   QString meter_serial;
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

}   /* namespace scan */
