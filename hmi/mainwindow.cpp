#include "mainwindow.h"

#include "axispanel.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

/* 连接前先摆两个灰面板占位, 真轴数连接之后才知道 */
static const int HMI_PLACEHOLDER_AXES = 2;

static const char *kBannerWarn =
   "QLabel#banner { background:#4a3a12; color:#ffd479; padding:6px; border-radius:4px; }";
static const char *kBannerFault =
   "QLabel#banner { background:#5a1f1f; color:#ffb3b3; padding:6px; border-radius:4px; }";
static const char *kBannerInfo =
   "QLabel#banner { background:#1d3346; color:#a9cfe8; padding:6px; border-radius:4px; }";

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
   setWindowTitle(QStringLiteral("滑台 CSP 定位台 —— YKD2205PE / SOEM"));

   m_thr = new EcatThread(this);

   connect(m_thr, &EcatThread::notify,         this, &MainWindow::onNotify);
   connect(m_thr, &EcatThread::adaptersListed, this, &MainWindow::onAdapters);

   buildUi();

   m_tick = new QTimer(this);
   connect(m_tick, &QTimer::timeout, this, &MainWindow::refresh);
   m_tick->start(33);

   m_bannerTimer = new QTimer(this);
   m_bannerTimer->setSingleShot(true);
   connect(m_bannerTimer, &QTimer::timeout, this, [this]
   {
      /* 只有非故障的提示才自己消失 —— 故障得一直挂着 */
      BusTelem t = m_thr->telemetry();
      if (!t.fault)
         m_banner->setVisible(false);
   });

   m_thr->start();
   m_thr->postListAdapters();

   hint(QStringLiteral("未连接: 界面此时是**只读**的, 一个字节都不往总线上写。"
                       "先选网卡再点「连接」"), false);
}

MainWindow::~MainWindow()
{
   /* 兜底, 别让线程活着跑出 main() */
   if (m_thr->isRunning())
      disconnectAndStop();
}

void MainWindow::buildUi()
{
   QWidget *central = new QWidget(this);

   m_nic = new QComboBox(central);
   m_nic->setMinimumWidth(360);

   m_btnNic = new QPushButton(QStringLiteral("刷新网卡"), central);
   connect(m_btnNic, &QPushButton::clicked, m_thr, &EcatThread::postListAdapters);

   m_btnConn = new QPushButton(QStringLiteral("连接 (进 OP)"), central);
   m_btnConn->setToolTip(QStringLiteral(
      "打开发帧 + 建立过程数据 + 进 OP。**电机仍不带电** —— 使能才会带电。\n"
      "生效映射里主站拥有的那些项会被每周期覆盖 (6040h/6060h/607Ah/...), 收尾时还原。"));
   connect(m_btnConn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);

   m_btnEnable = new QPushButton(QStringLiteral("使能"), central);
   m_btnEnable->setObjectName(QStringLiteral("danger"));
   m_btnEnable->setToolTip(QStringLiteral("切 CSP 模式并使能 —— **这是唯一让电机带电的按钮**"));
   connect(m_btnEnable, &QPushButton::clicked, this, &MainWindow::onEnableClicked);

   m_btnStop = new QPushButton(QStringLiteral("停止"), central);
   m_btnStop->setToolTip(QStringLiteral("目标冻在当前位置, **保持保持力矩**(不卸力)"));
   connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onStopClicked);

   m_btnDis = new QPushButton(QStringLiteral("失能"), central);
   m_btnDis->setToolTip(QStringLiteral("回失能态, 电机释放 (滑台可能因自重下滑)"));
   connect(m_btnDis, &QPushButton::clicked, this, &MainWindow::onDisableClicked);

   m_btnCenter = new QPushButton(QStringLiteral("全部回中"), central);
   m_btnCenter->setToolTip(QStringLiteral("两根轴都走到显示坐标 0"));
   connect(m_btnCenter, &QPushButton::clicked, this, &MainWindow::onCenterAllClicked);

   QHBoxLayout *bar = new QHBoxLayout;
   bar->addWidget(new QLabel(QStringLiteral("网卡"), central));
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

   m_banner = new QLabel(central);
   m_banner->setObjectName(QStringLiteral("banner"));
   m_banner->setWordWrap(true);
   m_banner->setStyleSheet(kBannerInfo);
   m_banner->setVisible(false);

   QLabel *caution = new QLabel(QStringLiteral(
      "范围 ±500000 脉冲 (50000 pul/圈 => ±10 圈), 零点 = 连接时读到的位置。"
      "电机带电后点画布即走 —— 点远处就是一次长距离移动; "
      "使能前确认人在设备旁、手放在物理急停上、行程里没有手和线。"), central);
   caution->setObjectName(QStringLiteral("caution"));
   caution->setWordWrap(true);

   m_axisHost = new QWidget(central);
   m_axisLay  = new QHBoxLayout(m_axisHost);
   m_axisLay->setContentsMargins(0, 0, 0, 0);
   m_axisLay->setSpacing(8);

   QVBoxLayout *v = new QVBoxLayout(central);
   v->addLayout(bar);
   v->addWidget(caution);
   v->addWidget(m_banner);
   v->addWidget(m_axisHost, 1);
   setCentralWidget(central);

   m_lNote = new QLabel(this);
   m_lWkc  = new QLabel(this);
   m_lWkc->setFont(QFont("Consolas"));
   statusBar()->addWidget(m_lNote, 1);
   statusBar()->addPermanentWidget(m_lWkc);

   rebuildPanels(HMI_PLACEHOLDER_AXES);
   setConnected(false);
}

void MainWindow::rebuildPanels(int n)
{
   for (AxisPanel *p : m_panels)
   {
      m_axisLay->removeWidget(p);
      p->deleteLater();
   }
   m_panels.clear();

   for (int i = 0; i < n; i++)
   {
      AxisPanel *p = new AxisPanel(i, m_axisHost);
      connect(p, &AxisPanel::targetRequested, this, &MainWindow::onTarget);
      connect(p, &AxisPanel::speedChanged,    this, &MainWindow::onSpeed);
      connect(p, &AxisPanel::zeroRequested,   this,
              [this](int ax) { m_thr->postZeroHere(ax); });
      connect(p, &AxisPanel::centerRequested, this,
              [this](int ax) { m_thr->postCenter(ax); });

      m_thr->setSpeed(i, p->speed());

      m_axisLay->addWidget(p, 1);
      m_panels.append(p);
   }
}

void MainWindow::setConnected(bool on)
{
   m_connected = on;

   m_nic->setEnabled(!on);
   m_btnNic->setEnabled(!on);
   m_btnConn->setText(on ? QStringLiteral("断开") : QStringLiteral("连接 (进 OP)"));
   m_btnEnable->setEnabled(on);
   m_btnStop->setEnabled(on);
   m_btnDis->setEnabled(on);
   m_btnCenter->setEnabled(on);
}

void MainWindow::hint(const QString &s, bool fault)
{
   m_banner->setText(s);
   m_banner->setStyleSheet(fault ? kBannerFault : kBannerWarn);
   m_banner->setVisible(true);
   if (!fault)
      m_bannerTimer->start(8000);
}

void MainWindow::onConnectClicked()
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
                      "  · 进 OP, **开始每 2ms 发帧**\n\n"
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

void MainWindow::onEnableClicked()
{
   /* 「使能」= CLI 的 --allow-motion。每次都问, 不做持久勾选、不自动使能 */
   QMessageBox box(QMessageBox::Warning,
                   QStringLiteral("使能 —— 电机会带电"),
                   QStringLiteral(
                      "确认:\n"
                      "  · 人已经在设备旁边\n"
                      "  · 手放在物理急停上\n"
                      "  · 滑台行程里没有手、工具、线\n\n"
                      "使能后轴进入 CSP 并带保持力矩。使能那一帧不会动作\n"
                      "(目标被钉在当前位置), 之后点画布才会走。"),
                   QMessageBox::Ok | QMessageBox::Cancel, this);
   box.setDefaultButton(QMessageBox::Cancel);
   if (box.exec() != QMessageBox::Ok)
      return;

   m_thr->postEnable();
}

void MainWindow::onDisableClicked()
{
   m_thr->postDisable();
}

void MainWindow::onStopClicked()
{
   m_thr->postStop();
}

void MainWindow::onCenterAllClicked()
{
   m_thr->postCenterAll();
   hint(QStringLiteral("两根轴都去显示坐标 0 (= 连接时读到的位置)"), false);
}

void MainWindow::onAdapters(const QStringList &names, const QStringList &descs)
{
   QString keep = m_nic->currentData().toString();

   m_nic->clear();
   for (int i = 0; i < names.size(); i++)
      m_nic->addItem(descs.value(i, names[i]), names[i]);   /* 数据是 name, 显示是 desc */

   if (keep.isEmpty())
      return;

   int idx = m_nic->findData(keep);
   if (idx >= 0)
      m_nic->setCurrentIndex(idx);
}

void MainWindow::onNotify(const QString &s)
{
   /* 工作线程已经在控制台打过一份了; 这里上横幅 */
   bool fault = s.contains(QStringLiteral("失败")) || s.contains(QStringLiteral("故障"))
             || s.contains(QStringLiteral("拒绝")) || s.contains(QStringLiteral("带电"));

   hint(s, fault);
}

void MainWindow::onTarget(int axis, int want)
{
   m_thr->setTarget(axis, (int32_t)want);
}

void MainWindow::onSpeed(int axis, uint32_t vel)
{
   m_thr->setSpeed(axis, vel);
}

void MainWindow::refresh()
{
   BusTelem t = m_thr->telemetry();

   /* 按钮形态完全由遥测决定: in_op = 正在发帧, busy = 正在连接或收尾 (中间那段阻塞的
    * SDO / 状态机迁移)。两者任一为真就是"占着总线", 网卡选择与「连接」都锁住。 */
   bool onair = t.in_op || t.busy;
   if (onair != m_connected)
      setConnected(onair);

   /* 轴数变了就重建面板 (连接后 naxis 才有效) */
   if (t.naxis > 0 && t.naxis != m_panels.size())
      rebuildPanels(t.naxis);

   for (int i = 0; i < m_panels.size() && i < t.naxis; i++)
      m_panels[i]->refresh(t.ax[i]);

   if (t.in_op)
   {
      m_lWkc->setText(QStringLiteral("WKC %1 / 期望 %2")
                         .arg(t.wkc).arg(t.expected_wkc));
      /* WKC 偏短 = 有从站没参与过程数据交换 */
      m_lWkc->setStyleSheet(t.wkc >= t.expected_wkc ? "color:#7b8391"
                                                    : "color:#ffb020; font-weight:bold");
   }
   else
   {
      m_lWkc->setText(QString());
      m_lWkc->setStyleSheet(QString());
   }

   m_lNote->setText(t.note);

   /* 故障横幅 **分两拍** —— 与 scan 侧 (ScanWindow::refresh) 同一套, 两处说法不许不一致。
    * 下同第一拍: 故障沿。这一刻 603Fh 还没读回来 (那条 SDO 在工作线程下一圈的圈顶做),
    * 所以只能说"还没读到"; 第二拍: 码到了, 把带码的那一句顶上去 ——
    * 「驱动器故障要给出故障码」要的就是这一拍。 */
   if (t.fault && !m_faultShown)
   {
      m_faultShown = true;
      hint(ecatcmd::fault_banner_text(t) + QStringLiteral("  查清原因再「失能」重来"), true);

      for (int i = 0; i < 2; i++)
         if (t.ax[i].valid && t.ax[i].mirror_ok && t.ax[i].fault)
            m_faultCodeShown[i] = t.ax[i].fault_code;
   }
   else if (!t.fault)
   {
      m_faultShown = false;

      /* 故障没了就忘掉说过哪个码, 否则下一次故障读到的若是同一个码, 第二拍永远不会弹 */
      for (int i = 0; i < 2; i++)
         m_faultCodeShown[i] = HMI_FAULT_CODE_UNREAD;
   }

   if (t.fault)
   {
      for (int i = 0; i < 2; i++)
      {
         const AxisTelem &a = t.ax[i];

         if (!m_connected || !a.valid || !a.mirror_ok || !a.fault)
            continue;
         if (a.fault_code == m_faultCodeShown[i])
            continue;

         m_faultCodeShown[i] = a.fault_code;

         if (a.fault_code != HMI_FAULT_CODE_UNREAD)
            hint(ecatcmd::fault_banner_text(t) + QStringLiteral("  查清原因再「失能」重来"), true);
      }
   }

   /* 收尾时没能确认失能 (CLI 退出码 10 的语义) —— 唯一必须弹模态的事。用 singleShot
    * 挪出当前这一帧再弹, 免得在 30Hz 的槽里嵌套事件循环。 */
   if (m_thr->maybeLive() && !m_warnedLive)
   {
      m_warnedLive = true;
      QTimer::singleShot(0, this, [this] { warnMaybeLive(); });
   }
}

void MainWindow::warnMaybeLive()
{
   QMessageBox::critical(this, QStringLiteral("电机可能仍带电"),
      QStringLiteral(
         "收尾时**未能确认所有轴都失能** —— 与控制台退出码 10 同一语义。\n\n"
         "立即断开驱动器的动力电源, 不要只依赖软件。\n\n"
         "常见原因是收尾过程中总线已经掉了: 那时驱动器还带着力矩, 而我们已经没有\n"
         "通道去撤它。"));
}

void MainWindow::disconnectAndStop()
{
   if (!m_thr->isRunning())
      return;

   m_thr->postDisconnect();

   /* 等它真的收完 —— 条件是**不在发帧且不在忙**, 不是"点过断开" (teardown 是排队执行的,
    * 从投递到开跑有一小段)。每根轴的失能确认与状态机迁移都有超时, 所以给到 12 秒。 */
   for (int i = 0; i < 600; i++)
   {
      BusTelem t = m_thr->telemetry();
      if (!t.in_op && !t.busy)
         break;
      QThread::msleep(20);
   }

   setConnected(false);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
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
