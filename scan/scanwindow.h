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
   /* 「取样源」。**不连总线也要能选真机**, 所以它不受任何门管 —— 那一整块框的判据全在
    * refreshMeterPanel() 一处 (见它的注释) */
   void onMeterChanged(int idx);
   /* 设备信息回来了: 真机那三行填选项表 / 当前选中项。**灰不灰与露不露都不归这里管**,
    * 那是 refreshMeterPanel() 每拍算的 */
   void onMeterInfoChanged();
   /* 操作员改了波长/量程/模式 (真机才有那三项) */
   void onMeterCfgChanged();
   void onManualValueChanged(double v);
   /* 脚本: 框里的文本与真正读的那份表是两份东西, 同步点只有 pushScriptPath() 一处 */
   void onBrowseScript();
   bool pushScriptPath();
   /* 「读一次」。与 ScanController、以及连续读数器抢同一个未决请求, 三方仲裁在 refresh()
    * 一处; 靠 m_readPending 认出回调是不是自己那一份 */
   void onReadOnceClicked();
   void onReadOnceReady(double watts);
   void onReadOnceFailed(const QString &err);
   /* 连续读数: 开始 / 停止 / 清空 / 换输出文件 / 把当前缓冲整份导出 */
   void onMtrStartClicked();
   void onMtrStopClicked();
   void onMtrClearClicked();
   void onMtrBrowseCsv();
   void onMtrExportClicked();
   void onMtrIntervalChanged(int ms);
   void onMtrAvgChanged(int n);
   /* "这几行是哪个仪器什么配置采的" -> MeterLog::setMeta (建文件那一刻写进 CSV 的头几行) */
   void pushMeterMeta();

   /* 「X/Y 正/反向回零」与「X/Y 找正/负限位」共用的槽。
    * dir: 0 = 正那侧, 1 = 负那侧; find_limit: false -> 6098h = 24/29 (找**原点开关** X0),
    * true -> 18/17 (找**限位开关**, 手册叫"找限位")。不弹确认框, 说明在按钮 tooltip 上;
    * 回零中按「停止」= 立即中止。它同时 +1 零点世代 (两者都重定义零点, 续扫必须换世代)。 */
   void onHomeClicked(int axis, int dir, bool find_limit);
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

   void pushManualSpeed(const BusTelem &t, bool running);
   void refreshAxisSignals(const BusTelem &t);   /* 限位/使能/故障: 状态栏 + 参数栏, 一份遥测 */
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
   /* 「最小」「最大」是一对: 改一个顶到另一个头上时把另一个推过去 (跨度保留), 见实现 */
   void onShadeLoChanged(double lo);
   void onShadeHiChanged(double hi);
   void applyCsvDefaultName();
   /* 连续读数那份 CSV 的文件名留空时按时间戳起一个 (与 applyCsvDefaultName 是两件事) */
   void applyMtrCsvDefaultName();

   /* ---- 三件套 ---- */
   EcatThread       *m_thr  = nullptr;
   EcatBusView      *m_busv = nullptr;
   ScanController   *m_ctl  = nullptr;
   MapCanvas        *m_canvas = nullptr;

   /* 四个功率计都留着, m_meter 指向当前选中的那个。m_ophir 是真机那一个 (PD300R + Juno+),
    * 有自己的工作线程, 还会主动报 infoChanged。**它只有这一个实例**: Ophir 表头是独占的
    * (第二个实例打开同一个头会 0x80040201), 所以不能有第二个 OphirMeter */
   ManualMeter *m_manual = nullptr;
   RandomMeter *m_random = nullptr;
   ScriptMeter *m_script = nullptr;
   OphirMeter  *m_ophir  = nullptr;
   PowerMeter  *m_meter  = nullptr;

   /* ---- 连续读数那一版 ----
    * m_mlog 常驻 (它不依赖界面: 跟随扫描时点照收)。
    * 三方仲裁 (扫描 / 「读一次」/ 连续读数) 的唯一写点就是 refresh() —— 全在这一个函数里,
    * 因为接口约定"同一时刻只允许一个未决请求" (powermeter.h:28-31)。 */
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
   /* 「原点模式」框里就是这两组按钮, 一行一根轴: [轴][0/1] 找原点开关 (方式 24/29),
    * 左边两个按钮; 右边两个找限位开关 (方式 18/17)。文字见 kHomeBtnText */
   QPushButton *m_btnHome[2][2] = {};   /* [轴][方向] 0 = 正向回零, 1 = 反向回零 */
   QPushButton *m_btnLim[2][2] = {};    /* [轴][侧] 0 = 找正限位, 1 = 找负限位 */
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
    * 这一框在可用性表里 (PI_SHADE): 里面有一个要记进 scan.ini 的模式 (「自动跟随」, 上下限
    * 那两个数不记) 和一个只在手动定标时才该按的按钮 (「按数据定标」), 那两条判据得有个住处。
    *
    * 两个数是一对, 得一起保证 min < max, 规则是**推着走**: 改一个顶到另一个头上, 就把另一个
    * 一起推过去并保留原来的跨度 (见 onShadeLoChanged / onShadeHiChanged)。 */
   QDoubleSpinBox *m_edShadeLo   = nullptr;
   QDoubleSpinBox *m_edShadeHi   = nullptr;
   /* 色标那两个数是什么单位 —— 跟着取样源走, 每拍由 refreshMeterReadout() 写 */
   QLabel         *m_lShadeUnit  = nullptr;
   QPushButton    *m_btnFit      = nullptr;
   QCheckBox      *m_cbShadeAuto = nullptr;
   QLabel         *m_lblLocked   = nullptr;   /* 锁定模式那两句说明 (两行, 按模式显隐) */
   QLabel         *m_lblAuto     = nullptr;

   /* ---- 功率计 ----
    * **这一块框不在可用性表里** (2026-09-22 起): 它里面没有"参数", 只有一台随时可以接上/断开的
    * 仪器, 而它的用处恰恰是"还没连总线, 先把真机选上"。所以它不需要先过一个门。判据全在
    * refreshMeterPanel() 一处, 与其余控件同一条规矩 (每拍重算, 不缓存)。
    *
    * 2026-09-22 曾经把这七样东西搬进一个独立的功率计窗口、参数栏只留一行取样源; 当天又搬回来
    * —— 那个窗口要靠菜单点开, 而"外露"是这一块的要求 (见 docs/scan_sweep.md §23)。 */
   QComboBox      *m_cbMeter   = nullptr;
   QLabel         *m_lMeter    = nullptr;   /* 状态行: kind · 已打开/没打开 · 真机摘要 */
   QDoubleSpinBox *m_edManualV = nullptr;   /* 手填值源 */
   QDoubleSpinBox *m_edRandomBase = nullptr;/* 随机源的基值 (随机源本来就有这一个旋钮) */
   QDoubleSpinBox *m_edRandomN = nullptr;   /* 随机源的噪声幅度 */
   QLineEdit      *m_edScript  = nullptr;   /* 脚本源: 框里的文本 */
   QPushButton    *m_btnScript = nullptr;
   /* 两个模拟源共用的"模拟往返延迟": 真机一次往返可能上百毫秒, 而模拟源默认是 20ms ——
    * 想在没有真机的时候看时序 (间隔、超时、一个未决请求那条约束) 就得把它调大 */
   QWidget  *m_simRow     = nullptr;
   QSpinBox *m_edSimDelay = nullptr;
   /* 真机那三项。选项表由设备给 (探头不同, 能选的波长与量程就不同), 一个都不写死。
    * 每一项没有单独的"那一行"要露/藏: 整块 m_devBox 一起显隐, 而某一项设备根本没有时
    * 它是**空的 + 灰的** (判据在 refreshMeterPanel 里) */
   QComboBox *m_cbWl = nullptr, *m_cbRange = nullptr, *m_cbMeasMode = nullptr;
   QLabel    *m_lDevInfo = nullptr;
   /* 各源自己那一行: 选到谁只露谁 (都摊开的话一半的控件永远是灰的) */
   QWidget *m_manualRow = nullptr, *m_randomRow = nullptr, *m_scriptRow = nullptr;
   /* 真机那一块 (波长/量程/模式 + 设备信息), 只在选到真机且它开着时才露 */
   QWidget *m_devBox = nullptr;
   /* 填充那三个下拉框时挡掉信号: 每 addItem 一次都会被当成操作员改配置 (一串 stop/set/start) */
   bool m_meterCfgQuiet = false;

   /* 「读一次」的按钮、读数与未决状态。兜底定时器必须留着: 接口约定"恰好回一次"是源那边的
    * 义务, 源不回时这个动作会永远卡住 —— 而它卡住会连带把连续读数的「开始」压死 */
   QPushButton    *m_btnRead   = nullptr;
   QLabel         *m_lReadout  = nullptr;
   QTimer         *m_readTimer = nullptr;
   bool            m_readPending = false;
   qint64          m_readSentMs  = 0;   /* m_clock 的读数, 单调钟 */

   /* 连续读数 (见 meterlog.h) */
   QSpinBox    *m_edMtrInterval  = nullptr;
   QSpinBox    *m_edMtrAvg       = nullptr;   /* 一次采样平均几个读数 (1 = 每次都要) */
   QPushButton *m_btnMtrStart    = nullptr;
   QPushButton *m_btnMtrStop     = nullptr;
   QPushButton *m_btnMtrClear    = nullptr;
   QPushButton *m_btnMtrExport   = nullptr;
   QLineEdit   *m_edMtrCsv       = nullptr;
   QPushButton *m_btnMtrCsv      = nullptr;
   QLabel      *m_lMtrCount      = nullptr;   /* 缓冲 N 点 / 采集中 / 跟随扫描中 / **卡住了** */
   QLabel      *m_lMtrLast       = nullptr;   /* 最近一次读数 (大字号) */
   QLabel      *m_lMtrStats      = nullptr;
   QLabel      *m_lMtrWritten    = nullptr;

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
