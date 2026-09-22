/*
 * scan/scanstyle.h —— 界面样式表 (QSS) 放在**exe 旁边那个文件**里, 不编进程序
 *
 * 为什么外置: 外观这件事只能靠眼睛调 —— 颜色深一点浅一点、按钮内边距多一点少一点, 编一次
 * 看一次太慢。外置之后改文件 + 界面上按 **F5** 就换过来了 (见 ScanWindow::reloadStyle)。
 *
 * **程序里那份是"缺省", 不是"备份"**: exe 旁边没有 scan.qss 时, styleLoad() 会把这份写出去
 * 再返回它 —— 于是第一次跑完那个文件就在那儿, 可以改。文件在的时候**从不覆盖**, 改坏了想回到
 * 出厂样子就把文件删掉 (与 bin/scan.ini 同一条规矩, 见 README)。
 *
 * 纯 QtCore (QFile/QDir), 与 Qt6::Widgets 无关 —— 与 scanprefs 一样, 想链进自检也不碍事
 * (目前只在 GUI 目标里用)。
 */
#pragma once

#include <QString>

/* exe 旁边的 scan.qss (与 prefsPath() 同一个目录) */
QString stylePath();

/* 程序里那一份 (出厂样式)。给"文件不在"与"文件是空的"两种情况兜底 */
QString styleDefault();

/* 读文件; 没有 (或只有空白) 就返回 styleDefault()。**读不到不是错误**: 样式表缺了照样开机 */
QString styleLoad();
