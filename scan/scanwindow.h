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
#include "ophirmeter.h"
#include "powermeter.h"
#include "scancontroller.h"
#include "scanplan.h"
#include "scanprefs.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
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

   /* 一块"每根轴一行、每格一盏灯 + 一行字"的网格。两块: 轴信号 (2 列 使能/故障)、
    * 限位开关 (3 列 原点/正限位/负限位)。ncol = 实际用到的列数, 也是 setColumnStretch 的列号 */
   struct LampGrid
   {
      int      ncol = 0;
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
   /* 两块: 「轴信号」(使能/故障 + 故障复位按钮) 与「限位开关」(原点/正限位/负限位) */
   QWidget *buildAxisPanel();
   QWidget *buildLimitPanel();
   /* 「回零」(驱动器自带的 HM 模式)。独立的第三个框, 不塞进扫描参数栏 */
   QWidget *buildHomePanel();

   /* ---- 操作 ---- */
   /* 「连接」。不弹确认框, 说明写在按钮 tooltip 上 */
   void onConnectClicked();
   /* 「使能」。不弹确认框: 带不带电由按钮文字与灰/亮表示 (已使能时是灰的「已使能」) */
   void onEnableClicked();
   /* 清驱动器的故障位 */
   void onFaultResetClicked();
   /* 「让 60FDh 进 TxPDO」。已连接时改动下次才生效, 界面看不出来, 须由提示说明 */
   void onWantDigInToggled(bool on);
   /* 「输入电平反转 (NPN)」。运行期参数: 勾一下就生效, 不重连、不写驱动器、不进 ini。安全相关 */
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

   /* 「X/Y 正/反向回零」。dir: 0 = 正向 (6098h = 24), 1 = 反向 (29)。不弹确认框,
    * 说明在按钮 tooltip 上; 回零中按「停止」= 立即中止。它同时 +1 零点世代。 */
   void onHomeClicked(int axis, int dir);
   /* 「停止」。回零期间它必须变成立即中止 (队列救不了回零) */
   void onStopClicked();

   /* ---- 内部 ---- */
   void pushParams();                 /* 控件 → Params → 控制器 + 量程 */
   void applyDefaults();              /* 控件 ← Params 缺省 (构造时一次 + 「恢复默认」) */
   /* 记忆 (exe 旁边的 scan.ini)。loadSettings 必须在 applyDefaults 之后调, 反了会被缺省值盖掉 */
   void loadSettings();
   void saveSettings();
   void pushManualSpeed(const BusTelem &t, bool running);
   /* 回零框里那行数 (照现在这个速度能找多远)。随「回零速度」实时变 */
   void pushHomeNote();
   /* 回零框里另一行: 两根轴的实际运行模式 6061h (手册 §3.7 把"读回 6"当 HM 的前提)。
    * 只在工作线程真读过之后才有值 —— 没读过与读到 0 是两句不同的话 */
   void pushHomeMode(const BusTelem &t);
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

   /* ---- 回零 ---- */
   /* 6099h:01 找原点速度。上限 = 与工作线程夹取共用的宏 HMI_HOME_VEL_MAX (2000); 不进 scan.ini */
   QSpinBox    *m_edHomeVel = nullptr;
   QPushButton *m_btnHome[2][2] = {};   /* [轴][方向] 0 = 正向, 1 = 反向 */
   /* 回零框里那行说明: 照现在这个速度, 一次回零最多走多远 / 多久判超时。随速度实时变 */
   QLabel      *m_lHomeNote = nullptr;
   QString      m_homeNoteLast;
   /* 6061h 那一行。只在工作线程读到新值、且文字真变了才 setText */
   QLabel      *m_lHomeMode = nullptr;
   QString      m_homeModeLast;
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

   /* ---- 参数栏的两块信号网格 (见 LampGrid) ---- */
   LampGrid m_axGrid;    /* 0=使能 1=故障 */
   LampGrid m_limGrid;   /* 0=原点 1=正限位 2=负限位 */

   QPushButton *m_btnFaultRst = nullptr;   /* 「轴信号」第三行, 跨全部列 */
   QCheckBox   *m_cbWantDigIn = nullptr;   /* 「限位开关」第三行, 连接期参数 */
   /* 「限位开关」第四行。安全相关: 开着时限位判据整个换掉 (limit_rule_for), 不随 onair 变灰, 也不持久化 */
   QCheckBox   *m_cbDiInvert = nullptr;

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
