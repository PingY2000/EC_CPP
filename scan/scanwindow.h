/*
 * scan/scanwindow.h —— 扫描主窗口
 *
 * 收集操作 → 转成 EcatThread 的 post/set 系列 → 30Hz 刷遥测。不持有 em_bus_t, 不 include SOEM。
 *
 * 三种角色: EcatThread = 总线与运动 (2ms 插补), 唯一碰 em_bus_t 的线程;
 * ScanController = 扫描时序 (到点 → 停留 → 采样 → 落盘), 活在 GUI 线程, 只经 BusView 说话;
 * ScanWindow = 界面与安全护栏 (按钮形态 / 收尾 / 断连时的自动中止)。
 *
 * 状态机所有时间判断都用单调钟 QElapsedTimer —— 墙上时间会被 NTP 往回拨。
 */
#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include "busview.h"
#include "editgate.h"
#include "ophirmeter.h"
#include "powermeter.h"
#include "scancontroller.h"
#include "scanplan.h"
#include "scanprefs.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QGroupBox;
class QLayout;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace scan {

class MapCanvas;
/* 连续读数器与那条曲线 (见 meterlog.h / metercurve.h)。**只用指针**, 所以头文件里不认识
 * 它们就够 —— 定义在 .cpp 里 include */
class MeterLog;
class MeterCurve;

/* 滚轮闸, 定义在 .cpp 里 */
class WheelNeedsCtrl;

/* spin box 上下箭头那道闸 (windows11 样式下输入框会盖住箭头), 定义在 .cpp 里 */
class SpinArrowGate;

class ScanWindow : public QMainWindow
{
   Q_OBJECT

public:
   /* 信号灯的四种样子。放在头里, 因为窗口要记住每一格上一次画的是什么, 只在真变了才动控件。
    *   Unknown = 灰: 不知道       Off = 灭: 这件事没发生
    *   Ok      = 绿亮: 使能带电 / 原点开关压着    Bad = 红亮: 出事了 */
   enum class Lamp { Unknown, Off, Ok, Bad };

   /* 一块"每根轴一行、每格一盏灯"的网格。两块, 但**排在同一张表上** (见
    * buildAxisPanel): 轴信号 (2 列 使能/故障, 问 6041h) 与限位开关 (3 列 原点/正限位/
    * 负限位, 问 60FDh) —— 列号是各自那组自己的 (AX_ 与 LIM_ 那几个枚举),
    * 表上占哪几列由 buildAxisPanel 决定, 所以这里没有"列数"这个字段。
    *
    * **每格只有灯, 没有字** (2026-09-21 起): 状态由亮暗一位说清 —— 绿亮 = 使能带电 /
    * 原点压着, 红亮 = 故障 / 撞限位, 灭 = 这件事没发生, 灰 = 不知道。原先灯旁边那行
    * "已使能/未使能" 是把同一件事说了两遍, 五列并排时反倒把灯挤得难认。名字仍在表头上,
    * 判据仍在 tooltip 里 (kTip + kLampRule)。状态栏那一对是另一回事: 那里地方宽, 且
    * "有效!"那种话要能一眼扫到 (见 refreshAxisSignals 的最后一节) */
   struct LampGrid
   {
      QLabel  *lamp[2][3] = {};      /* [轴][信号] */
      Lamp     lampLast[2][3] = {};  /* 上一次画的是什么 */
   };

   explicit ScanWindow(QWidget *parent = nullptr);
   ~ScanWindow() override;

protected:
   /* 关窗 = 中止扫描 + 断开: 失能 → 还原映射 → 降 PRE_OP → 关网卡, 必要时弹"可能仍带电" */
   void closeEvent(QCloseEvent *e) override;
   /* 横幅不在布局里 (见 placeBanner): 窗口一变宽就得自己重算位置与折行高度 */
   void resizeEvent(QResizeEvent *e) override;

private:
   /* ---- 界面 ---- */
   void buildUi();
   QWidget *buildTopBar();
   QWidget *buildParamPanel();
   QWidget *buildScanPanel();
   QWidget *buildMeterPanel();
   QWidget *buildShadePanel();
   /* 「轴信号」: 一根轴一行, 一行五盏灯 (使能/故障/原点/正限位/负限位) + 最右边那格
    * 「故障复位」。原先分「轴信号」「限位开关」两个框 (2026-09-20 合成一个) */
   QWidget *buildAxisPanel();
   /* 「原点模式」(驱动器自带的 HM 模式)。独立的一块, 不塞进扫描参数栏 */
   QWidget *buildHomePanel();

   /* ---- 操作 ---- */
   /* 「连接」。不弹确认框, 说明写在按钮 tooltip 上 */
   void onConnectClicked();
   /* 「重连总线」。断开(先卸力) + 重连(重新进 OP, 各轴未使能)。未连接时等同「连接」 */
   void onReconnectClicked();
   /* 「使能」。不弹确认框: 带不带电由按钮文字与灰/亮表示 (已使能时是灰的「已使能」) */
   void onEnableClicked();
   /* 清驱动器的故障位 */
   void onFaultResetClicked();
   /* 「高级选项」那三个勾里任意一个动了 (前两个连接期, 第三个运行期) */
   void onAdvToggled();
   /* 「上位机侧取反」。运行期参数: 勾一下就生效, 不重连、不写驱动器。安全相关 */
   void onDiInvertToggled(bool on);
   void onRestoreDefaults();          /* 「恢复默认」: 扫描参数回 Params 缺省 */
   void onCenterAllClicked();
   void onZeroHereClicked();          /* 「设为区域中心」 */
   void onBrowseCsv();
   void onOpenCsvClicked();           /* 断点续扫 */
   void onStartClicked();
   void onPauseClicked();
   void onResumeRunClicked();
   void onAbortClicked();
   void onRetestClicked();
   /* 设备信息回来了: 真机那三行填选项表 / 当前选中项。**灰不灰与露不露都不归这里管**,
    * 那是 refreshMeterPanel() 每拍算的 */
   void onMeterInfoChanged();
   /* 操作员改了波长/量程/模式 (真机才有那三项) */
   void onMeterCfgChanged();
   /* 操作员换了一台设备 (下拉里那一列序列号)。**与上面那一条不同**: 它列的不是配置而是
    * 哪一台仪器, 所以不能挂在那个 connect 循环上 —— 走这儿: 记下序列号 -> 停采集 -> 清缓冲
    * -> 重开 (openMeter)。切换是**无声地清缓冲**的, 那条 tooltip 就在下拉框上 */
   void onMtrDeviceChanged(int idx);
   /* 「添加」: 把旋钮上那个波长**写进表头**并选中 (设备那张表是只读的, 表里没有的值没有
    * 下标可用 —— 这是本程序唯一一处写设备)。与那三个下拉框同一套: m_cfgBusy + setHold,
    * 靠 infoChanged / configFailed 收尾。写入后会向设备读回一次列表核对。
    * **2026-09-28 晚起没人能按到它了** (§37.12: 那一行在界面上藏起来了) —— 槽留着,
    * 恢复显示就又能用 */
   void onMtrAddWavelength();
   /* 「重试」—— 打开失败、或采集卡死时的唯一出口。与构造函数里那一次打开**走同一条路**
    * (openMeter), 没有第二条打开路径 */
   void onMtrRetryOpen();
   /* 记录: **开始与停止同一个按钮** (按字换名, 见 §37.10) / 清空 / 换输出文件 / 把当前缓冲整份导出。
    * **它们只管文件** —— 采集与曲线与此无关 (2026-09-28 起采集常开, 见 openMeter 的注释) */
   void onMtrRecordToggled();
   void onMtrClearClicked();
   void onMtrBrowseCsv();
   void onMtrExportClicked();
   void onMtrIntervalChanged(int ms);
   /* 「曲线时长」旋钮: 转一下就把新窗口推给 MeterLog (它自己夹值并当场裁一次) */
   void onMtrWindowChanged(int min);
   /* "这几行是哪个仪器什么配置采的" -> MeterLog::setMeta (建文件那一刻写进 CSV 的头几行) */
   void pushMeterMeta();

   /* 「X/Y 正/反向回零」与「X/Y 找正/负限位」共用的槽。
    * dir: 0 = 正那侧, 1 = 负那侧; find_limit: false -> 6098h = 24/29 (找**原点开关** X0),
    * true -> 18/17 (找**限位开关**, 手册叫"找限位")。不弹确认框, 说明在按钮 tooltip 上;
    * 回零中按「停止」= 立即中止。它同时 +1 零点世代 (两者都重定义零点, 续扫必须换世代)。 */
   void onHomeClicked(int axis, int dir, bool find_limit);

   /* 「回零校准」—— 一次对 X 与 Y 都做**正向回零** (方式 24), 两根**同时**动。
    *
    * 它取代不了那八个按钮, 是给"平时那一趟"用的: 用户要的是这块面板平时只外露一个按钮,
    * 八个按钮收进 m_homeDetail 里 (「展开」看得到, 见 buildHomePanel)。
    *
    * 为什么值得一个专门的入口而不是"点两下 X/Y 的正向回零": 逐轴点两下是**串行**的
    * (第一根走完才轮到第二根), 而这个动作在机械上本来就是两根各走各的 —— 串行不是硬件的
    * 限制, 是上位机以前那个阻塞循环造成的 (docs/scan_sweep.md §33)。 */
   void onHomeBothClicked();
   /* 「测量原点宽度」—— 一趟两段, 两根轴一起量 (与「回零校准」同语义: 要么都量, 要么都不动)。
    * 量的是原点信号两个边沿之间的脉冲数, 读数落在 m_lSpan 那一行上, 结论走状态栏。
    * 速度与超时用与「回零校准」**同一对框** (段 1 那个速度就是它, 段 2 取它的 1/4)。 */
   void onSpanClicked();
   /* 「展开 / 收起」—— 只翻 m_homeDetail 的可见性, 不碰任何值。
    * 收起时速度与超时**照样在生效**, 只是看不见; 这是有意的 (见 buildHomePanel 里那段)。 */
   void onHomeFoldToggled();
   /* 「停止」。回零期间它必须变成立即中止 (队列救不了回零) */
   void onStopClicked();

   /* ---- 参数框: 项表 + [保存][取消] ----
    * 一块框里"什么时候不能改"的项登记在 m_panels 里: lock_running = 跑到一半改掉会让落进 CSV
    * 的 (ix,iy) 跟滑台实际站的地方对不上, need_manual = 只在"色阶手动定标"时才可用。
    * 表里没有的控件 (按钮这类动作) 各由刷新它那一处管。
    *
    * **控件的可用性只由 refreshEditability() 一处写** —— 它在 30Hz 的 refresh() 末尾被调,
    * 别处再 setEnabled 会被下一拍覆盖 (写按钮槽里更是当场就被盖掉)。标记与「取消」的灰不灰
    * 也由那一处每拍重算。
    * 功率计那一整块框不在这张表里, 它的判据在 refreshMeterPanel() 里, 由 refreshEditability()
    * 转调 —— 仍然是"一处写", 只是那处自己又调了一个函数。
    *
    * 2026-09-23 撤掉了「编辑」: 控件平时就能改 (只有运行中锁那几项除外), 「保存」把这一框
    * 固化进 ini, 「取消」退回**上次保存的那一份**。**没有编辑态就没有那层门**, 防滚轮误触
    * 归"按住 Ctrl 才认滚轮"那条闸管 (见 WheelNeedsCtrl)。
    * 状态机 (scan/editgate.h) 照旧用着, 只是 `gate.editing` 进门时 begin 一次、之后一直是真
    * —— "这一框可以改"现在恒成立。装回「编辑」按钮时把 begin/drop 接回按钮即可。 */
   struct GateItem
   {
      QWidget *w = nullptr;
      bool     lock_running = false;  /* 运行中也锁住 (几何 / 输出路径这类) */
      bool     need_manual = false;   /* 只在"色阶手动定标"时才可用 (自动跟随时它是多余的) */
   };
   struct PanelItems
   {
      editgate::Gate  gate;             /* editing 恒真 (见上), dirty 由每拍比对算出来 */
      QGroupBox      *box = nullptr;
      QWidget        *bar = nullptr;    /* [保存][取消] 那一行 (框的孩子, 不进布局) */
      QPushButton    *btnSave = nullptr;
      QPushButton    *btnCancel = nullptr;
      QString         title_base;       /* 框标题原文 (标记拼在它后面) */
      QList<GateItem> items;
      QList<QVariant> baseline;         /* 「上次保存」那一份 (= ini 里那一份) */
   };

   QWidget *buildAdvPanel();
   /* 登记一块框: 项表 + 标题原文, 并记住 [保存][取消] 那一行。**那两个按钮不进 items** */
   void addPanel(int pi, QGroupBox *box, const QList<GateItem> &items);
   /* 造 [保存][取消] 那一行。它是**框的孩子、不进框的布局**, 位置由 placePanelBar() 摆在
    * 标题那一行的右端 (所以传的是 box 而不是某个布局) */
   QWidget *panelBar(int pi, QGroupBox *box);
   /* 把这一行的右上角对齐到框标题那一行的右端。标题那一行的下沿 = contentsRect().top()
    * (样式自己留出来的), 所以不用去问样式; 框一变宽就得重摆 —— 见 eventFilter() */
   void placePanelBar(int pi);
   void placePanelBars();
   bool eventFilter(QObject *o, QEvent *e) override;
   /* F5: 重读 exe 旁边那个 scan.qss 并套上去 (外观不用重编译, 见 scanstyle.h) */
   void reloadStyle();
   /* 基线 = 控件此刻的值。三处调: 建完界面 (ini 里那一份)、「保存」之后、连接/关窗落盘之后 */
   void panelCapture(int pi);
   bool panelDiffers(int pi) const;              /* 逐项与基线比对 (幂等: 改回原值就落下去) */
   void panelRevert(int pi);                     /* 控件 ← 基线, 不拦信号: 回退要顺带重新下推 */
   /* 「保存」= 把**这一框**管的字段写进 ini。别的框还没保存的改动不被顺手固化 */
   void panelSavePrefs(int pi);
   void onPanelSave(int pi);
   void onPanelCancel(int pi);
   void refreshEditability();
   /* 功率计那一块框的**全部**判据 (灰不灰 + 哪几行露出来)。只由 refreshEditability() 调 */
   void refreshMeterPanel();
   /* 那一框每拍要跟新的字 (状态行 / 统计 / 计数 / 曲线)。放 refresh() 里, 与可用性分开 */
   void refreshMeterReadout();
   void refreshAdvWarn();           /* 双反相那行红字 */

   /* ---- 内部 ---- */
   void pushParams();                 /* 控件 → Params → 控制器 + 量程 */
   void applyDefaults();              /* 控件 ← Params 缺省 (构造时一次 + 「恢复默认」) */
   /* 记忆 (exe 旁边的 scan.ini)。loadSettings 必须在 applyDefaults 之后调, 反了会被缺省值盖掉 */
   void loadSettings();
   void saveSettings();
   /* 软件零点: scan.ini 里那份 → 工作线程 (在 start() 之前) + 世代接到 m_epoch 上。
    * 名字不叫 loadSettings 的一部分是刻意的 —— 它必须在 start() 之前跑完, 而 loadSettings
    * 是在 buildParamPanel() 里、和控件一起建的 (§39)。 */
   void seedZeroFromPrefs();
   /* 限位记录: scan.ini 里那几条 → m_lim[] (主) + 世代接到 m_limEpoch 上。
    * 与 seedZeroFromPrefs 一同在 start() 之前跑完, 理由相同 (§39)。 */
   void seedLimitFromPrefs();

   void pushManualSpeed(const BusTelem &t, bool running);
   void refreshAxisSignals(const BusTelem &t);   /* 限位/使能/故障: 状态栏 + 参数栏, 一份遥测 */
   /* 限位守卫 (2026-09-29, 见 limitguard.h): 记线 + 撞到就停。挂在 refreshAxisSignals 之后 ——
    * 它要用那条红横幅刚算完的 m_limBanner[] 才知道"这一句说过了没有" */
   void updateLimitGuard(const BusTelem &t);
   /* 把 m_lim[] 推到两个收的人: 控制器 (起扫前那道闸) 与画布 (那条线与阴影)。
    * **一处出、两处进** —— 少推一处就会出现"界面说锁住了而画布上还是白的" */
   void pushLimitLines();
   void setSignalCell(LampGrid &g, int i, int s, bool known, bool on, Lamp lit);
   Params currentParams() const;
   void refresh();                    /* 30Hz: tick 状态机 + 刷遥测 + 刷按钮可用性 */
   void setConnected(bool on);
   void hint(const QString &s, bool fault);
   /* 把横幅摆到画布顶上那一层 (它不在布局里, 所以位置与折行高度都得自己算) */
   void placeBanner();
   void showFault(const QString &why);   /* 自动中止: 红色横幅 + 模态 (无人值守时不会错过) */
   void warnMaybeLive();
   void disconnectAndStop();
   void syncShadeEdits();
   void syncShadeAuto();                   /* 自动跟随: 画布的色阶 -> 两个输入框 (屏蔽信号) */
   void applyShadeAutoUi(bool on);         /* 自动跟随开着时两个框是"显示"不是"输入" */
   /* 色标数字的单位 (SHADE_UNIT_*)。它**只改显示**: 原始上下限与颜色一个字都不动,
    * 见 scanwindow.h 里色标那一段的 ★ */
   int  shadeUnitMode() const;
   void applyShadeUnitUi();                /* 把当前单位推到画布与那一行只读字上 */
   void onShadeUnitChanged();
   /* 「最小」「最大」是一对: 改一个顶到另一个头上时把另一个推过去 (跨度保留), 见实现 */
   void onShadeLoChanged(double lo);
   void onShadeHiChanged(double hi);
   void applyCsvDefaultName();
   /* 连续读数那份 CSV 的文件名留空时按时间戳起一个 (与 applyCsvDefaultName 是两件事) */
   void applyMtrCsvDefaultName();

   /* ---- 功率计的打开 ----
   * **唯一的打开入口**, 构造函数与「重试」都走它。它做三件事: 两道"装没装"的闸 (不阻塞)、
   * m_meter->open() (阻塞, 正常几百 ms, 最坏 12 s)、按成败摆状态。
   *
   * 打开成功之后**这里不 start() 采集** —— 采集是"源开着就该在采"的每拍不变量, 写在
   * refresh() 那一处 (全仓库唯一写点), 见那段注释。任何来源的一次意外 stop() 都会在
   * 下一拍被纠回来, 包括这里的 m_mlog->stop()。 */
   void openMeter();
   /* 真机打不开那两句原因 (纯注册表查询, 不阻塞)。空 = 可以试着开 */
   static QString ophirOpenWhy();

   /* ---- 三件套 ---- */
   EcatThread       *m_thr  = nullptr;
   EcatBusView      *m_busv = nullptr;
   ScanController   *m_ctl  = nullptr;
   MapCanvas        *m_canvas = nullptr;

   /* 功率计的那一个源。**m_meter 在构造里定死一次, 此后不再改变** —— 界面上已经没有换源的
    * 地方了 (onMeterChanged 随下拉框一起删了)。
    *
    * 默认构建下它就是 m_ophir。三个模拟源留下来只为一个用途: 编译开关
    * `SCAN_ALLOW_SIM_METER` (CMakeLists, 默认 OFF) 打开时 m_meter 指向 m_random, 于是
    * **没有硬件也能把整条扫描流水线跑通**。类本身在 powermeter.h —— selftest.cpp 的
    * test_meter_sources() 直接构造它们三个, 与界面无关, 所以它们一个都不能删。
    *
    * m_ophir 有自己的工作线程, 还会主动报 infoChanged。**它只有这一个实例**: Ophir 表头是
    * 独占的 (第二个实例打开同一个头会 0x80040201), 所以不能有第二个 OphirMeter */
   ManualMeter *m_manual = nullptr;
   RandomMeter *m_random = nullptr;
   ScriptMeter *m_script = nullptr;
   OphirMeter  *m_ophir  = nullptr;
   PowerMeter  *m_meter  = nullptr;

   /* ---- 采集与记录那一版 ----
    * m_mlog 常驻, 且**采集常开** —— "源开着就该在采"是 refresh() 里每拍重算的不变量,
    * 不是一次按下去的事件。
    * 两方仲裁 (扫描 / 采集) 的唯一写点也是 refresh() —— 全在那一个函数里, 因为接口约定
    * "同一时刻只允许一个未决请求" (powermeter.h:28-31)。
    * 「读一次」那第三方 2026-09-28 删了 (见 docs/scan_sweep.md §36)。 */
   MeterLog   *m_mlog  = nullptr;
   MeterCurve *m_curve = nullptr;

   /* ---- 顶栏 ---- */
   QComboBox   *m_nic       = nullptr;
   QPushButton *m_btnNic    = nullptr;
   QPushButton *m_btnConn   = nullptr;
   /* 「重连总线」—— 断开(先卸力) + 重连(重新进 OP)。AutoRecover 救不回来时的出口,
    * 从前这条路只能靠人自己悟 ("断开连接后重新连接, 可以复位") */
   QPushButton *m_btnReconn = nullptr;
   QPushButton *m_btnEnable = nullptr;
   QPushButton *m_btnStop   = nullptr;
   QPushButton *m_btnDis    = nullptr;
   QPushButton *m_btnCenter = nullptr;
   QPushButton *m_btnZero   = nullptr;

   /* ---- 参数 ---- */
   QDoubleSpinBox *m_edAreaX = nullptr;
   QDoubleSpinBox *m_edAreaY = nullptr;
   QDoubleSpinBox *m_edRes   = nullptr;
   QDoubleSpinBox *m_edPpu   = nullptr;
   QSpinBox       *m_edSpeed = nullptr;   /* 扫描速度: 由 ScanController::start 下发 */
   QSpinBox       *m_edManSpeed = nullptr;/* 手动速度: 点画布 / 全部回中用 */
   QSpinBox       *m_edDwell = nullptr;
   QSpinBox       *m_edSettle = nullptr;
   QSpinBox       *m_edSamples = nullptr;
   QComboBox      *m_cbDir   = nullptr;
   QComboBox      *m_cbMode  = nullptr;
   QLineEdit      *m_edCsv   = nullptr;
   QPushButton    *m_btnDef  = nullptr;   /* 恢复默认 */

   QLabel *m_lGrid = nullptr;
   QLabel *m_lEst  = nullptr;
   QLabel *m_lWarn = nullptr;      /* 参数不合法 / 超量程 的那行红字 */

   /* ---- 原点模式 ---- */
   /* 6099h:01 找原点速度 (八个按钮共用)。上下限 = 与工作线程夹取共用的那一对宏; 正文在 tooltip */
   QSpinBox    *m_edHomeVel = nullptr;
   /* 等 6041h bit12 的超时, 单位**秒**, 八个按钮共用。上下限 = 与工作线程夹取共用的那一对宏;
    * 正文在 tooltip。它不是驱动器参数, 是上位机的耐心 —— 所以它没有 6099h 那种"哪一份在生效" */
   QSpinBox    *m_edHomeTmo = nullptr;
   /* 「回零偏移」逐轴一个 (pul): 找原点收尾时把显示坐标 0 放在落点正方向多少个脉冲处。
    * 上下限 = 与工作线程夹取共用的那一组宏; **0 是合法值** (0 = 落点即零点), 所以 ini 里
    * "没记过"用 -1, 读回来必须过 ecatcmd::home_off_from_pref。它不写驱动器 607Ch */
   QSpinBox    *m_edHomeOff[2] = {};    /* [轴] */
   /* 「原点模式」框里就是这两组按钮, 一行一根轴: [轴][0/1] 找原点开关 (方式 24/29),
    * 左边两个按钮; 右边两个找限位开关 (方式 18/17)。文字见 kHomeBtnText */
   QPushButton *m_btnHome[2][2] = {};   /* [轴][方向] 0 = 正向回零, 1 = 反向回零 */
   QPushButton *m_btnLim[2][2] = {};    /* [轴][侧] 0 = 找正限位, 1 = 找负限位 */
   /* 「回零校准」: 一次把 X 与 Y 都按方式 24 (正向找原点) **同时**发起 */
   QPushButton *m_btnHomeBoth = nullptr;
   /* 「测量原点宽度」: 先按方式 24 把两根回到原点开关上, 再顺着正向慢慢走过去, 量出这条
    * 信号从进入到退出跨了多少脉冲。判据与 m_btnHomeBoth 同源, 再叠加"60FDh 读得到" ——
    * 采不到那条信号, 这一趟什么也量不出来, 所以要在**发起之前**就拦住 (而不是等它跑完)。 */
   QPushButton *m_btnSpan = nullptr;
   /* 那一行读数 (只读)。**不进 addPanel 的 GateItem 表**: 它是读数不是值, 表管的是"能不能改"。
    * 文案只由 ecatcmd::span_readout() 一处出 —— 自检能钉住它, 写在这里的那份钉不住。 */
   QLabel      *m_lSpan = nullptr;
   /* 那八个按钮 + 速度 + 超时所在的那个容器。**收起 = 整个不可见** (QLayout 会自动收掉它
    * 占的行), 展开 = 回到今天这个样子。收起时里面那几个值**照样在生效** —— 它们是"正在用的
    * 参数", 不是"存档" (见 buildHomePanel 里那段)。 */
   QWidget     *m_homeDetail = nullptr;
   /* 「展开 / 收起」开关。文字随状态变 (收起时写「展开」), 所以它自己的字由
    * onHomeFoldToggled() 一处写 —— 别处再写会被那一次覆盖 */
   QPushButton *m_btnHomeFold = nullptr;
   /* 我们发出去的那条"正在回零"横幅的原文, 下降沿靠它认现在挂着的是不是我们自己那条 */
   QString      m_homeBanner;

   /* ---- 扫描控制 ---- */
   QPushButton *m_btnStart  = nullptr;
   QPushButton *m_btnPause  = nullptr;
   QPushButton *m_btnResume = nullptr;
   QPushButton *m_btnAbort  = nullptr;
   QPushButton *m_btnRetest = nullptr;
   QPushButton *m_btnOpen   = nullptr;
   QLabel      *m_lProg     = nullptr;
   QLabel      *m_lTime     = nullptr;

   /* ---- 色标 ----
    * 这一框在可用性表里 (PI_SHADE): 里面有两个要记进 scan.ini 的模式 (「自动跟随」与「单位」,
    * 上下限那两个数不记) 和一个只在手动定标时才该按的按钮 (「按数据定标」), 那几条判据得有
    * 个住处。
    *
    * 两个数是一对, 得一起保证 min < max, 规则是**推着走**: 改一个顶到另一个头上, 就把另一个
    * 一起推过去并保留原来的跨度 (见 onShadeLoChanged / onShadeHiChanged)。
    *
    * ★ **那两个数在屏幕上是什么单位, 由「单位」那一项定** (2026-09-29): 控件里放的是**显示
    *   值** (原始值 / shadeUnitDivisor), 画布里面那份是**原始值**。两边的换算只在下面这五处
    *   做 (onShadeLo/HiChanged「×d」、syncShadeEdits「/d」、syncShadeAuto「/d」、
    *   panelRevert 尾那一句「×d」、buildShadePanel 初值「/d」), **漏一处就是静默错一位数**。 */
   QDoubleSpinBox *m_edShadeLo   = nullptr;
   QDoubleSpinBox *m_edShadeHi   = nullptr;
   /* 「单位」四项 (随取样源 / W / mW / μW) —— **下标就是 SHADE_UNIT_***, 顺序不许动 */
   QComboBox      *m_cbShadeUnit = nullptr;
   /* 实际生效的是哪个单位 (只读)。随取样源模式下它跟着源每拍变, 由 refreshMeterReadout() 写 */
   QLabel         *m_lShadeUnit  = nullptr;
   QPushButton    *m_btnFit      = nullptr;
   QCheckBox      *m_cbShadeAuto = nullptr;
   QLabel         *m_lblLocked   = nullptr;   /* 锁定模式那两句说明 (两行, 按模式显隐) */
   QLabel         *m_lblAuto     = nullptr;

   /* ---- 功率计 ----
    * **这一块框不在可用性表里** (2026-09-22 起): 它里面没有"参数", 只有一台随时可以接上/断开的
    * 仪器, 而它的用处恰恰是"还没连总线, 先把真机打开"。所以它不需要先过一个门。判据全在
    * refreshMeterPanel() 一处, 与其余控件同一条规矩 (每拍重算, 不缓存)。
    *
    * 2026-09-28 起**取样源只剩真机一个**, 下拉框与各源参数行连同「读一次」一起删了
    * (见 docs/scan_sweep.md §36)。面板只剩三件事: 仪器状态 / 实时读数 / 记录。 */
   QLabel      *m_lMeter    = nullptr;   /* 状态行: kind · 已打开/没打开 · 原因或真机摘要 */
   /* 「重试」。打开失败、以及采集卡死 (MeterLog::timedOut) 时唯一的出口 ——
    * 那两条路原来各靠换源与「停止」→「开始」出去, 随面板简化一起没了 */
   QPushButton *m_btnMtrRetry = nullptr;
   bool         m_mtrOpening  = false;   /* 打开中: 防重入 (嵌套事件循环能造出重入) */
   bool         m_mtrTried    = false;   /* 试过一次(不论成败) —— 失败且没打开才露「重试」 */
   /* 上一次打开失败的原因 (openMeter 写, 状态行读)。**存下来而不是每拍现问**:
    * 它有两个来源 —— 打开前那两道注册表闸, 与 open() 自己回的那句错误 —— 后者只在失败
    * 那一瞬间拿得到。成功时清空 */
   QString      m_mtrWhy;
   /* 改波长/量程/模式期间让采集让位。**它是一段有头有尾的窗口**, 由工作线程回话关闭
    * (infoChanged 或 configFailed); 工作线程自己死了就两条都到不了 —— 那时采集会一直停着,
    * 界面上"采集中"的字还在而数不涨。这个缺口写在这里, 见 refreshMeterPanel 的说明 */
   bool         m_cfgBusy     = false;
   /* 真机那三项。选项表由设备给 (探头不同, 能选的波长与量程就不同), 一个都不写死。
    * 每一项没有单独的"那一行"要露/藏: 整块 m_devBox 一起显隐, 而某一项设备根本没有时
    * 它是**空的 + 灰的** (判据在 refreshMeterPanel 里) */
   QComboBox *m_cbWl = nullptr, *m_cbRange = nullptr, *m_cbMeasMode = nullptr;
   /* 诊断那一行 (ROM 版本 / 探头类型 / 驱动报的两个版本号)。
    * **设备的身份 (型号 + 序列号) 不在这里** —— 它现在只由「设备」下拉说 (§37.9/§37.10);
    * 这一段在设备没读回来、两个版本号也取不到时是**空的**, 那时整行藏起来 (见 onMeterInfoChanged) */
   QLabel    *m_lDevInfo = nullptr;
   /* 真机那一块 (波长/量程/模式 + 设备信息), 只在真机开着时才露 */
   QWidget *m_devBox = nullptr;
   /* 填充那三个下拉框时挡掉信号: 每 addItem 一次都会被当成操作员改配置 (一串 stop/set/start) */
   bool m_meterCfgQuiet = false;

   /* ---- 设备那一行 ----
    * **常驻**, 不在 m_devBox 里, 也不在 m_mtrCfgQuiet 那套信号闸的保护范围内: 它列的是
    * "哪一台仪器" (ScanUSB 给的序列号), 换成它等于换一台设备而不是改它的配置, 所以它**另接**
    * onMtrDeviceChanged, 不进那三个框的 connect 循环。枚举结果为空时它是灰的 (refreshMeterPanel)
    *
    * **每一项的 data 才是序列号, 字只是给人看的** (字是「探头 (s/n: …) · Juno (s/n: …)」那种
    * 说法, 见 ophirmeter.h 的 deviceLabel / §37.9): 认设备、写 ini 一律走 data
    *
    * 它的值也是**唯一**进 scan.ini 的那一项 (meter/serial): "上一次用的是这一台"是操作习惯,
    * 与波长/量程/模式那种设备内部状态不同 (见 scanprefs.h) */
   QComboBox *m_cbMtrDev = nullptr;

   /* ---- 自定义波长 (**2026-09-28 晚起在界面上藏起来了**, 见 §37.12) ----
    * 设备给的波长表是只读的, 表里没有的值**根本没有下标可用**, 所以只有"写进设备"这一条路
    * (ophirmeter.h 的 addCustomWavelength)。范围的两个端点只写一遍: 这里的量程与工作线程
    * 的越界拒绝都读 ophirmeter.h 里那两个常量。
    * **下面这三样都还在、还在算, 只是那一行不显示** (保留但不露): 恢复显示 = 删掉
    * buildMeterPanel 里那两句 setVisible(false), 别处一行都不用动 */
   QSpinBox    *m_sbWlAdd  = nullptr;
   QPushButton *m_btnWlAdd = nullptr;
   /* 正在加的那个波长 (nm), -1 = 没有这一件事在等着回话。
    * **为什么要有它**: 成功那条回话是 infoChanged, 而那条信号"改了波长/量程/模式"也用 ——
    * 不记着这一趟是"加波长", 就没法只在那一种情形下报"已加入" (报错了更糟: 每次改配置成功
    * 都会冒出一句)。失败那条回话 configFailed 会把它清掉 */
   int          m_wlAddNm  = -1;

   /* ---- 采集与记录 (见 meterlog.h) ----
    * **采集常开**, 记录是另一个动作: 那一个按钮只管那份 CSV, 采集与曲线照旧跑 */
   QSpinBox    *m_edMtrInterval  = nullptr;
   /* **开与关共用这一个按钮** (§37.10): 没在写文件时写「开始记录」, 写着了写「停止记录」。
    * 名字与可用性每拍由 refreshMeterPanel() 重算 —— 别处不要再动它 */
   QPushButton *m_btnMtrRec      = nullptr;
   QPushButton *m_btnMtrClear    = nullptr;
   QPushButton *m_btnMtrExport   = nullptr;
   QLineEdit   *m_edMtrCsv       = nullptr;
   QPushButton *m_btnMtrCsv      = nullptr;
   /* 上面那四件 (路径框 / 「…」/ 「导出当前缓冲」/ 「开始记录」) 与它们那一行, 2026-09-29 起
    * **藏在代码里** (§41.1): m_csvBox 装着 CSV 那一整行, 它与记录那个按钮各有一句
    * setVisible(false)。**控件与槽一个都没删** —— 恢复显示 = 删掉那两句, 别处一行不用动 */
   QWidget     *m_csvBox         = nullptr;
   /* 「曲线时长」(分钟): 曲线与统计只留最近这一段, 更旧的从内存里真丢掉。
    * 量程与夹值住在 MeterLog 里 (kMinWindowMinutes … kMaxWindowMinutes), 这里只照抄 */
   QSpinBox    *m_sbMtrWindow    = nullptr;
   QLabel      *m_lMtrCount      = nullptr;   /* 状态那一行: **只在有事可说时才有字** ——
                                               * 卡住 / 跟随扫描中 / 已写入 N 行, 三件都没有
                                               * 时整行藏起来 (2026-09-28 §37.11)。原来开头的
                                               * 「采集中 ·」与「缓冲 N 点」都去掉了 */
   QLabel      *m_lMtrLast       = nullptr;   /* 最近一次读数 (大字号, 按量级换前缀) */
   QLabel      *m_lMtrStats      = nullptr;   /* 一行: 缓冲 N 点 + 最小 + 最大 (§37.11) */

   /* ---- 「轴信号」那块表的两个网格 (见 LampGrid)。同一张表上左右排开, 前两格归
    * m_axGrid, 后三格归 m_limGrid ---- */
   LampGrid m_axGrid;    /* 0=使能 1=故障 */
   LampGrid m_limGrid;   /* 0=原点 1=正限位 2=负限位 */

   QPushButton *m_btnFaultRst = nullptr;   /* 「轴信号」那两行最右边一格, 跨两根轴 */

   /* ---- 「高级选项」那三个勾 (见 buildAdvPanel) ---- */
   QCheckBox   *m_cbWantDigIn = nullptr;   /* 让 60FDh 进 TxPDO (连接期参数), 默认开 */
   QCheckBox   *m_cbNpnWrite = nullptr;    /* 写驱动器 2300h = 0x0007 (连接期参数), 默认开 */
   /* 上位机侧取反。安全相关: 开着时限位判据整个换掉 (limit_rule_for), 默认关 */
   QCheckBox   *m_cbDiInvert = nullptr;
   QLabel      *m_lAdvWarn = nullptr;      /* 双反相那行红字 (见 refreshAdvWarn) */

   /* ---- 参数框: 项表 + [保存][取消] ---- */
   QList<PanelItems> m_panels;             /* 下标 = 框号, 见 scanwindow.cpp 顶上那几个 PI_ */
   /* 「取消」正在回灌控件。回灌时那对色阶值会因为"先写 lo、此时 hi 还是新值"错配一下, 而
    * 那条横幅说的是"操作员把数改坏了" —— 程序自己回灌不该弹它 (见 onShadeLoChanged) */
   bool m_panelRevert = false;
   QPushButton     *m_btnCsv = nullptr;    /* 「扫描参数」的 CSV「…」(进可用性表, 故不能是局部量) */
   /* 上一次推给工作线程的两个连接期参数: 只用来认出"勾了但本次连接不生效"这个情形 */
   bool m_advLastWantDig  = true;
   bool m_advLastNpnWrite = true;


   /* ---- 状态栏 ---- */
   QLabel *m_banner = nullptr;
   QLabel *m_lNote  = nullptr;
   QLabel *m_lWkc   = nullptr;
   /* 限位判据 (默认 = 6041h bit11「硬件限位信号有效」), 一根轴一个, 扫描中成立即自动中止, 故必须常显。
    * 措辞是「有效!」而非「撞上」: 该位就是限位信号的电平, 回零时它本来就该是 1。 */
   QLabel *m_lampX  = nullptr;
   QLabel *m_lampY  = nullptr;
   QLabel *m_lLimX  = nullptr;
   QLabel *m_lLimY  = nullptr;

   /* ---- 状态 ---- */
   QTimer        *m_tick        = nullptr;
   QTimer        *m_bannerTimer = nullptr;
   QElapsedTimer  m_clock;

   /* 滚轮闸 (类体在 .cpp 里): 挂在参数输入框及其子控件上的事件过滤器。
    * 用具体类而非 QObject*, 因为装闸时要调它的 guard() */
   WheelNeedsCtrl *m_wheelGuard = nullptr;

   /* 上下箭头闸 (类体在 .cpp 里): 同上, 挂在每个 spin box 内部那个输入框上 */
   SpinArrowGate  *m_spinArrows = nullptr;

   /* 上次用的网卡 (从 scan.ini 读回, 可能已不在机器上)。适配器清单异步到, 故先存着等 adaptersListed */
   QString        m_savedNic;

   /* 零点世代。**由工作线程维护**(它在真写 m_origin[] 的那三处 +1), 这里只跟着遥测往前同步
    * (ecatcmd::origin_epoch_sync, 在 refresh() 里)。只有出 CSV 表头这一个用处。
    * 连接**不再**推它 —— scan 现在跨重连沿用零点, 连接本身不动零点。 */
   int  m_epoch     = 0;
   /* 上次递给工作线程的量程。只在数值真变了才 postRange, 否则状态栏会被逐个按键的回执刷屏 */
   int32_t m_last_range = 0;
   bool m_connected = false;
   bool m_faultShown = false;
   /* 故障横幅上**已经说过**的那个 603Fh (一根轴一格)。初值 = HMI_FAULT_CODE_UNREAD ——
    * 哨兵值都是负数, 不会与任何真实读数撞上。用处: 码是故障沿**之后**才到的, 到了要重弹一次
    * 横幅, 而"弹过了没有"只能按轴存着, 否则 30Hz 每帧都弹 */
   int  m_faultCodeShown[2] = {HMI_FAULT_CODE_UNREAD, HMI_FAULT_CODE_UNREAD};
   bool m_limShown[2] = {false, false};   /* 限位横幅的上升沿防重入, 一根轴一个 */

   /* ---- 限位记录 (2026-09-29): 撞到哪一侧就把当时的位置记下来, 从此那一侧不许再越过。
    * **这里是唯一的一份** —— 落盘走 scanprefs, 控制器与画布由 pushLimitLines() 推过去。
    *
    * ★ 与 m_limShown 是**两件事, 不能合并**: m_limShown 在 !known 那一帧会被清掉
    *   ("不知道了就重新上膛"), 拿它当 latch 会在丢一帧之后、滑台正往回撤的半路上
    *   再发一次 postStop, 把那一次撤退冻住。m_limLatch 只在**信号真的松开**时清零。
    * ★ m_lim[i].pos/neg.known 与 pressed 每拍从遥测刷 (存不住也不必存);
    *   has_pos/pos 是记下来的, 只随零点世代作废。 */
   limitguard::LimitAxis m_lim[2];
   int  m_limEpoch = 0;                        /* 记下它们时的零点世代 */
   bool m_limLatch[2] = {false, false};        /* "这一侧这一趟已经处理过", 松开才重新上膛 */
   /* 我们发出去的那条限位横幅原文。下降沿靠它认现在挂着的是不是我们自己那条: 是才清, 不是不能动 */
   QString m_limBanner[2];
   /* 「通讯」红横幅 (t.comm_bad)。**总线级, 不是一根轴一个** —— 帧不够是整条总线的事,
    * 所以不像限位那样按轴存两份。与限位同一套做法: 上升沿弹一次 (不自动消失),
    * 下降沿按原文比对撤掉自己那一条 */
   bool    m_commShown = false;
   QString m_commBanner;
   bool m_warnedLive = false;
   int  m_autoStop   = 0;     /* 自动中止弹窗的防重入 */
   QString m_last_dir;
};

}   /* namespace scan */
