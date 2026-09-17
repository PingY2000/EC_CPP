/*
 * scan/scanwindow.h —— 扫描主窗口
 *
 * 它跟 hmi/mainwindow 是**同一个角色**: 收集操作 → 转成 EcatThread 的 post 系列 / set 系列
 * → 30Hz 刷遥测。区别只在中间那一层: hmi 摆的是两根一维轨道, 这里摆的是一块二维面板 + 一个
 * 扫描状态机。
 *
 * **它不持有 em_bus_t, 也不 include SOEM** —— 连 ecx_* 都不出现, 与 hmi 同一条不变量。
 * 总线线程是 hmi/ecatworker 原样编进来的那一份, 一行没改。
 *
 * 三种角色各管各的, 不要混:
 *   EcatThread      —— 总线与运动 (2ms 插补), 唯一碰 em_bus_t 的线程
 *   ScanController  —— 扫描时序 (到点 → 停留 → 采样 → 落盘), 活在 GUI 线程, 只经 BusView 说话
 *   ScanWindow      —— 界面与安全护栏 (确认弹窗 / 收尾 / 断连时的自动中止)
 *
 * 时钟是**单调钟** (QElapsedTimer): 状态机所有的时间判断都用它。墙上时间会被 NTP 和对时
 * 往回拨, 那样"停留了 200ms"可能永远不成立 —— 扫描会卡在那一点上不动。
 */
#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include "busview.h"
#include "powermeter.h"
#include "scancontroller.h"
#include "scanplan.h"

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

class ScanWindow : public QMainWindow
{
   Q_OBJECT

public:
   /* 信号灯的四种样子。**放在头里, 因为窗口要记住每一格上一次是什么** ——
    * 只在真变了才动控件 (见 refreshAxisSignals)。
    *   Unknown = 灰: 不知道       Off = 灭: 这件事没发生
    *   Ok      = 绿亮: 使能带电    Bad = 红亮: 出事了 */
   enum class Lamp { Unknown, Off, Ok, Bad };

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
   QWidget *buildAxisPanel();         /* 每根轴三个信号: 使能 / 故障 / 限位 */

   /* ---- 操作 ---- */
   void onConnectClicked();
   void onEnableClicked();
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
   void onBrowseScript();
   void onManualValueChanged(double v);

   /* ---- 内部 ---- */
   void pushParams();                 /* 控件 → Params → 控制器 + 量程 */
   void applyDefaults();              /* 控件 ← Params 缺省 (构造时一次 + 「恢复默认」) */
   void pushManualSpeed(const BusTelem &t, bool running);
   void refreshAxisSignals(const BusTelem &t);   /* 限位/使能/故障: 状态栏 + 参数栏, 一份遥测 */
   void setSignalCell(int i, int s, bool known, bool on, Lamp lit,
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

   /* 三个模拟功率计都留着, m_meter 指向当前选中的那个。**不做工厂/注册表** ——
    * 一个下拉框 + 三个成员就是终态, 加真机时这里多一个成员、多一个 case。 */
   ManualMeter *m_manual = nullptr;
   RandomMeter *m_random = nullptr;
   ScriptMeter *m_script = nullptr;
   PowerMeter  *m_meter  = nullptr;

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

   /* ---- 轴信号 (参数栏): [轴][信号], 信号 0=使能 1=故障 2=限位 ---- */
   QLabel *m_axLamp[2][3] = {};
   QLabel *m_axText[2][3] = {};
   /* 上一次画的是什么。**只在真变了才写控件** —— 30Hz 每帧给 14 个控件重设样式表
    * 会把重绘刷爆 (横幅那个上升沿判断是同一个理由) */
   Lamp     m_axLampLast[2][3] = {};
   QString  m_axTextLast[2][3];

   /* ---- 状态栏 ---- */
   QLabel *m_banner = nullptr;
   QLabel *m_lNote  = nullptr;
   QLabel *m_lWkc   = nullptr;
   /* 硬件限位 (6041h bit11), 一根轴一个。**扫描中撞限位会自动中止** ——
    * 这条必须常显, 不是"点开某个面板才看得到"的东西。
    * 信号灯在左、文字在右, **两个一起读**才算一条信息 */
   QLabel *m_lampX  = nullptr;
   QLabel *m_lampY  = nullptr;
   QLabel *m_lLimX  = nullptr;
   QLabel *m_lLimY  = nullptr;

   /* ---- 状态 ---- */
   QTimer        *m_tick        = nullptr;
   QTimer        *m_bannerTimer = nullptr;
   QElapsedTimer  m_clock;

   int  m_epoch     = 0;      /* 零点世代。每次连接 / 每次「设为区域中心」+1, 进 CSV 表头 */
   /* 上次递给工作线程的量程。**只在数值真的变了才 postRange** —— 每敲一个键都投一条
    * 队列命令的话, 工作线程会为 "2" 和 "27" 各回一句 note, 状态栏自己跟自己打架 */
   int32_t m_last_range = 0;
   bool m_connected = false;
   bool m_faultShown = false;
   bool m_limShown[2] = {false, false};   /* 限位横幅的上升沿防重入, 一根轴一个 */
   bool m_warnedLive = false;
   int  m_autoStop   = 0;     /* 自动中止弹窗的防重入 */
   QString m_last_dir;
};

}   /* namespace scan */
