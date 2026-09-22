/*
 * scan/scanprefs.h —— 参数的记忆 (exe 旁边那个 scan.ini): 扫描参数 / 上次用的网卡 (Npcap 名) /
 * 手动速度 (pul/s) / 回零速度 / 「高级选项」三项 / 色标是不是自动跟随。其余一概不记: CSV 路径
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

   /* ---- 「高级选项」(只有 scan 界面有这三项; hmi 不读不写)。
    * 下面的初值就是"默认值"的唯一定义处: 界面初值 / ini 缺项回落 / 恢复默认都读它 ---- */
   bool    want_dig_in     = true;  /* 让 60FDh 进 TxPDO: 连接时补写 1A00h, 仅 RAM, 收尾还原 */
   bool    npn_write_drive = true;  /* 写驱动器 2300h bit0~bit2 = 1 (常闭); NPN 传感器要的极性, 仅 RAM */
   bool    npn_sw_invert   = false; /* 上位机侧取反: 只换本程序的限位判据, 不写驱动器 */

   /* 色阶自动跟随数据 (见 MapCanvas::setAutoFit)。缺省关 —— 锁定的色阶是能对比两张图的那一种 */
   bool    shade_auto      = false;

   /* ---- 独立功率计窗口那两项 (2026-09-22)。窗口自己读写这两个键, 不经主窗口。
    * 记的是**模式与去处**, 不是数据: 采样间隔是操作习惯, 上一次写到哪个文件是接着写的依据。
    * 取样源本身照旧不记 —— 它的理由与"波长/量程/模式"一样 (那是设备自己的状态)。 ---- */
   int     meter_interval_ms = 200;   /* 「连续读数」的间隔; 超出 MeterLog 的上下限会被它夹回去 */
   QString meter_csv;                 /* 上一次的 CSV 路径; 空 = 开始时按时间戳生成一个 */
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
