#include "scanwindow.h"

#include "ophircom.h"

#include "mapcanvas.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QSplitter>
#include <QStringList>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>

namespace scan {

static const char *kBannerWarn =
   "QLabel#banner { background:#4a3a12; color:#ffd479; padding:6px; border-radius:4px; }";
static const char *kBannerFault =
   "QLabel#banner { background:#5a1f1f; color:#ffb3b3; padding:6px; border-radius:4px; }";
static const char *kBannerInfo =
   "QLabel#banner { background:#1d3346; color:#a9cfe8; padding:6px; border-radius:4px; }";

/* 默认 CSV 目录, 相对当前工作目录 (从仓库根敲 ./bin/scan.exe 时就落在 scan_out/) */
static const char *kOutDir = "scan_out";

/* 「读一次」等回话的上限。必须比真机那条腿自己的报错阈值 1800ms 长, 否则它那句诊断
 * 永远来不及发出来, 界面上只剩"没有回应"。 */
static const int kReadOnceTimeoutMs = 6000;

/* 功率的显示格式 (量级从 nW 到 W, 不固定小数位):
 *   |v| >= 1e-3 -> 'g' 6 位有效数字;  否则 / 0 -> 'e' 4 位有效数字。
 * 只给操作员核对用, 入 CSV 的是 double, 一位没少。 */
static QString fmtWatts(double v)
{
   if (std::fabs(v) >= 1e-3)
      return QString::number(v, 'g', 6);
   return QString::number(v, 'e', 4);
}

/* 「读一次」那条固定说明, 只有一份: buildMeterPanel 装上, refresh() 按不了时把原因拼在它前面 */
static const QString &readOnceTip()
{
   static const QString s = QStringLiteral(
      "向**当前取样源**要一个数并显示出来 —— 不用跑整趟扫描就能确认功率计接上了、"
      "读数合理。\n\n"
      "它走的是扫描用的同一条路 (请求 → 读数信号), 所以这里通了, 采集那条路也就通了。\n\n"
      "**扫描进行中它是禁用的**: 接口约定同一时刻只允许一个未决请求, 而那个请求归"
      "扫描状态机。扫描停下来 (空闲/跑完/已中止) 才能手动读。\n\n"
      "显示的时延是**一次往返的实测值** —— 它必须远小于扫描参数里的读数超时, "
      "否则真机上会每点都超时。");
   return s;
}

/* ---------------------------------------------------------------- 信号灯 */

/* 信号灯: 一个 12px 的圆点, 四种样子 (ScanWindow::Lamp)。判据只一条: 灯亮 = 这件事正在发生。
 * Unknown 灰 = 不知道; Off 灭 = 没发生; Ok 绿亮 = 使能带电; Bad 红亮 = 出事了 (故障 / 撞限位) */
/* 每一盏灯都带上的那一句 (状态栏那两盏也用它) */
static const char *kLampRule =
   "\n\n灯亮 = 这件事正在发生; 灭 = 没发生; 灰 = 不知道。";

static void paintLamp(QLabel *l, ScanWindow::Lamp s)
{
   static const char *kCss[] = {
      "border:1px solid #6b7480; border-radius:6px; background:#4a5058;",  /* Unknown */
      "border:1px solid #3c434e; border-radius:6px; background:#20242b;",  /* Off */
      "border:1px solid #1f8a4c; border-radius:6px; background:#33d17a;",  /* Ok */
      "border:1px solid #ffb3b3; border-radius:6px; background:#ff4d4d;"   /* Bad */
   };
   l->setStyleSheet(QString::fromLatin1(kCss[(int)s]));
}

static QLabel *makeLamp(QWidget *parent, const QString &tip)
{
   QLabel *l = new QLabel(parent);
   l->setFixedSize(12, 12);
   l->setToolTip(tip);
   paintLamp(l, ScanWindow::Lamp::Unknown);
   return l;
}

/* 灯 + 字 = 一条信息。装在一起, 免得布局把它们排散了 */
static QWidget *lampUnit(QWidget *parent, QLabel *lamp, QLabel *text, int left = 10)
{
   QWidget *w = new QWidget(parent);
   QHBoxLayout *h = new QHBoxLayout(w);
   h->setContentsMargins(left, 0, 2, 0);   /* 状态栏里留一点: 两根轴的信息要能分开读 */
   h->setSpacing(5);
   h->addWidget(lamp);
   h->addWidget(text);
   return w;
}

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

/* ---------------------------------------------------------------- 滚轮闸 */

/* 只有已经选中的输入框才认滚轮 (先点它拿焦点, 滚轮才改值): 否则滚参数栏时一次翻页会把
 * 路过的那几个框各改一格 —— 而扫描区域算错一格是要撞限位的。没焦点时不吃这一滚。
 *
 * 事件先落在 spin box 内部那个 QLineEdit 上, 所以过滤器要挂在输入框及其每一个子控件上
 * (见 guard()); 判"选中没有"要从收到事件的控件往上找到被闸的输入框, 再比焦点。
 * 参数栏一滚, 选中就作废 (见 buildUi 里那个 connect)。 */
class WheelNeedsFocus : public QObject
{
public:
   explicit WheelNeedsFocus(QObject *parent = nullptr) : QObject(parent) {}

   /* 把 w 以及它的每一个子控件都闸上 */
   void guard(QWidget *w)
   {
      if (w == nullptr)
         return;

      m_guards.insert(w);
      w->installEventFilter(this);
      for (QWidget *k : w->findChildren<QWidget *>())
         k->installEventFilter(this);
   }

protected:
   bool eventFilter(QObject *obj, QEvent *e) override
   {
      if (e->type() != QEvent::Wheel)
         return false;

      QWidget *w = qobject_cast<QWidget *>(obj);
      if (w == nullptr)
         return false;

      QWidget *input = guardedAncestor(w);
      if (input == nullptr)
         return false;                 /* 这一滚不落在参数框上 (画布 / 滚动区自己) */

      if (hasFocusInside(input))
         return false;                 /* 选中过了 —— 按 Qt 原来的规矩改值 */

      e->ignore();                     /* 没选中: 不吃, 让父级 (参数栏滚动区) 去滚 */
      return true;
   }

private:
   QWidget *guardedAncestor(QWidget *w) const
   {
      for (QObject *o = w; o != nullptr; o = o->parent())
      {
         auto *qw = qobject_cast<QWidget *>(o);
         if (qw != nullptr && m_guards.contains(qw))
            return qw;
      }
      return nullptr;
   }

   static bool hasFocusInside(const QWidget *w)
   {
      for (const QObject *o = QApplication::focusWidget(); o != nullptr; o = o->parent())
      {
         if (o == w)
            return true;
      }
      return false;
   }

   QSet<QWidget *> m_guards;
};

/* 网卡名 (\Device\NPF_{GUID}) 太长, 横幅里只说得出那截 GUID —— 那是唯一有信息量的部分 */
static QString nicShort(const QString &n)
{
   const int i = n.lastIndexOf(QLatin1Char('\\'));
   QString s = (i >= 0) ? n.mid(i + 1) : n;
   s.remove(QLatin1Char('{'));
   s.remove(QLatin1Char('}'));
   return s;
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
   m_ophir  = new OphirMeter(this);

   /* 信息是工作线程攒好之后发过来的 (跨线程 → 自动排队, 落回 GUI 线程执行) */
   connect(m_ophir, &OphirMeter::infoChanged, this, &ScanWindow::onMeterInfoChanged);
   connect(m_ophir, &OphirMeter::configFailed, this,
           [this](const QString &e) { hint(QStringLiteral("改功率计配置失败: ") + e, true); });

   /* 「读一次」要能听见任何一个源的回话 (m_meter 会换): 四个都接上, 靠 m_readPending
    * 认出"这一份是不是我的"。控制器在跑时它是 false, 那时所有回话都归状态机 */
   for (PowerMeter *m : { static_cast<PowerMeter *>(m_manual),
                          static_cast<PowerMeter *>(m_random),
                          static_cast<PowerMeter *>(m_script),
                          static_cast<PowerMeter *>(m_ophir) })
   {
      connect(m, &PowerMeter::readingReady,  this, &ScanWindow::onReadOnceReady);
      connect(m, &PowerMeter::readingFailed, this, &ScanWindow::onReadOnceFailed);
   }

   m_meter  = m_random;                 /* 默认随机源: 一按开始就有数据可看 */
   m_meter->open(nullptr);

   m_ctl = new ScanController(m_busv, m_meter, this);

   /* 单调钟先起: 没 start 的 QElapsedTimer 读数未定义, 而 pushParams() 会走到 refresh()
    * 里的 m_ctl->tick(elapsed()) */
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
              /* 当前选着的那条优先保住 (点「刷新网卡」不该把选好的卡弄丢);
               * 还没选过 (开机第一次) 才轮到 scan.ini 里记住的那条 */
              const QString cur = m_nic->currentData().toString();
              const QString want = cur.isEmpty() ? m_savedNic : cur;

              m_nic->clear();
              for (int i = 0; i < names.size(); i++)
                 m_nic->addItem(descs.value(i, names[i]), names[i]);

              if (want.isEmpty())
                 return;

              const int idx = m_nic->findData(want);
              if (idx >= 0)
              {
                 m_nic->setCurrentIndex(idx);
                 return;
              }

              /* 上次那张卡不在了 (换了机器 / USB 网卡没插)。必须说出来, 否则人以为程序记错了 */
              if (cur.isEmpty())
                 hint(QStringLiteral("上次用的网卡 (%1) 不在列表里 —— 请重新选一块。"
                                     "(记在 exe 旁边的 scan.ini 里)").arg(nicShort(want)),
                      false);
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

   /* 状态机的节拍 + 界面刷新, 30Hz 一把。状态机自己不持定时器 —— 时间由外面喂,
    * 单测里才能换一个手动时钟跑 */
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
       * 画布已吞掉扫描中的点击, 这里再拦一道 */
      if (m_ctl->running())
         return;
      m_thr->setTarget(0, x);
      m_thr->setTarget(1, y);
   });
   connect(m_canvas, &MapCanvas::cellPicked, this, [this](int, int) { refresh(); });

   /* 右侧一列 (扫描参数 / 扫描控制 / 功率计 / 色标) 装进 QScrollArea: 这一列比窗口高是常态,
    * 否则窗口一矮最下面的「色标」就被挤得看不见了 */
   QWidget *side = new QWidget;
   side->setMinimumWidth(340);

   QVBoxLayout *sv = new QVBoxLayout(side);
   sv->setContentsMargins(0, 0, 0, 0);
   sv->setSpacing(8);
   /* 「轴信号」和「限位开关」排在最上面: 它们是状态, 要滚才能看到的状态指示器不算指示器 */
   sv->addWidget(buildAxisPanel());
   sv->addWidget(buildLimitPanel());
   /* 第三块:「回零」。它是动作不是参数, 但回零找的就是上面那三盏灯说的那几个开关 */
   sv->addWidget(buildHomePanel());
   sv->addWidget(buildParamPanel());
   sv->addWidget(buildScanPanel());
   sv->addWidget(buildMeterPanel());
   sv->addWidget(buildShadePanel());
   sv->addStretch(1);

   QScrollArea *sideScroll = new QScrollArea(central);
   sideScroll->setWidget(side);
   sideScroll->setWidgetResizable(true);
   sideScroll->setFrameShape(QFrame::NoFrame);
   /* 两条滚动条都用 AsNeeded: 内容不允许被悄悄裁掉, 放不下时才出来提醒 */
   sideScroll->setMinimumWidth(320);
   /* 下限压到 0: 否则滚动区会拿里面那列的高度去顶窗口最小高度, 窗口就再也缩不小了 */
   sideScroll->setMinimumHeight(0);

   /* 左画布 | 右参数栏, 中间一根可拖的分隔条。参数栏那些输入框有自己的最小宽度, 用"固定
    * 宽度"方案一压到它下面 QScrollArea 就把右边裁掉、拖都拖不回来; QSplitter 自己算分配。 */
   QSplitter *split = new QSplitter(Qt::Horizontal, central);
   split->addWidget(m_canvas);
   split->addWidget(sideScroll);
   split->setStretchFactor(0, 1);        /* 窗口变大, 多出来的地方全给画布 */
   split->setStretchFactor(1, 0);
   split->setSizes({760, 400});
   split->setCollapsible(0, false);      /* 别把哪一边拖没了 —— 两边都是要看的 */
   split->setCollapsible(1, false);
   split->setChildrenCollapsible(false);

   QVBoxLayout *v = new QVBoxLayout(central);
   v->addWidget(buildTopBar());
   v->addWidget(m_banner);
   v->addWidget(split, 1);
   setCentralWidget(central);

   /* 滚轮闸: 名单是找出来的, 不是手写的表 (漏一个只是那个框还在被滚轮改, 不报错)。
    * 挂在最后 —— 这里才保证上面那六组框全都建出来了。
    * QLineEdit (CSV / 脚本路径) 不闸: 滚轮改不了它的文字, 那一滚照旧去滚参数栏。 */
   m_wheelGuard = new WheelNeedsFocus(this);
   for (QAbstractSpinBox *x : findChildren<QAbstractSpinBox *>())
      m_wheelGuard->guard(x);
   for (QComboBox *x : findChildren<QComboBox *>())
      m_wheelGuard->guard(x);

   /* 参数栏一滚, 选中就作废 (另一半是 WheelNeedsFocus 那条"点过才认"): 只清滚动区里那个焦点 */
   connect(sideScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [sideScroll](int) {
      QWidget *fw = QApplication::focusWidget();
      if (fw != nullptr && sideScroll->isAncestorOf(fw))
         fw->clearFocus();
   });

   /* ---- 状态栏 ---- */
   m_lNote = new QLabel(this);
   m_lWkc  = new QLabel(this);
   m_lWkc->setFont(QFont(QStringLiteral("Consolas")));

   /* 限位两根轴各一个, 灯 + 字一起常显 (没连接时写 "--"、灯是灰的, 于是"看不到它"
    * 不会跟"限位正常"混淆) */
   /* 这一盏说的是判据, 不是开关本身: 默认判据是 6041h bit11 (勾了输入反转就换成反相后的
    * 那两个限位开关), 回答的是"会不会中止扫描"那一个问题 */
   const QString limTip = QStringLiteral(
      "硬件限位信号灯 (6041h bit11) —— **这一盏说的是「会不会中止扫描」**。\n"
      "红亮 = 手册对这一位的定义「**硬件限位信号有效**」成立了, 扫描中会自动中止。\n"
      "它报的是那路信号**此刻的电平**, 不是「撞过了」—— 所以它未必真有个开关压着: "
      "极性配反 (NPN 传感器 + 2300h 按常开配) 会让它一直亮着。\n\n"
      "它**不**等同于参数栏「限位开关」那六盏: 那一组说的是**开关本身压着没有** (60FDh)。\n"
      "两者可能不一致 —— 不一致时**以这一盏为准**。扫描经过原点开关不会中止。\n\n"
      "勾上「输入电平反转」之后这一盏换了判据: 那时它看的是**反相之后的**两个限位开关, "
      "与 6041h bit11 无关 (「以这一盏为准」仍然成立 —— 只是它背后的来源换了)。")
      + QString::fromUtf8(kLampRule);

   m_lampX = makeLamp(this, limTip);
   m_lampY = makeLamp(this, limTip);
   m_lLimX = new QLabel(this);
   m_lLimY = new QLabel(this);

   statusBar()->addWidget(m_lNote, 1);
   statusBar()->addPermanentWidget(lampUnit(this, m_lampX, m_lLimX));
   statusBar()->addPermanentWidget(lampUnit(this, m_lampY, m_lLimY));
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
   m_btnEnable->setToolTip(QStringLiteral(
      "切 CSP 模式并使能 —— 让电机持续带电的按钮之一\n"
      "(另一个是「回零」: 它自己也要带电才能找开关)"));
   connect(m_btnEnable, &QPushButton::clicked, this, &ScanWindow::onEnableClicked);

   m_btnStop = new QPushButton(QStringLiteral("停止"), w);
   m_btnStop->setToolTip(QStringLiteral(
      "目标冻在当前位置, **保持保持力矩**(不卸力)。"
      "扫描中请用「中止」, 它除了冻住还会把进度留在 CSV 里。\n"
      "**回零进行中按它 = 立即中止回零** (不用等 30 秒超时)"));
   /* 走槽而不是直连 postStop: 回零期间这个按钮必须换成立即中止 (非回零时两者一字不差) */
   connect(m_btnStop, &QPushButton::clicked, this, &ScanWindow::onStopClicked);

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

/* 参数栏两块信号网格的列号。列号 = 本组信号数, 这个值同时要交给 setColumnStretch ——
 * 单独写错一次就会凭空多出一个空列, 把前面几列挤成一个字宽。 */
enum { AX_ENABLED = 0, AX_FAULT = 1, AX_NCOL = 2 };
enum { LIM_HOME = 0, LIM_POS = 1, LIM_NEG = 2, LIM_NCOL = 3 };

/* 两块网格共用的那一句 kLampRule 定义在本文件顶部 (状态栏那两盏也要用)。 */

/* 「轴信号」: 每根两个信号 (使能 / 故障), 第三行是跨全部列的「故障复位」按钮。
 * 判据同 kLampRule; 名字写在表头上, 不每格重复。 */
QWidget *ScanWindow::buildAxisPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("轴信号"), this);
   QGridLayout *g = new QGridLayout(box);
   g->setContentsMargins(6, 4, 6, 6);
   g->setHorizontalSpacing(12);
   g->setVerticalSpacing(5);

   m_axGrid.ncol = AX_NCOL;

   static const char *kHead[AX_NCOL] = { "使能", "故障" };
   static const char *kTip[AX_NCOL] = {
      "6041h bit2 —— 电机带电。\n"
      "未使能时点画布不会动: 这是「这个轴现在能不能走」的答案。",

      "6041h bit3 —— 驱动器故障位。\n"
      "**扫描中置起会自动中止**; 用下面的「故障复位」清掉它再启扫。"
   };

   for (int s = 0; s < AX_NCOL; s++)
   {
      QLabel *h = new QLabel(QString::fromUtf8(kHead[s]), box);
      h->setStyleSheet(QStringLiteral("color:#6b7480;"));
      g->addWidget(h, 0, s + 1);
   }

   for (int i = 0; i < 2; i++)
   {
      QLabel *nm = new QLabel(QStringLiteral("轴%1").arg(i == 0 ? 'X' : 'Y'), box);
      nm->setStyleSheet(QStringLiteral("color:#9aa3ae;"));
      g->addWidget(nm, i + 1, 0);

      for (int s = 0; s < AX_NCOL; s++)
      {
         m_axGrid.lamp[i][s] = makeLamp(box, QString::fromUtf8(kTip[s])
                                             + QString::fromUtf8(kLampRule));
         m_axGrid.text[i][s] = new QLabel(box);
         g->addWidget(lampUnit(box, m_axGrid.lamp[i][s], m_axGrid.text[i][s], 0),
                      i + 1, s + 1);
      }
   }

   /* 第三行: 「故障复位」跨满全部列。它不按故障灯决定可用性 —— 那盏灯是遥测推的, 永远
    * 滞后于驱动器; 这里只拦界面自己才知道的两件事: 没连接、扫描在跑 (工作线程另有闸)。 */
   m_btnFaultRst = new QPushButton(QStringLiteral("故障复位"), box);
   m_btnFaultRst->setToolTip(QStringLiteral(
      "清驱动器的故障位 (6040h bit7 上升沿)。\n\n"
      "· **只对报了故障的轴做** (判据是 6041h bit3, 不是操作员点了哪个按钮);\n"
      "  一根轴都没报故障时**一个字节都不会写** —— 点下去也安全。\n"
      "· 复位的动作是**先写 6040h = 0x0000 (卸力) 再抬 bit7** —— bit7 是上升沿触发,\n"
      "  不先把 0 压下去就构不成沿。所以它**不能**对一根健康的保持轴做: 会松开保持力矩。\n"
      "· 复位成功后该轴停在**未使能**, 要接着走请重新点「使能」。\n"
      "· 扫描进行中不能复位 —— 复位后该轴失能, 而失能轴扫不动。先「中止」。\n"
      "· 每根轴最多 1 秒, 期间按钮不可用; 这段时间**不可中断**。\n"
      "· **不弹确认框**: 上面那几条就是「事前」能说的全部, 点下去直接做。\n"
      "  判据是 6041h bit3 而不是你点了哪个按钮, 所以「点错」这件事本身不存在 ——\n"
      "  一根没故障的轴不会被它碰到。"));
   connect(m_btnFaultRst, &QPushButton::clicked, this, &ScanWindow::onFaultResetClicked);
   g->addWidget(m_btnFaultRst, 3, 0, 1, AX_NCOL + 1);

   /* 多出来的宽度全给本组最后一列 (不是写死 3), 让灯和字靠左排成一条; 写死会多出一个空列 */
   g->setColumnStretch(AX_NCOL, 1);
   return box;
}

/* 「限位开关」: 每根三个信号 (原点 / 正限位 / 负限位), 第三行是可选的 60FDh 重映射。
 * 与「轴信号」分开两个框: 这一组问开关本身压着没有, 状态栏那盏问会不会中止扫描。
 * 三个灯纯显示, 不新增中止判据: 会中止的仍然只有 6041h bit11。 */
QWidget *ScanWindow::buildLimitPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("限位开关"), this);
   QGridLayout *g = new QGridLayout(box);
   g->setContentsMargins(6, 4, 6, 6);
   g->setHorizontalSpacing(12);
   g->setVerticalSpacing(5);

   m_limGrid.ncol = LIM_NCOL;

   static const char *kHead[LIM_NCOL] = { "原点", "正限位", "负限位" };
   static const char *kTip[LIM_NCOL] = {
      /* 原点灯是绿的, 所以要说出声来 */
      "60FDh bit2 —— 原点开关现在压着没有 (2310h X0 = 原点)。\n"
      "**绿亮 = 正压着, 这是位置信息不是故障** —— 回零时压到原点是正常动作,\n"
      "扫描经过原点开关**不会**中止。\n"
      "这一格说的是**开关本身**, 与「会不会中止扫描」是两个问题: 后者看 6041h bit11。",

      "60FDh bit1 —— 正限位开关现在压着没有 (2311h X1 = 正限位)。\n"
      "**红亮 = 正压着, 要立刻处理** —— 先手动把滑台走离限位。\n"
      "这一格说的是**开关本身**; 会不会中止扫描看状态栏那盏 (6041h bit11)。\n"
      "两者本该同源 (都经 2300h + 2310h 出来), 不一致时**默认以 bit11 为准**;\n"
      "但勾了「输入电平反转 (NPN)」之后改以反相后的开关为准 (见那个框的说明)。",

      "60FDh bit0 —— 负限位开关现在压着没有 (2312h X2 = 负限位)。\n"
      "**红亮 = 正压着, 要立刻处理** —— 先手动把滑台走离限位。\n"
      "这一格说的是**开关本身**; 会不会中止扫描看状态栏那盏 (6041h bit11)。\n"
      "两者本该同源 (都经 2300h + 2310h 出来), 不一致时**默认以 bit11 为准**;\n"
      "但勾了「输入电平反转 (NPN)」之后改以反相后的开关为准 (见那个框的说明)。"
   };

   for (int s = 0; s < LIM_NCOL; s++)
   {
      QLabel *h = new QLabel(QString::fromUtf8(kHead[s]), box);
      h->setStyleSheet(QStringLiteral("color:#6b7480;"));
      g->addWidget(h, 0, s + 1);
   }

   for (int i = 0; i < 2; i++)
   {
      QLabel *nm = new QLabel(QStringLiteral("轴%1").arg(i == 0 ? 'X' : 'Y'), box);
      nm->setStyleSheet(QStringLiteral("color:#9aa3ae;"));
      g->addWidget(nm, i + 1, 0);

      for (int s = 0; s < LIM_NCOL; s++)
      {
         m_limGrid.lamp[i][s] = makeLamp(box, QString::fromUtf8(kTip[s])
                                              + QString::fromUtf8(kLampRule));
         m_limGrid.text[i][s] = new QLabel(box);
         g->addWidget(lampUnit(box, m_limGrid.lamp[i][s], m_limGrid.text[i][s], 0),
                      i + 1, s + 1);
      }
   }

   /* 第三行: 可选的 60FDh 重映射, 默认关。本机实测生效的 1A00h 只有 6041h/6064h/606Ch,
    * 60FDh 不在里面 —— 不勾这个, 上面三个灯一直是"灰 + --"。优先用厂家上位机改一次
    * 驱动器的 1A00h; 勾这个框让主站连接时补一次 (仅写 RAM, 收尾还原)。改了不重连不生效。 */
   m_cbWantDigIn = new QCheckBox(QStringLiteral("让 60FDh 进 TxPDO"), box);
   m_cbWantDigIn->setToolTip(QStringLiteral(
      "连接时把 60FDh (数字输入) 追加进 TxPDO —— 不勾这个, 上面三个灯就一直是灰的。\n\n"
      "**改了不重连不生效**, 所以只能在未连接时改。\n\n"
      "本机实测生效的映射里没有 60FDh。推荐先用**厂家上位机**改一次驱动器的 1A00h\n"
      "(不用改代码, 而且断电也在), 那样这个框永远不必勾。\n\n"
      "勾上之后的代价: 过程数据从 10 字节变 14 字节; 只写 RAM, 收尾 (断开/关窗) 时还原;\n"
      "但**崩在收尾之前**那次改写会留在驱动器里, 直到断电为止。"));
   connect(m_cbWantDigIn, &QCheckBox::toggled, this, &ScanWindow::onWantDigInToggled);
   g->addWidget(m_cbWantDigIn, 3, 0, 1, LIM_NCOL + 1);

   /* 第四行: 「输入电平反转 (NPN)」, 默认关。治的是 NPN 传感器 (高电平 = 未触发) 配
    * 2300h 常开时 60FDh 那三位整排反相。立即生效 (不用重连), 不持久化, 且只治本程序
    * 这一侧 —— 驱动器自己的限位保护仍按原极性算。 */
   m_cbDiInvert = new QCheckBox(QStringLiteral("输入电平反转 (NPN)"), box);
   m_cbDiInvert->setToolTip(QStringLiteral(
      "X0~X3 接的是 NPN 传感器 (高电平 = **未**触发), 而驱动器的 2300h (输入有效电平\n"
      "逻辑) 按常开配着 —— 此时 60FDh 的原点/正限位/负限位三位**整排反相**, 两个限位\n"
      "常年都报「压着」。勾上这个, 把三个一起翻回来。\n\n"
      "**它只治本程序这一侧。** 60FDh 与 6041h bit11 都是驱动器给的, 勾这个框不会让\n"
      "驱动器改变主意 —— 它自己的限位保护仍然按那套错的极性算。**能改 2300h 就去改它**\n"
      "(那个才是修根, 改完这个框就不必勾了)。\n\n"
      "勾上之后限位判据**整个换掉**: 不再看 bit11 (那台机器上它恒为 1, 已经不携带\n"
      "信息了), 改看反相之后的正/负限位开关; 读不到 60FDh 时一律中止。\n\n"
      "它**立即生效**, 不用重新连接, 不写驱动器一个字节, 也**不会被记住** ——\n"
      "反转是「这台机器的线就是这么接的」的一条断言, 而断言错了的后果不是灯显示不对,\n"
      "是**保护反过来** (真压着限位时它说没压着)。所以每次都要人当面确认。"));
   connect(m_cbDiInvert, &QCheckBox::toggled, this, &ScanWindow::onDiInvertToggled);
   g->addWidget(m_cbDiInvert, 4, 0, 1, LIM_NCOL + 1);

   /* 最后一列 = 本组信号数。理由同上一块 */
   g->setColumnStretch(LIM_NCOL, 1);
   return box;
}

/* 「回零」—— 驱动器自带的正/反向找原点 (6060h = 6)。单开一个框: 它是动作不是参数,
 * 且是本程序里唯一"按下之后滑台会带电自己走"的按钮。回零会先失能再走 (6098h 只能在未
 * 使能时写), 竖直轴在这期间失去保持力矩, 所以"只回我这一根"必须点得出来。 */
QWidget *ScanWindow::buildHomePanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("回零 (驱动器自己找原点)"), this);
   QGridLayout *g = new QGridLayout(box);
   g->setContentsMargins(6, 4, 6, 6);
   g->setHorizontalSpacing(8);
   g->setVerticalSpacing(5);

   m_edHomeVel = new QSpinBox(box);
   m_edHomeVel->setRange(HMI_HOME_VEL_MIN, HMI_HOME_VEL_MAX);
   m_edHomeVel->setSingleStep(1000);   /* 与「扫描速度」「手动速度」同一个步长 */
   m_edHomeVel->setSuffix(QStringLiteral(" pul/s"));
   /* 缺省必须显式设: 新建的 QSpinBox 是 0, setRange 会把它夹到下限 HMI_HOME_VEL_MIN,
    * 显示的就成了 100 而不是 HMI_HOME_VEL_DEF。
    * 放这里而不是 applyDefaults(): 后者会被「恢复默认」再跑一遍, 那是给扫描几何用的 */
   m_edHomeVel->setValue(HMI_HOME_VEL_DEF);
   m_edHomeVel->setToolTip(QStringLiteral(
      "6099h:01 找原点速度 (返回速度 6099h:02 是它的 1/4, 加减速 609Ah 由它派生,\n"
      "都是自动算的 —— 见 hmi/ecatworker.h 里 home_vel_slow / home_accel_for)。\n"
      "上限与「扫描速度」一样 (100000 pul/s ≈ 2 圈/秒), 缺省是驱动器自己 6099h:01 的\n"
      "实测值 50000 (≈ 1 圈/秒)。\n\n"
      "**它同时是一条「能找多远」的上限**: 速度 × 30 秒 = 一次回零最多走过的距离。\n"
      "缺省 50000 时是 30 圈 (够走完缺省区域的一头到另一头); 调低会跟着缩小 ——\n"
      "2000 pul/s 只有 1.2 圈, 那时碰不到开关该做的是**先把滑台手动挪到开关附近**。\n"
      "第一次在陌生的机器上试方向, 把它压到下限 100。\n\n"
      "它不会被记住 —— 每改一次, 回零框里那行字就会把当次的数 (返回速度 / 加减速 /\n"
      "最多走多远 / 多久判超时) 跟着改一遍, 那是「这一趟」的事, 不该从 ini 里继承。"));

   g->addWidget(new QLabel(QStringLiteral("回零速度"), box), 0, 0);
   g->addWidget(m_edHomeVel, 0, 1, 1, 2);

   /* 按钮文字自带轴与方向 ("X 正向回零"), 不做"表头 + 四个短标签": 布局一挤短标签会归错列,
    * 而错点它的代价是滑台朝反方向去找开关。下面那段 tooltip 是唯一的"事前"防线 */
   for (int i = 0; i < 2; i++)
      for (int d = 0; d < 2; d++)
      {
         const bool neg  = (d == 1);
         const int  meth = ecatcmd::home_method_for(neg);

         m_btnHome[i][d] = new QPushButton(
            QStringLiteral("%1 %2回零")
               .arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y"),
                    QString::fromUtf8(ecatcmd::home_dir_text(neg))), box);
         m_btnHome[i][d]->setObjectName(QStringLiteral("danger"));
         m_btnHome[i][d]->setToolTip(
            QStringLiteral("6098h = %1 —— 原点开关 (X0) 为原点, **%2**高速先找。\n\n"
                           "驱动器会自己带电去找: 朝哪走、什么时候停、撞不撞开关, "
                           "全由它按这个方式决定。本程序只发一条「开始回零」。\n"
                           "**软件拦不住它撞开关** —— 能做的只有「停止」立即中止。\n\n"
                           "该轴会**先失能** (6098h 只能在未使能时写), 竖直轴会在这时\n"
                           "失去保持力矩。回零结束后自动切回 CSP 并保持使能。\n\n"
                           "「%2」说的是**电机轴**的正反向, 与画布上 +X/+Y 是不是同一个\n"
                           "方向, 只有现场试一次才知道 —— 所以第一次务必把速度设到最低、\n"
                           "人在物理急停旁。方向不对就换另一个按钮, 别硬顶。")
               .arg(meth).arg(QString::fromUtf8(ecatcmd::home_dir_text(neg))));

         connect(m_btnHome[i][d], &QPushButton::clicked, this,
                 [this, i, d] { onHomeClicked(i, d); });

         g->addWidget(m_btnHome[i][d], 1 + i, d);
      }

   /* 这一行随「回零速度」实时变 (见 refresh): "这一趟能找多远"唯一看得见的地方。
    * 碰不到开关时该先把滑台挪近, 不要为了够得着去调高速度 */
   m_lHomeNote = new QLabel(box);
   m_lHomeNote->setWordWrap(true);
   m_lHomeNote->setStyleSheet(QStringLiteral("color:#6b7480;"));
   g->addWidget(m_lHomeNote, 3, 0, 1, 2);

   /* 6061h 那一行 (见 pushHomeMode)。与上面那行更新时机不同: 它只在工作线程真读过之后
    * 才变 (连接 / 使能 / 回零收尾各读一次) —— 它是"驱动器当时认的模式", 不是现在的猜测 */
   m_lHomeMode = new QLabel(box);
   m_lHomeMode->setWordWrap(true);
   m_lHomeMode->setStyleSheet(QStringLiteral("color:#6b7480;"));
   m_lHomeMode->setToolTip(QStringLiteral(
      "6061h 是驱动器自报的**实际**运行模式 (6060h 是我们要求它切的模式)。\n"
      "手册 §3.7: 回零要求 6061h = 6 (HM) —— 它没读回 6 的话, 驱动器根本不认这次回零,\n"
      "抬 6040h bit4 不会有任何反应。\n\n"
      "这一行不是每周期刷新的: 6061h 不在过程数据里, 只能 SDO 读。工作线程只在\n"
      "连接、使能、回零收尾这三处各读一次, 所以它显示的是**上一次读到**的值 ——\n"
      "「— (还没读过)」和「0 (未定义)」是两件不同的事, 前者是不知道, 后者是驱动器说的。"));
   g->addWidget(m_lHomeMode, 4, 0, 1, 2);

   g->setColumnStretch(1, 1);
   return box;
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
   m_edSpeed->setToolTip(QStringLiteral("扫描时走多快。**一轮扫描从头到尾一个速度** ——\n"
                                        "它在「开始扫描」那一刻下发一次, 中途改不了。"));

   /* 手动速度: 点画布 /「全部回中」时用。扫描中不生效 (那时速度归 ScanController 管),
    * 扫描一结束/中止会自己把手动速度推回去 */
   m_edManSpeed = new QSpinBox(box);
   m_edManSpeed->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_edManSpeed->setSingleStep(1000);
   m_edManSpeed->setSuffix(QStringLiteral(" pul/s"));
   /* 提示里不提「重测选中点」—— 它走扫描状态机 (armRun), 用的是「扫描速度」 */
   m_edManSpeed->setToolTip(QStringLiteral(
      "手动点画布走点、以及「全部回中」的速度。\n"
      "**只在没扫描时生效**: 扫描 (含「重测选中点」) 用的是上面那个「扫描速度」。\n"
      "嫌对位时走得太快就调小它, 不必动扫描速度。"));

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
   f->addRow(QStringLiteral("手动速度"), m_edManSpeed);
   f->addRow(QStringLiteral("单点停留"), m_edDwell);
   f->addRow(QStringLiteral("稳定窗口"), m_edSettle);
   f->addRow(QStringLiteral("每点采样"), m_edSamples);
   f->addRow(QStringLiteral("起始方向"), m_cbDir);
   f->addRow(QStringLiteral("扫描方式"), m_cbMode);
   f->addRow(QStringLiteral("CSV"), csvRow);

   m_btnDef = new QPushButton(QStringLiteral("恢复默认"), box);
   {
      /* 提示里的数字从 Params 现算 —— 手抄一份的话, 缺省值一改这里就成了假话 */
      const Params d0;
      m_btnDef->setToolTip(QStringLiteral(
         "这一组里的每一项都回到程序里的缺省值:\n"
         "区域 %1 × %2 单位 / 分辨率 %3 / 1 单位 = %4 脉冲 / 速度 %5 pul/s / 停留 %6 ms …\n"
         "**CSV 输出路径不动** —— 那是这一趟数据写哪儿, 不是扫描的参数。")
         .arg(d0.area_x_unit, 0, 'f', 0).arg(d0.area_y_unit, 0, 'f', 0)
         .arg(d0.res_unit, 0, 'f', 3).arg(d0.pulses_per_unit, 0, 'f', 0)
         .arg(d0.speed_pul_s).arg(d0.dwell_ms));
   }
   connect(m_btnDef, &QPushButton::clicked, this, &ScanWindow::onRestoreDefaults);

   QHBoxLayout *defRow = new QHBoxLayout;
   defRow->setContentsMargins(0, 0, 0, 0);
   defRow->addStretch(1);
   defRow->addWidget(m_btnDef);
   f->addRow(defRow);

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

   /* 必须显式把缺省值填进控件: QDoubleSpinBox 初值是 0, 会被自己夹到最小值 0.1 —— 界面看着
    * 像那么回事, 而扫描区域其实只有 0.1 单位。必须在下面那些 connect 之前, 否则每 set 一个
    * 值都会触发 pushParams() → refresh(), 而 refresh() 要用的按钮和标签此刻还没建出来。 */
   applyDefaults();

   /* 再拿记忆覆盖一遍 (顺序不能反, 见 scanwindow.h)。也在 connect 之前 */
   loadSettings();

   /* 参数一改就重算, 让操作员在按开始之前就看见这一趟多长 */
   const QList<QDoubleSpinBox *> dspins{m_edAreaX, m_edAreaY, m_edRes, m_edPpu};
   for (QDoubleSpinBox *s : dspins)
      connect(s, &QDoubleSpinBox::valueChanged, this, &ScanWindow::pushParams);
   const QList<QSpinBox *> ispins{m_edSpeed, m_edDwell, m_edSettle, m_edSamples};
   for (QSpinBox *s : ispins)
      connect(s, &QSpinBox::valueChanged, this, &ScanWindow::pushParams);

   /* 手动速度不进 Params (与扫描几何无关, 也不写进 CSV 表头), 所以不走 pushParams */
   connect(m_edManSpeed, &QSpinBox::valueChanged, this, &ScanWindow::refresh);
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
   m_btnRetest->setToolTip(QStringLiteral("在画布上 **左键** 选中一格, 再点这个。\n"
                                          "(选中是只读的, 扫描中也能选)"));

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
   m_cbMeter->addItem(m_ophir->kind());
   m_cbMeter->setCurrentIndex(1);
   connect(m_cbMeter, &QComboBox::currentIndexChanged, this, &ScanWindow::onMeterChanged);

   /* 没装 StarLab 的那一项照样列出来, 但灰掉: 不列的话操作员会以为这程序没有真机这条路 */
   if (!OphirCom::isRegistered())
   {
      const QString why = QStringLiteral(
         "这台机器上没找到 OphirLMMeasurement 这个 COM 对象 —— "
         "要先装 Ophir 的 StarLab (PD300R + Juno+ 的驱动就在里面)");
      auto *m = qobject_cast<QStandardItemModel *>(m_cbMeter->model());
      if (m != nullptr && m->item(3) != nullptr)
      {
         m->item(3)->setEnabled(false);
         m->item(3)->setToolTip(why);
      }
   }

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

   /* 真机的三项。选项表由设备给, 这里一个都没写死 (探头不同, 能选的波长与量程就不同)。
    * 三行连着小标签一起收进容器, 切到模拟源时整块 setVisible(false) */
   auto makeDevRow = [&](const QString &name, QComboBox **cb, QLabel **lb) {
      QWidget *w = new QWidget(box);
      QHBoxLayout *h = new QHBoxLayout(w);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(6);
      *lb = new QLabel(name, w);
      (*lb)->setMinimumWidth(28);
      *cb = new QComboBox(w);
      (*cb)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      (*cb)->setEnabled(false);
      connect(*cb, &QComboBox::currentIndexChanged, this, &ScanWindow::onMeterCfgChanged);
      h->addWidget(*lb);
      h->addWidget(*cb, 1);
      return w;
   };
   m_devRowWl    = makeDevRow(QStringLiteral("波长"), &m_cbWl,    &m_lWl);
   m_devRowRange = makeDevRow(QStringLiteral("量程"), &m_cbRange, &m_lRange);
   m_devRowMode  = makeDevRow(QStringLiteral("模式"), &m_cbMeasMode, &m_lMeasMode);

   f->addRow(QStringLiteral("取样源"), m_cbMeter);
   f->addRow(QStringLiteral("手填值"), m_edManualV);
   f->addRow(QStringLiteral("噪声"), m_edRandomN);
   f->addRow(QStringLiteral("脚本"), scriptRow);
   f->addRow(m_devRowWl);
   f->addRow(m_devRowRange);
   f->addRow(m_devRowMode);

   /* 「读一次」: 当前取样源的一次普通请求, 走的正是扫描用的那条路 (requestReading →
    * readingReady/readingFailed), 所以这一下通了采集那条路也就通了。接口约定同一时刻
    * 只允许一个未决请求, 控制器不在 Idle 时这个按钮是禁用的。 */
   m_btnRead = new QPushButton(QStringLiteral("读一次"), box);
   m_btnRead->setToolTip(readOnceTip());
   connect(m_btnRead, &QPushButton::clicked, this, &ScanWindow::onReadOnceClicked);

   m_lReadout = new QLabel(box);
   m_lReadout->setWordWrap(true);
   m_lReadout->setStyleSheet(QStringLiteral("color:#7b8391;"));
   /* 这一格会显示设备来的字 (探头报的过量程原因之类)。明写 PlainText: QLabel 默认
    * AutoText, 字里有个 `<` 就会被当 HTML 解析 */
   m_lReadout->setTextFormat(Qt::PlainText);
   m_lReadout->setText(QStringLiteral("—"));

   m_readTimer = new QTimer(this);
   m_readTimer->setSingleShot(true);
   connect(m_readTimer, &QTimer::timeout, this, [this] {
      if (!m_readPending)
         return;
      m_readPending = false;
      const double took = (double)(m_clock.elapsed() - m_readSentMs);
      m_lReadout->setText(QStringLiteral("读一次: 没有回应 (等了 %1 秒) —— "
                                         "取样源答应了却一个数都没回, 那是源自己的 bug")
                             .arg(took / 1000.0, 0, 'f', 1));
      refresh();
   });

   f->addRow(m_btnRead);
   f->addRow(m_lReadout);

   m_lMeter = new QLabel(box);
   m_lMeter->setWordWrap(true);
   m_lMeter->setStyleSheet(QStringLiteral("color:#7b8391;"));
   /* 同样: 这句里全是设备给的字 (表头/探头型号、序列号), 明写 PlainText */
   m_lMeter->setTextFormat(Qt::PlainText);
   f->addRow(m_lMeter);

   onMeterInfoChanged();      /* 一开始选的是模拟源 -> 把真机那三行藏起来 */
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

void ScanWindow::applyDefaults()
{
   /* Params (scanplan.h) 是缺省值的唯一定义处, 这里不另抄一份数字 */
   const Params d;

   m_edAreaX->setValue(d.area_x_unit);
   m_edAreaY->setValue(d.area_y_unit);
   m_edRes  ->setValue(d.res_unit);
   m_edPpu  ->setValue(d.pulses_per_unit);
   m_edSpeed->setValue((int)d.speed_pul_s);
   m_edDwell->setValue(d.dwell_ms);
   m_edSettle->setValue(d.settle_ms);
   m_edSamples->setValue(d.samples_per_point);
   m_cbDir  ->setCurrentIndex(d.start_positive ? 0 : 1);
   m_cbMode ->setCurrentIndex(d.serpentine ? 0 : 1);

   /* 手动速度不在 Params 里 (与扫描几何无关), 缺省就是 HMI_VEL_DEF */
   m_edManSpeed->setValue(HMI_VEL_DEF);

   /* 回零速度刻意不在这里: 「恢复默认」会把 applyDefaults 再跑一遍, 会把为试回零特意
    * 压小的速度抬回去。它的缺省设在 buildHomePanel 里, 只走一次。 */
}

/* 记忆: 读回上次的参数 (见 scanprefs.h)。只覆盖 ini 里真有的项, 缺的留在 applyDefaults
 * 刚填的缺省上。setValue / setCurrentIndex 会自动夹进控件的量程, 一个被手改坏的 ini
 * (区域写成 1e9) 会变成 500 而不是让界面炸掉; 夹掉了不吭声。 */
void ScanWindow::loadSettings()
{
   const Prefs pf = prefsLoad(prefsPath());

   m_edAreaX ->setValue(pf.params.area_x_unit);
   m_edAreaY ->setValue(pf.params.area_y_unit);
   m_edRes   ->setValue(pf.params.res_unit);
   m_edPpu   ->setValue(pf.params.pulses_per_unit);
   m_edSpeed ->setValue((int)pf.params.speed_pul_s);
   m_edDwell ->setValue(pf.params.dwell_ms);
   m_edSettle->setValue(pf.params.settle_ms);
   m_edSamples->setValue(pf.params.samples_per_point);
   m_cbDir   ->setCurrentIndex(pf.params.start_positive ? 0 : 1);
   m_cbMode  ->setCurrentIndex(pf.params.serpentine ? 0 : 1);

   /* 手动速度: **-1 = ini 里没这一项** (或是个旧 ini), 那就留在 applyDefaults 填的缺省上 */
   if (pf.manual_speed > 0)
      m_edManSpeed->setValue(pf.manual_speed);

   /* 网卡此刻还选不了 (适配器清单是异步到的), 先存着, 到了再选 */
   m_savedNic = pf.nic;
}

/* 记忆: 把当前参数写回去。「连接」时与关窗时各一次 —— 连接那一次记的是真连过的那张卡 */
void ScanWindow::saveSettings()
{
   Prefs pf = prefsLoad(prefsPath());     /* 先读回上次那份: 下面两处都是"覆盖不了就留着" */

   /* 过不了体检的参数不覆盖旧的 (规则在 scanprefs 里, 那里能被自检钉住) */
   prefsMergeParams(&pf, currentParams());

   const QString nic = m_nic->currentData().toString();
   pf.nic = nic.isEmpty() ? m_savedNic : nic;   /* 清单还没到 / 卡被拔了: 别把记住的抹掉 */
   pf.manual_speed = m_edManSpeed->value();

   prefsSave(prefsPath(), pf);
   m_savedNic = pf.nic;
}

void ScanWindow::onRestoreDefaults()
{
   /* 扫描中锁着这些控件, 这里再判一次: 恢复默认会重建网格, 几何改到一半的扫描落进
    * CSV 的 (ix,iy) 与实际位置就对不上了 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能改参数"), true);
      return;
   }

   applyDefaults();     /* 每一 setValue 都会经 pushParams 重算一遍, 幂等 */

   const Params d;
   hint(QStringLiteral("扫描参数已恢复默认 (区域 %1 × %2 单位, 分辨率 %3, "
                       "1 单位 = %4 脉冲, 速度 %5 pul/s)。CSV 输出路径没动")
           .arg(d.area_x_unit, 0, 'f', 0).arg(d.area_y_unit, 0, 'f', 0)
           .arg(d.res_unit, 0, 'f', 3).arg(d.pulses_per_unit, 0, 'f', 0)
           .arg(d.speed_pul_s), false);
   refresh();
}

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

   /* 量程永远按区域自动算, 不暴露给操作员: 它是个会因为手滑而把区域边缘悄悄削掉的量 */
   p.range_pul = autoRangePul(p);
   return p;
}

void ScanWindow::pushParams()
{
   const Params p = currentParams();

   /* 量程跟着区域走。只在数值真变了才投命令, 否则每敲一个键都投一条, 状态栏会被回执刷屏 */
   if (p.range_pul != m_last_range)
   {
      m_last_range = p.range_pul;
      m_thr->postRange(m_last_range);
   }

   m_ctl->setParams(p);
   m_ctl->rebuildPlan();

   /* 网格那一行自己按参数算 nx/ny, 不读 m_ctl->gridNx(): 超上限时控制器那边是 0×0
    * (一个 Point 都没建), 而这一行要说的是"参数说的网格有多大" */
   const int nx = axisCount(p.area_x_unit, p.res_unit);
   const int ny = axisCount(p.area_y_unit, p.res_unit);
   const qint64 total = (qint64)nx * (qint64)ny;

   if (total > kMaxPlanPoints)
      m_lGrid->setText(QStringLiteral("网格 %1 × %2 = **%3 点**   ±%4 单位\n"
                                      "**超过上限 %5 —— 网格没有建** (画布此刻是空的, "
                                      "「开始扫描」按不动)")
                          .arg(nx).arg(ny).arg(total)
                          .arg(p.area_x_unit / 2.0, 0, 'f', 3)
                          .arg(kMaxPlanPoints));
   else
      m_lGrid->setText(QStringLiteral("网格 %1 × %2 = **%3 点**   ±%4 单位")
                          .arg(nx).arg(ny).arg(total)
                          .arg(p.area_x_unit / 2.0, 0, 'f', 3));

   /* 预估是线性的, 实际一定更长 (每次移动的进近段都要减速), 这一句必须写出来 */
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

   /* 不弹确认框, 进来就发。它写什么由按钮上的 tooltip 一直写着: 进 OP 开始发帧、
    * 覆盖生效映射里主站拥有的项, 但不发使能 (电机不带电) */
   hint(QStringLiteral("正在连接... (选轴 / 补映射 / 进 OP 都要做 SDO, 慢是正常的)"), false);

   /* 连上之前先记住这张卡: 连接失败也说明人选的就是它, 下次开机仍该默认它 */
   saveSettings();

   m_thr->postConnect(m_nic->currentData().toString());
}

void ScanWindow::onEnableClicked()
{
   /* 「使能」= CLI 的 --allow-motion。不弹确认框: 带不带电靠按钮自己的形态说 ——
    * 已使能时它是灰的、写着「已使能」; 点得动就说明还有轴没使能 */
   m_thr->postEnable();
}

/* 清驱动器的故障位。两处闸分工: 这里的闸管界面自己才知道的事 (扫描在不在跑 / 有没有连上);
 * 工作线程的闸 (ecatcmd::axis_needs_reset) 管该不该对这根轴写字节 —— 该复位哪根是驱动器的
 * 事实 (6041h bit3), 不是操作员的选择, 所以按钮不带轴参数。 */
void ScanWindow::onFaultResetClicked()
{
   /* 复位后该轴失能, 而失能轴扫不动 —— 所以是「先中止」, 不是「暂停再继续」 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能做故障复位 "
                          "(复位后该轴失能, 失能轴扫不动)"), true);
      return;
   }

   /* 不弹确认框, 点了就发。但"它以为哪根轴报了故障"必须用横幅说出来 —— 遥测最多滞后
    * ~33ms, 真正的判据在工作线程里 (刚读到的 6041h bit3), 所以措辞用"看起来"。
    * 复位是 6040h = 0x0000 (卸力) 再抬 bit7 (上升沿触发)。 */
   const BusTelem t = m_thr->telemetry();
   int todo[EM_MAX_AXES];
   const int ntodo = ecatcmd::pick_faulted_axes(t, todo, EM_MAX_AXES);

   QString who;
   if (ntodo == 0)
   {
      who = QStringLiteral("看起来没有轴报故障 (6041h bit3 都是 0) —— "
                           "这一下大概率**一个字节都不会写**。"
                           "(故障要真是刚起来的, 工作线程会看到它并照常复位。)");
   }
   else
   {
      QStringList names;
      for (int k = 0; k < ntodo && k < EM_MAX_AXES; k++)
         names << QStringLiteral("轴%1").arg(todo[k] == 0 ? 'X' : 'Y');
      who = QStringLiteral("复位看起来报故障的: %1。复位成功后该轴停在**未使能**, "
                           "要接着走请重新点「使能」。").arg(names.join(QStringLiteral("、")));
   }

   hint(who, false);

   m_thr->postFaultReset();
}

void ScanWindow::onWantDigInToggled(bool on)
{
   m_thr->setWantDigIn(on);

   /* 这个框永远可以勾 (人恰恰是在连上、看见三个灯全是"灰 + --"之后才知道要勾它)。
    * 但它是连接期参数: em_require_dig_in() 只在 em_setup 里被读一次, 勾了对当前这次连接
    * 没有影响 —— 已经连上了就说清"下次连接才生效"。 */
   if (m_connected)
   {
      hint(on ? QStringLiteral(
                       "已记下: 下次「连接」时把 60FDh 追加进 TxPDO。"
                       "**本次连接不受影响** —— 三个限位灯要等重新连接之后才会亮。")
              : QStringLiteral("已记下: 下次「连接」不再动 TxPDO 映射。**本次连接不受影响**。"),
           false);
   }
}

/* 「输入电平反转 (NPN)」。运行期参数: 不写驱动器、不进 ini、不用重连, 且只治上位机这一侧
 * —— 驱动器自己的 bit11 与限位保护不受影响。
 *
 * 不做持久化: 反转是「这台机器的 X0~X3 就是这么接的」一条断言, 断言错了的后果是保护反过来
 * (真压着限位时它说没压着), 必须每次由人当面确认。
 * 反转生效时判据是 LIMIT_RULE_INVERT (不看 bit11), 读不到 60FDh 就等于一条判据都没有
 * -> 限位一律按「有效」中止 -> 扫描永远开不了, 所以要在勾的一瞬间就说。 */
void ScanWindow::onDiInvertToggled(bool on)
{
   m_thr->setDiInvert(on);

   if (!on)
   {
      hint(QStringLiteral("已关掉输入反转: 限位判据退回 6041h bit11 单独判定。"), false);
      return;
   }

   const BusTelem t = m_thr->telemetry();

   if (!t.connected)
   {
      hint(QStringLiteral(
         "已打开输入反转 (连上之后生效)。**它只治本程序这一侧** —— "
         "驱动器自己的 6041h bit11 与限位保护不受影响。"), false);
      return;
   }

   if (!t.ax[0].dig_known)
   {
      hint(QStringLiteral(
         "已打开输入反转, 但**读不到 60FDh** —— 反转生效时 bit11 不参与判定, "
         "所以现在一条判据都没有: 扫描会因为「限位信号有效」永远开不了。\n"
         "两条出路: 先把 60FDh 弄进 TxPDO (勾上一行那个框, 再重新「连接」), "
         "或者把这个反转关掉、退回只看 bit11。"), true);
      return;
   }

   const AxisTelem &a = t.ax[0];
   hint(QStringLiteral(
      "已打开输入反转 (轴0 反相后: 正限位 %1 / 负限位 %2)。\n"
      "**它只治本程序这一侧** —— 驱动器自己的 bit11 与限位保护不受影响; "
      "能改 2300h 还是去改它, 那个修的才是根。")
         .arg(a.dig_pos ? QStringLiteral("压着") : QStringLiteral("松开"),
              a.dig_neg ? QStringLiteral("压着") : QStringLiteral("松开")),
      false);
}

void ScanWindow::onCenterAllClicked()
{
   m_thr->postCenterAll();
   hint(QStringLiteral("两根轴都去显示坐标 0 (= 区域中心)"), false);
}

/* 「停止」。回零是阻塞在工作线程里的 (em_home 自己泵帧、自己轮询), 而命令队列是那个线程
 * 在 run() 顶部排空的 —— 一条 CMD_STOP 要等回零自己退出来才轮到, 那时 30 秒超时早过了。
 * 所以回零期间走 requestMotionStop(): GUI 直呼 em_request_stop() 只往一个 volatile 标志
 * 里存 1, em_home 的 2ms 轮询下个周期就看见。这是全程序唯一一处 GUI 直呼 motor_api。 */
void ScanWindow::onStopClicked()
{
   if (m_thr->telemetry().homing)
   {
      m_thr->requestMotionStop();

      /* 不追加 postStop(): 回零的收尾自己会把目标冻在落点, 再投一条只会让它的 note
       * 把"回零是被中止的还是到位了"那句盖掉 (note 是覆盖写) */
      hint(QStringLiteral("正在中止回零… 「停止」已发出 (收尾要 失能 → 切回 CSP → 重新使能, "
                          "最多几秒)"), false);
      return;
   }

   /* 非回零时与从前一字不差 (hmi 那边这个按钮仍然直连 postStop, 共用同一份 ecatworker.cpp) */
   m_thr->postStop();
}

/* 「X/Y 正/反向回零」—— 全程序最危险的一个按钮: 按下之后滑台自己带电朝开关走, 朝哪走、
 * 什么时候停、撞不撞开关全由驱动器按 6098h 决定, 软件拦不住它撞开关。
 * 不弹确认框, 事前的话分两处常驻: 按钮 tooltip (轴 / 方向 / 方式号 / 该轴会先失能) 与
 * 回零框里随速度实时变的那行 (见 pushHomeNote)。限位判据已成立由 refresh 挂红横幅说。 */
void ScanWindow::onHomeClicked(int axis, int dir)
{
   if (axis < 0 || axis > 1 || dir < 0 || dir > 1)
      return;

   /* 扫描中一律拦住。这是第二道 —— 按钮在扫描期间本来就是灰的 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能回零"), true);
      return;
   }

   const bool     neg  = (dir == 1);
   const int      meth = ecatcmd::home_method_for(neg);
   const uint32_t vel  = ecatcmd::home_vel_clamp(m_edHomeVel->value());
   const QString  ax   = (axis == 0) ? QStringLiteral("X") : QStringLiteral("Y");
   const QString  dtxt = QString::fromUtf8(ecatcmd::home_dir_text(neg));

   /* 零点世代 +1, 在 postHome 之前且无条件。放 GUI 是因为它无法知道工作线程那道闸是拦还是
    * 放, 而两个方向的代价不对称: 多发一代最多让续扫多问一次 (无害); 漏发一代则续扫把回零
    * 前后的两半坐标静默拼在一起 —— 那正是这套机制存在的全部理由。
    * 所以往保守那边偏: GUI 每次派发都 +1。 */
   m_epoch++;
   m_ctl->setZeroEpoch(m_epoch);

   m_thr->postHome(axis, meth, vel);

   /* 这一句只在**命令没被那道闸接住**时才留得住 (真开始回零的话, 最多 33ms 之后
    * refresh 就会用"轴X 正在回零…"那条**状态**横幅把它盖掉 —— 那是设计如此)。 */
   hint(QStringLiteral("轴 %1 的 %2回零已发出 (方式 %3, 速度 %4 pul/s)。"
                       "**按「停止」可立即中止** —— 收尾要几秒, 请等状态栏里那句结果")
           .arg(ax, dtxt).arg(meth).arg(vel), false);
}

void ScanWindow::onZeroHereClicked()
{
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能重设零点"), true);
      return;
   }

   /* 零点一动, 同一个显示坐标指的就不是同一个物理位置了, 所以世代 +1 (续扫时对不上
    * 会要求操作员确认, 这一句就是那个确认的依据) */
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
      /* 扫描中途换源 = 同一张图上的数据来自两个不同的东西, 禁掉。
       * 回退时必须挡掉信号, 否则 setCurrentIndex 会再进来一次, 两个下标之间来回弹 */
      hint(QStringLiteral("扫描进行中, 不能换取样源"), false);
      QSignalBlocker b(m_cbMeter);
      m_cbMeter->setCurrentIndex(m_cbMeter->findText(m_meter->kind()));
      return;
   }

   m_meter->close();

   PowerMeter *pick = nullptr;
   switch (idx)
   {
   case 0:  pick = m_manual; break;
   case 2:  pick = m_script; break;
   case 3:  pick = m_ophir;  break;
   default: pick = m_random; break;
   }
   m_meter = pick;

   /* 换了源, 上一格那个数就不再属于任何东西了 (留着会被读成新源的读数)。未决的那个
    * 请求同理要作废: 回话即使来了也不该再往这一格写 */
   if (m_readTimer != nullptr)
      m_readTimer->stop();
   m_readPending = false;
   if (m_lReadout != nullptr)
      m_lReadout->setText(QStringLiteral("—"));

   /* open() 对真机是阻塞的 (枚举 USB → 开设备 → 读探头 → 开流), 上限 12s */
   QString err;
   const bool ok = m_meter->open(&err);

   /* 必须告诉控制器, 否则它还在听旧的源 (旧源还活着, 会照常出数) */
   m_ctl->setMeter(m_meter);

   if (!err.isEmpty())
      hint(QStringLiteral("功率计打不开: ") + err, true);
   else if (ok && m_meter == m_ophir)
   {
      const OphirInfo i = m_ophir->info();
      hint(i.summary.isEmpty()
              ? QStringLiteral("取样源已切到「%1」").arg(m_meter->kind())
              : QStringLiteral("功率计已接上: ") + i.summary,
           false);
   }
   else
      hint(QStringLiteral("取样源已切到「%1」").arg(m_meter->kind()), false);

   onMeterInfoChanged();
   refresh();
}

/* 设备信息回来了: 真机那三行露不露出来 / 三个下拉框装设备的选项表 / 当前选中项 / 没选上或
 * 没这一项的框灰掉。只在 infoChanged 时跑, 不放进 30Hz 的 refresh(): 那个频率下重填下拉框
 * 会跟操作员正在点的那一下抢, 而且每帧重建选项是白烧 CPU。 */
void ScanWindow::onMeterInfoChanged()
{
   const bool is_ophir = (m_meter == m_ophir);

   m_devRowWl->setVisible(is_ophir);
   m_devRowRange->setVisible(is_ophir);
   m_devRowMode->setVisible(is_ophir);
   if (!is_ophir)
      return;

   const OphirInfo i = m_ophir->info();
   const bool open = m_ophir->isOpen();

   /* 填的时候挡掉信号, 否则每 addItem 一次都会被当成操作员改配置 (一连串 stop/set/start) */
   m_meterCfgQuiet = true;

   auto fill = [&](QComboBox *cb, QLabel *lb, const QStringList &opts, int cur) {
      cb->clear();
      cb->addItems(opts);
      if (cur >= 0 && cur < cb->count())
         cb->setCurrentIndex(cur);
      /* 探头没有这一项 (手册: options 为空 / index 为 -1) 是正常的, 灰掉就是 */
      const bool usable = open && !opts.isEmpty();
      cb->setEnabled(usable);
      lb->setEnabled(usable);
   };
   fill(m_cbWl,    m_lWl,    i.wavelengths, i.wl_index);
   fill(m_cbRange, m_lRange, i.ranges,      i.range_index);
   fill(m_cbMeasMode, m_lMeasMode, i.modes, i.mode_index);

   m_meterCfgQuiet = false;
}

/* 操作员改了波长/量程/模式。异步 —— 工作线程收到后是 停流 → 改 → 重新开流, 改完再发一次
 * infoChanged 回来 (见 ophirmeter.h), 这里不阻塞等结果 */
void ScanWindow::onMeterCfgChanged()
{
   if (m_meterCfgQuiet || m_meter != m_ophir || !m_ophir->isOpen())
      return;
   if (m_ctl->running())
      return;                 /* 扫描中那三个框本来就是灰的, 这是兜底 */

   m_ophir->setWavelengthIndex(m_cbWl->currentIndex());
   m_ophir->setRangeIndex(m_cbRange->currentIndex());
   m_ophir->setModeIndex(m_cbMeasMode->currentIndex());
}

void ScanWindow::onManualValueChanged(double v)
{
   m_manual->setValue(v);
}

/* ---------------------------------------------------------------- 读一次 */

/* 发一次普通请求, 走的是扫描用的同一条路, 所以它通了 = 采集那条路也通了。
 *
 * 硬闸: PowerMeter 约定 (powermeter.h) 调用方保证同一时刻只有一个未决请求。扫描跑着时
 * "调用方"是 ScanController, 这时按下去就是两个请求撞在同一个源上, 于是 CSV 里会悄悄少一个
 * 点或者错一个点, 且不报错。refresh() 里那条 setEnabled 是闸门, 这里再兜一次底。
 *
 * 两个槽里要比一次 sender(): 换取样源时未决请求作废, 但旧源的回话可能已排在事件队列里,
 * 只有当前源的回话算数。四个源都是本窗口的子对象、一样长寿, 所以 sender() 是安全的。 */
void ScanWindow::onReadOnceClicked()
{
   if (m_meter == nullptr || !m_meter->isOpen() || m_ctl->running() || m_readPending)
      return;

   /* 顺序有讲究: 先把 m_readPending / 起始时刻 / 兜底定时器都摆好, 最后才发请求 ——
    * 没打开时三个模拟实现是直接 emit readingFailed (在 requestReading() 的调用栈里就回来
    * 了, 见 powermeter.cpp), 倒过来就是在别人的调用栈里改自己的状态。 */
   m_readPending = true;
   m_readSentMs  = m_clock.elapsed();
   m_lReadout->setText(QStringLiteral("读一次: 读取中…"));
   m_readTimer->start(kReadOnceTimeoutMs);
   refresh();                       /* 立刻把按钮灰掉, 免得连点出两个请求 */

   m_meter->requestReading();
}

void ScanWindow::onReadOnceReady(double watts)
{
   /* 不是我们的那一份就是扫描的读数, 一个字都别动 */
   if (!m_readPending || sender() != m_meter)
      return;

   m_readPending = false;
   m_readTimer->stop();

   const double took = (double)(m_clock.elapsed() - m_readSentMs);
   m_lReadout->setText(QStringLiteral("读一次 [%1]: %2 W   (往返 %3 ms)")
                          .arg(m_meter->kind(), fmtWatts(watts))
                          .arg(took, 0, 'f', 0));
   refresh();
}

void ScanWindow::onReadOnceFailed(const QString &err)
{
   if (!m_readPending || sender() != m_meter)
      return;

   m_readPending = false;
   m_readTimer->stop();

   const double took = (double)(m_clock.elapsed() - m_readSentMs);
   /* 失败原文照贴 —— 没插表头 / 过量程 / 流没起来 是三种完全不同的错, 原话只有源知道 */
   m_lReadout->setText(QStringLiteral("读一次 [%1]: 失败 —— %2   (往返 %3 ms)")
                          .arg(m_meter->kind(), err)
                          .arg(took, 0, 'f', 0));
   refresh();
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

   /* 不弹确认框 —— 这一趟的范围在按之前就全在眼前了 (参数栏填的数、扫描栏那两行、
    * 旁边的「开不了」红字)。这里只留一句"出发了": 区域、点数、时长、数据写到哪 */
   const Params p = currentParams();

   QString err;
   if (!m_ctl->start(path, &err))
   {
      hint(QStringLiteral("开不了: ") + err, true);
      return;
   }

   /* 开跑之前把源重开一遍 —— 脚本源要靠这个把游标拨回第一个数。
    * 真机不跟着做: 它那次 close/open 要收线程 → 枚举 USB → 开设备 → 读探头 → 重新开流,
    * 几百毫秒起步, 而它本来就没有"游标"要复位 (取数按设备时间戳走水位线)。 */
   if (m_meter != m_ophir)
   {
      m_meter->close();
      m_meter->open(nullptr);
   }

   /* 出发那一句必须在 m_ctl->start 之后: 它念的是控制器真建出来的网格 (gridNx/totalPoints) */
   hint(QStringLiteral("扫描开始: 区域 %1 × %2 单位 (±%3) 共 %4 × %5 = **%6 点**, "
                       "预计全程 %7; 取样源 %8, 数据写入 %9")
           .arg(p.area_x_unit, 0, 'f', 3).arg(p.area_y_unit, 0, 'f', 3)
           .arg(p.area_x_unit / 2.0, 0, 'f', 3)
           .arg(m_ctl->gridNx()).arg(m_ctl->gridNy())
           .arg(m_ctl->totalPoints())
           .arg(fmtDur(m_ctl->estimateTotalMs()))
           .arg(m_meter->kind())
           .arg(QDir::toNativeSeparators(path)), false);

   m_bannerTimer->stop();    /* 这一句别自己消失: 它说清的是"现在在跑什么" */
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

   /* why 非空 = 只是"有个地方对不上, 要你知道", 不是错误: 零点世代不一致意味着中途重连过,
    * 下半场和上半场可能不在同一个物理位置上。
    * 不弹确认框: 直接按"允许世代差异"续下去, 把那句 why 留在红横幅上 (不自动消失的横幅
    * 比会被条件反射按掉的模态可靠), 觉得接得不对可随时「中止」, 已采的点都在 CSV 里。 */
   const QString mismatch = why;   /* resume 会把 why 重新写一遍, 先留住这一句 */

   err.clear();
   why.clear();
   if (!m_ctl->resume(f, true, &err, &why))
   {
      hint(QStringLiteral("还是接不上: ") + (err.isEmpty() ? why : err), true);
      return;
   }

   hint(QStringLiteral("续扫 (已按「允许零点世代不同」继续): %1\n%2")
           .arg(QDir::toNativeSeparators(f), mismatch), true);
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
      hint(QStringLiteral("先在画布上用 **左键** 选中一格"), false);
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

/* 手动速度只在没在扫描的时候推: 扫描中的速度是 ScanController::start() 设的那一个, 这里
 * 再推就是中途改速度, 而状态机的「每点耗时」与「走到位超时」判据都建立在那个速度上。
 * 扫描结束/中止后 running 变假就自己推回去, 不需要谁记"该恢复了"。只在数值真不一样时才推。 */
void ScanWindow::pushManualSpeed(const BusTelem &t, bool running)
{
   if (!m_connected || running)
      return;

   const uint32_t v = (uint32_t)m_edManSpeed->value();
   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
      if (t.ax[i].vel != v)
         m_thr->setSpeed(i, v);
}

/* 回零框里那行说明: 照现在这个速度, 一次回零最多走多远、多久判超时。随速度实时变。
 * 三个数必须问工作线程那几个函数 (home_vel_slow / home_accel_for / 那个超时宏), 不能在这里
 * 另写一遍斜坡与超时的算法。一圈多少脉冲取「分辨率」框 (默认 50000, 为 0 时退回它以免出 inf)。
 * 只在文字真变了才 setText (30Hz 调的)。 */
void ScanWindow::pushHomeNote()
{
   /* refresh() 可能在 buildUi 还没走完时就被叫到 (构造里那几个 connect 里就有会转调
    * refresh 的)。回零框和「分辨率」框不是同一块建的, 所以这里认一遍指针 */
   if (m_lHomeNote == nullptr || m_edHomeVel == nullptr || m_edPpu == nullptr)
      return;

   const uint32_t vel   = ecatcmd::home_vel_clamp(m_edHomeVel->value());
   const uint32_t slow  = ecatcmd::home_vel_slow(vel);
   const uint32_t acc   = ecatcmd::home_accel_for(vel);
   const double   ppu   = (m_edPpu->value() > 0.0) ? m_edPpu->value() : 50000.0;
   const double   reach = (double)vel * (HMI_HOME_TMO_MS / 1000.0) / ppu;

   const QString s = QStringLiteral(
      "回零 = 让驱动器自己带电朝开关走, **软件拦不住它撞开关**; 该轴会先失能 "
      "(竖直轴此时失去保持力矩, 可能下滑)。\n"
      "本速度: 找原点 %1 / 返回 %2 pul/s (6099h), 加减速 %3 (609Ah), "
      "一次最多走 **%4 圈**, %5 秒后判超时 —— 够不着开关请先把滑台挪近, "
      "**不要**为了够得着去调高速度。")
      .arg(vel).arg(slow).arg(acc)
      .arg(QString::number(reach, 'f', 1))
      .arg(HMI_HOME_TMO_MS / 1000);

   if (m_homeNoteLast == s)
      return;

   m_homeNoteLast = s;
   m_lHomeNote->setText(s);
}

/* 6061h 那一行, 两根轴各一格 (面板是整机的, "哪根轴认的模式不对"必须看得出来)。
 * 三种"没有值"要分开说: 还没连接 / 连上了但工作线程还没读过 (HMI_MODE_DISP_UNREAD) /
 * 读过但读失败 (-1) —— 措辞取自 mode_text, 不写 "?" 也不写 "0"。只在文字真变了才 setText。 */
void ScanWindow::pushHomeMode(const BusTelem &t)
{
   if (m_lHomeMode == nullptr)
      return;

   QString cells[2];
   for (int i = 0; i < 2; i++)
   {
      const char *nm = (i == 0) ? "X" : "Y";

      if (!t.connected || i >= t.naxis)
      {
         cells[i] = QStringLiteral("%1 = — (未连接)").arg(QString::fromUtf8(nm));
         continue;
      }

      const int m = t.ax[i].mode_disp;

      if (m == HMI_MODE_DISP_UNREAD)
         cells[i] = QStringLiteral("%1 = — (还没读过)").arg(QString::fromUtf8(nm));
      else
         cells[i] = QStringLiteral("%1 = %2 (%3)")
                       .arg(QString::fromUtf8(nm)).arg(m)
                       .arg(QString::fromUtf8(ecatcmd::mode_text(m)));
   }

   const QString s = QStringLiteral("驱动器实际模式 (6061h): %1 / %2 —— 回零要求它 = 6 (HM), "
                                    "回零收尾后应当是 8 (CSP)。")
                        .arg(cells[0], cells[1]);

   if (m_homeModeLast == s)
      return;

   m_homeModeLast = s;
   m_lHomeMode->setText(s);
}

/* 一根轴的五盏灯 + 状态栏那对。参数栏与状态栏都从这里出 (分开算就会两边说的不一样)。
 * 撞限位的判定只有一处定义: ecatcmd::limit_hit, 在 EcatThread::publish() 里算好,
 * 这里只读 a.limit_active。 */
void ScanWindow::refreshAxisSignals(const BusTelem &t)
{
   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      /* "不知道"的判据跟状态机完全一致 (ScanController 自动中止那一段用的就是
       * !valid || !mirror_ok): 判得比状态机宽的话, 会出现灯说"正常"而程序已经因为
       * 丢了过程数据帧中止了的不一致 */
      const bool known = m_connected && a.valid && a.mirror_ok;
      const bool lim   = known && a.limit_active;

      /* ---- 「轴信号」那四个 ---- */
      setSignalCell(m_axGrid, i, AX_ENABLED, known, a.enabled, Lamp::Ok,
                    QStringLiteral("已使能"), QStringLiteral("未使能"));
      setSignalCell(m_axGrid, i, AX_FAULT, known, a.fault, Lamp::Bad,
                    QStringLiteral("有故障"), QStringLiteral("无故障"));

      /* ---- 「限位开关」那六个: 三个开关本身压着没有 (60FDh) ----
       * known 要再与 a.dig_known: 少了它, 读不到 60FDh 时那三位是 0, 界面会显示"三个都没
       * 压住" —— 一个看起来完全正常的结论。原点用 Ok (绿亮 = 正在压着, 位置信息), 正负限位
       * 用 Bad; 绿色在这里不是"没事", 灯亮一律表示这件事正在发生。 */
      const bool dk = known && a.dig_known;
      setSignalCell(m_limGrid, i, LIM_HOME, dk, a.dig_home, Lamp::Ok,
                    QStringLiteral("压住"), QStringLiteral("松开"));
      setSignalCell(m_limGrid, i, LIM_POS, dk, a.dig_pos, Lamp::Bad,
                    QStringLiteral("压住"), QStringLiteral("松开"));
      setSignalCell(m_limGrid, i, LIM_NEG, dk, a.dig_neg, Lamp::Bad,
                    QStringLiteral("压住"), QStringLiteral("松开"));

      /* ---- 状态栏那一对: 用的是上面同一个 lim ---- */
      QLabel *lb   = (i == 0) ? m_lLimX : m_lLimY;
      QLabel *lamp = (i == 0) ? m_lampX : m_lampY;
      const QString ax = QStringLiteral("限位%1").arg(i == 0 ? 'X' : 'Y');

      if (!known)
      {
         lb->setText(ax + QStringLiteral(" --"));
         lb->setStyleSheet(QStringLiteral("color:#5a6270; padding:2px 6px;"));
         paintLamp(lamp, Lamp::Unknown);
         m_limShown[i] = false;      /* 不知道了就重新上膛: 再知道时该说的话还得说一遍 */

         /* 断线/丢帧也是"这一位现在怎么样我们不知道了": 挂着的横幅同理该走,
          * 否则界面会在没有任何数据时继续断言"正压着" */
         if (!m_limBanner[i].isEmpty() && m_banner->isVisible()
             && m_banner->text() == m_limBanner[i])
         {
            m_banner->setVisible(false);
            m_bannerTimer->stop();
         }
         m_limBanner[i] = QString();
         continue;
      }

      if (lim)
      {
         /* 「有效」而不是「撞上」—— 这一位是 CiA402 的 internal limit active,
          * 不等于"物理上已经撞到限位开关"。这一格短, 只能放驱动器自己那句话 */
         lb->setText(ax + QStringLiteral(" 有效!"));
         lb->setStyleSheet(QStringLiteral(
            "background:#5a1f1f; color:#ffb3b3; padding:2px 6px;"
            "border-radius:3px; font-weight:bold;"));
         paintLamp(lamp, Lamp::Bad);
      }
      else
      {
         lb->setText(ax + QStringLiteral(" 正常"));
         lb->setStyleSheet(QStringLiteral("color:#7b8391; padding:2px 6px;"));
         paintLamp(lamp, Lamp::Off);   /* 灭, 不是绿 —— 见 paintLamp 上面那段 */
      }

      /* 上升沿弹一次横幅, 与上面故障那条同一个理由: 30Hz 每帧都设一遍会把重绘刷爆。
       * 扫描中 bit11 置起会走自动中止 (那条有模态框), 这一条管的是**平时手动走的时候** */
      if (lim && !m_limShown[i])
      {
         m_limShown[i] = true;

         /* 顺带把当时的 60FDh 打到 stdout: 30Hz 的面板灯会一闪而过, 而 stdout 留证据。
          *   bit11 置起而三位全 0   -> 两个视图不一致 (多半 2310h~2312h 功能码没配对);
          *   bit11 置起而只有原点位 -> 若原点也让 bit11 置起, 扫描经过原点就会误中止;
          *   bit11 置起而正/负限位位 -> 自洽: 那一路限位信号确实有效。
          * 说的都是信号不是"撞上了" (手册: 该位是电平不是闩锁)。这里不改判定。 */
         if (a.dig_known)
            std::printf("[scan] 轴%d 限位判据置起: 反转=%d  sw=0x%04X  60FDh 位: "
                        "原点(bit2)=%d 正限位(bit1)=%d 负限位(bit0)=%d\n",
                        i, t.di_invert ? 1 : 0, (unsigned)a.sw, a.dig_home ? 1 : 0,
                        a.dig_pos ? 1 : 0, a.dig_neg ? 1 : 0);
         else
            std::printf("[scan] 轴%d 限位判据置起: 反转=%d  sw=0x%04X  "
                        "60FDh 不在生效映射里 (三个开关的状态无从得知)\n",
                        i, t.di_invert ? 1 : 0, (unsigned)a.sw);
         std::fflush(stdout);

         /* 「是什么状态」与「接下来查哪儿」两句都从 ecatcmd 里取 —— 那里是唯一一处定义,
          * 拒绝启扫与自动中止用的也是同一对函数。
          * 开场白: 反转开着时判据不看 bit11 (见 limit_hit_headline), 所以它不能再写
          * "bit11 置起" —— 那会让人去查一个决定不了任何事的位。上面那行 printf 同理,
          * 记下反转状态, 否则同一行日志在两种配置下看着一模一样。 */
         const QString ev = QString::fromUtf8(
            ecatcmd::limit_switch_text(a.dig_known, a.dig_pos, a.dig_neg, t.di_invert));
         const QString act = QString::fromUtf8(
            ecatcmd::limit_hit_advice(a.dig_known, a.dig_pos, a.dig_neg, a.dig_home,
                                      t.di_invert));

         const QString banner =
            QStringLiteral("%1。\n%2\n%3")
               .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)).arg(i),
                    ev, act);

         hint(banner, true);
         m_limBanner[i] = banner;
      }
      else if (!lim)
      {
         m_limShown[i] = false;

         /* 这一位掉了, 横幅就该跟着走: hint(s,true) 不自动消失, 而"压着"是个状态,
          * 早松开了还留着这句, 操作员会去处理一个不存在的问题。
          * 只清还是我们自己那条 (比对原文), 中止之类的消息换掉的那些不能动。 */
         if (!m_limBanner[i].isEmpty() && m_banner->isVisible()
             && m_banner->text() == m_limBanner[i])
         {
            m_banner->setVisible(false);
            m_bannerTimer->stop();
         }
         m_limBanner[i] = QString();
      }
   }
}

void ScanWindow::setSignalCell(LampGrid &g, int i, int s, bool known, bool on, Lamp lit,
                               const QString &litTxt, const QString &offTxt)
{
   QString txt;
   QString col;
   Lamp    l = Lamp::Unknown;

   if (!known)
   {
      txt = QStringLiteral("--");
      col = QStringLiteral("#5a6270");            /* 灰字: 连灯一起暗下去 */
   }
   else if (on)
   {
      txt = litTxt;
      col = (lit == Lamp::Ok) ? QStringLiteral("#5fd693") : QStringLiteral("#ff8f8f");
      l   = lit;
   }
   else
   {
      txt = offTxt;
      col = QStringLiteral("#7b8391");
      l   = Lamp::Off;
   }

   /* 只在真变了才动控件。灯和字一起变: 文字决定颜色, 字没变颜色就没变 */
   if (g.lampLast[i][s] != l)
   {
      g.lampLast[i][s] = l;
      paintLamp(g.lamp[i][s], l);
   }
   if (g.textLast[i][s] != txt)
   {
      g.textLast[i][s] = txt;
      g.text[i][s]->setText(txt);
      g.text[i][s]->setStyleSheet(QStringLiteral("color:") + col);
   }
}

void ScanWindow::refresh()
{
   /* 状态机先走一格, 再读遥测 —— 顺序反过来的话, 界面上显示的永远是上一拍的状态 */
   m_ctl->tick(m_clock.elapsed());

   const BusTelem t = m_thr->telemetry();

   /* 按钮形态只看遥测, 不看"点过哪个按钮" —— 后者会与线程的真实状态错开。
    * in_op = 正在发帧 (真连上了), busy = 正在连接或正在收尾; 两者任一为真就是占着总线 */
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

   /* 画布: 扫描中吞掉手动定位 (查看只读, 不归这条管)。只有画布一处决定吞不吞 */
   const bool running = m_ctl->running();
   m_canvas->setManualAllowed(!running);

   pushManualSpeed(t, running);

   /* 扫描中锁住参数与取样源: 几何改到一半, 落进 CSV 的 (ix,iy) 就和实际位置对不上了 */
   const QList<QWidget *> locked{
      m_edAreaX, m_edAreaY, m_edRes, m_edPpu, m_edSpeed, m_edManSpeed, m_edDwell,
      m_edSettle, m_edSamples, m_cbDir, m_cbMode, m_edCsv, m_cbMeter,
      m_edManualV, m_edRandomN, m_edScript, m_btnScript, m_btnOpen, m_btnDef
   };
   for (QWidget *w : locked)
      w->setEnabled(!running);

   /* 真机那三项不进上面那张表: 可用性还取决于设备有没有这一项 (选项表可能是空的) */
   const bool dev_ok = !running && (m_meter == m_ophir) && m_ophir->isOpen();
   m_cbWl->setEnabled(dev_ok && m_cbWl->count() > 0);
   m_cbRange->setEnabled(dev_ok && m_cbRange->count() > 0);
   m_cbMeasMode->setEnabled(dev_ok && m_cbMeasMode->count() > 0);

   refreshAxisSignals(t);

   const bool can_move = m_connected && !running;
   /* 回零期间不给目标 / 不改坐标 / 不改状态: 总线线程正阻塞在 doHome 里, interpolate()
    * 一帧不跑, 且轴此刻按 HM 解释, 607Ah 不是目标位置 (命令只会排队, 点了没反应) */
   const bool can_home = can_move && t.in_op && !t.homing && !t.resetting;

   /* 「使能」: 已使能就灰掉并把字改成「已使能」—— 没有确认框了, 带不带电只能由按钮自己说。
    * 判据看每一根而非只看轴0: 还有轴没使能就仍可按 (故障复位会把某一根单独打回未使能)。 */
   bool any_enabled = false, any_off = false;
   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
   {
      if (t.ax[i].enabled)
         any_enabled = true;
      else
         any_off = true;
   }

   m_btnEnable->setEnabled(can_move && !t.homing && any_off);
   m_btnEnable->setText(any_enabled && !any_off ? QStringLiteral("已使能")
                                                : QStringLiteral("使能"));
   /* 「停止」在回零中也必须可按: 它兼任"立即中止回零", 刻意不加 !t.homing */
   m_btnStop->setEnabled(m_connected);
   /* 失能排在回零后面执行的话, 收尾会再使能一次 —— "失能"以带电告终 */
   m_btnDis->setEnabled(m_connected && !t.homing);
   m_btnCenter->setEnabled(can_move);
   m_btnZero->setEnabled(can_move && !t.homing);

   /* 四个回零按钮逐轴判, 只看这一根; 另一根带不带电、有没有故障都与它无关。
    * 刻意不看 enabled: 未使能也能回零 (em_home 要求未使能才能写 6098h)。
    * fault / mirror_ok 这里再判一次: 工作线程那道闸才是权威, 但让按钮先按不动更好。 */
   for (int i = 0; i < 2; i++)
   {
      const bool ok = can_home && t.ax[i].valid && t.ax[i].mirror_ok && !t.ax[i].fault;

      for (int d = 0; d < 2; d++)
      {
         m_btnHome[i][d]->setEnabled(ok);
         /* 回零中只改正在动的那一根的名字, 否则回 X 时 Y 的按钮也写着"回零中…" */
         const bool mine = t.homing && (t.homing_axis == i);
         m_btnHome[i][d]->setText(
            mine ? QStringLiteral("%1 回零中…")
                      .arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y"))
                 : QStringLiteral("%1 %2回零")
                      .arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y"),
                           QString::fromUtf8(ecatcmd::home_dir_text(d == 1))));
      }
   }

   /* 「回零速度」不进上面那张 locked 表, 扫描期间也可改 (它是个值不是动作)。
    * 但它是回零框里那行数的来源, 所以下面顺手把那行字刷出来。 */
   pushHomeNote();

   /* 6061h 那一行同一处刷 —— 它读的是同一份电文, 分开刷就会有一天两行说的不是同一时刻 */
   pushHomeMode(t);

   /* 回零横幅: 上升沿起一条, 下降沿只清我们自己写的那条 (原文比对, 同 m_limBanner) */
   if (t.homing)
   {
      const QString s = QStringLiteral("轴%1 正在回零 (方式 %2, %3高速先找) —— "
                                       "**按「停止」可立即中止**")
                           .arg(t.homing_axis == 0 ? QStringLiteral("X")
                                                   : QStringLiteral("Y"))
                           .arg(t.homing_method)
                           .arg(QString::fromUtf8(
                                   ecatcmd::home_dir_text(t.homing_method == 29)));
      if (m_homeBanner != s)
      {
         m_homeBanner = s;
         hint(s, false);
         /* hint() 会给非故障提示挂 8 秒自尽。回零是进行中的状态不是事件, 且这句上挂着
          * "按「停止」可立即中止" —— 让它自己消失, 超时那条路上就没了出路 */
         m_bannerTimer->stop();
      }
   }
   else if (!m_homeBanner.isEmpty())
   {
      if (m_banner->text() == m_homeBanner)
      {
         m_banner->setVisible(false);
         m_bannerTimer->stop();
      }
      m_homeBanner.clear();
   }

   /* 故障复位: 没连接 / 扫描中 / 正在复位 -> 不可用。
    * 刻意不看故障灯: 那盏灯是遥测推的, 永远滞后; 拿它灰掉按钮会出现"灯灭而故障还在,
    * 按钮按不动"。没故障时按下去可证无害 (工作线程那道闸会拦住并说明一个字节都没写)。 */
   m_btnFaultRst->setEnabled(m_connected && !running && !t.resetting);
   m_btnFaultRst->setText(t.resetting ? QStringLiteral("正在复位…")
                                      : QStringLiteral("故障复位"));

   /* 「让 60FDh 进 TxPDO」不跟着 onair 变灰: 它确是连接期参数 (改了不重连不生效),
    * 但人正是连上之后看见三个灯全灰才想到要勾它。改成永远可勾, 由提示说明"下次连接
    * 才生效"。勾错代价为零: em_require_dig_in 只在 em_setup 里读一次。 */
   m_cbWantDigIn->setEnabled(true);

   /* 「输入电平反转 (NPN)」同理不跟着 onair 变灰, 理由更强: 它是运行期参数, 勾一下就
    * 立刻生效。安全相关, 按不了只会把人挡在一个正当的修法外面。
    * 但不从遥测回灌它的勾选状态: setDiInvert 立刻写而 publish 每周期才拷一次, 中间那一拍
    * 回灌会让它自己跳回去。措辞那边走 BusTelem::di_invert (那是真值)。 */
   m_cbDiInvert->setEnabled(true);

   const bool meter_ok = (m_meter != nullptr) && m_meter->isOpen();
   const bool params_ok = m_ctl->paramsError().isEmpty();
   /* 回零中不能起扫: 回零把轴留在使能, 扫描半路撞上既不会自动中止也等不到插补 ——
    * 状态机会以为到了点, 其实一格没动。纵深防御, armRun() 里那道才是权威 */
   m_btnStart->setEnabled(!running && m_connected && meter_ok && params_ok && !t.homing);

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
      else if (m_meter == m_ophir)
      {
         /* 真机把接的是什么摆出来 (表头/探头型号序列号, 当前波长/量程/模式)。
          * 由工作线程拼好 (ophirmeter.cpp 的 buildSummary), 这里原样显示 */
         const OphirInfo i = m_ophir->info();
         if (i.valid && !i.summary.isEmpty())
            s += QStringLiteral(" · ") + i.summary;
      }
      m_lMeter->setText(s);
   }

   /* 「读一次」的硬闸: 控制器不在 Idle 时未决请求归它, 一个手动请求都不能发。
    * 顺带挡掉源没打开和已有手动请求在飞两种情形 (失败原文会盖掉上一次那个好读数)。
    * 不去比对 isEnabled() 再决定要不要设 —— 每帧照设, 影子状态正是忘记同步的来源。 */
   if (m_btnRead != nullptr)
   {
      const bool can = (m_meter != nullptr) && m_meter->isOpen()
                    && !m_ctl->running() && !m_readPending;
      m_btnRead->setEnabled(can);

      /* 为什么按不了要说清楚: 灰按钮加一句原因。基础说明始终在下面 (readOnceTip) */
      QString why;
      if (m_meter == nullptr)
         why = QStringLiteral("没有取样源");
      else if (!m_meter->isOpen())
         why = QStringLiteral("取样源没打开 (先在上面「取样源」里选一个)");
      else if (m_readPending)
         why = QStringLiteral("已经有一个读数在等回话");
      else if (m_ctl->running())
         why = QStringLiteral("扫描进行中: 未决请求归扫描状态机, 手动读会和它抢同一个数");

      m_btnRead->setToolTip(why.isEmpty()
         ? readOnceTip()
         : QStringLiteral("现在读不了 —— ") + why + QStringLiteral("\n\n") + readOnceTip());
   }

   /* 故障横幅只在上升沿弹一次: 每帧都设会把重绘刷爆, 也会把刚点掉的提示顶回来 */
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
   /* 连接会把零点重设为当时所在的位置 (tryInitOrigin), 同一个显示坐标可能已经是另一个
    * 物理位置 —— 世代 +1, 续扫时这一条会被查出来 */
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

   /* 故障不自动消失: 无人值守的一趟扫下来, 一闪而过的提示等于没提示 */
   if (!fault)
      m_bannerTimer->start(8000);
   else
      m_bannerTimer->stop();
}

void ScanWindow::showFault(const QString &why)
{
   hint(QStringLiteral("**扫描已自动中止**: ") + why, true);

   /* 自动中止还要弹模态 (横幅之外, 无人值守时不会错过), 用 singleShot 挪出这一帧再弹,
    * 免得在 30Hz 的槽里嵌套事件循环 */
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
   /* 回零进行中先掐掉它: 这条断开路径是同步等的 (下面 12 秒), 而回零单次能阻塞到 30 秒 ——
    * 不掐的话 12 秒空转到底, 最后 QThread 会在 em_home 还在泵帧时被拆掉。
    * requestMotionStop() 只往一个标志里存 1, 与「停止」按钮直呼的是同一个。
    * 必须先于下面的 postDisconnect: 命令排队, 队列要等回零退出来才轮到。 */
   if (m_thr->isRunning() && m_thr->telemetry().homing)
      m_thr->requestMotionStop();

   /* 先中止扫描再断总线: 反过来状态机会把"掉出 OP"当异常自动中止, 正常收尾变成红色告警 */
   if (m_ctl->running())
      m_ctl->abort(QStringLiteral("断开总线"));

   if (!m_thr->isRunning())
      return;

   /* 断开 = 让工作线程自己走 teardown (失能 → 还原映射 → 降 PRE_OP → 关网卡)。
    * 用队列命令而不是 ::terminate: 收尾这几步不能被打断 */
   m_thr->postDisconnect();

   /* 等它真的收完: 条件是"不在发帧且不在忙", 不是"点过断开" —— teardown 排队执行,
    * 从投递到开跑之间有一小段, 只看 in_op 会在那一段误判成收完了。
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
   /* 扫描在跑也不弹确认框: 关窗 = 中止 + 断开, 已经采到的点都已经落在 CSV 里,
    * 之后「打开 CSV 续扫」能接着跑。要留住这一趟就别关窗 */

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

   /* 参数与网卡写回 scan.ini, 收尾之后才写: 上面那几步万一要弹"可能仍带电"的模态,
    * 那才是人要看的东西, 不该被一次写文件挡在前面 */
   saveSettings();

   e->accept();
}

}   /* namespace scan */
