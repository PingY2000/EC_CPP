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

/* 滚轮闸, 定义在 .cpp 里 (它只跟窗口内部那几个输入框打交道, 别处用不着) */
class WheelNeedsFocus;

class ScanWindow : public QMainWindow
{
   Q_OBJECT

public:
   /* 信号灯的四种样子。**放在头里, 因为窗口要记住每一格上一次是什么** ——
    * 只在真变了才动控件 (见 refreshAxisSignals)。
    *   Unknown = 灰: 不知道       Off = 灭: 这件事没发生
    *   Ok      = 绿亮: 使能带电 / 原点开关压着    Bad = 红亮: 出事了 */
   enum class Lamp { Unknown, Off, Ok, Bad };

   /*
    * 一块"每根轴一行、每格一盏灯 + 一行字"的网格。参数栏有**两块**, 列数不同:
    *
    *   轴信号   (2 列) 使能 / 故障
    *   限位开关 (3 列) 原点 / 正限位 / 负限位
    *
    * 为什么拆成两个结构体而不是共用一个 [2][3]: 共用一个的话 "第 2 列是什么" 在两组里
    * 意思不同 (轴信号里第 2 列是故障, 限位里是负限位), 而它们会共用同一套
    * refreshAxisSignals 循环变量 —— 那是写错了也能编译过去的类型。
    *
    * **灯与字的画法只有一份实现** (paintLamp / makeLamp / lampUnit / setSignalCell),
    * 这里存的是"哪些控件"和"上一次画成什么"。ncol 就是本组实际用到的列数,
    * 也正是 setColumnStretch 要用到的那个列号 (见 buildAxisPanel 末尾那条警告)。
    */
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
   /* 两块: 「轴信号」(使能/故障 + 故障复位按钮) 与「限位开关」(原点/正限位/负限位)。
    * 拆开的理由见 §9 与 LampGrid 上面那段 —— 一行五个"灯+字"在 370px 里会被挤扁。 */
   QWidget *buildAxisPanel();
   QWidget *buildLimitPanel();
   /* 「回零」(驱动器自带的 HM 模式)。**独立的第三个框**, 不塞进扫描参数栏 ——
    * 它是"让滑台自己去找开关"这类危险动作, 与几何参数不是一回事 */
   QWidget *buildHomePanel();

   /* ---- 操作 ---- */
   void onConnectClicked();
   void onEnableClicked();
   /* 清驱动器的故障位。**每次点击都弹模态确认**, 见 .cpp 里那个函数的注释 */
   void onFaultResetClicked();
   /* 「让 60FDh 进 TxPDO」。转发给工作线程之外, **已经连上了还要说明白它下次才生效** ——
    * 这一条只能说出来, 界面上看不出来, 见 .cpp 里那个函数的注释 */
   void onWantDigInToggled(bool on);
   /* 「输入电平反转 (NPN)」。**运行期参数** —— 勾一下就生效 (下一帧 publish), 不重连,
    * 不写驱动器, 也不进 ini。但它是**安全相关**的: 见 .cpp 里那个函数的注释 */
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
   /* 「读一次」。它**会跟 ScanController 抢同一个未决请求**, 所以有一条硬闸:
    * 控制器不在 Idle 时按钮是禁用的 (见 .cpp 里 onReadOnceClicked 那段)。
    * 三个槽挂在四个取样源上, 靠 m_readPending 认出"这是不是我那一份"。 */
   void onReadOnceClicked();
   void onReadOnceReady(double watts);
   void onReadOnceFailed(const QString &err);

   /* 「X/Y 正/反向回零」。dir: 0 = 正向 (6098h = 24), 1 = 反向 (29)。
    * **每次点击都弹模态** —— 这个动作会让滑台带电自己朝开关走, 见 .cpp 里那段。
    * 它同时 +1 零点世代: 回零会把物理位置整个搬走, 与「设为区域中心」同一个理由。 */
   void onHomeClicked(int axis, int dir);
   /* 「停止」。回零期间它必须变成**立即中止** —— 见 .cpp 里那段 (队列救不了回零) */
   void onStopClicked();

   /* ---- 内部 ---- */
   void pushParams();                 /* 控件 → Params → 控制器 + 量程 */
   void applyDefaults();              /* 控件 ← Params 缺省 (构造时一次 + 「恢复默认」) */
   /* 记忆 (exe 旁边的 scan.ini, 见 scanprefs.h)。
    * load 在 buildParamPanel 里 applyDefaults **之后**调一次 —— 顺序不能反:
    * 反过来的话, 紧接着的 applyDefaults 会把刚读回来的值全按缺省值盖掉, 等于白读。 */
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

   /* ---- 三件套 ---- */
   EcatThread       *m_thr  = nullptr;
   EcatBusView      *m_busv = nullptr;
   ScanController   *m_ctl  = nullptr;
   MapCanvas        *m_canvas = nullptr;

   /* 四个功率计都留着, m_meter 指向当前选中的那个。**不做工厂/注册表** ——
    * 一个下拉框 + 四个成员就是终态。
    *
    * m_ophir 是**真机**那一个 (PD300R + Juno+), 跟三个模拟源的区别不只是"真":
    * 它有自己的工作线程 (见 ophirmeter.h), 还会主动报 infoChanged/configFailed。 */
   ManualMeter *m_manual = nullptr;
   RandomMeter *m_random = nullptr;
   ScriptMeter *m_script = nullptr;
   OphirMeter  *m_ophir  = nullptr;
   PowerMeter  *m_meter  = nullptr;

   /* 下拉框正在被程序填 (从设备回来的信息), 不是操作员在点 —— 那三项的
    * currentIndexChanged 要吞掉, 否则会把刚读回来的值又写回设备一遍, 无限来回 */
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
   /* 6099h:01 找原点速度。上限夹在 HMI_HOME_VEL_MAX (2000) —— 与工作线程那道夹取
    * **同一个宏**, 所以界面上显示的数就是线上发的数。
    * **不进 scan.ini**: 它是运动参数, 而每次点击的模态都会把当次数值念一遍 */
   QSpinBox    *m_edHomeVel = nullptr;
   QPushButton *m_btnHome[2][2] = {};   /* [轴][方向] 0 = 正向, 1 = 反向 */
   /* 我们发出去的那条"正在回零"横幅的原文。下降沿靠它认"现在挂着的是不是我们自己
    * 那条" —— 同 m_limBanner 那套 */
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
   /* 接上之后第一件想做的事: **点一下, 出一个数**, 不用跑整趟扫描就能确认链路通了、
    * 探头出的数合理。硬件没插时按接口约定干净报错 (见 onReadOnceFailed) */
   QPushButton    *m_btnRead   = nullptr;
   QLabel         *m_lReadout  = nullptr;
   /* 未决请求的兜底。接口约定是"恰好回一次", 但那句话是**源那边的义务** ——
    * 真机那条腿要是有 bug 就一个都不回, 界面会永远停在"读取中…" */
   QTimer         *m_readTimer = nullptr;
   bool            m_readPending = false;
   qint64          m_readSentMs  = 0;   /* m_clock 的读数, 单调钟 */

   /* 真机那三项。**选项表由设备给**, 不是这里写死的 —— 手册明确说了不要按型号
    * 推断规格。选中模拟源时整行藏起来, 免得看着像能用 */
   QComboBox *m_cbWl       = nullptr;
   QComboBox *m_cbRange    = nullptr;
   /* **不叫 m_cbMode** —— 那个名字已经是上面「扫描模式」(方向/蛇形那个) 的了,
    * 两个都要在, 名字撞了会很难看 */
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
   /* 「限位开关」第四行。**安全相关** —— 开着的时候限位判据整个换掉 (见
    * ecatcmd::limit_rule_for), 所以它必须一直显眼: 不随 onair 变灰, 也不持久化 */
   QCheckBox   *m_cbDiInvert = nullptr;

   /* ---- 状态栏 ---- */
   QLabel *m_banner = nullptr;
   QLabel *m_lNote  = nullptr;
   QLabel *m_lWkc   = nullptr;
   /* 限位判据 (默认 = 6041h bit11「**硬件限位信号有效**」), 一根轴一个。
    * **扫描中它成立就会自动中止** —— 这条必须常显, 不是"点开某个面板才看得到"的东西。
    * 信号灯在左、文字在右, **两个一起读**才算一条信息。
    * 注意状态栏那一格的措辞是「有效!」而不是「撞上」: 手册对这一位的定义就是
    * 限位信号的电平, 回零时它本来就该是 1 (见 ecatworker.h 顶部那段) */
   QLabel *m_lampX  = nullptr;
   QLabel *m_lampY  = nullptr;
   QLabel *m_lLimX  = nullptr;
   QLabel *m_lLimY  = nullptr;

   /* ---- 状态 ---- */
   QTimer        *m_tick        = nullptr;
   QTimer        *m_bannerTimer = nullptr;
   QElapsedTimer  m_clock;

   /* 滚轮闸 (见 .cpp 里的 WheelNeedsFocus)。它自己不画任何东西, 只是一个挂在
    * 参数输入框 (以及它们的子控件) 上的事件过滤器 —— 挂在本窗口名下只是为了有人负责删它。
    * 类型用具体类而不是 QObject*, 因为装闸时要调它的 guard(); 类体在 .cpp 里 */
   WheelNeedsFocus *m_wheelGuard = nullptr;

   /* 上次用的网卡 (从 scan.ini 读回来的, 可能已经不在机器上了)。**适配器清单是
    * 异步到的**, 所以先存在这儿, 等 adaptersListed 到了再拿它去选 */
   QString        m_savedNic;

   int  m_epoch     = 0;      /* 零点世代。每次连接 / 每次「设为区域中心」+1, 进 CSV 表头 */
   /* 上次递给工作线程的量程。**只在数值真的变了才 postRange** —— 每敲一个键都投一条
    * 队列命令的话, 工作线程会为 "2" 和 "27" 各回一句 note, 状态栏自己跟自己打架 */
   int32_t m_last_range = 0;
   bool m_connected = false;
   bool m_faultShown = false;
   bool m_limShown[2] = {false, false};   /* 限位横幅的上升沿防重入, 一根轴一个 */
   /* 我们发出去的那条限位横幅**原文**。下降沿靠它认"现在挂着的是不是我们自己那条" ——
    * 是才清, 不是就不能动 (中止之类的消息可能已经把它换掉了)。见 refreshAxisSignals() */
   QString m_limBanner[2];
   bool m_warnedLive = false;
   int  m_autoStop   = 0;     /* 自动中止弹窗的防重入 */
   QString m_last_dir;
};

}   /* namespace scan */
