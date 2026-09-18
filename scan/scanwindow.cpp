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

/* 默认 CSV 目录。放在**当前工作目录**下而不是 exe 旁边: exe 在 bin/, 而
 * 从仓库根目录敲 ./bin/scan.exe 是最常见的用法 —— 那样数据就落在仓库根的 scan_out/,
 * 正是 .gitignore 里那一条。 */
static const char *kOutDir = "scan_out";

/*
 * 「读一次」等回话的上限。
 *
 * **必须比真机那条腿自己的报错阈值长**: ophirmeter 在工作线程里等不到新数时是
 * 1800ms 先报一次错 (一句比"超时"更有用的原因, 见 ophirmeter.h)。这里要是设得比它短,
 * 那句诊断就永远来不及发出来, 界面上只剩一句"没有回应" —— 把源想说的话盖掉是最坏的一种
 * 兜底。模拟源的延迟是几十毫秒, 用不到这么长。
 */
static const int kReadOnceTimeoutMs = 6000;

/*
 * 功率的显示格式。**不固定小数位**: 光电探头的量级从 nW 到 W 都可能 ——
 * 用 'f' 固定 6 位的话 1.2e-9 会显示成 0.000000 (看着像坏了), 而这个量级本来就是
 * 科学计数才读得出来。所以按量级挑一套:
 *
 *   |v| >= 1e-3   -> 'g' 6 位有效数字 ("1", "0.00123457", "12.3456")
 *   否则 / 0      -> 'e' 4 位有效数字 ("1.2000e-09")
 *
 * 'g' 会自己去掉尾零, 不用手写剥零。这一格是给操作员**核对**用的, 不是入 CSV 的那份数
 * —— 入盘的是 double, 一位没少 (所以这里少显示几位不影响数据)。
 */
static QString fmtWatts(double v)
{
   if (std::fabs(v) >= 1e-3)
      return QString::number(v, 'g', 6);
   return QString::number(v, 'e', 4);
}

/* 「读一次」那条固定说明。**只有一份** —— buildMeterPanel 第一次装上去, refresh()
 * 在按不了的时候把原因拼在它前面。两处各写一遍的话, 迟早只剩一处被改 */
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

/*
 * 信号灯: 一个 12px 的圆点。四种样子 (ScanWindow::Lamp)。
 *
 *   Unknown 灰   —— 不知道 (没连接 / 轴上没遥测 / 丢过帧)
 *   Off     灭   —— 这个名字代表的事**没发生**
 *   Ok      绿亮 —— 使能带电
 *   Bad     红亮 —— 出事了 (故障 / 撞限位)
 *
 * 判据只有一条: **灯亮 = 这个名字代表的事正在发生**。这是配电柜上的老规矩,
 * 好处是每一格只需要读一个字就懂。
 *
 * 为什么不是"绿 = 没事": 那样的话正常时是**一片绿灯常亮**(没故障、没撞限位),
 * 而真正要处理的那一秒也是绿的 —— 灯就成了墙纸。现在正常运行时只亮一盏 (使能),
 * 那一盏恰恰是"电机带电, 动手之前先看清楚"的意思, 它是信息不是墙纸。
 *
 * 四种样子**都画满**(不搞"只在出问题时才显示"): 没连接时它坐在灰上, 于是"看不见灯"
 * 本身不会跟"灯灭了"混淆 —— 那正是"一个只在出问题时才出现的指示器"会骗人的地方。
 */
/* 每一盏灯都带上的那一句。**放在这里而不是参数栏那个函数里**, 因为状态栏那两盏也用它 ——
 * 悬停任何一盏都能学到同一条规矩, 否则"为什么使能是绿的、限位却是红的"只能去翻文档。 */
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

/*
 * **只有已经选中的输入框才认滚轮。**
 *
 * 从前滚轮停在参数栏上就能改值 —— 而参数栏比窗口高是常态, 人**就是在滚它**。
 * 于是一次翻页会把路过的那几个框各改一格 (区域 / 分辨率 / 速度 …), 而且改完**看不出来**:
 * 控件里的数变了, 可没人会去逐个核对那十二个数字 —— 而扫描区域算错一格是要撞限位的。
 *
 * 规矩定成"**先点它 (拿焦点), 滚轮才改值**", 与 hmi / 画布那条"会动滑台的动作要多做一步"
 * 是同一条纪律: 最顺手的那个动作永远不产生后果。
 *
 * 没焦点时**不吃这一滚** (ignore 之后事件继续往上传) —— 于是同一个滚轮在参数栏上照样
 * 是滚动条, 而不是"什么都没发生"。这一点不能省: 一个什么都不做的滚轮会让人以为程序死了。
 * 参数栏本来就在 QScrollArea 里, 所以"滚轮滚不动"恰恰是最坏的那种无反应。
 *
 * ── 事件真正落在哪个控件上 (这一段是**实测**出来的, 别再照直觉改) ──────────
 *
 * 光标停在 QDoubleSpinBox 上滚一格, 投递顺序 (用装在 qApp 上的探针抓下来的) 是:
 *
 *    QWidgetWindow  ->  QLineEdit 'qt_spinbox_lineedit'  ->  QDoubleSpinBox
 *
 * 也就是说**先接到滚轮的是 spin box 内部那个 QLineEdit**, 不是 spin box 自己 ——
 * 而 QAbstractSpinBox 正好在那个 line edit 上装了自己的事件过滤器 (它内部编辑框的
 * 按键与滚轮都走那一条), 值就是在那里被改掉的。过滤器只挂在外层 spin box 上的话,
 * 这一滚**根本轮不到我们** (第一版就是这么错的: 探针把事件直接 sendEvent 给 spin box,
 * 看着闸住了; 真机上事件先落在内部 line edit 上, 值照样一格一格地变)。
 *
 * 两条由此而来, 缺一条就白闸:
 *   · 过滤器要挂在输入框**以及它的每一个子控件**上 (见 guard());
 *   · 判"选中没有"要从**收到事件的那个控件往上找**到被闸的输入框, 再拿它去比焦点 ——
 *     拿 line edit 自己去比焦点是永远比不上的 (焦点在 spin box 上), 那会变成一个
 *     "点进去了也不许改"的坏锁。
 *
 * ── 为什么还要"一滚页面就作废" ────────────────────────────────────────────
 *
 * 光有"点过才认"还是不够: 点过之后焦点一直留在那个框上, 而人要滚参数栏时
 * **鼠标正是从参数框上划过去的** —— 路过刚点过的那个框, 它照样改值。
 * 所以还要接住滚动区: **参数栏一滚, 选中就作废** (见 buildUi 里那个 connect)。
 * 于是"翻页"这个动作本身永远改不了任何参数, 想用滚轮改就重新点一下。
 */
class WheelNeedsFocus : public QObject
{
public:
   explicit WheelNeedsFocus(QObject *parent = nullptr) : QObject(parent) {}

   /* 把 w 以及它的每一个子控件都闸上 (子控件那一条见上面"事件真正落在哪个控件上") */
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

   /* 「读一次」要能听见**任何一个**源的回话 —— m_meter 是会换的, 每换一次重接一遍
    * 就是漏接的机会。干脆四个都接上, 靠 m_readPending 认出"这一份是不是我的":
    * 控制器在跑的时候它是 false, 那时所有回话都归状态机, 这边一句都不插嘴。 */
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
              /* 当前选着的那条**优先保住** (点一次「刷新网卡」不该把选好的卡弄丢);
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

              /* 上次那张卡不在了 (换了机器 / USB 网卡没插)。**必须说出来** ——
               * 不说的话现象是"开机默认选了另一块卡", 而人以为程序记错了 */
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

   /* 右侧一列: 扫描参数 / 扫描控制 / 功率计 / 色标。**装进 QScrollArea** ——
    * 这一列比窗口高是常态 (六组框加起来约 1330px, 而窗口里给它的位置通常只有 670),
    * 从前窗口一矮, 最下面的「色标」就被挤得看不见了, 而那正是操作员要改的东西。 */
   QWidget *side = new QWidget;
   side->setMinimumWidth(340);

   QVBoxLayout *sv = new QVBoxLayout(side);
   sv->setContentsMargins(0, 0, 0, 0);
   sv->setSpacing(8);
   /* 「轴信号」和「限位开关」排在最上面。这一列比窗口高是常态, 而它们是**状态** ——
    * 要滚才能看到的状态指示器不算状态指示器。两个框加起来也就七行, 挤不掉什么 */
   sv->addWidget(buildAxisPanel());
   sv->addWidget(buildLimitPanel());
   /* 第三块:「回零」。它是**动作**不是参数, 但它与「限位开关」上面那两块有直接的
    * 依赖 —— 回零找的就是那三盏灯说的那几个开关。紧挨着摆, 点之前眼睛能扫到它们 */
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
   /* 两条滚动条都用默认的 AsNeeded: **内容不允许被悄悄裁掉**。横向的那条平时不出现,
    * 只有把下面那根分隔条拖得太左、输入框真的放不下时才出来提醒一声 */
   sideScroll->setMinimumWidth(320);
   /* **下限压到 0**: 否则滚动区会拿里面那列的高度去顶窗口的最小高度,
    * 窗口就再也缩不小了 —— 那正是加这个滚动区要解决的问题 */
   sideScroll->setMinimumHeight(0);

   /*
    * 左画布 | 右参数栏, 中间一根**可以拖的分隔条**。
    *
    * 为什么不用"画布伸缩 + 参数栏固定宽度": 参数栏里那些输入框有自己的最小宽度,
    * 固定宽度一旦压到它下面, QScrollArea 就**把右边裁掉** (横向滚动条关着的话连
    * 拖都拖不回来)。交给 QSplitter 之后, 宽度分配由它算, 拖不动就是真的放不下了。
    * 顺带还给了操作员一个旋钮: 嫌画布小就往左拖, 嫌参数栏窄就往右拖。
    */
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

   /*
    * 滚轮闸 (见 WheelNeedsFocus)。**名单是"找出来的", 不是手写的** ——
    * 手写一张"哪些算输入框"的表, 漏一个就是漏一个 (而且漏了不报错, 只是那个框
    * 还在被滚轮改), 而这里能漏的唯一方式是"新加了一个既不是 QAbstractSpinBox
    * 也不是 QComboBox 的输入控件" —— 那种东西现在还没有。
    *
    * 挂在最后: 这里才保证上面那六组框全都建出来了。
    *
    * QLineEdit (CSV 路径 / 脚本路径) 不闸 —— 滚轮**改不了**它的文字, 没有要闸的东西;
    * 它的那一滚照旧落给参数栏滚动区, 页面该滚还是滚。
    */
   m_wheelGuard = new WheelNeedsFocus(this);
   for (QAbstractSpinBox *x : findChildren<QAbstractSpinBox *>())
      m_wheelGuard->guard(x);
   for (QComboBox *x : findChildren<QComboBox *>())
      m_wheelGuard->guard(x);

   /*
    * **参数栏一滚, "选中"就作废。**
    *
    * 这是"翻页永远不会改参数"的另一半 (另一半是 WheelNeedsFocus 那条"点过才认"):
    * 点过某个框之后, 人接着滚参数栏 —— 鼠标正是从那些框上划过去的, 路过刚点过的
    * 那个, 它照样改值。滚动区的滚动条一动就说明**人在翻页, 不是在改参数**,
    * 于是把参数框的选中 (焦点) 清掉, 想用滚轮改就得重新点一下。
    *
    * 只清**滚动区里面**那个焦点: 焦点在画布或别处时不受影响。
    */
   connect(sideScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [sideScroll](int) {
      QWidget *fw = QApplication::focusWidget();
      if (fw != nullptr && sideScroll->isAncestorOf(fw))
         fw->clearFocus();
   });

   /* ---- 状态栏 ---- */
   m_lNote = new QLabel(this);
   m_lWkc  = new QLabel(this);
   m_lWkc->setFont(QFont(QStringLiteral("Consolas")));

   /* 限位两根轴各一个, **灯 + 字一起常显**。常显的理由: 没连接时它写 "--"、灯是灰的,
    * 于是"看不到它"本身不会跟"限位正常"混淆 —— 一个只在出问题时才出现的指示器,
    * 出问题时也未必有人正看着它 */
   /* 参数栏那两组灯用的是同一套语法 (见 kLampRule 与 buildAxisPanel/buildLimitPanel)。
    *
    * **这一盏说的是判据, 不是开关本身** —— 默认判据是 6041h bit11 (勾了输入反转就换成
    * 反相后的那两个限位开关)。两者是两个问题, 而这一盏回答的是 "会不会中止扫描"那一个。
    * tooltip 必须说清, 否则它跟参数栏那六盏会被读成同一件事。 */
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
   /* 走槽而不是直连 postStop: 回零期间这个按钮必须换成立即中止。
    * 非回零时那个槽做的事与直连 postStop 一字不差 —— 见 onStopClicked */
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

/*
 * 参数栏两块信号网格的列号。**列号 = 本组实际用到的最后一列 (= 信号数)** ——
 * 这个值同时要交给 setColumnStretch, 单独写错一次就会凭空多出一个空列, 把前面几列
 * 挤成一个字宽 (这个坑已经付过一次学费, 见 docs/scan_sweep.md §15 的踩坑表)。
 * 所以列数只在这里写一遍, 上面造列和末尾拉伸都读它。
 */
enum { AX_ENABLED = 0, AX_FAULT = 1, AX_NCOL = 2 };
enum { LIM_HOME = 0, LIM_POS = 1, LIM_NEG = 2, LIM_NCOL = 3 };

/* 两块网格共用的那一句 kLampRule 定义在本文件顶部 (状态栏那两盏也要用)。 */

/*
 * 「轴信号」: 每根两个信号 (使能 / 故障), 第三行是跨全部列的「故障复位」按钮。
 *
 * 判据一律是**同一个规矩**: 灯亮 = 这个名字代表的事正在发生; 灭 = 没发生; 灰 = 不知道。
 * 使能灯是绿亮 (带电), 故障灯是红亮 (出事了) —— 每一格旁边都有字, 所以"绿"
 * 不必再单独背一套含义。
 *
 * **名字写在表头上**, 不是每格重复一遍: 两行三列都写"使能 故障 限位"的话,
 * 这块地方会变成一片字, 而灯反而看不见了。
 */
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

   /*
    * 第三行: 「故障复位」跨满全部列。
    *
    * 它**不按故障灯决定可用性** —— 那盏灯是 30Hz 遥测推的, 永远滞后于驱动器,
    * 拿它去灰掉按钮正是"两个指示器互相矛盾"那一类。没故障时按下去是**可证无害**的:
    * 工作线程那条闸 (ecatcmd::axis_needs_reset) 会拦住并说明"一个字节都没写"。
    * 这里只拦界面自己才知道的两件事: 没连接、扫描在跑 (见 onFaultResetClicked)。
    */
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
      "· 每次点击都会弹一次确认, 不做持久勾选。"));
   connect(m_btnFaultRst, &QPushButton::clicked, this, &ScanWindow::onFaultResetClicked);
   g->addWidget(m_btnFaultRst, 3, 0, 1, AX_NCOL + 1);

   /* 多出来的宽度全给**本组最后一列** (不是写死 3), 让灯和字都靠左排成一条。
    * 写成 AX_NCOL + 1 就会凭空多一个空列 —— 那正是上面那张警告说的坑 */
   g->setColumnStretch(AX_NCOL, 1);
   return box;
}

/*
 * 「限位开关」: 每根三个信号 (原点 / 正限位 / 负限位), 第三行是可选的 60FDh 重映射。
 *
 * 与上面那块**分开两个框**, 理由不只是宽度: 这一组问的是**开关本身压着没有**,
 * 而「轴信号 → 故障」和状态栏的「限位」问的是**驱动器会不会为此中止扫描**。
 * 两者可能不一致, 而把它们并排放在一行里, 就会看起来像同一个问题的两个答案。
 *
 * 三个灯**纯显示, 不新增任何中止判据** —— 扫描区域本来就可能正好停在一个开关上,
 * 用监视量去触发中止会白中止一趟一小时的活。会中止的仍然只有 6041h bit11。
 */
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
      /* 原点灯是**绿的**, 所以要说出声来 —— 见 docs/scan_sweep.md §9「原点为什么不是红的」 */
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

   /*
    * 第三行: 可选的 60FDh 重映射开关。**默认关**。
    *
    * 本机实测生效的 1A00h 只有 6041h/6064h/606Ch 三项 10 字节, 60FDh **不在里面**
    * (docs/ykd2205pe_ci402.md) —— 也就是说默认情况下上面三个灯会一直是"灰 + --"。
    * 两条出路, **优先走第一条**:
    *   1. 用厂家上位机改一次驱动器的 1A00h (零代码、断电也还在);
    *   2. 勾上这个框, 让主站在连接时补一次 (仅写 RAM, 收尾时还原; 崩在收尾前会留在
    *      驱动器里直到断电 —— 与 motor_test --allow-pdo 同一风险等级)。
    *
    * 它是**连接期**参数: 改了不重连不生效, 所以只在未连接时可改 (见 refresh())。
    */
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

   /*
    * 第四行: 「输入电平反转 (NPN)」。**默认关**。
    *
    * 治的是 2026-09-18 真机上那个病: X0~X3 接的是 NPN 传感器 (高电平 = 未触发),
    * 而驱动器的 2300h (输入有效电平逻辑) 按常开配着 —— 于是 60FDh 那三位**整排反相**,
    * 两个限位常年都报"压着", 6041h bit11 恒为 1, 扫描一步都开不了。
    *
    * tooltip 第一段就是那句最要紧的话: **它只治上位机这一侧**。不把这一句放最前面,
    * 人会以为勾了就"修好了", 而驱动器自己的限位保护还按那套错的极性算着 ——
    * 那是个比原来更坏的状态, 因为原来它至少是拦住的。
    *
    * 与上一行那个框的两处不同, 都在 tooltip 里说清了: 它**立即生效**(不用重连),
    * 而且**不持久化**。理由见 onDiInvertToggled 上面那段。
    */
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

/*
 * 「回零」—— 驱动器自带的正/反向找原点 (6060h = 6)。
 *
 * 为什么单开一个框、还摆在第二块之后: 它是**动作**不是参数, 而且是本程序里唯一一个
 * "按下之后滑台会带电自己走"的按钮。塞进扫描参数栏会和「区域」「分辨率」那些一起
 * 被当成一行设置; 混进顶栏又会和「全部回中」挨在一起 —— 而那一个是走到软件零点,
 * 完全不带电。分开摆, 名字、颜色、灰法才能各自正确。
 *
 * 四个按钮而不是一对: 一根轴一个方向一个。理由是回零**会失能**再走 (6098h 只能在
 * 未使能时写), 竖直轴会在这期间失去保持力矩 —— 所以"只回我这一根"必须点得出来。
 */
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
   /*
    * 缺省**必须显式设**: 新建的 QSpinBox 是 0, 而 setRange 会把它夹到**下限**
    * HMI_HOME_VEL_MIN (100) —— 不写这一句, 界面上显示的就不是 HMI_HOME_VEL_DEF,
    * 而是 100。那个值本身不危险, 只是与 em_home 的缺省悄悄不一致。
    *
    * 放这里而不是 applyDefaults(): 后者会被「恢复默认」再跑一遍, 那个按钮是给扫描
    * 几何用的 —— 顺手点一下就把为试回零特意改过的速度抬回去, 是那种最难查的改动。
    */
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
      "它不会被记住 —— 每次回零的确认弹窗都会把当次数值念一遍。"));

   g->addWidget(new QLabel(QStringLiteral("回零速度"), box), 0, 0);
   g->addWidget(m_edHomeVel, 0, 1, 1, 2);

   /*
    * 按钮文字自带轴与方向 ("X 正向回零"), 不做"一个表头 + 四个短标签"——
    * 布局一挤, 短标签就归错了列, 而错点这个按钮的代价是滑台朝**反方向**去找开关。
    * 模态里还会把轴、方向、方式号再念一遍, 但那不该是唯一一道防线。
    */
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

   QLabel *note = new QLabel(QStringLiteral(
      "回零 = 让驱动器自己带电朝开关走。方向与开关位置只有现场知道。"), box);
   note->setWordWrap(true);
   note->setStyleSheet(QStringLiteral("color:#6b7480;"));
   g->addWidget(note, 3, 0, 1, 2);

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

   /* 手动速度: 点画布 /「全部回中」时用。
    * **扫描中不生效** —— 那时候速度归 ScanController 管, 中途改会让"每点耗时"的
    * 估算和状态机的超时判据都对不上。扫描一结束/中止, 这里会自己把手动速度推回去 */
   m_edManSpeed = new QSpinBox(box);
   m_edManSpeed->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_edManSpeed->setSingleStep(1000);
   m_edManSpeed->setSuffix(QStringLiteral(" pul/s"));
   /* 提示里**不提「重测选中点」** —— 那一个走的是扫描状态机 (armRun), 用的是
    * 「扫描速度」。写上就成了假话, 而操作员照着它会得出错的预期 */
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

   /*
    * **必须显式把缺省值填进控件。** QDoubleSpinBox 初值是 0, 而它的最小值是 0.1 ——
    * 于是"没设过"的控件会**自己夹到最小值**上: 区域变成 0.1×0.1、每单位脉冲变成 100,
    * 量程算出来只有 105 脉冲。界面看着像那么回事, 而扫描的区域其实是 0.1 单位。
    *
    * 这一段必须在下面那些 connect **之前** —— 否则每 set 一个值都会触发一轮
    * pushParams() → refresh(), 而 refresh() 要用的按钮和标签此刻还没建出来。
    */
   applyDefaults();

   /* 再拿记忆覆盖一遍 (顺序不能反, 见 scanwindow.h)。
    * 也在这里、也在 connect 之前: 读回来的值走的是同一条"不要触发信号"的路 */
   loadSettings();

   /* 参数一改就重算 —— 让操作员在**按开始之前**就看见这一趟多长 */
   const QList<QDoubleSpinBox *> dspins{m_edAreaX, m_edAreaY, m_edRes, m_edPpu};
   for (QDoubleSpinBox *s : dspins)
      connect(s, &QDoubleSpinBox::valueChanged, this, &ScanWindow::pushParams);
   const QList<QSpinBox *> ispins{m_edSpeed, m_edDwell, m_edSettle, m_edSamples};
   for (QSpinBox *s : ispins)
      connect(s, &QSpinBox::valueChanged, this, &ScanWindow::pushParams);

   /* 手动速度不进 Params (它跟扫描几何无关, 也不该写进 CSV 表头), 所以不走 pushParams ——
    * 直接叫 refresh(), 那里有"没扫描就把手动速度推给工作线程"那一段 */
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

   /* **没装 StarLab 的那一项照样列出来, 但灰掉** —— 直接不列的话, 操作员会以为
    * 这个程序没有真机这条路; 列出来灰着, 至少看得出"这一项存在, 但这台机器上
    * 缺东西"。缺的是什么由 statusTip 说清楚。 */
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

   /* 真机的三项。选项表**由设备给**, 这里一个都没写死 —— 探头不同, 能选的波长和
    * 量程就不同, 按型号推断规格正是手册不让做的事 (见 ophirmeter.h)。
    *
    * 三行连着小标签一起收进一个容器里, 切到模拟源时整块 setVisible(false) ——
    * 只灰着不藏起来的话, 那块空地会让人以为"这几个就是给模拟源调的" */
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

   /*
    * 「读一次」—— 接上之后第一件想做的事就是点一下看个数, 而不是先开一趟一小时的扫描。
    *
    * 它是**当前取样源**的一次普通请求, 走的正是扫描用的那条路 (requestReading →
    * readingReady/readingFailed), 所以这一下通了, 采集那条路也就通了 —— 四个源都能点,
    * 拿模拟源先对流程也行。
    *
    * 它**不是**第二个采集器: 接口约定"同一时刻只允许一个未决请求", 所以控制器不在
    * Idle 时这个按钮是禁用的 (见 refresh() 里那条闸), 抢数的事不会发生。
    */
   m_btnRead = new QPushButton(QStringLiteral("读一次"), box);
   m_btnRead->setToolTip(readOnceTip());
   connect(m_btnRead, &QPushButton::clicked, this, &ScanWindow::onReadOnceClicked);

   m_lReadout = new QLabel(box);
   m_lReadout->setWordWrap(true);
   m_lReadout->setStyleSheet(QStringLiteral("color:#7b8391;"));
   /* 这一格会显示**设备来的字** (探头报的过量程原因之类)。QLabel 默认 AutoText,
    * 那串字里只要有个 `<` 就会被当 HTML 解析 —— 于是要么显示不全, 要么被当成标签吃掉。
    * 明写 PlainText: 这里只显示, 不排版 */
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
   /* 同样: 这句里全是**设备给的字** (表头/探头型号、序列号)。AutoText 会试着按 HTML
    * 解析它 —— 明写 PlainText, 免得某个序列号里的尖括号把这一行吃掉 */
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
   /* scanplan.h 里的 Params 就是缺省值的唯一定义处 —— 这里不另抄一份数字 */
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

   /* 手动速度不在 Params 里 (它跟扫描几何无关), 缺省就是 HMI_VEL_DEF —— 与 hmi 一致 */
   m_edManSpeed->setValue(HMI_VEL_DEF);

   /* **回零速度刻意不在这里**: 它也不在 Params 里, 但「恢复默认」会把 applyDefaults
    * 再跑一遍 —— 于是操作员为了第一次试回零特意压到 100, 却因为顺手点了「恢复默认」
    * (那是给扫描几何用的) 被悄悄抬回 500。它的缺省设在 buildHomePanel 里, 只走一次。 */
}

/*
 * 记忆: 读回上次的参数 (见 scanprefs.h)。
 *
 * **它只覆盖 ini 里真有的项** —— prefsLoad 把缺的项留在缺省上, 而上面那句 applyDefaults
 * 刚把缺省填进控件, 于是"没记过的项 = 缺省"是同一件事, 不必在这里再判一次。
 *
 * setValue / setCurrentIndex 都会**自动夹进控件的量程** —— 一个被手改坏的 ini
 * (区域写成 1e9) 会变成 500 而不是让界面炸掉。夹掉了不吭声: 这是记忆, 不是配置校验,
 * 一开机就为一句话弹框只会被条件反射地关掉。
 */
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

/*
 * 记忆: 把当前参数写回去。**「连接」时与关窗时各一次**。
 *
 * 连接那一次是关键: "上次用的是哪张卡"说的是**真连过的那张**, 而不是"下拉框最后停在
 * 哪张"。关窗那一次则是补上"改了参数但没连接就关掉"这种情况。
 */
void ScanWindow::saveSettings()
{
   Prefs pf = prefsLoad(prefsPath());     /* 先读回上次那份: 下面两处都是"覆盖不了就留着" */

   /* **过不了体检的参数不覆盖旧的** —— 规则本身在 scanprefs 里 (那里能被自检钉住) */
   prefsMergeParams(&pf, currentParams());

   const QString nic = m_nic->currentData().toString();
   pf.nic = nic.isEmpty() ? m_savedNic : nic;   /* 清单还没到 / 卡被拔了: 别把记住的抹掉 */
   pf.manual_speed = m_edManSpeed->value();

   prefsSave(prefsPath(), pf);
   m_savedNic = pf.nic;
}

void ScanWindow::onRestoreDefaults()
{
   /* 扫描中锁着这些控件 (见 refresh 的 locked 列表), 但那是"控件变灰"这一层的拦 ——
    * 这里再判一次, 是因为恢复默认会**重建网格**: 几何改到一半的扫描落进 CSV 的
    * (ix,iy) 与实际位置就对不上了 */
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

   /*
    * 网格那一行**自己按参数算 nx/ny, 不读 m_ctl->gridNx()**。
    *
    * 超上限时控制器那边是 0×0 (它一个 Point 都没建, 见 rebuildPlan 那段), 而这一行
    * 恰恰是要把"你要的这个网格有多大"说清楚的 —— 读控制器就只会显示 "0 × 0 = 0 点",
    * 一句正确但没用的话。同一件事算两遍在这里是**必要的**: 控制器答的是"建出来的网格",
    * 这一行答的是"参数说的网格", 超上限时这两者本来就该不一样。
    */
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

   /* **连上之前先记住这张卡** —— "上次用的网卡"说的是真连过的这张。
    * 放在这里而不是等连接成功: 连接失败也说明人选的就是它, 下次开机仍然该默认它 */
   saveSettings();

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

/*
 * 清驱动器的故障位。
 *
 * 两处闸分工明确, 各管一件事, **不要合并**:
 *   · 这里的闸 = 界面自己才知道的事 (扫描在不在跑 / 有没有连上);
 *   · 工作线程的闸 (ecatcmd::axis_needs_reset) = **该不该对这根轴写字节**。
 *     "该复位哪根"是驱动器的事实 (6041h bit3), 不是操作员的选择 —— 所以按钮不带轴参数。
 */
void ScanWindow::onFaultResetClicked()
{
   /* 复位后该轴**失能**, 而失能轴扫不动 —— 从 Paused 继续会在第一拍就以一个费解的
    * 理由中止。所以是「先中止」, 不是「暂停再继续」。沿用既有先例 (设零点那道闸)。 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能做故障复位 "
                          "(复位后该轴失能, 失能轴扫不动)"), true);
      return;
   }

   /*
    * **每次点击都弹模态确认**, 不做持久勾选 —— 与「使能」「连接」同一个先例。
    *
    * 弹窗里写的是**它以为**哪根轴报了故障: 遥测最多滞后 ~33ms, 而真正的判据在工作
    * 线程里 (它读的是**刚读到的** 6041h bit3)。所以措辞用"看起来", 并明确说出
    * "一个字节都没写"那种情况 —— 操作员按下去之前该知道最坏也就是白按一下。
    */
   const BusTelem t = m_thr->telemetry();
   int todo[EM_MAX_AXES];
   const int ntodo = ecatcmd::pick_faulted_axes(t, todo, EM_MAX_AXES);

   QString who;
   if (ntodo == 0)
   {
      who = QStringLiteral("**现在看起来没有轴报故障 (6041h bit3 都是 0)** —— "
                           "按下去大概率**一个字节都不会写**。\n"
                           "(如果故障是刚刚起来的, 工作线程会看到它并照常复位。)");
   }
   else
   {
      QStringList names;
      for (int k = 0; k < ntodo && k < EM_MAX_AXES; k++)
         names << QStringLiteral("轴%1").arg(todo[k] == 0 ? 'X' : 'Y');
      who = QStringLiteral("看起来报故障的是: %1。").arg(names.join(QStringLiteral("、")));
   }

   QMessageBox box(QMessageBox::Warning,
                   QStringLiteral("故障复位"),
                   QStringLiteral(
                      "%1\n\n"
                      "确认:\n"
                      "  · 人已经在设备旁边\n"
                      "  · 手放在物理急停上\n"
                      "  · **竖直轴下面没有人**\n\n"
                      "复位的动作是**先写 6040h = 0x0000 (卸力) 再抬 bit7** ——\n"
                      "bit7 是上升沿触发, 不先把 0 压下去就构不成沿。\n"
                      "所以只对报了故障的轴做; 对一根健康的保持轴做这件事会松开它的保持力矩。\n\n"
                      "复位成功后该轴停在**未使能**, 要接着走请重新点「使能」。\n"
                      "每根轴最多 1 秒, 这段时间不可中断。").arg(who),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   m_thr->postFaultReset();
}

void ScanWindow::onWantDigInToggled(bool on)
{
   m_thr->setWantDigIn(on);

   /*
    * 这个框**永远可以勾** —— 从前是 setEnabled(!onair), 那是错的: 人恰恰是在连上
    * 之后、看见上面三个灯全是"灰 + --"的时候, 才知道要勾它, 而那时候框是灰的
    * (而且 onair 里那个 busy 一旦被漏掉就永远为真, 见 EcatThread::doConnect 那段),
    * 于是表现就是"勾不上"。
    *
    * 但它是**连接期**参数: em_require_dig_in() 只在 em_setup 里被读一次, 所以勾了
    * 对当前这一次连接没有任何影响。这一件事界面上看不出来, 只能说出来 —— 已经连上了
    * 就讲明白"下次连接才生效", 并指一下眼前这一路 (那三个灯现在是灰的, 但扫描、
    * 中止判定、故障复位都不依赖它, 不必为它停下手上的活)。
    */
   if (m_connected)
   {
      hint(on ? QStringLiteral(
                       "已记下: 下次「连接」时把 60FDh 追加进 TxPDO。"
                       "**本次连接不受影响** —— 三个限位灯要等重新连接之后才会亮。")
              : QStringLiteral("已记下: 下次「连接」不再动 TxPDO 映射。**本次连接不受影响**。"),
           false);
   }
}

/*
 * 「输入电平反转 (NPN)」。**运行期参数**: 不写驱动器、不进 ini、不用重连。
 *
 * ── 为什么不做持久化 ────────────────────────────────────────────────────────
 * 与「让 60FDh 进 TxPDO」同一个先例 (见 scan/scanprefs.h 那份"不记什么"的清单):
 * 反转是「这台机器的 X0~X3 就是这么接的」的一条**断言**。断言对了是省事, 断言错了的
 * 后果不是"灯显示不对", 是**保护反过来** —— 真压着限位时它说没压着。这种东西必须
 * 每次由人当面确认, 不能从一个 ini 里悄悄继承下来: 换台机器、或者哪天有人把 2300h
 * 改对了, 继承下来的那个"开"就是一个已经静悄悄失效了的保护。
 *
 * ── 这里必须"说出来"的两件事 (界面上都看不出来) ──────────────────────────────
 *   · 它**立即生效** —— 与上一行那个框正好相反, 别让人以为要重新连接;
 *   · 它**只治上位机这一侧** —— 驱动器自己的 bit11 与限位保护不受影响。
 *     这一句不能省: 省了, 人会以为勾一下就"修好了", 而实际上驱动器还按错的极性
 *     保护着。原来那个状态至少是拦住的, 现在这个不是。
 *
 * ── 为什么 60FDh 读不到时要弹重话 ────────────────────────────────────────────
 * 反转生效时判据是 LIMIT_RULE_INVERT (不看 bit11), 所以读不到 60FDh 就等于
 * **一条判据都没有** -> 限位信号一律按「有效」中止 -> 扫描永远开不了。这不是"可能出问题",
 * 是必然开不了, 所以要在勾的一瞬间就说, 并指清那两条出路。
 */
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

/*
 * 「停止」。
 *
 * 两副面孔, 而这不是"顺手加的功能": 回零是**阻塞在工作线程里**的 (em_home 自己泵帧、
 * 自己轮询), 而命令队列是那个线程在 run() 顶部排空的 —— 一条 CMD_STOP 要等回零自己
 * 退出来才轮到, 那时 30 秒超时早就过去了。**靠队列实现不了"按停止键直接停"。**
 *
 * 所以回零期间走 requestMotionStop(): GUI 线程直呼 em_request_stop(), 只往一个
 * volatile 标志里存 1, em_home 的 2ms 轮询下一个周期就看见并撤掉 6040h bit4。
 * 这是全程序唯一一处 GUI 直呼 motor_api —— 理由与安全性的论证见 ecatworker.h 顶部。
 */
void ScanWindow::onStopClicked()
{
   if (m_thr->telemetry().homing)
   {
      m_thr->requestMotionStop();

      /* **不追加 postStop()**: 回零的收尾自己会把目标冻在落点 (重新锚定零点 + 目标归零),
       * 再投一条只会让 doStop() 的 note 把回零结果那一句从状态栏盖掉 (note 是覆盖写),
       * 而"回零是被中止的还是到位了"正是此刻要看的那句话。 */
      hint(QStringLiteral("正在中止回零… 「停止」已发出 (收尾要 失能 → 切回 CSP → 重新使能, "
                          "最多几秒)"), false);
      return;
   }

   /* 非回零时与从前一字不差 —— hmi 那边这个按钮仍然直连 postStop, 共用的是同一份
    * ecatworker.cpp, 行为不变 */
   m_thr->postStop();
}

/*
 * 「X/Y 正/反向回零」。
 *
 * ⚠️ 这是全程序**最危险的一个按钮**: 按下之后滑台会自己带电朝开关走, 朝哪走、什么时候
 * 停、撞不撞开关, 全由驱动器按 6098h 决定。本程序只发一条"开始回零", 软件**拦不住它
 * 撞开关** —— 能做的只有: 慢速缺省、每次点击都要人当面确认方向与速度、以及一个真能
 * 生效的中止。这三条就是下面这个模态的全部内容。
 *
 * 与 onEnableClicked / onFaultResetClicked 同一套写法: **每次都问**, 不做持久勾选,
 * 默认按钮是 Cancel, 不抽公共 helper (三处的清单各说各的, 抽出去反而看不清少了哪一条)。
 */
void ScanWindow::onHomeClicked(int axis, int dir)
{
   if (axis < 0 || axis > 1 || dir < 0 || dir > 1)
      return;

   /* 扫描中一律拦住。这一条其实是**第二道** —— 按钮本身在扫描期间就是灰的
    * (见 refresh 里的 can_home), 但模态前面这道闸留着, 因为"灰掉的按钮"不是
    * 一个能读的理由 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中 —— 先「中止」才能回零"), true);
      return;
   }

   const bool        neg  = (dir == 1);
   const int         meth = ecatcmd::home_method_for(neg);
   const uint32_t    vel  = ecatcmd::home_vel_clamp(m_edHomeVel->value());
   const uint32_t    slow = ecatcmd::home_vel_slow(vel);
   const uint32_t    acc  = ecatcmd::home_accel_for(vel);
   const QString     ax   = (axis == 0) ? QStringLiteral("X") : QStringLiteral("Y");
   const QString     dtxt = QString::fromUtf8(ecatcmd::home_dir_text(neg));

   /*
    * "能找多远"。**必须用工作线程那三个函数算, 不能在这里另写一遍** —— 抄一遍的话,
    * 哪天 home_accel_for 改了斜坡时间或超时改了, 弹窗还在念旧的数, 而操作员正是照
    * 这三个数决定按不按下去。
    *
    * 一圈多少脉冲取界面上那个「分辨率」框 (它本来就是 2400h 的实测值, 默认 50000);
    * 框里是 0 的时候退回 50000, 免得除出 inf。
    */
   const double ppu   = (m_edPpu->value() > 0.0) ? m_edPpu->value() : 50000.0;
   const double reach = (double)vel * (HMI_HOME_TMO_MS / 1000.0) / ppu;

   /* 遥测只用来**读一遍现场**, 用来把该说的话说全 —— 它不参与"能不能做"的判断,
    * 那一条在工作线程的那道闸里 (doHome), 用的是刚读到的值。 */
   const BusTelem t = m_thr->telemetry();

   QString warn;
   if (t.ax[axis].valid && t.ax[axis].limit_active)
   {
      /* 复用限位那三句**唯一的定义** (它们放在头文件里正是为了这个): 此刻判据成立,
       * 而回零是朝开关走 —— 操作员必须知道现在这一路已经是"有效"的。
       * 顺带把 NPN 极性那件事摆出来: 极性配反时 bit11 恒为 1, 回零会找不到跳变。 */
      const AxisTelem &a = t.ax[axis];
      warn = QStringLiteral("\n"
         "⚠️ **这一根现在的限位判据就是成立的** —— 回零是朝开关走, 这一点要看清楚:\n"
         "  %1\n"
         "  %2\n"
         "  %3\n")
         .arg(QString::fromUtf8(ecatcmd::limit_hit_headline(t.di_invert)),
              QString::fromUtf8(ecatcmd::limit_switch_text(a.dig_known, a.dig_pos,
                                                           a.dig_neg, t.di_invert)),
              QString::fromUtf8(ecatcmd::limit_hit_advice(a.dig_known, a.dig_pos,
                                                           a.dig_neg, a.dig_home,
                                                           t.di_invert)));
   }

   QMessageBox box(QMessageBox::Warning,
                   QStringLiteral("回零 —— 滑台会自己带电去找开关"),
                   QStringLiteral(
                      "轴 %1, **%2**找原点 (6098h = %3)。\n\n"
                      "确认:\n"
                      "  · 人已经在设备旁边\n"
                      "  · 手放在物理急停上\n"
                      "  · 滑台行程里没有手、工具、线\n"
                      "  · **竖直轴下面没有人**\n\n"
                      "**驱动器会自己带电并移动。** 本程序只发一条「开始回零」—— 之后朝哪个\n"
                      "方向走、什么时候停、撞不撞开关, 全由驱动器按上面那个方式自己决定。\n"
                      "**软件拦不住它撞开关**, 能做的只有「停止」立即中止。\n\n"
                      "该轴会**先失能** (6098h/6099h/609Ah/607Ch 只能在未使能时写) ——\n"
                      "竖直轴会在这时候失去保持力矩, 可能下滑。\n\n"
                      "%4\n"
                      "回零结束后会自动切回 CSP 并**保持使能** (停在落点带保持力矩),\n"
                      "显示坐标会把回零点当做 0 (**零点世代 +1**)。\n\n"
                      "**按「停止」可立即中止** (不用等那 %5 秒)。\n\n"
                      "**「%2」说的是电机轴的正反向** —— 与画布上 +%6 是不是同一个方向,\n"
                      "只有现场试一次才知道。方向不对就换另一个按钮, 别硬顶。%7")
                      .arg(ax, dtxt).arg(meth)
                      .arg(QStringLiteral(
                         "本次: 找原点速度 %1 pul/s (6099h:01), 返回速度 %2 pul/s (6099h:02),\n"
                         "加减速 %3 pul/s² (609Ah), 原点偏移 0 (607Ch), 上限 %4 秒后判超时。\n"
                         "速度 × %4 秒 = 一次回零最多走过的距离 (%5 圈) —— 碰不到开关时请\n"
                         "先把滑台手动挪近, **不要**为了够得着去调高速度。\n")
                         .arg(vel).arg(slow).arg(acc)
                         .arg(HMI_HOME_TMO_MS / 1000)
                         .arg(QString::number(reach, 'f', 1)))
                      .arg(HMI_HOME_TMO_MS / 1000).arg(ax, warn),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   /*
    * 零点世代 +1。**在 postHome 之前**, 而且**无条件**。
    *
    * 为什么在 GUI 而不是在工作线程 (真正重新锚定 m_origin 的地方): GUI 无法知道工作
    * 线程那道闸是拦还是放, 而两个方向的代价**不对称** ——
    *   · 多发一代: 最多让续扫在开工前多问一次 (它本来就会问), 无害;
    *   · 漏发一代: 续扫把回零前后的两半坐标**静默拼在一起** —— 而那正是这套机制
    *     存在的全部理由。
    * 所以往保守那边偏: **工作线程在所有它真动过的路径上重新锚定, GUI 每次派发都 +1**。
    * 于是"锚定而不 +1"这个危险方向在结构上不可能出现。
    */
   m_epoch++;
   m_ctl->setZeroEpoch(m_epoch);

   m_thr->postHome(axis, meth, vel);

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
      /* 扫描中途换源 = 同一张图上的数据来自两个不同的东西。禁掉。
       *
       * 回退时**必须挡掉信号**: setCurrentIndex 会再进来一次, 又落到这一句,
       * 又是回退 —— 两个下标之间来回弹, 直接把栈撑爆 */
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

   /* 换了源, 上一格那个数就**不再属于任何东西**了 —— 留着它, 操作员会把它读成新源
    * 的读数 (一个从没读过的源旁边挂着一个数, 是最坏的一种残留)。未决的那个请求同理
    * 要作废: 回话即使来了也不该再往这一格写。 */
   if (m_readTimer != nullptr)
      m_readTimer->stop();
   m_readPending = false;
   if (m_lReadout != nullptr)
      m_lReadout->setText(QStringLiteral("—"));

   /* open() 对真机是**阻塞**的 (枚举 USB → 开设备 → 读探头 → 开流), 上限 12s。
    * 换源本来就不该在扫描中做, 所以这一下卡住不会拖慢任何采集。 */
   QString err;
   const bool ok = m_meter->open(&err);

   /* **必须告诉控制器**, 否则它还在听旧的源 —— 而旧源还活着, 会照常出数 */
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

/*
 * 设备信息回来了 —— 把这四样落到界面上:
 *   1. 真机那三行该不该露出来
 *   2. 三个下拉框的选项表 (设备给的, 原样装进去)
 *   3. 当前选中项
 *   4. 没选上/没这一项的那个框灰掉
 *
 * **只在 infoChanged 时跑, 不放进 30Hz 的 refresh()**: 那个频率下重填下拉框会跟
 * 操作员正在点的那一下抢, 而且每帧重建选项是白烧 CPU。
 */
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

   /* 填的时候挡掉信号 —— 否则每 addItem 一次就当成操作员改了一次配置,
    * 一连串 stop/set/start 打到设备上 */
   m_meterCfgQuiet = true;

   auto fill = [&](QComboBox *cb, QLabel *lb, const QStringList &opts, int cur) {
      cb->clear();
      cb->addItems(opts);
      if (cur >= 0 && cur < cb->count())
         cb->setCurrentIndex(cur);
      /* 探头没有这一项 (手册: options 为空 / index 为 -1) 是**正常**的, 灰掉就是 */
      const bool usable = open && !opts.isEmpty();
      cb->setEnabled(usable);
      lb->setEnabled(usable);
   };
   fill(m_cbWl,    m_lWl,    i.wavelengths, i.wl_index);
   fill(m_cbRange, m_lRange, i.ranges,      i.range_index);
   fill(m_cbMeasMode, m_lMeasMode, i.modes, i.mode_index);

   m_meterCfgQuiet = false;
}

/* 操作员改了波长/量程/模式。**异步** —— 工作线程收到后是 停流 → 改 → 重新开流,
 * 改完再发一次 infoChanged 回来 (见 ophirmeter.h)。这里不阻塞等结果 */
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

/*
 * ---------------------------------------------------------------- 读一次
 *
 * 「接上了」这件事在界面上原来只有一句状态行 (表头/探头型号 + 序列号), 想看到
 * **一个数**必须先开一趟扫描 —— 而一趟扫描是一小时。刚插上表头的时候没人愿意先赌上
 * 一小时才知道探头是不是坏的。
 *
 * 这里就发一次普通请求, 走的是扫描用的同一条路, 所以它通了 = 采集那条路也通了。
 *
 * ── 那条硬闸 ─────────────────────────────────────────────────────────────
 *
 * PowerMeter 的约定 (powermeter.h): **调用方负责保证同一时刻只有一个未决请求**。
 * 扫描跑着的时候"调用方"是 ScanController, 所以这时候按下去就是两个请求撞在同一个源上:
 * 真机那条 (每个采样只认严格更新的时间戳) 会让其中一边白等到超时, 模拟源则会把两个
 * 数分给两边 —— 于是扫描的 CSV 里会**悄悄少一个点或者错一个点**。这种错不会报任何错,
 * 所以只能靠闸门挡。refresh() 里那条 setEnabled 就是闸门, 这里再兜一次底。
 *
 * ── 为什么两个槽里还要比一次 sender() ─────────────────────────────────────
 *
 * 换取样源时那个未决请求会作废 (见 onMeterChanged), 但**旧源的回话可能已经排在事件
 * 队列里**了。正常情况下它先被处理, 那时 m_readPending 还是 false, 于是丢掉 ——
 * 可这靠的是队列的先后顺序。多比一次 `sender() != m_meter` 就把这个赌注去掉了:
 * 只有**当前源**的回话算数。四个源都是本窗口的子对象、一样长寿, 所以 sender() 在这
 * 里是安全的 (不是那种"发送者可能已经死了"的用法)。
 */
void ScanWindow::onReadOnceClicked()
{
   if (m_meter == nullptr || !m_meter->isOpen() || m_ctl->running() || m_readPending)
      return;

   /*
    * 顺序是**有讲究的**: 先把 m_readPending / 起始时刻 / 兜底定时器全都摆好, **最后**才发
    * 请求。
    *
    * 因为源有一个**同步回话**的口子: 没打开时三个模拟实现都是直接 `emit readingFailed`
    * (在 requestReading() 的调用栈里就回来了, 见 powermeter.cpp)。虽然上面那道闸已经
    * 挡掉了"没打开"这一种, 但把顺序倒过来就是**在别人的调用栈里改自己的状态** ——
    * 那类 bug 只有在某个源改成同步回话的那天才炸, 而且炸在别处的代码上。
    */
   m_readPending = true;
   m_readSentMs  = m_clock.elapsed();
   m_lReadout->setText(QStringLiteral("读一次: 读取中…"));
   m_readTimer->start(kReadOnceTimeoutMs);
   refresh();                       /* 立刻把按钮灰掉, 免得连点出两个请求 */

   m_meter->requestReading();
}

void ScanWindow::onReadOnceReady(double watts)
{
   /* 不是我们的那一份 —— 那就是扫描的读数, 一个字都别动 */
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
   /* 失败原文照贴。**这一行是排查时最有用的一句** —— 没插表头 / 过量程 / 流没起来
    * 是三种完全不同的错, 而它们各自的原话只有源那边知道 */
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

   /* 开跑之前把源重开一遍 —— 脚本源要靠这个把游标拨回第一个数。
    *
    * **真机不跟着做**: 它那次 close/open 是 收线程 → 枚举 USB → 开设备 → 读探头
    * → 重新开流, 几百毫秒起步, 而且它本来就没有"游标"要复位 (取数逻辑按设备时间戳
    * 走水位线, 缓冲区里压着的旧数一概不要)。为一件没发生的事每次按开始都去动一次
    * USB, 换来的是"设备偶尔不高兴就开不了扫描" —— 不值。 */
   if (m_meter != m_ophir)
   {
      m_meter->close();
      m_meter->open(nullptr);
   }

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

/*
 * 手动速度只在**没在扫描**的时候推。
 *
 * 扫描中的速度是 ScanController::start() 设的那一个, 这里再推就是中途改速度 ——
 * 而状态机的「每点耗时」估算与「走到位超时」判据都建立在那个速度上 (见
 * scancontroller.cpp 的 startPoint)。扫描一结束/中止, running 变假, 这里就自己
 * 把手动速度推回去 —— **不需要谁来记"该恢复了"**, 那种记账总有一条路径会漏掉。
 *
 * 只在数值真的不一样时才推: 这是 30Hz 调的, 每帧都 setSpeed 是白做功。
 */
void ScanWindow::pushManualSpeed(const BusTelem &t, bool running)
{
   if (!m_connected || running)
      return;

   const uint32_t v = (uint32_t)m_edManSpeed->value();
   for (int i = 0; i < t.naxis && i < EM_MAX_AXES; i++)
      if (t.ax[i].vel != v)
         m_thr->setSpeed(i, v);
}

/*
 * 一根轴的五盏灯 + 状态栏那对。
 *
 * **参数栏与状态栏都从这里出** —— 分开算就会有一天两边说的不一样, 而"两个指示器
 * 互相矛盾"比"少一个指示器"坏得多。撞限位的判定本身更是**只有一处定义**:
 * ecatcmd::limit_hit, 在 EcatThread::publish() 里算好, 这里只读 a.limit_active。
 */
void ScanWindow::refreshAxisSignals(const BusTelem &t)
{
   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      /*
       * "不知道"的判据**跟状态机完全一致** (ScanController 自动中止那一段用的就是
       * `!valid || !mirror_ok`)。判得比状态机宽的话, 会出现最坏的那种不一致:
       * 灯说"正常", 而程序已经因为"丢了过程数据帧"中止了 —— 而且此时 sw 是陈值,
       * 那个"正常"根本不是现在的状态。
       */
      const bool known = m_connected && a.valid && a.mirror_ok;
      const bool lim   = known && a.limit_active;

      /* ---- 「轴信号」那四个 ---- */
      setSignalCell(m_axGrid, i, AX_ENABLED, known, a.enabled, Lamp::Ok,
                    QStringLiteral("已使能"), QStringLiteral("未使能"));
      setSignalCell(m_axGrid, i, AX_FAULT, known, a.fault, Lamp::Bad,
                    QStringLiteral("有故障"), QStringLiteral("无故障"));

      /*
       * ---- 「限位开关」那六个: 三个开关**本身**压着没有 (60FDh) ----
       *
       * known 要**再与 a.dig_known**。少了它, 读不到 60FDh 时那三位是 0, 于是界面会
       * 显示"三个都没压住" —— 而那是个**看起来完全正常**的结论。不知道和"都没压住"
       * 必须分得开, 这就是 AxisTelem::dig_known 单独占一个字段的理由。
       *
       * 六个格子的颜色: 原点用 Ok (绿亮 = 正在压着, 位置信息), 正负限位用 Bad。
       * **绿色在这里不是"没事"** —— 灯亮一律表示"这件事正在发生", 见 kLampRule。
       */
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

         /* 断线/丢帧也是"这一位现在怎么样我们不知道了" —— 挂着的那条横幅同理该走,
          * 否则界面会在没有任何数据的情况下继续断言"正压着" */
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
         /* 「有效」而不是「撞上」—— 这一位是 CiA402 的 "internal limit active",
          * 它**不等于**"物理上已经撞到限位开关" (见下面上升沿那条横幅的说明)。
          * 状态栏这一格短, 只能放一个词, 那就放驱动器自己那句话。 */
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
         paintLamp(lamp, Lamp::Off);   /* **灭, 不是绿** —— 见 paintLamp 上面那段 */
      }

      /* 上升沿弹一次横幅, 与上面故障那条同一个理由: 30Hz 每帧都设一遍会把重绘刷爆。
       * 扫描中 bit11 置起会走自动中止 (那条有模态框), 这一条管的是**平时手动走的时候** */
      if (lim && !m_limShown[i])
      {
         m_limShown[i] = true;

         /*
          * **顺带把当时的 60FDh 打到 stdout。** 30Hz 的面板灯可能一闪而过, 而 stdout
          * 会把证据留下 —— 这是真机上唯一能定"bit11 到底什么时候置起"的东西:
          *
          *   bit11 置起, 而三位全 0  -> 两个视图**不一致**。按手册它们同源, 所以最可能
          *                              是 2310h~2312h 的功能码没配对 (那两位于是恒 0);
          *   bit11 置起, 只有原点位  -> 若原点也让 bit11 置起, 扫描经过原点就会误中止,
          *                              该打开 kRefineLimitWithDigIn (见 ecatworker.h);
          *   bit11 置起, 正/负限位位 -> 自洽: 那一路限位信号确实有效。
          *
          * 注意三行说的都是**信号**, 不是"撞上了" —— 手册对这一位的定义就是
          * 「硬件限位信号有效时置 1」, 它是电平不是闩锁 (见 ecatworker.h 顶部那段)。
          *
          * 这里**不改判定**。判定的开关在 ecatworker.h, 而它必须是一次有证据的改动。
          */
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

         /*
          * 「是什么状态」与「接下来查哪儿」两句都从 ecatcmd 里取 —— 那里是**唯一**
          * 一处定义, 拒绝启扫与自动中止用的也是同一对函数。三处各写各的措辞会在某一天
          * 只改了其中一处 (措辞里为什么不许出现"已撞上", 见 limit_hit_advice 上面那段)。
          *
          * 开场白也是一样: 反转开着时判据不看 bit11 (见 limit_hit_headline), 所以
          * 它**不能**再写"bit11 置起" —— 那会让人去查一个决定不了任何事的位。
          * 上面那行 printf 同理: 记下反转状态, 否则同一行日志在两种配置下看着一模一样。
          */
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

         /*
          * 这一位掉了, 那条红横幅就该跟着走。
          *
          * hint(s, true) 是**不自动消失**的 (理由见 hint() 那段: 无人值守的一趟扫下来,
          * 一闪而过的提示等于没提示) —— 但"不自动消失"对一句**描述当前状态**的话就是
          * 个陷阱: 它说的是"正压着", 而压着的东西可能早松开了, 于是屏幕上留着一句过期
          * 的话。过期的话比没说过更坏 —— 操作员会去处理一个不存在的问题。
          *
          * 只清**还是我们自己那条**的 (比对原文): 中止之类的消息可能已经把它换掉了,
          * 那些不该被抹掉。
          */
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

   /* 只在真变了才动控件。**一个格子两样 (灯 + 字) 是一起变的** ——
    * 文字决定颜色, 所以字没变就说明颜色也没变, 不必再比一遍 */
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

   /* 画布: 扫描中吞掉**手动定位** (查看是只读的, 不归这条管)。
    * **只有画布一处决定吞不吞** —— 窗口再拦一道是兜底, 两处都判就会有一天只改了一处 */
   const bool running = m_ctl->running();
   m_canvas->setManualAllowed(!running);

   pushManualSpeed(t, running);

   /* 扫描中锁住参数与取样源。几何改到一半的扫描是没有意义的 —— 落进 CSV 的
    * (ix,iy) 和实际位置就对不上了 */
   const QList<QWidget *> locked{
      m_edAreaX, m_edAreaY, m_edRes, m_edPpu, m_edSpeed, m_edManSpeed, m_edDwell,
      m_edSettle, m_edSamples, m_cbDir, m_cbMode, m_edCsv, m_cbMeter,
      m_edManualV, m_edRandomN, m_edScript, m_btnScript, m_btnOpen, m_btnDef
   };
   for (QWidget *w : locked)
      w->setEnabled(!running);

   /* 真机那三项**不进 locked 那张表**: 它们的可用性还取决于"这台设备有没有这一项"
    * (探头没有可调量程时选项表是空的, 得灰着)。扫描中一起锁上。 */
   const bool dev_ok = !running && (m_meter == m_ophir) && m_ophir->isOpen();
   m_cbWl->setEnabled(dev_ok && m_cbWl->count() > 0);
   m_cbRange->setEnabled(dev_ok && m_cbRange->count() > 0);
   m_cbMeasMode->setEnabled(dev_ok && m_cbMeasMode->count() > 0);

   refreshAxisSignals(t);

   const bool can_move = m_connected && !running;
   /* 回零期间一切"给目标 / 改坐标 / 改状态"的动作都要停: 总线线程正阻塞在 doHome 里,
    * interpolate() 一帧都不跑, 而且轴此刻按 HM 解释, 607Ah 根本不是目标位置。
    * 投进去的命令也不会丢 —— 它们排着队, 等回零退出来才执行, 那正是"点了没反应" */
   const bool can_home = can_move && t.in_op && !t.homing && !t.resetting;

   m_btnEnable->setEnabled(can_move && !t.homing && !t.ax[0].enabled);
   /* 「停止」在回零中也**必须可按** —— 它现在兼任"立即中止回零" (见 onStopClicked)。
    * 这一条刻意不加 !t.homing: 加了就等于把唯一那根救命绳藏起来 */
   m_btnStop->setEnabled(m_connected);
   /* 失能排在回零后面执行的话, 收尾会**再使能一次** —— 一次"失能"最后以带电告终,
    * 比灰着更坏 */
   m_btnDis->setEnabled(m_connected && !t.homing);
   m_btnCenter->setEnabled(can_move);
   m_btnZero->setEnabled(can_move && !t.homing);

   /*
    * 四个回零按钮: **逐轴**判, 只看这一根。
    * 另一根带不带电、有没有故障, 与"我这一根能不能回零"是两件事 —— 不该为它灰掉。
    *
    * 刻意**不看 t.ax[i].enabled**: 未使能也能回零 (em_home 自己会先失能再使能,
    * 而且它**要求**未使能才能写 6098h)。把"已使能"当成不许回零, 会挡住最常见的那条路。
    *
    * fault / mirror_ok 这两条要在这里再判一次 —— 工作线程那道闸才是权威, 但让按钮
    * 先按不动, 比让人点开模态、读完一屏清单、按了确认才被告知"有故障"要好。
    */
   for (int i = 0; i < 2; i++)
   {
      const bool ok = can_home && t.ax[i].valid && t.ax[i].mirror_ok && !t.ax[i].fault;

      for (int d = 0; d < 2; d++)
      {
         m_btnHome[i][d]->setEnabled(ok);
         /* 回零中把那**正在动的那一根**的按钮改名。不看轴就改名的话, 回 X 的时候
          * Y 的两个按钮也写着"回零中…", 而它们其实只是被灰掉了 */
         const bool mine = t.homing && (t.homing_axis == i);
         m_btnHome[i][d]->setText(
            mine ? QStringLiteral("%1 回零中…")
                      .arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y"))
                 : QStringLiteral("%1 %2回零")
                      .arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y"),
                           QString::fromUtf8(ecatcmd::home_dir_text(d == 1))));
      }
   }

   /*
    * 「回零速度」**不进上面那张 locked 表**, 所以这里不用管它: 它在扫描期间也可改。
    * 它是个**值**不是动作, 灰掉只会让人以为"现在改它有用" —— 同「让 60FDh 进 TxPDO」
    * 与「输入电平反转」那两条的先例。
    */

   /*
    * 回零的横幅: 上升沿起一条, 下降沿**只清我们自己写的那条** (原文比对, 同
    * m_limBanner 那一套) —— 期间工作线程可能已经把 note 换成了别的话, 那不能动。
    * 只在真变了才动控件, 同 refreshAxisSignals 的习惯。
    */
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
         /* hint() 会给非故障的提示挂上 8 秒自尽。**回零不是"事件"是"进行中的状态"** ——
          * 而这句横幅上挂着"按「停止」可立即中止", 是这30秒里唯一写着出路的地方。
          * 让它自己消失, 就等于在最长的那条路(超时)上把话说了一半。 */
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

   /*
    * 故障复位: 没连接 / 扫描中 / 正在复位 -> 不可用。
    *
    * **刻意不看故障灯**。那盏灯是 30Hz 遥测推的, 永远滞后于驱动器; 拿它去灰掉按钮
    * 就是"两个指示器互相矛盾"那一类 —— 灯刚灭而故障还在, 按钮灰着, 操作员只能重启程序。
    * 没故障时按下去是**可证无害**的 (工作线程那条闸会拦住并说明"一个字节都没写"),
    * 所以让它永远可按比猜更安全。
    */
   m_btnFaultRst->setEnabled(m_connected && !running && !t.resetting);
   m_btnFaultRst->setText(t.resetting ? QStringLiteral("正在复位…")
                                      : QStringLiteral("故障复位"));

   /*
    * 「让 60FDh 进 TxPDO」**不跟着 onair 变灰**。
    *
    * 它确实是个连接期参数 (改了不重连不生效), 但从前的做法是 `setEnabled(!onair)` ——
    * 那弄出了一个比"勾了没反应"更坏的困惑: 人正是**在连上之后**看到三个灯全灰, 才想到
    * 要勾它, 而那时候它已经灰了。再加上 busy 那个漏 (见 EcatThread::doConnect 那段),
    * 连接一旦失败过就永远为真, 于是这个框再也勾不上 —— 表现就是"勾不上"。
    *
    * 改成永远可勾, 用"说出来"代替"灰掉": 已经连上了就提示"下次连接才生效"
    * (见 onWantDigInToggled)。勾错了的代价为零 —— em_require_dig_in 只在 em_setup
    * 里被读一次, 当前这一次连接一个字都不会被它改动。
    */
   m_cbWantDigIn->setEnabled(true);

   /*
    * 「输入电平反转 (NPN)」同理**不跟着 onair 变灰**, 而且理由更强 —— 它是个**运行期**
    * 参数, 任何时候勾都立刻生效, 连"下次才生效"这个代价都没有 (见 onDiInvertToggled)。
    * 它是安全相关的, 让它按不了只会把人挡在一个正当的修法外面。
    *
    * 但**不从遥测回灌它的勾选状态** (曾经想加): setDiInvert 是立刻写、publish 是每周期
    * 才拷一次, 界面在中间那一拍回灌就会把它弹回去 —— 表现为"勾上又自己跳开, 下一拍再
    * 跳回来"。而能改这个原子量的**只有这一个勾选框**, 所以它不会和实际生效值分叉,
    * 回灌本身也就没有要修的东西。措辞那边走的是 BusTelem::di_invert (那是真值)。
    */
   m_cbDiInvert->setEnabled(true);

   const bool meter_ok = (m_meter != nullptr) && m_meter->isOpen();
   const bool params_ok = m_ctl->paramsError().isEmpty();
   /* **回零中不能起扫。** 回零把轴留在**使能**, 所以扫描要是半路撞上它, 既不会像
    * "掉使能"那样自动中止, 也等不到插补 —— 状态机会以为到了点, 其实一格没动。
    * 这一条是纵深防御: scancontroller 的 armRun() 里还有一道 (那道才是权威, 因为
    * 「打开 CSV 续扫」那条路也能起扫) */
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
         /* 真机把"到底接的是什么"摆出来: 表头型号/序列号, 探头型号/序列号,
          * 当前波长/量程/模式。**这一行是排查时唯一能证明链路真的通了的东西**,
          * 所以它由工作线程拼 (ophirmeter.cpp 的 buildSummary), 这里原样显示 */
         const OphirInfo i = m_ophir->info();
         if (i.valid && !i.summary.isEmpty())
            s += QStringLiteral(" · ") + i.summary;
      }
      m_lMeter->setText(s);
   }

   /*
    * 「读一次」那条**硬闸** (理由见 onReadOnceClicked 上面那段):
    * 控制器只要不在 Idle, 未决请求就归它, 这时候一个手动请求都不能发。
    *
    * 顺带把两个"按下去也没用"的情形一起挡了: 源没打开 (请求发出去必然失败, 而失败
    * 原文会盖掉上一次那个好读数)、以及已经有一个手动请求在飞。
    *
    * **不去比对 isEnabled() 再决定要不要设** —— 每帧照设。30Hz 重设同一个值是廉价的,
    * 而"只在变化时设"要为它多存一份影子状态, 那种状态正是忘记同步的来源。
    */
   if (m_btnRead != nullptr)
   {
      const bool can = (m_meter != nullptr) && m_meter->isOpen()
                    && !m_ctl->running() && !m_readPending;
      m_btnRead->setEnabled(can);

      /* 为什么按不了, 说清楚 —— 一个灰按钮加一句原因, 比一个能点但注定失败的按钮好。
       * 基础说明始终在下面 (readOnceTip), 按不了的时候把原因摆在前面 */
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
   /*
    * **回零进行中: 先掐掉它。**
    *
    * 这条断开路径是**同步等**的 (下面那个循环, 12 秒), 而回零是这个程序里唯一一个
    * 单次能阻塞到 30 秒的动作 —— 不掐的话, 12 秒一轮空转到底, 回零还在跑,
    * 最后 `QThread` 会在它的 `em_home` 还在泵帧的时候被拆掉。
    *
    * 这一句是**「停止」按钮那条直呼的近亲**, 但不需要另开先例: 走的是同一个
    * requestMotionStop() (它只往一个标志里存 1, 见 ecatworker.h 里那段)。
    *
    * 注意它**必须先于**下面的 postDisconnect: 命令是排队的, 而队列要等回零退出来
    * 才轮到 —— 先投那条命令再掐, 时序上其实一样, 但先掐能少等一个 33ms 的遥测轮询。
    */
   if (m_thr->isRunning() && m_thr->telemetry().homing)
      m_thr->requestMotionStop();

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

   /* 参数与网卡写回 scan.ini。**收尾之后才写** —— 上面那几步 (终止扫描 / 失能 / 还原
    * 映射) 万一要弹"可能仍带电"的模态, 那才是人要看的东西, 不该被一次写文件挡在前面。
    * 顺序上也更保险: 写盘失败只是这次没记住, 不影响收尾 */
   saveSettings();

   e->accept();
}

}   /* namespace scan */
