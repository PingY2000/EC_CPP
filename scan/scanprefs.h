/*
 * scan/scanprefs.h —— 参数的记忆 (exe 旁边那个 scan.ini): 只记三项 —— 扫描参数 /
 * 上次用的网卡 (Npcap 名) / 手动速度 (pul/s)。其余一概不记: CSV 路径带时间戳, 色标
 * 上下限只属于当次显示, 功率计的波长/量程/模式是设备自己的状态 (打开时向设备读)。
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
