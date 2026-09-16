#include "scanwindow.h"

#include "mapcanvas.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace scan {

static const char *kBannerWarn =
   "QLabel#banner { background:#4a3a12; color:#ffd479; padding:6px; border-radius:4px; }";
static const char *kBannerFault =
   "QLabel#banner { background:#5a1f1f; color:#ffb3b3; padding:6px; border-radius:4px; }";
static const char *kBannerInfo =
   "QLabel#banner { background:#1d3346; color:#a9cfe8; padding:6px; border-radius:4px; }";

/* 默认 CSV 目录。放在**当前工作目录**下而不是 exe 旁边: exe 在 bin/, 而
 * 从仓库根目录敲 ./bin/scan.exe 是最常见的用法 —— 那样数据就落在仓库根的 scan_out/,
 * 正是 .gitignore 里那一条。 */
static const char *kOutDir = "scan_out";

static QString fmtDur(int64_t ms)
{
   if (ms <= 0)
      return QStringLiteral("--");

   const int64_t s = ms / 1000;
   if (s < 60)
      return QStringLiteral("%1 s").arg(s);

   const int64_t m = s / 60;
   if (m < 60)
      return QStringLiteral("%1 min %2 s").arg(m).arg(s % 60);

   return QStringLiteral("%1 h %2 min").arg(m / 60).arg(m % 60);
}

/* ---------------------------------------------------------------- 构造 */

ScanWindow::ScanWindow(QWidget *parent) : QMainWindow(parent)
{
   setWindowTitle(QStringLiteral("滑台蛇形扫描采集 —— YKD2205PE / SOEM"));

   m_thr = new EcatThread(this);
   m_busv = new EcatBusView(m_thr);

   m_manual = new ManualMeter(this);
   m_random = new RandomMeter(this);
   m_script = new ScriptMeter(this);
   m_meter  = m_random;                 /* 默认随机源: 一按开始就有数据可看 */
   m_meter->open(nullptr);

   m_ctl = new ScanController(m_busv, m_meter, this);

   /* 单调钟先起 —— pushParams() 会走到 refresh(), 那里第一句就是 m_ctl->tick(elapsed())。
    * 没 start 的 QElapsedTimer 读数未定义, 喂进状态机的时间会是很久以前或者乱数 */
   m_clock.start();

   connect(m_thr, &EcatThread::notify, this,
           [this](const QString &s) {
              /* 工作线程已经在控制台打过一份了; 这里上横幅 */
              const bool fault = s.contains(QStringLiteral("失败"))
                              || s.contains(QStringLiteral("故障"))
                              || s.contains(QStringLiteral("拒绝"))
                              || s.contains(QStringLiteral("带电"));
              hint(s, fault);
           });
   connect(m_thr, &EcatThread::adaptersListed, this,
           [this](const QStringList &names, const QStringList &descs) {
              const QString keep = m_nic->currentData().toString();
              m_nic->clear();
              for (int i = 0; i < names.size(); i++)
                 m_nic->addItem(descs.value(i, names[i]), names[i]);
              if (keep.isEmpty())
                 return;
              const int idx = m_nic->findData(keep);
              if (idx >= 0)
                 m_nic->setCurrentIndex(idx);
           });

   connect(m_ctl, &ScanController::autoAborted, this, &ScanWindow::showFault);
   connect(m_ctl, &ScanController::runFinished,  this, [this](bool complete) {
      if (complete)
         hint(QStringLiteral("扫描跑完 %1 点, 数据在 %2")
                 .arg(m_ctl->totalPoints()).arg(m_ctl->csvPath()), false);
      else if (m_ctl->state() != ScanController::State::Aborted)
         hint(QStringLiteral("扫描结束了 (未完)"), false);
      refresh();
   });
   connect(m_ctl, &ScanController::pointLogged, this, [this](int, int, bool) {
      m_canvas->update();
   });

   buildUi();
   applyCsvDefaultName();

   m_ctl->setZeroEpoch(m_epoch);
   pushParams();
   syncShadeEdits();

   /* 状态机的节拍 + 界面刷新, 30Hz 一把。**状态机自己不持定时器** —— 时间由外面喂,
    * 单测里才能换一个手动时钟在没有硬件的情况下跑 (见 scan/selftest.cpp) */
   m_tick = new QTimer(this);
   connect(m_tick, &QTimer::timeout, this, &ScanWindow::refresh);
   m_tick->start(33);

   m_bannerTimer = new QTimer(this);
   m_bannerTimer->setSingleShot(true);
   connect(m_bannerTimer, &QTimer::timeout, this, [this] { m_banner->setVisible(false); });

   m_thr->start();
   m_thr->postListAdapters();

   hint(QStringLiteral("未连接: 界面此时是**只读**的, 一个字节都不往总线上写。"
                       "先把滑台推到想要的区域中心, 连接之后点「设为区域中心」对位"), false);
   refresh();
}

ScanWindow::~ScanWindow()
{
   if (m_thr->isRunning())
      disconnectAndStop();
}

/* ---------------------------------------------------------------- 界面 */

void ScanWindow::buildUi()
{
   QWidget *central = new QWidget(this);

   m_banner = new QLabel(central);
   m_banner->setObjectName(QStringLiteral("banner"));
   m_banner->setWordWrap(true);
   m_banner->setStyleSheet(kBannerInfo);
   m_banner->setVisible(false);

   m_canvas = new MapCanvas(central);
   m_canvas->setController(m_ctl);
   m_canvas->setBus(m_busv);
   connect(m_canvas, &MapCanvas::manualMove, this, [this](int32_t x, int32_t y) {
      /* 与 hmi 的点击同义: 只是给目标, 走不走由总线线程的插补器决定。
       * 扫描中画布已经把点击吞了, 这里再拦一道 —— 只有一处生效才叫纪律 */
      if (m_ctl->running())
         return;
      m_thr->setTarget(0, x);
      m_thr->setTarget(1, y);
   });
   connect(m_canvas, &MapCanvas::cellPicked, this, [this](int, int) { refresh(); });

   /* 右侧一列: 扫描参数 / 扫描控制 / 功率计 / 色标。整列固定宽度, 免得拖窗口时
    * 输入框跟着一起变形 */
   QWidget *side = new QWidget(central);
   side->setFixedWidth(370);

   QVBoxLayout *sv = new QVBoxLayout(side);
   sv->setContentsMargins(0, 0, 0, 0);
   sv->setSpacing(8);
   sv->addWidget(buildParamPanel());
   sv->addWidget(buildScanPanel());
   sv->addWidget(buildMeterPanel());
   sv->addWidget(buildShadePanel());
   sv->addStretch(1);

   QHBoxLayout *mid = new QHBoxLayout;
   mid->setContentsMargins(0, 0, 0, 0);
   mid->setSpacing(8);
   mid->addWidget(m_canvas, 1);
   mid->addWidget(side);

   QVBoxLayout *v = new QVBoxLayout(central);
   v->addWidget(buildTopBar());
   v->addWidget(m_banner);
   v->addLayout(mid, 1);
   setCentralWidget(central);

   /* ---- 状态栏 ---- */
   m_lNote = new QLabel(this);
   m_lWkc  = new QLabel(this);
   m_lWkc->setFont(QFont(QStringLiteral("Consolas")));
   statusBar()->addWidget(m_lNote, 1);
   statusBar()->addPermanentWidget(m_lWkc);
}

QWidget *ScanWindow::buildTopBar()
{
   QWidget *w = new QWidget(this);

   m_nic = new QComboBox(w);
   m_nic->setMinimumWidth(300);

   m_btnNic = new QPushButton(QStringLiteral("刷新网卡"), w);
   connect(m_btnNic, &QPushButton::clicked, m_thr, &EcatThread::postListAdapters);

   m_btnConn = new QPushButton(QStringLiteral("连接 (进 OP)"), w);
   m_btnConn->setToolTip(QStringLiteral(
      "打开发帧 + 建立过程数据 + 进 OP。**电机仍不带电** —— 使能才会带电。\n"
      "连接时会把**当前位置**重新设为显示坐标 0 (零点世代 +1)。"));
   connect(m_btnConn, &QPushButton::clicked, this, &ScanWindow::onConnectClicked);

   m_btnEnable = new QPushButton(QStringLiteral("使能"), w);
   m_btnEnable->setObjectName(QStringLiteral("danger"));
   m_btnEnable->setToolTip(QStringLiteral("切 CSP 模式并使能 —— **这是唯一让电机带电的按钮**"));
   connect(m_btnEnable, &QPushButton::clicked, this, &ScanWindow::onEnableClicked);

   m_btnStop = new QPushButton(QStringLiteral("停止"), w);
   m_btnStop->setToolTip(QStringLiteral("目标冻在当前位置, **保持保持力矩**(不卸力)。"
                                        "扫描中请用「中止」, 它除了冻住还会把进度留在 CSV 里"));
   connect(m_btnStop, &QPushButton::clicked, m_thr, &EcatThread::postStop);

   m_btnDis = new QPushButton(QStringLiteral("失能"), w);
   m_btnDis->setToolTip(QStringLiteral("回失能态, 电机释放 (滑台可能因自重下滑)"));
   connect(m_btnDis, &QPushButton::clicked, m_thr, &EcatThread::postDisable);

   m_btnCenter = new QPushButton(QStringLiteral("全部回中"), w);
   m_btnCenter->setToolTip(QStringLiteral("两根轴都走到显示坐标 0"));
   connect(m_btnCenter, &QPushButton::clicked, this, &ScanWindow::onCenterAllClicked);

   m_btnZero = new QPushButton(QStringLiteral("设为区域中心"), w);
   m_btnZero->setToolTip(QStringLiteral(
      "把**当前位置**定为显示坐标 0 —— 也就是扫描区域的中心。\n"
      "量程够大时先在画布上把滑台点到想要的位置, 再按这个。\n"
      "这会 +1 零点世代: 之后的坐标与之前的不是同一个物理位置。"));
   connect(m_btnZero, &QPushButton::clicked, this, &ScanWindow::onZeroHereClicked);

   QHBoxLayout *bar = new QHBoxLayout(w);
   bar->setContentsMargins(0, 0, 0, 0);
   bar->addWidget(new QLabel(QStringLiteral("网卡"), w));
   bar->addWidget(m_nic, 1);
   bar->addWidget(m_btnNic);
   bar->addSpacing(12);
   bar->addWidget(m_btnConn);
   bar->addSpacing(12);
   bar->addWidget(m_btnEnable);
   bar->addWidget(m_btnStop);
   bar->addWidget(m_btnDis);
   bar->addSpacing(12);
   bar->addWidget(m_btnCenter);
   bar->addWidget(m_btnZero);
   return w;
}

QWidget *ScanWindow::buildParamPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("扫描参数"), this);
   QFormLayout *f = new QFormLayout(box);
   f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
   f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

   m_edAreaX = new QDoubleSpinBox(box);
   m_edAreaX->setRange(0.1, 500.0);
   m_edAreaX->setDecimals(3);
   m_edAreaX->setSingleStep(1.0);
   m_edAreaX->setSuffix(QStringLiteral(" 单位"));
   m_edAreaX->setToolTip(QStringLiteral("区域的 X 边长, **以原点为中心** → ±(X/2)"));

   m_edAreaY = new QDoubleSpinBox(box);
   m_edAreaY->setRange(0.1, 500.0);
   m_edAreaY->setDecimals(3);
   m_edAreaY->setSingleStep(1.0);
   m_edAreaY->setSuffix(QStringLiteral(" 单位"));

   m_edRes = new QDoubleSpinBox(box);
   m_edRes->setRange(0.001, 50.0);
   m_edRes->setDecimals(3);
   m_edRes->setSingleStep(0.1);
   m_edRes->setSuffix(QStringLiteral(" 单位"));
   m_edRes->setToolTip(QStringLiteral("网格间距。点数 = (floor(区域/分辨率)+1)²"));

   m_edPpu = new QDoubleSpinBox(box);
   m_edPpu->setRange(100.0, 1000000.0);
   m_edPpu->setDecimals(0);
   m_edPpu->setSingleStep(1000.0);
   m_edPpu->setSuffix(QStringLiteral(" 脉冲"));
   m_edPpu->setToolTip(QStringLiteral("1 单位 = 多少脉冲。默认 50000 = 2400h 实测的一圈"));

   m_edSpeed = new QSpinBox(box);
   m_edSpeed->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_edSpeed->setSingleStep(1000);
   m_edSpeed->setSuffix(QStringLiteral(" pul/s"));

   m_edDwell = new QSpinBox(box);
   m_edDwell->setRange(0, 60000);
   m_edDwell->setSingleStep(50);
   m_edDwell->setSuffix(QStringLiteral(" ms"));
   m_edDwell->setToolTip(QStringLiteral("到点稳定之后, 再停这么久才采样"));

   m_edSettle = new QSpinBox(box);
   m_edSettle->setRange(0, 10000);
   m_edSettle->setSingleStep(20);
   m_edSettle->setSuffix(QStringLiteral(" ms"));
   m_edSettle->setToolTip(QStringLiteral(
      "「到位」要**连续**成立这么久才算数。\n"
      "单帧瞬时满足可能只是高速掠过时的一刹那。"));

   m_edSamples = new QSpinBox(box);
   m_edSamples->setRange(1, 100);
   m_edSamples->setToolTip(QStringLiteral(
      "每点连采几次取平均。**代价是每点多 n 倍读数时间** ——\n"
      "不是塞在停留期里白拿的, 真机一次往返几百 ms 时这项很贵。"));

   m_cbDir = new QComboBox(box);
   m_cbDir->addItem(QStringLiteral("X 正向 (+X)"));
   m_cbDir->addItem(QStringLiteral("X 负向 (-X)"));

   m_cbMode = new QComboBox(box);
   m_cbMode->addItem(QStringLiteral("蛇形 (逐行往返)"));
   m_cbMode->addItem(QStringLiteral("每行同向") );

   m_edCsv = new QLineEdit(box);
   m_edCsv->setPlaceholderText(QStringLiteral("scan_out/scan_YYYYmmdd_HHMMSS.csv"));
   QPushButton *btnCsv = new QPushButton(QStringLiteral("…"), box);
   btnCsv->setFixedWidth(28);
   connect(btnCsv, &QPushButton::clicked, this, &ScanWindow::onBrowseCsv);
   QHBoxLayout *csvRow = new QHBoxLayout;
   csvRow->setContentsMargins(0, 0, 0, 0);
   csvRow->addWidget(m_edCsv, 1);
   csvRow->addWidget(btnCsv);

   f->addRow(QStringLiteral("区域 X"), m_edAreaX);
   f->addRow(QStringLiteral("区域 Y"), m_edAreaY);
   f->addRow(QStringLiteral("分辨率"), m_edRes);
   f->addRow(QStringLiteral("1 单位 ="), m_edPpu);
   f->addRow(QStringLiteral("扫描速度"), m_edSpeed);
   f->addRow(QStringLiteral("单点停留"), m_edDwell);
   f->addRow(QStringLiteral("稳定窗口"), m_edSettle);
   f->addRow(QStringLiteral("每点采样"), m_edSamples);
   f->addRow(QStringLiteral("起始方向"), m_cbDir);
   f->addRow(QStringLiteral("扫描方式"), m_cbMode);
   f->addRow(QStringLiteral("CSV"), csvRow);

   m_lGrid = new QLabel(box);
   m_lGrid->setStyleSheet(QStringLiteral("color:#c8ced8;"));
   m_lEst  = new QLabel(box);
   m_lEst->setWordWrap(true);
   m_lEst->setStyleSheet(QStringLiteral("color:#7b8391;"));
   m_lWarn = new QLabel(box);
   m_lWarn->setWordWrap(true);
   m_lWarn->setStyleSheet(QStringLiteral("color:#ff8f8f;"));
   f->addRow(m_lGrid);
   f->addRow(m_lEst);
   f->addRow(m_lWarn);

   /*
    * **必须显式把缺省值填进控件。** QDoubleSpinBox 初值是 0, 而它的最小值是 0.1 ——
    * 于是"没设过"的控件会**自己夹到最小值**上: 区域变成 0.1×0.1、每单位脉冲变成 100,
    * 量程算出来只有 105 脉冲。界面看着像那么回事, 而扫描的区域其实是 0.1 单位。
    *
    * 这一段必须在下面那些 connect **之前** —— 否则每 set 一个值都会触发一轮
    * pushParams() → refresh(), 而 refresh() 要用的按钮和标签此刻还没建出来。
    */
   {
      const Params d;                    /* scanplan.h 里那一份就是缺省 */
      m_edAreaX->setValue(d.area_x_unit);
      m_edAreaY->setValue(d.area_y_unit);
      m_edRes  ->setValue(d.res_unit);
      m_edPpu  ->setValue(d.pulses_per_unit);
      m_edSpeed->setValue((int)d.speed_pul_s);
      m_edDwell->setValue(d.dwell_ms);
      m_edSettle->setValue(d.settle_ms);
      m_edSamples->setValue(d.samples_per_point);
      m_cbDir ->setCurrentIndex(d.start_positive ? 0 : 1);
      m_cbMode->setCurrentIndex(d.serpentine ? 0 : 1);
   }

   /* 参数一改就重算 —— 让操作员在**按开始之前**就看见这一趟多长 */
   const QList<QDoubleSpinBox *> dspins{m_edAreaX, m_edAreaY, m_edRes, m_edPpu};
   for (QDoubleSpinBox *s : dspins)
      connect(s, &QDoubleSpinBox::valueChanged, this, &ScanWindow::pushParams);
   const QList<QSpinBox *> ispins{m_edSpeed, m_edDwell, m_edSettle, m_edSamples};
   for (QSpinBox *s : ispins)
      connect(s, &QSpinBox::valueChanged, this, &ScanWindow::pushParams);
   connect(m_cbDir,  &QComboBox::currentIndexChanged, this, &ScanWindow::pushParams);
   connect(m_cbMode, &QComboBox::currentIndexChanged, this, &ScanWindow::pushParams);

   return box;
}

QWidget *ScanWindow::buildScanPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("扫描控制"), this);

   m_btnStart = new QPushButton(QStringLiteral("开始扫描"), box);
   m_btnStart->setObjectName(QStringLiteral("go"));
   connect(m_btnStart, &QPushButton::clicked, this, &ScanWindow::onStartClicked);

   m_btnPause = new QPushButton(QStringLiteral("暂停"), box);
   m_btnPause->setToolTip(QStringLiteral("立刻冻在当前位置 (**保持保持力矩**)。\n"
                                         "继续时会**重新走完当前点并重采**, 不会留下半点数据"));
   connect(m_btnPause, &QPushButton::clicked, this, &ScanWindow::onPauseClicked);

   m_btnResume = new QPushButton(QStringLiteral("继续"), box);
   connect(m_btnResume, &QPushButton::clicked, this, &ScanWindow::onResumeRunClicked);

   m_btnAbort = new QPushButton(QStringLiteral("中止"), box);
   m_btnAbort->setObjectName(QStringLiteral("danger"));
   m_btnAbort->setToolTip(QStringLiteral("冻住并结束本轮。**已采的数据留在 CSV 里** —— "
                                         "之后可以「打开 CSV 续扫」接着跑"));
   connect(m_btnAbort, &QPushButton::clicked, this, &ScanWindow::onAbortClicked);

   m_btnRetest = new QPushButton(QStringLiteral("重测选中点"), box);
   m_btnRetest->setToolTip(QStringLiteral("在画布上 **Shift + 左键** 选中一格, 再点这个"));
   connect(m_btnRetest, &QPushButton::clicked, this, &ScanWindow::onRetestClicked);

   m_btnOpen = new QPushButton(QStringLiteral("打开 CSV 续扫"), box);
   m_btnOpen->setToolTip(QStringLiteral("读回已有数据 → 只补没采过的点 → **继续追加同一个文件**"));
   connect(m_btnOpen, &QPushButton::clicked, this, &ScanWindow::onOpenCsvClicked);

   m_lProg = new QLabel(box);
   m_lProg->setStyleSheet(QStringLiteral("color:#c8ced8;"));
   m_lTime = new QLabel(box);
   m_lTime->setWordWrap(true);
   m_lTime->setStyleSheet(QStringLiteral("color:#7b8391;"));

   QHBoxLayout *r1 = new QHBoxLayout;
   r1->setContentsMargins(0, 0, 0, 0);
   r1->addWidget(m_btnStart, 2);
   r1->addWidget(m_btnPause, 1);
   r1->addWidget(m_btnResume, 1);
   r1->addWidget(m_btnAbort, 1);

   QHBoxLayout *r2 = new QHBoxLayout;
   r2->setContentsMargins(0, 0, 0, 0);
   r2->addWidget(m_btnRetest, 1);
   r2->addWidget(m_btnOpen, 1);

   QVBoxLayout *v = new QVBoxLayout(box);
   v->addLayout(r1);
   v->addLayout(r2);
   v->addWidget(m_lProg);
   v->addWidget(m_lTime);
   return box;
}

QWidget *ScanWindow::buildMeterPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("功率计"), this);
   QFormLayout *f = new QFormLayout(box);

   m_cbMeter = new QComboBox(box);
   m_cbMeter->addItem(m_manual->kind());
   m_cbMeter->addItem(m_random->kind());
   m_cbMeter->addItem(m_script->kind());
   m_cbMeter->setCurrentIndex(1);
   connect(m_cbMeter, &QComboBox::currentIndexChanged, this, &ScanWindow::onMeterChanged);

   m_edManualV = new QDoubleSpinBox(box);
   m_edManualV->setRange(-1e9, 1e9);
   m_edManualV->setDecimals(6);
   m_edManualV->setValue(1.0);
   connect(m_edManualV, &QDoubleSpinBox::valueChanged, this, &ScanWindow::onManualValueChanged);

   m_edRandomN = new QDoubleSpinBox(box);
   m_edRandomN->setRange(0.0, 1e6);
   m_edRandomN->setDecimals(4);
   m_edRandomN->setValue(0.05);           /* 与 RandomMeter 的缺省噪声一致 */
   m_random->setNoise(0.05);
   connect(m_edRandomN, &QDoubleSpinBox::valueChanged, this,
           [this](double v) { m_random->setNoise(v); });

   m_edScript = new QLineEdit(box);
   m_edScript->setPlaceholderText(QStringLiteral("每行一个数; # 开头与空行忽略"));
   m_btnScript = new QPushButton(QStringLiteral("…"), box);
   m_btnScript->setFixedWidth(28);
   connect(m_btnScript, &QPushButton::clicked, this, &ScanWindow::onBrowseScript);
   QHBoxLayout *scriptRow = new QHBoxLayout;
   scriptRow->setContentsMargins(0, 0, 0, 0);
   scriptRow->addWidget(m_edScript, 1);
   scriptRow->addWidget(m_btnScript);

   f->addRow(QStringLiteral("取样源"), m_cbMeter);
   f->addRow(QStringLiteral("手填值"), m_edManualV);
   f->addRow(QStringLiteral("噪声"), m_edRandomN);
   f->addRow(QStringLiteral("脚本"), scriptRow);

   m_lMeter = new QLabel(box);
   m_lMeter->setWordWrap(true);
   m_lMeter->setStyleSheet(QStringLiteral("color:#7b8391;"));
   f->addRow(m_lMeter);

   return box;
}

QWidget *ScanWindow::buildShadePanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("色标 (功率)"), this);
   QFormLayout *f = new QFormLayout(box);

   const double lo = m_canvas->shadeLo();
   const double hi = m_canvas->shadeHi();

   m_edShadeLo = new QDoubleSpinBox(box);
   m_edShadeLo->setRange(-1e12, 1e12);
   m_edShadeLo->setDecimals(6);
   m_edShadeLo->setValue(lo);

   m_edShadeHi = new QDoubleSpinBox(box);
   m_edShadeHi->setRange(-1e12, 1e12);
   m_edShadeHi->setDecimals(6);
   m_edShadeHi->setValue(hi);

   connect(m_edShadeLo, &QDoubleSpinBox::valueChanged, this,
           [this](double v) { m_canvas->setShadeRange(v, m_edShadeHi->value()); });
   connect(m_edShadeHi, &QDoubleSpinBox::valueChanged, this,
           [this](double v) { m_canvas->setShadeRange(m_edShadeLo->value(), v); });

   m_btnFit = new QPushButton(QStringLiteral("按数据定标"), box);
   m_btnFit->setToolTip(QStringLiteral("取已采数据的最小/最大作为色阶两端。**只在按它的时候改一次**"));
   connect(m_btnFit, &QPushButton::clicked, this, [this] {
      if (!m_canvas->fitShadeToData())
      {
         hint(QStringLiteral("还没有采到任何数据, 没法定标"), false);
         return;
      }
      syncShadeEdits();
   });

   QLabel *note = new QLabel(QStringLiteral(
      "色阶**锁定**, 不跟着数据实时变 —— 否则每来一个点整张图都会重排颜色, "
      "看到的\"变化\"其实是色阶自己在动, 那种图不能用来判断任何事。\n"
      "超出范围的格子夹到两端, 数值仍可在悬停里读到。"), box);
   note->setWordWrap(true);
   note->setStyleSheet(QStringLiteral("color:#6b7480;"));

   f->addRow(QStringLiteral("最小"), m_edShadeLo);
   f->addRow(QStringLiteral("最大"), m_edShadeHi);
   f->addRow(m_btnFit);
   f->addRow(note);
   return box;
}

/* ---------------------------------------------------------------- 参数 */

Params ScanWindow::currentParams() const
{
   Params p;
   p.area_x_unit = m_edAreaX->value();
   p.area_y_unit = m_edAreaY->value();
   p.res_unit    = m_edRes->value();
   p.pulses_per_unit = m_edPpu->value();

   p.speed_pul_s = (uint32_t)m_edSpeed->value();
   p.dwell_ms    = m_edDwell->value();
   p.settle_ms   = m_edSettle->value();
   p.samples_per_point = m_edSamples->value();

   p.serpentine     = (m_cbMode->currentIndex() == 0);
   p.start_positive = (m_cbDir->currentIndex() == 0);

   /* 量程永远按区域自动算 —— **不暴露给操作员**。它是个会因为手滑而把区域边缘
    * 悄悄削掉的量, 让人去填它没有任何好处 */
   p.range_pul = autoRangePul(p);
   return p;
}

void ScanWindow::pushParams()
{
   const Params p = currentParams();

   /* 量程跟着区域走。**只在数值真的变了才投命令** —— 否则每敲一个键都投一条,
    * 工作线程会为 "2" 和 "27" 各回一句 note, 状态栏自己跟自己打架 */
   if (p.range_pul != m_last_range)
   {
      m_last_range = p.range_pul;
      m_thr->postRange(m_last_range);
   }

   m_ctl->setParams(p);
   m_ctl->rebuildPlan();

   const int nx = m_ctl->gridNx();
   const int ny = m_ctl->gridNy();

   m_lGrid->setText(QStringLiteral("网格 %1 × %2 = **%3 点**   ±%4 单位")
                       .arg(nx).arg(ny).arg((qint64)nx * ny)
                       .arg(m_ctl->params().area_x_unit / 2.0, 0, 'f', 3));

   /* 预估是**线性**的, 实际一定更长 —— 每次移动的进近段都要减速。
    * 不写这一句的话, 跑起来比预计慢 20% 就会被当成"卡住了" */
   m_lEst->setText(QStringLiteral("每点 ≈ %1 ms   全程 ≈ %2\n(线性估计, **实际更长** —— 每次移动的进近段都要减速)")
                      .arg(estimatePerPointMs(p))
                      .arg(fmtDur(m_ctl->estimateTotalMs())));

   /* 两条否决: 参数本身不合法 / 几何超出量程 (超出的部分会被静默夹掉) */
   QString bad = m_ctl->paramsError();
   if (bad.isEmpty())
   {
      std::string why;
      if (!fitsRange(p, &why))
         bad = QString::fromStdString(why);
   }
   m_lWarn->setText(bad);

   refresh();
}

void ScanWindow::syncShadeEdits()
{
   /* setValue 会触发 valueChanged → setShadeRange, 那是幂等的, 不必屏蔽信号 */
   m_edShadeLo->setValue(m_canvas->shadeLo());
   m_edShadeHi->setValue(m_canvas->shadeHi());
}

void ScanWindow::applyCsvDefaultName()
{
   if (!m_edCsv->text().trimmed().isEmpty())
      return;

   const QString dir = QDir::current().filePath(QString::fromLatin1(kOutDir));
   const QString name = QStringLiteral("scan_%1.csv")
                           .arg(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyyMMdd_HHmmss")));
   m_edCsv->setText(QDir(dir).filePath(name));
}

void ScanWindow::onBrowseCsv()
{
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中, 不能换输出文件"), false);
      return;
   }

   const QString start = m_edCsv->text().trimmed().isEmpty()
                            ? QDir::current().filePath(QString::fromLatin1(kOutDir))
                            : m_edCsv->text().trimmed();

   const QString f = QFileDialog::getSaveFileName(
      this, QStringLiteral("扫描数据写到哪个 CSV"), start,
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_edCsv->setText(QDir::toNativeSeparators(f));
   m_last_dir = QFileInfo(f).absolutePath();
}

/* ---------------------------------------------------------------- 总线操作 */

void ScanWindow::onConnectClicked()
{
   if (m_connected)
   {
      disconnectAndStop();
      return;
   }

   if (m_nic->currentIndex() < 0 || m_nic->currentData().toString().isEmpty())
   {
      hint(QStringLiteral("先选一块网卡。下拉框是空的就点「刷新网卡」, "
                          "还是没有就说明 **Npcap 没装**"), false);
      return;
   }

   QMessageBox box(QMessageBox::Question,
                   QStringLiteral("连接并进 OP"),
                   QStringLiteral(
                      "接下来会:\n"
                      "  · 打开网卡 (Npcap 单进程独占 —— 别的工具此刻不能同时用这张卡)\n"
                      "  · 按总线上的实际从站建立过程数据, 必要时补 607Ah/6064h 映射\n"
                      "  · 进 OP, **开始每 2ms 发帧**\n"
                      "  · 把**当前位置**设为显示坐标 0 (零点世代 +1)\n\n"
                      "生效映射里主站拥有的项 (6040h/6060h/607Ah/60FFh/6081h/6083h/6084h)\n"
                      "会被每周期覆盖 —— **但这一步不发使能, 电机不带电**。\n\n"
                      "收尾时 (断开 / 关窗) 会还原 PDO 映射并降回 PRE_OP。\n"
                      "只改 RAM, 断电自然恢复。"),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   hint(QStringLiteral("正在连接... (选轴 / 补映射 / 进 OP 都要做 SDO, 慢是正常的)"), false);
   m_thr->postConnect(m_nic->currentData().toString());
}

void ScanWindow::onEnableClicked()
{
   /* 「使能」= CLI 的 --allow-motion。**每次都问**, 不做持久勾选、不自动使能 */
   QMessageBox box(QMessageBox::Warning,
                   QStringLiteral("使能 —— 电机会带电"),
                   QStringLiteral(
                      "确认:\n"
                      "  · 人已经在设备旁边\n"
                      "  · 手放在物理急停上\n"
                      "  · 滑台行程里没有手、工具、线\n\n"
                      "使能后轴进入 CSP 并带保持力矩。使能那一帧不会动作\n"
                      "(目标被钉在当前位置), 之后点画布 / 开始扫描才会走。"),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   m_thr->postEnable();
}

void ScanWindow::onCenterAllClicked()
{
   m_thr->postCenterAll();
   hint(QStringLiteral("两根轴都去显示坐标 0 (= 区域中心)"), false);
}

void ScanWindow::onZeroHereClicked()
{
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能重设零点"), true);
      return;
   }

   /* 零点一动, 同一个显示坐标指的就不是同一个物理位置了 —— 所以世代 +1。
    * 续扫时对不上会要求操作员确认, 这一句就是那个确认的依据 */
   m_epoch++;
   m_ctl->setZeroEpoch(m_epoch);

   m_thr->postZeroHere(0);
   m_thr->postZeroHere(1);

   hint(QStringLiteral("当前位置已设为显示坐标 0 (= 区域中心), 零点世代 → %1").arg(m_epoch), false);
   m_canvas->update();
}

/* ---------------------------------------------------------------- 功率计 */

void ScanWindow::onMeterChanged(int idx)
{
   if (m_ctl->running())
   {
      /* 扫描中途换源 = 同一张图上的数据来自两个不同的东西。禁掉 */
      hint(QStringLiteral("扫描进行中, 不能换取样源"), false);
      m_cbMeter->setCurrentIndex(idx == 0 ? (idx + 1) : (idx - 1));
      return;
   }

   m_meter->close();

   m_meter = (idx == 0) ? (PowerMeter *)m_manual
           : (idx == 1) ? (PowerMeter *)m_random
                        : (PowerMeter *)m_script;

   QString err;
   m_meter->open(&err);

   /* **必须告诉控制器**, 否则它还在听旧的源 —— 而旧源还活着, 会照常出数 */
   m_ctl->setMeter(m_meter);

   if (!err.isEmpty())
      hint(QStringLiteral("功率计打不开: ") + err, true);
   else
      hint(QStringLiteral("取样源已切到「%1」").arg(m_meter->kind()), false);

   refresh();
}

void ScanWindow::onManualValueChanged(double v)
{
   m_manual->setValue(v);
}

void ScanWindow::onBrowseScript()
{
   const QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("选一个每行一个数的脚本"), m_last_dir,
      QStringLiteral("文本 (*.txt *.csv *.dat);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_edScript->setText(QDir::toNativeSeparators(f));
   m_last_dir = QFileInfo(f).absolutePath();

   QString err;
   if (!m_script->setPath(f, &err))
   {
      hint(QStringLiteral("脚本读不了: ") + err, true);
      return;
   }

   if (m_meter == m_script)
   {
      m_script->close();
      m_script->open(&err);
      if (!err.isEmpty())
         hint(QStringLiteral("脚本源打不开: ") + err, true);
   }

   refresh();
}

/* ---------------------------------------------------------------- 扫描 */

void ScanWindow::onStartClicked()
{
   if (m_ctl->running())
      return;

   applyCsvDefaultName();

   const QString path = m_edCsv->text().trimmed();
   if (path.isEmpty())
   {
      hint(QStringLiteral("先给一个 CSV 输出文件"), false);
      return;
   }

   /* 开始前把这一趟的范围再说一遍。区域填错一个数字, 结果就是一头撞上硬件限位 ——
    * 而这一趟是一两个小时, 值得多问一句 */
   const Params p = currentParams();
   QMessageBox box(QMessageBox::Question,
                   QStringLiteral("开始扫描"),
                   QStringLiteral(
                      "区域 %1 × %2 单位 (**以原点为中心**, 即 ±%3 单位)\n"
                      "网格 %4 × %5 = **%6 点**, 分辨率 %7 单位\n"
                      "速度 %8 pul/s, 停留 %9 ms, 稳定窗口 %10 ms\n"
                      "预计全程 **%11** (线性估计, 实际更长)\n\n"
                      "取样源: %12\n"
                      "数据写入: %13\n\n"
                      "确认行程里没有手、工具、线, 手放在物理急停上。")
                      .arg(p.area_x_unit, 0, 'f', 3).arg(p.area_y_unit, 0, 'f', 3)
                      .arg(p.area_x_unit / 2.0, 0, 'f', 3)
                      .arg(m_ctl->gridNx()).arg(m_ctl->gridNy())
                      .arg(m_ctl->totalPoints())
                      .arg(p.res_unit, 0, 'f', 3)
                      .arg(p.speed_pul_s)
                      .arg(p.dwell_ms).arg(p.settle_ms)
                      .arg(fmtDur(m_ctl->estimateTotalMs()))
                      .arg(m_meter->kind())
                      .arg(QDir::toNativeSeparators(path)),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   QString err;
   if (!m_ctl->start(path, &err))
   {
      hint(QStringLiteral("开不了: ") + err, true);
      return;
   }

   m_meter->close();
   m_meter->open(nullptr);

   m_banner->setVisible(false);
   refresh();
}

void ScanWindow::onOpenCsvClicked()
{
   if (m_ctl->running())
      return;

   const QString start = m_last_dir.isEmpty()
                            ? QDir::current().filePath(QString::fromLatin1(kOutDir))
                            : m_last_dir;

   const QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开一个扫到一半的 CSV"), start,
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_last_dir = QFileInfo(f).absolutePath();
   m_edCsv->setText(QDir::toNativeSeparators(f));

   QString err, why;
   if (m_ctl->resume(f, false, &err, &why))
   {
      m_banner->setVisible(false);
      hint(QStringLiteral("续扫: 已读回 %1, 补剩下的点").arg(QDir::toNativeSeparators(f)), false);
      refresh();
      return;
   }

   if (!err.isEmpty())
   {
      hint(QStringLiteral("接不上: ") + err, true);
      return;
   }

   /* why 非空 = 只是"要你确认一下", 不是错误。零点世代对不上意味着中途重连过,
    * 下半场和上半场可能不在同一个物理位置上 —— 这种事必须让人自己拍板 */
   QMessageBox box(QMessageBox::Warning,
                   QStringLiteral("续扫前确认"),
                   QStringLiteral("这一轮有个地方对不上, 继续之前请确认:\n\n%1\n\n"
                                  "确认无误再继续。")
                      .arg(why),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   err.clear();
   why.clear();
   if (!m_ctl->resume(f, true, &err, &why))
   {
      hint(QStringLiteral("还是接不上: ") + (err.isEmpty() ? why : err), true);
      return;
   }

   m_banner->setVisible(false);
   hint(QStringLiteral("续扫 (已确认零点世代差异): %1").arg(QDir::toNativeSeparators(f)), false);
   refresh();
}

void ScanWindow::onPauseClicked()
{
   m_ctl->pause();
   hint(QStringLiteral("已暂停 —— 目标冻在当前位置, **保持保持力矩**。"
                       "「继续」会重新走完当前点并重采"), false);
   refresh();
}

void ScanWindow::onResumeRunClicked()
{
   m_ctl->resumeRun();
   refresh();
}

void ScanWindow::onAbortClicked()
{
   m_ctl->abort(QStringLiteral("操作员按了「中止」"));
   hint(QStringLiteral("已中止。已采的数据在 %1 里 —— 可以「打开 CSV 续扫」接着跑")
           .arg(QDir::toNativeSeparators(m_ctl->csvPath())), false);
   refresh();
}

void ScanWindow::onRetestClicked()
{
   if (m_ctl->running())
      return;

   const int ix = m_canvas->selectedIx();
   const int iy = m_canvas->selectedIy();
   if (ix < 0 || iy < 0)
   {
      hint(QStringLiteral("先在画布上 **Shift + 左键** 选中一格"), false);
      return;
   }

   QString err;
   if (!m_ctl->retest(ix, iy, &err))
   {
      hint(QStringLiteral("重测不了: ") + err, true);
      return;
   }

   m_banner->setVisible(false);
   hint(QStringLiteral("重测 (%1, %2) —— 会往同一个 CSV 再追加一行").arg(ix).arg(iy), false);
   refresh();
}

/* ---------------------------------------------------------------- 刷新 */

void ScanWindow::refresh()
{
   /* 状态机先走一格, 再读遥测 —— 顺序反过来的话, 界面上显示的永远是上一拍的状态 */
   m_ctl->tick(m_clock.elapsed());

   const BusTelem t = m_thr->telemetry();

   /*
    * 按钮形态完全由遥测决定:
    *   in_op = 正在发帧 (真连上了)   busy = 正在连接或正在收尾
    * 两者任一为真就是"占着总线", 于是网卡选择与「连接」都锁住。
    * **不用"点过哪个按钮"来推** —— 那种推法会和线程的真实状态错开。
    */
   const bool onair = t.in_op || t.busy;
   if (onair != m_connected)
      setConnected(onair);

   if (t.in_op)
   {
      m_lWkc->setText(QStringLiteral("WKC %1 / 期望 %2").arg(t.wkc).arg(t.expected_wkc));
      m_lWkc->setStyleSheet(t.wkc >= t.expected_wkc
                               ? QStringLiteral("color:#7b8391")
                               : QStringLiteral("color:#ffb020; font-weight:bold"));
   }
   else
   {
      m_lWkc->setText(QString());
      m_lWkc->setStyleSheet(QString());
   }

   m_lNote->setText(t.note);

   /* 画布: 扫描中一律吞掉点击。**只有画布一处决定吞不吞** —— 窗口再拦一道是兜底,
    * 两处都判就会有一天只改了一处 */
   const bool running = m_ctl->running();
   m_canvas->setManualAllowed(!running);

   /* 扫描中锁住参数与取样源。几何改到一半的扫描是没有意义的 —— 落进 CSV 的
    * (ix,iy) 和实际位置就对不上了 */
   const QList<QWidget *> locked{
      m_edAreaX, m_edAreaY, m_edRes, m_edPpu, m_edSpeed, m_edDwell,
      m_edSettle, m_edSamples, m_cbDir, m_cbMode, m_edCsv, m_cbMeter,
      m_edManualV, m_edRandomN, m_edScript, m_btnScript, m_btnOpen
   };
   for (QWidget *w : locked)
      w->setEnabled(!running);

   const bool can_move = m_connected && !running;
   m_btnEnable->setEnabled(can_move && !t.ax[0].enabled);
   m_btnStop->setEnabled(m_connected);
   m_btnDis->setEnabled(m_connected);
   m_btnCenter->setEnabled(can_move);
   m_btnZero->setEnabled(can_move);

   const bool meter_ok = (m_meter != nullptr) && m_meter->isOpen();
   const bool params_ok = m_ctl->paramsError().isEmpty();
   m_btnStart->setEnabled(!running && m_connected && meter_ok && params_ok);

   const ScanController::State st = m_ctl->state();
   m_btnPause->setEnabled(running && st != ScanController::State::Paused);
   m_btnResume->setEnabled(st == ScanController::State::Paused);
   m_btnAbort->setEnabled(running);
   m_btnRetest->setEnabled(!running && m_canvas->selectedIx() >= 0);

   /* ---- 进度 ---- */
   if (running || st == ScanController::State::Paused)
   {
      m_lProg->setText(QStringLiteral("%1 / %2 点   剩 %3   第 %4/%5 个")
                          .arg(m_ctl->completedPoints())
                          .arg(m_ctl->totalPoints())
                          .arg(m_ctl->pendingPoints())
                          .arg(m_ctl->index() >= 0 ? m_ctl->index() + 1 : 0)
                          .arg(m_ctl->totalPoints()));

      QString extra;
      if (m_ctl->settlingNow())
         extra = QStringLiteral("  (等稳定 %1 ms)").arg(m_ctl->stateMs());

      m_lTime->setText(QStringLiteral("已用 %1   预计剩余 %2%3")
                          .arg(fmtDur(m_ctl->elapsedMs()))
                          .arg(fmtDur(m_ctl->state() == ScanController::State::Paused
                                         ? -1
                                         : (int64_t)((double)m_ctl->estimateTotalMs()
                                                     * m_ctl->pendingPoints()
                                                     / (m_ctl->totalPoints() > 0
                                                           ? m_ctl->totalPoints() : 1))))
                          .arg(extra));
   }
   else
   {
      m_lProg->setText(QStringLiteral("%1 / %2 点")
                          .arg(m_ctl->completedPoints())
                          .arg(m_ctl->totalPoints()));
      m_lTime->setText(m_ctl->stateText());
   }

   /* ---- 功率计状态 ---- */
   if (m_meter != nullptr)
   {
      QString s = QStringLiteral("%1 · %2")
                     .arg(m_meter->kind())
                     .arg(m_meter->isOpen() ? QStringLiteral("已打开")
                                            : QStringLiteral("**没打开**"));
      if (m_meter == m_script)
         s += QStringLiteral(" · %1 个值, 游标 %2")
                 .arg(m_script->count()).arg(m_script->cursor());
      m_lMeter->setText(s);
   }

   /* 故障横幅只在**上升沿**弹一次: 30Hz 每帧都设一遍会把重绘刷爆,
    * 也会把操作员刚点掉的提示又顶回来 */
   if (t.fault && !m_faultShown)
   {
      m_faultShown = true;
      hint(QStringLiteral("6041h bit3 = 故障 —— 目标已冻结, 电机状态请以驱动器面板为准。"
                          "查清原因再「失能」重来"), true);
   }
   else if (!t.fault)
   {
      m_faultShown = false;
   }

   if (m_thr->maybeLive() && !m_warnedLive)
   {
      m_warnedLive = true;
      QTimer::singleShot(0, this, [this] { warnMaybeLive(); });
   }

   m_canvas->update();
}

void ScanWindow::setConnected(bool on)
{
   /* 连接会把零点重设为"当时所在的位置" (tryInitOrigin) —— 于是同一个显示坐标
    * 指的**可能已经是另一个物理位置**。世代 +1, 续扫时这一条会被查出来 */
   if (on && !m_connected)
   {
      m_epoch++;
      m_ctl->setZeroEpoch(m_epoch);
   }

   m_connected = on;
   m_nic->setEnabled(!on);
   m_btnNic->setEnabled(!on);
   m_btnConn->setText(on ? QStringLiteral("断开") : QStringLiteral("连接 (进 OP)"));
}

void ScanWindow::hint(const QString &s, bool fault)
{
   m_banner->setText(s);
   m_banner->setStyleSheet(fault ? kBannerFault : kBannerWarn);
   m_banner->setVisible(true);

   /* **故障不自动消失** —— 一两个小时没人看着的一趟扫下来, 一闪而过的提示
    * 等于没提示 */
   if (!fault)
      m_bannerTimer->start(8000);
   else
      m_bannerTimer->stop();
}

void ScanWindow::showFault(const QString &why)
{
   hint(QStringLiteral("**扫描已自动中止**: ") + why, true);

   /* 自动中止是**无人值守时唯一能救回这一趟的东西** —— 所以横幅之外还要弹模态。
    * 用 singleShot 挪出当前这一帧再弹, 免得在 30Hz 的槽里嵌套一个事件循环 */
   if (m_autoStop > 0)
      return;
   m_autoStop = 1;
   QTimer::singleShot(0, this, [this, why] {
      m_autoStop = 0;
      QMessageBox::critical(this, QStringLiteral("扫描已自动中止"),
         QStringLiteral("%1\n\n"
                        "已经采到的数据都在 CSV 里 (%2), 没有丢。\n\n"
                        "查清原因、把滑台处理妥当之后, 可以「打开 CSV 续扫」接着跑。")
            .arg(why, QDir::toNativeSeparators(m_ctl->csvPath())));
   });
}

void ScanWindow::warnMaybeLive()
{
   QMessageBox::critical(this, QStringLiteral("电机可能仍带电"),
      QStringLiteral(
         "收尾时**未能确认所有轴都失能** —— 与控制台退出码 10 同一语义。\n\n"
         "立即断开驱动器的动力电源, 不要只依赖软件。\n\n"
         "常见原因是收尾过程中总线已经掉了: 那时驱动器还带着力矩, 而我们已经没有\n"
         "通道去撤它。"));
}

/* ---------------------------------------------------------------- 收尾 */

void ScanWindow::disconnectAndStop()
{
   /* **先中止扫描再断总线。** 反过来的话状态机会看到"掉出 OP", 然后作为一个
    * "异常"去自动中止 —— 一条本来正常的收尾路径会变成红色告警 */
   if (m_ctl->running())
      m_ctl->abort(QStringLiteral("断开总线"));

   if (!m_thr->isRunning())
      return;

   /* 断开 = 让工作线程自己走 teardown (失能 → 还原映射 → 降 PRE_OP → 关网卡)。
    * 用队列命令而不是 ::terminate: 收尾这几步不能被打断 */
   m_thr->postDisconnect();

   /* 等它真的收完 —— 条件是**不在发帧且不在忙**, 不是"点过断开"。teardown 是排队执行的,
    * 从投递到开跑之间有一小段, 只看 in_op 会在那一段里误判成"收完了"。
    * 每根轴的失能确认与状态机迁移都有超时, 所以给到 12 秒 */
   for (int i = 0; i < 600; i++)
   {
      const BusTelem t = m_thr->telemetry();
      if (!t.in_op && !t.busy)
         break;
      QThread::msleep(20);
   }

   setConnected(false);
}

void ScanWindow::closeEvent(QCloseEvent *e)
{
   if (m_ctl->running())
   {
      QMessageBox box(QMessageBox::Warning,
         QStringLiteral("扫描还在跑"),
         QStringLiteral("扫描正在跑, 关窗会**先中止它**。\n\n"
                        "已经采到的数据在 CSV 里, 不会丢 —— 之后可以「打开 CSV 续扫」接着跑。\n\n"
                        "确定关吗?"),
         QMessageBox::Ok | QMessageBox::Cancel, this);
      box.setDefaultButton(QMessageBox::Cancel);
      if (box.exec() != QMessageBox::Ok)
      {
         e->ignore();
         return;
      }
   }

   if (m_thr->isRunning())
   {
      disconnectAndStop();

      m_thr->requestQuit();
      if (!m_thr->wait(15000))
      {
         QMessageBox::warning(this, QStringLiteral("收尾超时"),
            QStringLiteral("工作线程 15 秒没退出来 —— 收尾可能没走完。\n"
                           "关掉窗口后, 请直接断掉驱动器的动力电源。"));
      }
      else if (m_thr->maybeLive() && !m_warnedLive)
      {
         m_warnedLive = true;
         warnMaybeLive();
      }
   }
   e->accept();
}

}   /* namespace scan */
