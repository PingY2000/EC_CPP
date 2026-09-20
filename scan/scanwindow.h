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

/* 滚轮闸, 定义在 .cpp 里 */
class WheelNeedsFocus;

class ScanWindow : public QMainWindow
{
   Q_OBJECT

public:
   /* 信号灯的四种样子。放在头里, 因为窗口要记住每一格上一次画的是什么, 只在真变了才动控件。
    *   Unknown = 灰: 不知道       Off = 灭: 这件事没发生
    *   Ok      = 绿亮: 使能带电 / 原点开关压着    Bad = 红亮: 出事了 */
   enum class Lamp { Unknown, Off, Ok, Bad };

   /* 一块"每根轴一行、每格一盏灯 + 一行字"的网格。两块, 但**排在同一张表上** (见
    * buildAxisPanel): 轴信号 (2 列 使能/故障, 问 6041h) 与限位开关 (3 列 原点/正限位/
    * 负限位, 问 60FDh) —— 列号是各自那组自己的 (AX_ 与 LIM_ 那几个枚举),
    * 表上占哪几列由 buildAxisPanel 决定, 所以这里没有"列数"这个字段 */
   struct LampGrid
   {
      QLabel  *lamp[2][3] = {};      /* [轴][信号] */
      QLabel  *text[2][3] = {};
      Lamp     lampLast[2][3] = {};  /* 上一次画的是什么 */
      QString  textLast[2][3];
   };

   explicit ScanWindow(QWidget *parent = nullptr);
   ~ScanWindow() override;

protected:
   /* 关窗 = 中止扫描 + 断开: 失能 → 还原映射 → 降 PRE_OP → 关网卡, 必要时弹"可能仍带电" */
   void closeEvent(QCloseEvent *e) override;

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
   void onMeterChanged(int idx);
   void onMeterInfoChanged();         /* 真机那三项下拉框 ← OphirMeter::info() */
   void onMeterCfgChanged();          /* 那三项下拉框 → 设备 */
   void onBrowseScript();
   void onManualValueChanged(double v);
   /* 「读一次」。与 ScanController 抢同一个未决请求, 故控制器不在 Idle 时按钮禁用;
    * 靠 m_readPending 认出回调是不是自己那一份 */
   void onReadOnceClicked();
   void onReadOnceReady(double watts);
   void onReadOnceFailed(const QString &err);

   /* 「X/Y 正/反向回零」与「X/Y 找正/负限位」共用的槽。
    * dir: 0 = 正那侧, 1 = 负那侧; find_limit: false -> 6098h = 24/29 (找**原点开关** X0),
    * true -> 18/17 (找**限位开关**, 手册叫"找限位")。不弹确认框, 说明在按钮 tooltip 上;
    * 回零中按「停止」= 立即中止。它同时 +1 零点世代 (两者都重定义零点, 续扫必须换世代)。 */
   void onHomeClicked(int axis, int dir, bool find_limit);
   /* 「停止」。回零期间它必须变成立即中止 (队列救不了回零) */
   void onStopClicked();

   /* ---- 参数框的编辑门控 ----
    * 一块框平时只读, 点这块框的「编辑」才能改, 改完「保存」(固化进 ini) 或「取消」(退回
    * 上次保存的值并重新下推)。状态机在 scan/editgate.h, 这里只管控件。
    *
    * **控件的可用性只由 refreshEditability() 一处写** —— 它在 30Hz 的 refresh() 末尾被调,
    * 别处再 setEnabled 会被下一拍覆盖 (写按钮槽里更是当场就被盖掉)。 */
   struct GateItem
   {
      QWidget *w = nullptr;
      bool     lock_running = false;  /* 运行中也锁住 (几何 / 输出路径这类) */
      bool     need_dev = false;      /* 还要求真机功率计就绪 (那三个下拉框) */
   };
   struct PanelGate
   {
      editgate::Gate  gate;
      QGroupBox      *box = nullptr;
      QPushButton    *btnEdit = nullptr;
      QPushButton    *btnSave = nullptr;
      QPushButton    *btnCancel = nullptr;
      QString         title_base;
      QList<GateItem> items;
      QList<QVariant> snapshot;       /* 进编辑态那一刻的控件值 */
   };

   QWidget *buildAdvPanel();
   /* 登记一块框: 造 [编辑][保存][取消] 那一行并记住成员。members 里**不放**这三个按钮 */
   void addGate(int gi, QGroupBox *box, const QList<GateItem> &items);
   QWidget *gateBar(int gi, QWidget *parent);
   void gateSnapshot(int gi);       /* 拍快照 (进编辑态时) */
   void gateRollback(int gi);       /* 控件 ← 快照, 不拦信号: 回滚要顺带重新下推 */
   void gateRebase(int gi, QWidget *w);  /* 程序自己改了这个控件 -> 快照跟上 + 重算标记 */
   void gateDirty(int gi);          /* 重算"有没有改动" (逐项与快照比对, 只影响标题标记) */
   void refreshEditability();
   void gateTitle(int gi);          /* 框标题 = 标题 + 标记 */
   bool meterDevOk() const;         /* 真机三项的可用判据, 一处共用 */
   void onGateEdit(int gi);
   void onGateSave(int gi);
   void onGateCancel(int gi);
   void refreshAdvWarn();           /* 双反相那行红字 */

   /* ---- 内部 ---- */
   void pushParams();                 /* 控件 → Params → 控制器 + 量程 */
   void applyDefaults();              /* 控件 ← Params 缺省 (构造时一次 + 「恢复默认」) */
   /* 记忆 (exe 旁边的 scan.ini)。loadSettings 必须在 applyDefaults 之后调, 反了会被缺省值盖掉 */
   void loadSettings();
   void saveSettings();

   void pushManualSpeed(const BusTelem &t, bool running);
   void refreshAxisSignals(const BusTelem &t);   /* 限位/使能/故障: 状态栏 + 参数栏, 一份遥测 */
   void setSignalCell(LampGrid &g, int i, int s, bool known, bool on, Lamp lit,
                      const QString &litTxt, const QString &offTxt);
   Params currentParams() const;
   void refresh();                    /* 30Hz: tick 状态机 + 刷遥测 + 刷按钮可用性 */
   void setConnected(bool on);
   void hint(const QString &s, bool fault);
   void showFault(const QString &why);   /* 自动中止: 红色横幅 + 模态 (无人值守时不会错过) */
   void warnMaybeLive();
   void disconnectAndStop();
   void syncShadeEdits();
   void applyCsvDefaultName();
   bool pushScriptPath();   /* 脚本框的文本 -> 功率计那一路 (文本与状态是两份东西) */

   /* ---- 三件套 ---- */
   EcatThread       *m_thr  = nullptr;
   EcatBusView      *m_busv = nullptr;
   ScanController   *m_ctl  = nullptr;
   MapCanvas        *m_canvas = nullptr;

   /* 四个功率计都留着, m_meter 指向当前选中的那个。m_ophir 是真机那一个 (PD300R + Juno+),
    * 有自己的工作线程, 还会主动报 infoChanged */
   ManualMeter *m_manual = nullptr;
   RandomMeter *m_random = nullptr;
   ScriptMeter *m_script = nullptr;
   OphirMeter  *m_ophir  = nullptr;
   PowerMeter  *m_meter  = nullptr;

   /* 下拉框正在被程序填, 不是操作员在点 —— 那三项 currentIndexChanged 要吞掉, 否则值会写回设备 */
   bool m_meterCfgQuiet = false;

   /* ---- 顶栏 ---- */
   QComboBox   *m_nic       = nullptr;
   QPushButton *m_btnNic    = nullptr;
   QPushButton *m_btnConn   = nullptr;
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

   /* ---- 色标 ---- */
   QDoubleSpinBox *m_edShadeLo = nullptr;
   QDoubleSpinBox *m_edShadeHi = nullptr;
   QPushButton    *m_btnFit    = nullptr;

   /* ---- 功率计 ---- */
   QComboBox      *m_cbMeter   = nullptr;
   QDoubleSpinBox *m_edManualV = nullptr;
   QDoubleSpinBox *m_edRandomN = nullptr;
   QLineEdit      *m_edScript  = nullptr;
   QPushButton    *m_btnScript = nullptr;
   QLabel         *m_lMeter    = nullptr;
   /* 点一下出一个数: 不跑整趟扫描就能确认链路通、探头出的数合理 */
   QPushButton    *m_btnRead   = nullptr;
   QLabel         *m_lReadout  = nullptr;
   /* 未决请求的兜底: 接口约定"恰好回一次"是源那边的义务, 源不回时界面会永远停在"读取中…" */
   QTimer         *m_readTimer = nullptr;
   bool            m_readPending = false;
   qint64          m_readSentMs  = 0;   /* m_clock 的读数, 单调钟 */

   /* 真机那三项。选项表由设备给, 不写死 (手册: 不要按型号推断规格)。选中模拟源时整行藏起来 */
   QComboBox *m_cbWl       = nullptr;
   QComboBox *m_cbRange    = nullptr;
   /* 不叫 m_cbMode —— 那名字已给「扫描模式」(方向/蛇形) 用了 */
   QComboBox *m_cbMeasMode = nullptr;
   QLabel    *m_lWl        = nullptr;
   QLabel    *m_lRange     = nullptr;
   QLabel    *m_lMeasMode  = nullptr;
   /* 每一行(标签+下拉)的容器, 整行藏起来用 */
   QWidget   *m_devRowWl    = nullptr;
   QWidget   *m_devRowRange = nullptr;
   QWidget   *m_devRowMode  = nullptr;

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

   /* ---- 编辑门控 ---- */
   QList<PanelGate> m_gates;               /* 下标 = gate 号, 见 scanwindow.cpp 顶上那几个 GI_ */
   QPushButton     *m_btnCsv = nullptr;    /* 「扫描参数」的 CSV「…」(进成员表, 故不能是局部量) */
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
   WheelNeedsFocus *m_wheelGuard = nullptr;

   /* 上次用的网卡 (从 scan.ini 读回, 可能已不在机器上)。适配器清单异步到, 故先存着等 adaptersListed */
   QString        m_savedNic;

   int  m_epoch     = 0;      /* 零点世代。每次连接 / 每次「设为区域中心」+1, 进 CSV 表头 */
   /* 上次递给工作线程的量程。只在数值真变了才 postRange, 否则状态栏会被逐个按键的回执刷屏 */
   int32_t m_last_range = 0;
   bool m_connected = false;
   bool m_faultShown = false;
   bool m_limShown[2] = {false, false};   /* 限位横幅的上升沿防重入, 一根轴一个 */
   /* 我们发出去的那条限位横幅原文。下降沿靠它认现在挂着的是不是我们自己那条: 是才清, 不是不能动 */
   QString m_limBanner[2];
   bool m_warnedLive = false;
   int  m_autoStop   = 0;     /* 自动中止弹窗的防重入 */
   QString m_last_dir;
};

}   /* namespace scan */
