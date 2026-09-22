#include "scanwindow.h"

#include "ophircom.h"

#include "mapcanvas.h"
#include "metercurve.h"
#include "meterlog.h"

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
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
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

/* 横幅浮在画布顶上时离画布左边/上边留的空。贴着边也行, 但画布那圈边框会把它的圆角吃掉。
 * 画布顶上本身已经空出一条带子给它了 (MapCanvas::kBannerBand, 40px) —— 这个数说的是
 * 横幅**在那条带子里**再缩进多少, 两个数合起来才是"HUD 不会被压住"。 */
static const int kBannerInset = 6;

/* 默认 CSV 目录, 相对当前工作目录 (从仓库根敲 ./bin/scan.exe 时就落在 scan_out/) */
static const char *kOutDir = "scan_out";

/* 「读一次」等回话的上限。必须比真机那条腿自己的报错阈值 1800ms 长, 否则它那句诊断
 * 永远来不及发出来, 界面上只剩"没有回应"。 */
static const int kReadOnceTimeoutMs = 6000;

/* 读数的显示格式 (量级从 nW 到 W 那一带, 不固定小数位):
 *   |v| >= 1e-3 -> 'g' 6 位有效数字;  否则 / 0 -> 'e' 4 位有效数字。
 * 只给操作员核对用, 入 CSV 的是 double, 一位没少。
 * **它只格式化数字, 不带单位** —— 单位一律由 unitLabel() 跟着取样源给 (真机可能是 J) */
static QString fmtWatts(double v)
{
   if (std::fabs(v) >= 1e-3)
      return QString::number(v, 'g', 6);
   return QString::number(v, 'e', 4);
}

/* 「读一次」那条固定说明, 只有一份: 按钮装上, refreshMeterPanel() 按不了时把原因拼在它前面 */
static const QString &readOnceTip()
{
   static const QString s = QStringLiteral(
      "向当前取样源请求一次读数, 结果显示在按钮下方。");
   return s;
}

/* ---------------------------------------------------------------- 编辑门控 */

/* 框号。**顺序就是 buildUi 里 addGate 的顺序**, 一块框只在这里出现一次。
 *
 * 「色标 (功率)」也在这张表里, 虽然它是纯显示设置 —— 一度把它摘出去过, 理由是"改它不碰滑台
 * 也不碰总线, 何必先点「编辑」"。结果是: 那一框没有「保存 / 取消」, 屏幕上一块框跟旁边几块
 * 长得不一样, 而且「自动跟随」这个要记进 ini 的模式没有一处该按"保存"。
 *
 * 「功率计」**不在这张表里** (2026-09-22 起): 那一框整块露在外面 —— 一直看得见、一直改得动,
 * 里面没有"参数"要保存 (门控的意思是"这里有一份要记住的设置", 套上去反而是在骗人)。
 * 它的可用性判据因此不走这张表, 走 refreshMeterPanel() —— 仍然只有一处写 (见那儿)。 */
enum GateIdx
{
   GI_PARAM = 0,   /* 扫描参数 */
   GI_HOME,        /* 回零 */
   GI_ADV,         /* 高级选项 */
   GI_SHADE,       /* 色标 (功率) */
   GI_N
};

/* 控件的值 <-> QVariant。按钮没有"值" (它触发动作, 不回滚), 返回无效 QVariant */
static QVariant gateValue(const QWidget *w)
{
   if (const auto *s = qobject_cast<const QSpinBox *>(w))
      return s->value();
   if (const auto *d = qobject_cast<const QDoubleSpinBox *>(w))
      return d->value();
   if (const auto *c = qobject_cast<const QComboBox *>(w))
      return c->currentIndex();
   if (const auto *b = qobject_cast<const QCheckBox *>(w))
      return b->isChecked();
   if (const auto *e = qobject_cast<const QLineEdit *>(w))
      return e->text();
   return QVariant();
}

/* 回滚一个控件。**刻意不挂 QSignalBlocker**: 「改动当场生效」就是靠 valueChanged /
 * currentIndexChanged 把值重新推给控制器与参数, 拦了信号就成了"界面回到旧值, 控制器还拿着
 * 新值"。每 set 一下都会经 markDirty, 所以调用方必须**先**退出编辑态 (见 onGateCancel)。 */
static void gateSetValue(QWidget *w, const QVariant &v)
{
   if (!v.isValid())
      return;
   if (auto *s = qobject_cast<QSpinBox *>(w))
      s->setValue(v.toInt());
   else if (auto *d = qobject_cast<QDoubleSpinBox *>(w))
      d->setValue(v.toDouble());
   else if (auto *c = qobject_cast<QComboBox *>(w))
      c->setCurrentIndex(v.toInt());
   else if (auto *b = qobject_cast<QCheckBox *>(w))
      b->setChecked(v.toBool());
   else if (auto *e = qobject_cast<QLineEdit *>(w))
      e->setText(v.toString());
}

void ScanWindow::addGate(int gi, QGroupBox *box, const QList<GateItem> &items)
{
   if (gi < 0 || gi >= m_gates.size())
      return;

   PanelGate &g = m_gates[gi];
   g.box        = box;
   g.items      = items;
   g.title_base = box->title();
   gateTitle(gi);

   /* 改一下就重算标记 (只影响标题, 不拦「保存」)。在登记处一次性接上, 免得每个槽各记一次
    * —— 漏一个的症状是"改了却没有未保存标记"。
    *
    * QLineEdit 用 textEdited 而不是 textChanged: 只有操作员敲字才算改。其余控件没有
    * "只由用户触发"的版本 —— setValue/setCurrentIndex/setChecked 一样发信号, 而程序自己
    * 也会改它们 (按数据定标 / 真机三项 / CSV 缺省名)。两件事挡住这种假标记: 标记本身是
    * 逐项与快照比对算出来的 (gateDirty), 而程序改过的那一项会先经 gateRebase 把快照跟上。 */

   for (const GateItem &it : g.items)
   {
      if (auto *s = qobject_cast<QSpinBox *>(it.w))
         connect(s, &QSpinBox::valueChanged, this, [this, gi] { gateDirty(gi); });
      else if (auto *d = qobject_cast<QDoubleSpinBox *>(it.w))
         connect(d, &QDoubleSpinBox::valueChanged, this, [this, gi] { gateDirty(gi); });
      else if (auto *c = qobject_cast<QComboBox *>(it.w))
         connect(c, &QComboBox::currentIndexChanged, this, [this, gi] { gateDirty(gi); });
      else if (auto *b = qobject_cast<QCheckBox *>(it.w))
         connect(b, &QCheckBox::toggled, this, [this, gi] { gateDirty(gi); });
      else if (auto *e = qobject_cast<QLineEdit *>(it.w))
         connect(e, &QLineEdit::textEdited, this, [this, gi] { gateDirty(gi); });
   }
}

/* "有没有改动"= 逐项与快照比出来的, 不是一个"改过没有"的布尔标记 —— 程序自己也会改控件值
 * (按数据定标 / 真机三项 / CSV 缺省名), 布尔标记分不出是谁改的, 而且改回原值也不会自己消失。
 * 比对是幂等的: 改回原值, 标记自己落下去。 */
void ScanWindow::gateDirty(int gi)
{
   if (gi < 0 || gi >= m_gates.size())
      return;
   PanelGate &g = m_gates[gi];
   if (!g.gate.editing)     /* 「未保存」是编辑态里的东西, 非编辑态不碰标题 */
      return;

   bool diff = false;
   for (int i = 0; i < g.items.size() && i < g.snapshot.size(); i++)
      if (gateValue(g.items[i].w) != g.snapshot[i])
      {
         diff = true;
         break;
      }

   if (diff)
      editgate::markDirty(&g.gate);
   else
      editgate::undirty(&g.gate);
   gateTitle(gi);
}

/* 框标题 = 原标题 + 标记。「未保存」只在这块框编辑态里出现 —— 别的框看不到别的框的标记 */
void ScanWindow::gateTitle(int gi)
{
   if (gi < 0 || gi >= m_gates.size() || m_gates[gi].box == nullptr)
      return;
   PanelGate &g = m_gates[gi];
   g.box->setTitle(g.title_base + QString::fromUtf8(editgate::titleMark(g.gate)));
}

QWidget *ScanWindow::gateBar(int gi, QWidget *parent)
{
   PanelGate &g = m_gates[gi];
   QWidget *bar = new QWidget(parent);
   QHBoxLayout *h = new QHBoxLayout(bar);
   h->setContentsMargins(0, 0, 0, 0);
   h->setSpacing(6);

   g.btnEdit   = new QPushButton(QStringLiteral("编辑"), bar);
   g.btnSave   = new QPushButton(QStringLiteral("保存"), bar);
   g.btnCancel = new QPushButton(QStringLiteral("取消"), bar);
   for (QPushButton *b : { g.btnEdit, g.btnSave, g.btnCancel })
      b->setFixedHeight(22);


   connect(g.btnEdit,   &QPushButton::clicked, this, [this, gi] { onGateEdit(gi); });
   connect(g.btnSave,   &QPushButton::clicked, this, [this, gi] { onGateSave(gi); });
   connect(g.btnCancel, &QPushButton::clicked, this, [this, gi] { onGateCancel(gi); });

   h->addWidget(g.btnEdit);
   h->addWidget(g.btnSave);
   h->addWidget(g.btnCancel);
   h->addStretch(1);

   /* 可见性只由 refreshEditability 一处改; 这里先摆成"没在编辑"的样子 */
   g.btnSave->setVisible(false);
   g.btnCancel->setVisible(false);
   return bar;
}

void ScanWindow::gateSnapshot(int gi)
{
   PanelGate &g = m_gates[gi];
   g.snapshot.clear();
   for (const GateItem &it : g.items)
      g.snapshot.append(gateValue(it.w));
}

void ScanWindow::gateRollback(int gi)
{
   PanelGate &g = m_gates[gi];
   for (int i = 0; i < g.items.size() && i < g.snapshot.size(); i++)
      gateSetValue(g.items[i].w, g.snapshot[i]);
}

/* 程序自己改了某个成员的值 -> 把快照里那一项跟上, 否则「取消」会把它滚回一份陈值 (而且
 * 滚完还会被程序再改一次, 两边打架)。只动这一个: 整框重拍会把同一框里别处的未保存改动
 * 一起"原谅"掉。快照跟上之后这一项就不算改动了 —— 顺手把标记重算一遍。 */
void ScanWindow::gateRebase(int gi, QWidget *w)
{
   if (gi < 0 || gi >= m_gates.size())
      return;
   PanelGate &g = m_gates[gi];
   if (!g.gate.editing)
      return;
   for (int i = 0; i < g.items.size(); i++)
      if (g.items[i].w == w)
      {
         if (i < g.snapshot.size())
            g.snapshot[i] = gateValue(w);
         gateDirty(gi);
         return;
      }
}

/* 全部参数控件 setEnabled 的**唯一写点**。由 refresh() 每拍调用。
 * enabled = 这块框在编辑态 && 这一项在运行期没被锁住。 */
void ScanWindow::refreshEditability()
{
   const bool running = m_ctl->running();

   for (int gi = 0; gi < m_gates.size(); gi++)
   {
      PanelGate &g = m_gates[gi];
      const bool editing = g.gate.editing;

      for (const GateItem &it : g.items)
      {
         bool ok = editing && !(running && it.lock_running);
         /* 空的下拉框打不开: 真机那三项在设备没报这一项时是空的 */
         if (ok)
            if (const auto *cb = qobject_cast<const QComboBox *>(it.w))
               ok = cb->count() > 0;
         /* 「按数据定标」在自动跟随时是多余的 (它做的事每帧都在做), 灰掉。
          * 判据跟勾选框的状态走而不是"模式", 所以它跟着勾一起变, 不用另外记一份 */
         if (ok && it.need_manual)
            ok = (m_cbShadeAuto == nullptr) || !m_cbShadeAuto->isChecked();
         it.w->setEnabled(ok);
      }

      /* 「保存 / 取消」不在成员表里 (进了编辑态它们反而必须按得动), 所以单独定。
       * 「编辑」在编辑态里藏起来 —— 再点一次没有意义 (重复点 begin 是 no-op) */
      if (g.btnEdit != nullptr)
      {
         g.btnEdit->setVisible(!editing);
         g.btnSave->setVisible(editing);
         g.btnCancel->setVisible(editing);
         g.btnSave->setEnabled(editing);
         g.btnCancel->setEnabled(editing);
      }
   }

   /* 不是参数、但也只能在运行外按的动作按钮 (原先是跟着那张"扫描中锁住"的表走的) */
   if (m_btnOpen != nullptr)
      m_btnOpen->setEnabled(!running);

   /* 「功率计」那一整块框。它不进门控, 判据全在 refreshMeterPanel() 里 —— 从这一处转过去,
    * 于是"可用性只有一处写"这条规矩在这一块上也成立 */
   refreshMeterPanel();
}

/*
 * 「功率计」那一框的**全部判据**: 灰不灰 + 哪几行露出来。只由 refreshEditability() 调
 * (即 30Hz 的 refresh() 末尾), 与其余控件同一条规矩 —— **每拍重算, 不缓存**。
 *
 * 这一框不进门控, 因为它里面没有"参数", 只有一台随时可以接上/断开的仪器; 而它的用处恰恰是
 * "还没连总线, 先把真机选上" —— 套进门控就等于先要点「编辑」, 那正是当初要解决的问题。
 *
 * 三条判据:
 *   ① 连续读数正在跑 -> 取样源与各源参数都锁住 (一条曲线上不能换源), 能按的只有「停止」
 *   ② 扫描正在跑     -> 「开始」**照样能按**, 按下去是「跟随」(自己不发请求, 只画扫描的点)
 *   ③ 真机那一块     -> 选到真机**且它开着**才露; 选项表是空的 (探头没这一项) 也不放开
 */
void ScanWindow::refreshMeterPanel()
{
   const bool running = m_ctl->running();
   const bool rec     = (m_mlog != nullptr && m_mlog->running());
   const bool held    = (m_mlog != nullptr && m_mlog->held());
   const bool open    = (m_meter != nullptr && m_meter->isOpen());
   const int  idx     = (m_cbMeter != nullptr) ? m_cbMeter->currentIndex() : 1;

   /* ---- 哪几行露出来 ---- */
   if (m_manualRow != nullptr)
      m_manualRow->setVisible(idx == 0);
   if (m_randomRow != nullptr)
      m_randomRow->setVisible(idx == 1);
   if (m_scriptRow != nullptr)
      m_scriptRow->setVisible(idx == 2);
   /* 模拟延迟是随机源与脚本源**共用**的一格 (手动源立刻回, 没有这一项), 所以两个下标都露 */
   if (m_simRow != nullptr)
      m_simRow->setVisible(idx == 1 || idx == 2);

   const bool dev = (idx == 3 && m_ophir != nullptr && m_ophir->isOpen());
   if (m_devBox != nullptr)
      m_devBox->setVisible(dev);

   /* ---- 取样源 ---- */
   if (m_cbMeter != nullptr)
   {
      m_cbMeter->setEnabled(!rec);
      m_cbMeter->setToolTip(rec
         ? QStringLiteral("连续读数进行中, 取样源不可更改。")
         : QStringLiteral("扫描与连续读数共用的取样源, 不需要连接总线。\n"
                          "选择真机需已安装 Ophir StarLab。"));
   }

   /* ---- 各源自己那几个参数 ---- */
   if (m_edManualV != nullptr)
      m_edManualV->setEnabled(idx == 0 && !rec);
   if (m_edRandomBase != nullptr)
      m_edRandomBase->setEnabled(idx == 1 && !rec);
   if (m_edRandomN != nullptr)
      m_edRandomN->setEnabled(idx == 1 && !rec);
   if (m_edSimDelay != nullptr)
      m_edSimDelay->setEnabled((idx == 1 || idx == 2) && !rec);
   if (m_edScript != nullptr)
      m_edScript->setEnabled(idx == 2 && !rec);
   if (m_btnScript != nullptr)
      m_btnScript->setEnabled(idx == 2 && !rec);

   /* 真机那三项: 设备开着才可改。空选项表也不放开 (探头没有这一项, 放开就是个假控件) */
   for (QComboBox *cb : { m_cbWl, m_cbRange, m_cbMeasMode })
      if (cb != nullptr)
         cb->setEnabled(dev && cb->count() > 0 && !rec);

   /* ---- 「读一次」 ----
    * 它和扫描、连续读数抢同一个未决请求, 三方仲裁在 refresh() 一处; 这里只是把结论画出来 */
   if (m_btnRead != nullptr)
   {
      const bool can = open && !running && !m_readPending && !rec;
      m_btnRead->setEnabled(can);

      /* 为什么按不了要说清楚 (灰按钮本身不会说话) */
      QString why;
      if (m_meter == nullptr)
         why = QStringLiteral("未选择取样源");
      else if (!open)
         why = QStringLiteral("取样源未打开");
      else if (m_readPending)
         why = QStringLiteral("已有读数请求未完成");
      else if (running)
         why = QStringLiteral("扫描进行中, 请求通道归扫描状态机");
      else if (rec)
         why = QStringLiteral("连续读数进行中");

      m_btnRead->setToolTip(why.isEmpty()
         ? readOnceTip()
         : QStringLiteral("不可用: ") + why + QStringLiteral("\n\n") + readOnceTip());
   }

   /* ---- 连续读数那几个 ---- */
   if (m_edMtrInterval != nullptr)
   {
      /* 跟随时可以改 (那时它一个请求都没发, 改了只是把下一拍往后推) */
      m_edMtrInterval->setEnabled(!rec || held);
      m_edMtrInterval->setToolTip(rec && !held
         ? QStringLiteral("连续读数进行中, 间隔不可更改。")
         : QStringLiteral(
              "两次请求之间的最小间隔。\n"
              "实际间隔 = 本值 + 单次往返耗时; 同一时刻只允许一个未完成请求。"));
   }

   if (m_edMtrAvg != nullptr)
   {
      /* 与「间隔」同一个判据: 跟随时可以改 (那时一个请求都没发) */
      m_edMtrAvg->setEnabled(!rec || held);
      m_edMtrAvg->setToolTip(rec && !held
         ? QStringLiteral("连续读数进行中, 平均次数不可更改。")
         : QStringLiteral(
              "每次采样连续读取的读数个数 (与扫描的「每点采样」相同)。\n"
              "1 = 每次读取一个读数; 增大可降低噪声, 代价是每次采样占用 N 次往返。\n"
              "N 次中任一次失败, 该样本即作废 (ok=false)。\n"
              "这 N 个读数连续请求, 因此「间隔」是两次采样之间的间隔。"));
   }

   if (m_btnMtrStart != nullptr)
   {
      const QString why = !open ? QStringLiteral("取样源未打开")
                              : (m_readPending ? QStringLiteral("「读一次」请求未完成") : QString());
      m_btnMtrStart->setEnabled(!rec && why.isEmpty());
      m_btnMtrStart->setToolTip(why.isEmpty()
         ? (running ? QStringLiteral("扫描进行中: 按下后进入跟随, 曲线显示扫描采到的点, "
                                     "不向功率计发送请求。")
                    : QStringLiteral("按设定间隔开始连续读数。"))
         : QStringLiteral("不可用: ") + why);
   }

   if (m_btnMtrStop != nullptr)
      m_btnMtrStop->setEnabled(rec);
   if (m_btnMtrClear != nullptr)
      m_btnMtrClear->setEnabled(m_mlog != nullptr && m_mlog->count() > 0);
   if (m_btnMtrExport != nullptr)
      m_btnMtrExport->setEnabled(m_mlog != nullptr && !rec && m_mlog->count() > 0);

   /* 输出路径: 正在写文件时锁住 (改了就写进另一个文件, 而界面上还显示着这一个) */
   const bool writing = (m_mlog != nullptr && m_mlog->recording());
   if (m_edMtrCsv != nullptr)
      m_edMtrCsv->setEnabled(!writing);
   if (m_btnMtrCsv != nullptr)
      m_btnMtrCsv->setEnabled(!writing);
}

/*
 * 「功率计」那一框每拍要跟新的字。与可用性分开: 那些归 refreshMeterPanel(), 这里只管显示。
 *
 * 曲线不在这条路上盯着 MeterLog 的信号重画, 而是**跟 30Hz 这一拍一起刷** —— 一条读数最多晚
 * 33ms 出现在屏幕上, 而少接一条信号就少一处"什么时候该重画"的判断。
 */
void ScanWindow::refreshMeterReadout()
{
   /* ---- 状态行 ---- */
   if (m_lMeter != nullptr && m_meter != nullptr)
   {
      QString s = QStringLiteral("%1 · %2")
                     .arg(m_meter->kind())
                     .arg(m_meter->isOpen() ? QStringLiteral("已打开")
                                            : QStringLiteral("未打开"));
      if (m_meter == m_script)
         s += QStringLiteral(" · %1 个值, 游标 %2")
                 .arg(m_script->count()).arg(m_script->cursor());
      else if (m_meter == m_ophir)
      {
         /* 真机把接的是什么摆出来 (表头/探头型号序列号)。由工作线程拼好 (ophirmeter.cpp 的
          * buildSummary), 这里原样显示 */
         const OphirInfo i = m_ophir->info();
         if (i.valid && !i.summary.isEmpty())
            s += QStringLiteral(" · ") + i.summary;
      }
      m_lMeter->setText(s);
   }

   if (m_mlog == nullptr)
      return;

   /* ---- 计数 ---- */
   if (m_lMtrCount != nullptr)
   {
      QString t = QStringLiteral("缓冲 %1 点").arg(m_mlog->count());

      /* 超时那一个是**停着等**的, 不是继续采 (见 meterlog.h): 计数不涨这一点必须在界面上
       * 有个说法, 否则看起来就像程序死了 */
      const bool wedged = m_mlog->timedOut();

      /* 缓冲满了要说出来: 屏幕上那条曲线看着照旧很健康, 只是**最旧的正在被丢掉**, 它已经
       * 不是全部了。要留全的只有一个办法 —— 让 CSV 记着 (那一份不设上限) */
      const bool full = m_mlog->full();
      if (full)
         t += QStringLiteral("  (缓冲已满, 最早的数据正在被丢弃; CSV 记录不受影响)");

      if (m_mlog->running())
         t = wedged ? QStringLiteral("请求无响应, 采集等待中 · ") + t
                    : (m_mlog->held() ? QStringLiteral("跟随扫描中 · ") + t
                                      : QStringLiteral("采集中 · ") + t);
      m_lMtrCount->setText(t);
      m_lMtrCount->setStyleSheet(wedged ? QStringLiteral("color:#ffb020; font-weight:bold;")
                                        : full ? QStringLiteral("color:#ffb020;")
                                               : QStringLiteral("color:#7b8391;"));
   }

   /* ---- 最近一次读数 (大字号) ----
    * 取的是**最后那个样本**, 不是统计里的 last: 一次失败也该让人看见"这一次没数" */
   if (m_lMtrLast != nullptr)
   {
      const QVector<MeterLog::Sample> &v = m_mlog->samples();
      if (v.isEmpty())
      {
         m_lMtrLast->setText(QStringLiteral("—"));
         m_lMtrLast->setStyleSheet(QString());
      }
      else if (v.last().ok)
      {
         /* 单位跟源头走 (unitLabel): 模拟源是 W, 真机是设备自己报的 W/J, 认不出来写
          * 「单位不明」—— 这里**不许**硬写一个 W (powermeter.h 的 unit()) */
         m_lMtrLast->setText(QStringLiteral("%1 %2")
                                .arg(fmtWatts(v.last().watts), unitLabel(m_meter)));
         m_lMtrLast->setStyleSheet(QString());
      }
      else
      {
         m_lMtrLast->setText(QStringLiteral("— 本次无响应"));
         m_lMtrLast->setStyleSheet(QStringLiteral("color:#ff9a9a;"));
      }
   }

   /* ---- 统计 ---- */
   if (m_lMtrStats != nullptr)
   {
      const MeterLog::Stats st = m_mlog->stats();
      m_lMtrStats->setText(st.n == 0
         ? QStringLiteral("暂无可用读数")
         : QStringLiteral("n=%1   最小 %2   最大 %3\n平均 %4   标准差 %5 (%6)")
              .arg(st.n)
              .arg(fmtWatts(st.min), fmtWatts(st.max), fmtWatts(st.mean), fmtWatts(st.sd),
                   unitLabel(m_meter)));

      /* 平均次数是"这些数是怎么来的": 一次采样是几个读数平均出来的, 直接决定曲线上那些点
       * 抖不抖、标准差有多小。= 1 时不写 (那是缺省, 不占地方) */
      const int avg = m_mlog->average();
      if (avg > 1 && st.n > 0)
         m_lMtrStats->setText(m_lMtrStats->text()
            + QStringLiteral("\n每次 %1 个读数取平均 (标准差按各次平均值计算)").arg(avg));
   }

   /* ---- 文件 ---- */
   if (m_lMtrWritten != nullptr)
   {
      m_lMtrWritten->setText(m_mlog->recording()
         ? QStringLiteral("正在写入: %1   (本次 %2 行)")
              .arg(QDir::toNativeSeparators(m_mlog->recordPath())).arg(m_mlog->written())
         : QStringLiteral("未写入文件"));
   }

   /* ---- 曲线 ---- */
   if (m_curve != nullptr)
   {
      m_curve->setSourceName(m_meter != nullptr ? m_meter->kind() : QString());
      /* 单位传给图, **空字符串照传** —— 图那边画一个 "?" 并把理由放进 tooltip,
       * 而不是自己挑一个 W 画上 (见 metercurve.h 的 setUnit) */
      m_curve->setUnit(m_meter != nullptr ? m_meter->unit() : QString());
      m_curve->update();
   }

   /* ---- 色标那两个数是什么单位 ----
    * 色阶画的就是取样源的读数, 所以它的单位跟着源头走: 真机报 J 的时候那两个数是 J 而不是
    * W, 而这一点在控件上原本一个字都没有 (标题写着"功率")。放在这儿是因为这一条每拍都跑 */
   if (m_lShadeUnit != nullptr)
   {
      const QString u = (m_meter != nullptr) ? m_meter->unit() : QString();
      /* 判不出来就说「单位不明」—— 与 powermeter::unitLabel() 同一句话, 不另造一个说法 */
      m_lShadeUnit->setText(u.isEmpty()
         ? QStringLiteral("单位不明 (随取样源)")
         : QStringLiteral("单位: %1 (随取样源)").arg(u));
   }
}

void ScanWindow::onGateEdit(int gi)
{
   if (gi < 0 || gi >= m_gates.size())
      return;

   /* 同一时刻只允许一块框在编辑态。已经在编辑别块框 -> **静默丢弃**它: 不弹框拦人,
   * 只把改动滚回去并在它的标题上留一个「已丢弃」。 */
   for (int k = 0; k < m_gates.size(); k++)
   {
      if (k == gi)
         continue;
      PanelGate &o = m_gates[k];
      if (!o.gate.editing)
         continue;
      const bool had = editgate::drop(&o.gate);
      if (had)
      {
         gateRollback(k);
         /* 说一声但**不弹框**: 改动没了却不吭声是更坏的做法。横幅会自己消失, 而那块框
          * 标题上的「未保存 (已丢弃)」会一直留到它下次进编辑态 */
         hint(QStringLiteral("「%1」的未保存改动已丢弃, 正在编辑「%2」。")
                 .arg(o.title_base, m_gates[gi].title_base),
              false);
      }
      gateTitle(k);
   }

   PanelGate &g = m_gates[gi];
   if (!editgate::begin(&g.gate))
      return;

   gateSnapshot(gi);      /* 快照 = 「取消」要退回的那一份 */
   gateTitle(gi);
   refreshEditability();
}

void ScanWindow::onGateSave(int gi)
{
   if (gi < 0 || gi >= m_gates.size())
      return;
   PanelGate &g = m_gates[gi];
   if (!g.gate.editing)
      return;

   /* 直接走现成那条路就够: 同一时刻最多一块框在编辑态, 别框此刻持有的必然是上次保存的值,
    * 所以重写整份 ini 不丢东西。**静默丢弃因此不是界面上的客气, 是这条正确性的前提。** */
   saveSettings();

   editgate::save(&g.gate);
   gateSnapshot(gi);      /* 刚保存的值就是新的"上次保存" */
   gateTitle(gi);
   hint(QStringLiteral("「%1」已保存至 scan.ini。").arg(g.title_base), false);
   refreshEditability();
}

void ScanWindow::onGateCancel(int gi)
{
   if (gi < 0 || gi >= m_gates.size())
      return;
   PanelGate &g = m_gates[gi];
   if (!g.gate.editing)
      return;

   /* 顺序不能反: 先退出编辑态, 再回灌。回灌的每一个信号都会经 gateDirty, 那时 editing
    * 已经是 false -> 不会把框重新点脏 (反了的话「取消」永远清不干净那个标记) */
   editgate::cancel(&g.gate);
   gateRollback(gi);

   gateTitle(gi);
   hint(QStringLiteral("「%1」已取消, 恢复为上次保存的值并已下发控制器。")
           .arg(g.title_base),
        false);
   refreshEditability();
}

void ScanWindow::refreshAdvWarn()
{
   const char *w = editgate::doubleInvertWarning(m_cbNpnWrite->isChecked(),
                                                 m_cbDiInvert->isChecked());
   m_lAdvWarn->setText(w == nullptr ? QString() : QString::fromUtf8(w));
   m_lAdvWarn->setVisible(w != nullptr);
}

/* 高级选项那三个勾。它们各自是连接期参数 (前两个) 或运行期参数 (第三个):
 * 前两个要重新「连接」才生效, 第三个下一帧就生效 —— 由 hmi/ecatworker 那边决定, 这里只推。 */
void ScanWindow::onAdvToggled()
{
   const bool dig = m_cbWantDigIn->isChecked();
   const bool wr  = m_cbNpnWrite->isChecked();
   const bool sw  = m_cbDiInvert->isChecked();

   m_thr->setWantDigIn(dig);
   m_thr->setNpnWriteDrive(wr);
   m_thr->setDiInvert(sw);

   refreshAdvWarn();

   if (m_connected && (dig != m_advLastWantDig || wr != m_advLastNpnWrite))
      hint(QStringLiteral("已记录。这两项为连接期参数, 下次连接时生效, 本次连接不受影响。"),
           false);

   m_advLastWantDig  = dig;
   m_advLastNpnWrite = wr;
}

/* 运行期那一个: 勾一下立刻生效, 且它整个换掉限位判据 —— 读不到 60FDh 就等于一条判据都没有
 * (限位一律按「有效」中止 -> 扫描永远开不了), 所以要在勾的一瞬间就说。 */
void ScanWindow::onDiInvertToggled(bool on)
{
   m_thr->setDiInvert(on);

   if (!on)
   {
      hint(QStringLiteral("上位机侧取反已关闭, 限位判据改为 6041h bit11。"), false);
      return;
   }

   const BusTelem t = m_thr->telemetry();

   if (!t.connected)
   {
      hint(QStringLiteral("上位机侧取反已开启 (连接后生效), 仅作用于本程序, 驱动器不受影响。"), false);
      return;
   }

   if (!t.ax[0].dig_known)
   {
      hint(QStringLiteral("读不到 60FDh, 取反后无可用判据, 扫描无法启动。请勾选「让 60FDh 进 TxPDO」并重新连接, 或关闭上位机侧取反。"), true);
      return;
   }

   const AxisTelem &a = t.ax[0];
   hint(QStringLiteral("上位机侧取反已开启 (轴0 反相后: 正限位 %1 / 负限位 %2)。仅作用于本程序, 驱动器 bit11 与限位保护不受影响。").arg(a.dig_pos ? QStringLiteral("触发") : QStringLiteral("未触发"), a.dig_neg ? QStringLiteral("触发") : QStringLiteral("未触发")), false);
}

/* ---------------------------------------------------------------- 信号灯 */

/* 信号灯: 一个 12px 的圆点, 四种样子 (ScanWindow::Lamp)。判据只一条: 灯亮 = 这件事正在发生。
 * Unknown 灰 = 不知道; Off 灭 = 没发生; Ok 绿亮 = 使能带电; Bad 红亮 = 出事了 (故障 / 撞限位) */
/* 每一盏灯都带上的那一句 (状态栏那两盏也用它) */
static const char *kLampRule =
   "\n\n指示灯: 亮 = 该状态成立; 灭 = 不成立; 灰 = 未知。";

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

/* 灯 + 字 = 一条信息。装在一起, 免得布局把它们排散了。
 * 只有状态栏那两盏用 (2026-09-21 起「轴信号」那块表每格只有灯, 没有字) */
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

/* 滚轮只滚参数栏 —— **除非按住 Ctrl**。没按 Ctrl 时不吃这一滚, 让它去滚参数栏; 按住 Ctrl
 * 才按 Qt 原来的规矩改值。理由是"一次翻页不能顺手改掉路过的参数" —— 扫描区域算错一格是要
 * 撞限位的。
 *
 * 判据原本是"点过它 (拿着焦点) 就认滚轮"。那个例外本身就是坑: 焦点停在下拉框上时, 那一滚
 * 既改了值又没滚成页面, 于是焦点一直留着, 接着滚接着改。而**一直使能的框只有「功率计」
 * 那一块** (其余几块不在编辑态时控件是禁用的, 禁用控件根本收不到滚轮), 于是误改全落在它
 * 身上。换成 Ctrl 这个一次性、明确的手势: 想用滚轮调值就按住, 不想动它就松开。
 *
 * 事件先落在 spin box 内部那个 QLineEdit 上, 所以过滤器要挂在输入框及其每一个子控件上
 * (见 guard()); 判"这一滚落在哪个输入框上"要从收到事件的控件往上找 —— 收到事件的不是被闸
 * 的那个控件本身 (见 guardedAncestor)。 */
class WheelNeedsCtrl : public QObject
{
public:
   explicit WheelNeedsCtrl(QObject *parent = nullptr) : QObject(parent) {}

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

      if (guardedAncestor(w) == nullptr)
         return false;                 /* 这一滚不落在参数框上 (画布 / 滚动区自己) */

      if (static_cast<QWheelEvent *>(e)->modifiers().testFlag(Qt::ControlModifier))
         return false;                 /* 按住 Ctrl = 明确要用滚轮调这个值 */

      e->ignore();                     /* 没按 Ctrl: 不吃, 让父级 (参数栏滚动区) 去滚 */
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
   /* **本程序这里开一个分叉**(2026-09-22): 断开重连时沿用上次那份零点, 显示坐标跨重连连续。
    * hmi 不开这个开关, 它保持"连接那一刻即零点"。这是两个程序**产品上的差别**, 不是同一件事
    * 的两种实现 —— 理由全在 EcatThread::setKeepOrigin 的注释里。
    * 必须在这里 (构造后立刻、任何连接之前) 调: 它是给 tryInitOrigin 看的旗标。 */
   m_thr->setKeepOrigin(true);
   m_busv = new EcatBusView(m_thr);

   m_manual = new ManualMeter(this);
   m_random = new RandomMeter(this);
   m_script = new ScriptMeter(this);
   m_ophir  = new OphirMeter(this);

   /* 连续读数器。**常驻**: 它是三方仲裁里的一方, 得跟窗口一样长寿 */
   m_mlog = new MeterLog(this);

   /* 它自己的报错上横幅。两种都走这一个信号: 一次读没要回来, 或那份 CSV 写不进去 ——
    * 两种都得让人看见, 却都不该打断采集 (读数还在曲线上, 只是那一份文件不再可信) */
   connect(m_mlog, &MeterLog::failed, this,
           [this](const QString &e) { hint(QStringLiteral("连续读数: ") + e, true); });

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
   m_mlog->setSource(m_meter);          /* 两边永远指着同一个源 (换源时同步, 见 onMeterChanged) */

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
                 hint(QStringLiteral("网卡 %1 不在当前列表中。请重新选择网卡。"
                                     "(记录在 scan.ini 中)").arg(nicShort(want)),
                      false);
           });

   connect(m_ctl, &ScanController::autoAborted, this, &ScanWindow::showFault);
   connect(m_ctl, &ScanController::runFinished,  this, [this](bool complete) {
      if (complete)
         hint(QStringLiteral("扫描完成: %1 点, 数据写入 %2。")
                 .arg(m_ctl->totalPoints()).arg(m_ctl->csvPath()), false);
      else if (m_ctl->state() != ScanController::State::Aborted)
         hint(QStringLiteral("扫描已结束 (未完成)"), false);
      refresh();
   });
   /* 一格落定。同时喂两个看数据的地方: 画布, 和功率计那一框里的「跟随」曲线。
    *
    * 跟那条之所以是白捡的: 这个信号在 m_watts[cell] 写完**之后**才 emit
    * (scancontroller.cpp:666), 所以 cellValue/cellHasValue 读到的必然是这一格刚采到的数。
    * ok=false 的格子 (采失败) 不进曲线 —— 那一格没有数, 补一个 0 出来就是编的。 */
   connect(m_ctl, &ScanController::pointLogged, this, [this](int ix, int iy, bool) {
      m_canvas->update();

      if (m_mlog != nullptr && m_mlog->running() && m_ctl->cellHasValue(ix, iy))
         m_mlog->addFollowSample((int64_t)m_ctl->elapsedMs(), m_ctl->cellValue(ix, iy));
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

   hint(QStringLiteral("未连接。界面为只读状态, 不向总线写入。"), false);
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

   /* 横幅**不进布局**, 它是浮在画布顶上的一层 (位置见 placeBanner)。进了布局就会这样:
    * 它一出现, 下面那根分隔条连同右边的参数栏被整体往下推一行高; 一消失又弹回去 ——
    * 而这条横幅会来回闪 (连上/断开/每次提示), 于是参数栏一直上下跳, 点偏一格就是点到
    * 别的按钮。它本来就只是"说一句话", 不该占体积。 */
   m_banner = new QLabel(central);
   m_banner->setObjectName(QStringLiteral("banner"));
   m_banner->setWordWrap(true);
   m_banner->setStyleSheet(kBannerInfo);
   m_banner->setVisible(false);
   /* 它纯是信息, 点在它身上要能穿过去落到画布上 —— 否则"点了画布没反应"会看起来像卡了 */
   m_banner->setAttribute(Qt::WA_TransparentForMouseEvents, true);

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

   /* 门控表按 gate 号建满, 后面每块框自己往里填 (下标 = .cpp 顶上那几个 GI_) */
   m_gates.resize(GI_N);

   QVBoxLayout *sv = new QVBoxLayout(side);
   sv->setContentsMargins(0, 0, 0, 0);
   sv->setSpacing(8);
   /* 「轴信号」排在最上面 (2026-09-20 起它是"轴信号 + 限位开关"合成的那一块: 五个灯一行,
    * 最右边那格是「故障复位」): 它是状态, 要滚才能看到的状态指示器不算指示器 */
   sv->addWidget(buildAxisPanel());
   /* 第三块:「回零」。它是动作不是参数, 但回零找的就是上面那三盏灯说的那几个开关 */
   sv->addWidget(buildHomePanel());
   /* 「高级选项」**必须**排在 buildParamPanel() 之前: loadSettings() 在它里面被调, 而
    * loadSettings 要把存下来的值写进那三个勾 —— 勾还没建出来就会被读空 */
   QWidget *advPanel = buildAdvPanel();
   /* 「色标」同上, 也是**先建后摆**: applyDefaults() 与 loadSettings() 都在
    * buildParamPanel() 里面调, 两个都要写那个「自动跟随」勾 —— 勾还没建出来就是一次空指针。
    * 摆放位置照旧 (它是"看颜色的", 跟着功率计那一块), 所以建在这儿、加到布局里在后面 */
   QWidget *shadePanel = buildShadePanel();
   sv->addWidget(buildParamPanel());
   sv->addWidget(buildScanPanel());
   sv->addWidget(buildMeterPanel());
   sv->addWidget(shadePanel);
   /* 高级选项摆在最底下: 它是"设好了就别再动"的东西, 平时不该占视线 */
   sv->addWidget(advPanel);
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
   /* 横幅不在这里 (见上面 m_banner 那段): 它浮在画布顶上, 不占这一列的高度 */
   v->addWidget(split, 1);
   setCentralWidget(central);

   /* 分隔条一拖, 画布宽度就变了 —— 横幅那一层的宽度与折行高度都得跟着重算 */
   connect(split, &QSplitter::splitterMoved, this, [this](int, int) { placeBanner(); });
   placeBanner();

   /* 滚轮闸: 名单是找出来的, 不是手写的表 (漏一个只是那个框还在被滚轮改, 不报错)。
    * 挂在最后 —— 这里才保证上面那六组框全都建出来了。
    * QLineEdit (CSV / 脚本路径) 不闸: 滚轮改不了它的文字, 那一滚照旧去滚参数栏。 */
   m_wheelGuard = new WheelNeedsCtrl(this);
   for (QAbstractSpinBox *x : findChildren<QAbstractSpinBox *>())
      m_wheelGuard->guard(x);
   for (QComboBox *x : findChildren<QComboBox *>())
      m_wheelGuard->guard(x);

   /* 参数栏一滚, 把滚动区里那个焦点清掉: 焦点留在一个已经滚出视线的框上时, 上下箭头与打字
    * 会改到一个看不见的框 —— 滚轮那条闸管不到键盘。只清滚动区里面那个 (顶栏那张网卡表
    * 在滚动区外面, 不受影响) */
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
      "6041h bit11: 硬件限位信号有效, 扫描中自动中止。\n"
      "红亮 = 该信号当前有效; 原点开关触发不中止扫描。\n"
      "与「轴信号」栏中 60FDh 的限位灯不一致时, 以本灯为准。")
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

/* 把横幅摆到"画布顶上那一层"去。它不在任何布局里 (见 buildUi 里 m_banner 那段),
 * 所以位置与折行高度都得自己算, 三处要调: 窗口缩放 (resizeEvent)、分隔条拖动
 * (splitterMoved)、换了一句话 (hint 里 setText 之后)。
 *
 * 宽度取**画布**那一栏而不是整窗: 横幅盖住画布顶部是可以的 (它盖的是图, 不是控件),
 * 盖住右边参数栏就不行 —— 那正是要让它别动、别被挡的那一栏。
 * raise() 每次都要: 横幅与分隔条是兄弟控件, 而分隔条是后建的, 默认压在它上面。 */
void ScanWindow::placeBanner()
{
   if (m_banner == nullptr || m_canvas == nullptr || centralWidget() == nullptr
       || !m_banner->isVisible())
      return;

   const int w = m_canvas->width() - 2 * kBannerInset;

   if (w <= 0)
      return;

   /* 折行之后的真实高度: wordWrap 开着时 heightForWidth 才给得出; 直接拿 sizeHint 是按
    * "整句不折行"算的, 长句会只露出第一行 */
   const int h = m_banner->heightForWidth(w);

   /* 横幅是 central 的孩子, 画布是分隔条的孩子 —— 坐标要换算一次, 不能用 m_canvas->x() */
   const QPoint at = m_canvas->mapTo(centralWidget(), QPoint(kBannerInset, kBannerInset));

   m_banner->setGeometry(at.x(), at.y(), w, (h > 0) ? h : m_banner->sizeHint().height());
   m_banner->raise();
}

void ScanWindow::resizeEvent(QResizeEvent *e)
{
   QMainWindow::resizeEvent(e);
   placeBanner();          /* 画布跟着变了宽, 横幅要重算 (它不在布局里, 不会自己跟) */
}

QWidget *ScanWindow::buildTopBar()
{
   QWidget *w = new QWidget(this);

   m_nic = new QComboBox(w);
   m_nic->setMinimumWidth(300);

   m_btnNic = new QPushButton(QStringLiteral("刷新网卡"), w);
   connect(m_btnNic, &QPushButton::clicked, m_thr, &EcatThread::postListAdapters);

   m_btnConn = new QPushButton(QStringLiteral("连接 (进 OP)"), w);
   m_btnConn->setToolTip(QStringLiteral("开始过程数据交换并进入 OP 状态, 电机不上电。\n"
                                        "零点沿用本次运行中已确定的值, 断开重连不重设。"));
   connect(m_btnConn, &QPushButton::clicked, this, &ScanWindow::onConnectClicked);

   m_btnEnable = new QPushButton(QStringLiteral("使能"), w);
   m_btnEnable->setObjectName(QStringLiteral("danger"));
   m_btnEnable->setToolTip(QStringLiteral("切换到 CSP 模式并使能电机。"));
   connect(m_btnEnable, &QPushButton::clicked, this, &ScanWindow::onEnableClicked);

   m_btnStop = new QPushButton(QStringLiteral("停止"), w);
   m_btnStop->setToolTip(QStringLiteral("目标锁定在当前位置并保持力矩。回零期间按下即中止回零。"));
   /* 走槽而不是直连 postStop: 回零期间这个按钮必须换成立即中止 (非回零时两者一字不差) */
   connect(m_btnStop, &QPushButton::clicked, this, &ScanWindow::onStopClicked);

   m_btnDis = new QPushButton(QStringLiteral("失能"), w);
   m_btnDis->setToolTip(QStringLiteral("回到失能状态, 电机释放。滑台可能因自重下滑。"));
   connect(m_btnDis, &QPushButton::clicked, m_thr, &EcatThread::postDisable);

   m_btnCenter = new QPushButton(QStringLiteral("全部回中"), w);
   m_btnCenter->setToolTip(QStringLiteral("两根轴均移动到显示坐标 0。"));
   connect(m_btnCenter, &QPushButton::clicked, this, &ScanWindow::onCenterAllClicked);

   m_btnZero = new QPushButton(QStringLiteral("设为区域中心"), w);
   m_btnZero->setToolTip(QStringLiteral("将当前位置设为显示坐标 0, 即扫描区域中心。\n"
                                        "本次运行内断开重连后仍然有效。"));
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

/* 两组信号的列号。列号 = 本组信号数, 两组相加就是那张表的列数 (SIGN_NCOL) ——
 * 单独写错一次就会凭空多出一个空列, 把前面几列挤成一个字宽。 */
enum { AX_ENABLED = 0, AX_FAULT = 1, AX_COMM = 2, AX_NCOL = 3 };
enum { LIM_HOME = 0, LIM_POS = 1, LIM_NEG = 2, LIM_NCOL = 3 };

/* 那张表一共五列 = 上面两组信号相加。表头、tooltip、格子都照它建满 */
enum { SIGN_NCOL = AX_NCOL + LIM_NCOL };

/* 那一句 kLampRule 定义在本文件顶部 (状态栏那两盏也要用)。 */

/* 「轴信号」: 每根一行, 一行里五盏灯 —— 使能 / 故障 (6041h bit2/bit3) 与 原点 / 正限位 /
 * 负限位 (60FDh bit2/bit1/bit0), 最右边一格是「故障复位」。判据同 kLampRule; 名字写在表头上,
 * 不每格重复。
 *
 * **每格只有灯** (2026-09-21 起, 原先灯旁边还有一行"已使能/未使能"这样的字): 同一件事不必
 * 写两遍 —— 亮/灭就是那句话, 而五列并排时那行字占的宽度反而把灯挤小、整块表看着像一张表
 * 而不是五个信号。要知道某盏灯亮了算什么事, 鼠标停在灯上 (tooltip 里写着它问的是哪个字、
 * 哪一位, 以及绿亮/红亮各是什么意思)。**灰 = 不知道这件事没有别的表达方式了**, 所以灯本身
 * 必须一眼能分出四种样子: 灰(浅底) / 灭(暗底) / 绿亮 / 红亮。
 *
 * 原先「轴信号」与「限位开关」是两个框 (2026-09-20 合成一个): 分开时同一个侧栏里要滚才
 * 看得全, 而它们说的都是"这一根现在什么状态"。**分组没有丢, 只是不再各占一个框**:
 * 表头上前两个是 6041h 的两盏, 后三个是 60FDh 那三个开关; 每一格的 tooltip 里写着它问的是
 * 哪一个字、哪一位。判据也仍然分开算 (见 refreshAxisSignals: 前两格与后三格的 known 不是
 * 同一个条件), 这不是一次"数据合并"。 */
QWidget *ScanWindow::buildAxisPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("轴信号"), this);
   QGridLayout *g = new QGridLayout(box);
   g->setContentsMargins(6, 4, 6, 6);
   g->setHorizontalSpacing(12);
   g->setVerticalSpacing(5);

   /* 表头 与 tooltip 一列一个, 次序就是格子的次序: 使能/故障/原点/正限位/负限位。
    * 每格只有一盏灯, 所以 tooltip 要自己说清"这一列亮/灭各是什么意思" —— 表头只有个名字,
    * 灯只会亮灭, 而"亮"在五列里不是同一件事 (绿亮 = 使能带电, 红亮 = 撞限位) */
   static const char *kHead[SIGN_NCOL] = { "使能", "故障", "通讯", "原点", "正限位", "负限位" };
   static const char *kTip[SIGN_NCOL] = {
      "6041h bit2: 电机已使能。\n"
      "绿亮 = 已使能; 未使能时画布点击不会产生运动。",

      "驱动器自报故障: 6041h bit3, 或 603Fh 返回非 0 故障码。\n"
      "红亮 = 有故障; 扫描中置起将自动中止。\n"
      "故障种类见 603Fh (过流 / 过压 / 欠压 / 动力线 / 通讯 / 传感器等)。",

      "上位机侧的过程数据通讯状态, 与「故障」灯是两件事。\n"
      "红亮 = 连续 10 帧工作计数器 (WKC) 不足, 位置与状态为最后一次完整帧的陈旧值。\n"
      "最长未发帧时间见状态栏。",

      /* 原点灯是绿的, 所以要说出声来 */
      "60FDh bit2: 原点开关 (2310h X0)。\n"
      "绿亮 = 开关触发; 原点触发属位置信息, 回零与扫描经过时不中止。\n"
      "本格为开关本身状态, 是否中止扫描见状态栏限位灯 (6041h bit11)。",

      "60FDh bit1: 正限位开关 (2311h X1)。\n"
      "红亮 = 开关触发。\n"
      "本格为开关本身状态, 是否中止扫描见状态栏限位灯 (6041h bit11)。",

      "60FDh bit0: 负限位开关 (2312h X2)。\n"
      "红亮 = 开关触发。\n"
      "本格为开关本身状态, 是否中止扫描见状态栏限位灯 (6041h bit11)。"
   };

   for (int s = 0; s < SIGN_NCOL; s++)
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

      for (int s = 0; s < SIGN_NCOL; s++)
      {
         /* 前两格归 m_axGrid, 后三格归 m_limGrid —— 两块网格仍在, 只是排在同一张表上。
          * refreshAxisSignals 照旧分两次填, 各用各的 known */
         LampGrid &gr = (s < AX_NCOL) ? m_axGrid : m_limGrid;
         const int c  = (s < AX_NCOL) ? s : (s - AX_NCOL);

         gr.lamp[i][c] = makeLamp(box, QString::fromUtf8(kTip[s])
                                       + QString::fromUtf8(kLampRule));
         /* 靠左排, 不靠网格拉伸: 灯是 12px 定尺, 居中的话五列会各对各不齐 */
         g->addWidget(gr.lamp[i][c], i + 1, s + 1, Qt::AlignLeft | Qt::AlignVCenter);
      }
   }

   /* 「故障复位」在两行最右边那一格, 跨两根轴。它**不按故障灯决定可用性** —— 那盏灯是遥测
    * 推的, 永远滞后于驱动器; 这里只拦界面自己才知道的两件事: 没连接、扫描在跑 (工作线程另
    * 有闸)。它也不是"一根轴一个": 命令是全总线的, 只对 6041h bit3 置起的那几根动手 */
   m_btnFaultRst = new QPushButton(QStringLiteral("故障复位"), box);
   m_btnFaultRst->setToolTip(QStringLiteral("清除驱动器故障位 (6040h=0 卸力, 再置 bit7)。\n仅对 6041h bit3 = 1 的轴执行; 复位后该轴停在未使能状态。"));
   connect(m_btnFaultRst, &QPushButton::clicked, this, &ScanWindow::onFaultResetClicked);
   g->addWidget(m_btnFaultRst, 1, SIGN_NCOL + 1, 2, 1);

   /* 多出来的宽度全给最后一列 (复位按钮那一格): 五盏灯靠左排成一条, 表不会被拉散。
    * 写死会多出一个空列 */
   g->setColumnStretch(SIGN_NCOL + 1, 1);
   return box;
}

/* 「高级选项」: 让三个灯与三个信号"对得上"的那两件事, 再加一个只改本程序判据的退路。
 *
 * 现场那台机器的根子在 2300h: 传感器是 NPN (高电平 = 未触发) 而驱动器配着常开, 于是
 * 60FDh 的 bit1/bit2 恒同时置起 -> 6041h bit11 恒为 1 -> 2204h = 0 (超程停车) 把两个方向
 * 都挡死 -> 回零进得去、一动不动。前两项默认开, 就是照这个来的:
 *   ① 让 60FDh 进 TxPDO —— 三个灯才有得看 (连接时补写 1A00h, 收尾还原, 仅 RAM);
 *   ② 写驱动器 2300h = 0x0007 —— 修根: 驱动器自己的限位保护 / 60FDh / bit11 一次全对;
 *   ③ 上位机侧取反 (默认关) —— 不写驱动器, 只换本程序的判据。②③ 同时开 = 判据恒成立。 */
QWidget *ScanWindow::buildAdvPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("高级选项"), this);
   QVBoxLayout *v = new QVBoxLayout(box);
   v->setContentsMargins(6, 4, 6, 6);
   v->setSpacing(4);

   m_cbWantDigIn = new QCheckBox(QStringLiteral("将 60FDh 加入 TxPDO (连接时写 1A00h)"), box);
   m_cbWantDigIn->setToolTip(QStringLiteral("连接时将 60FDh 追加到 TxPDO, 未追加时三个限位灯不显示。\n仅写 RAM, 断开时还原; 修改后需重新连接。"));

   m_cbNpnWrite = new QCheckBox(QStringLiteral("写入驱动器 2300h = 0x0007 (输入常闭 / NPN)"), box);
   m_cbNpnWrite->setToolTip(QStringLiteral("连接时将各轴 2300h 的 bit0~bit2 写为 1 (常闭), 断开时写回原值。\n仅写 RAM; 修改后需重新连接。"));

   m_cbDiInvert = new QCheckBox(QStringLiteral("上位机侧取反 (仅作用于本程序)"), box);
   m_cbDiInvert->setToolTip(QStringLiteral("对驱动器上报的 X0~X3 取反, 不修改驱动器。\n与「写入驱动器 2300h」同时启用将使判据恒成立, 扫描无法启动。立即生效。"));

   m_lAdvWarn = new QLabel(box);
   m_lAdvWarn->setWordWrap(true);
   m_lAdvWarn->setStyleSheet(QStringLiteral("color:#ff8f8f;"));
   m_lAdvWarn->setVisible(false);

   v->addWidget(gateBar(GI_ADV, box));
   v->addWidget(m_cbWantDigIn);
   v->addWidget(m_cbNpnWrite);
   v->addWidget(m_cbDiInvert);
   v->addWidget(m_lAdvWarn);

   /* 三个都在成员表里 (不含按钮行)。运行中也可以改: 前两个要重连才生效, 第三个立刻生效,
    * 都不影响已经跑起来的那一趟扫描的几何 */
   addGate(GI_ADV, box,
           QList<GateItem>{ GateItem{ m_cbWantDigIn, false, false },
                            GateItem{ m_cbNpnWrite,  false, false },
                            GateItem{ m_cbDiInvert,  false, false } });

   connect(m_cbWantDigIn, &QCheckBox::toggled, this, &ScanWindow::onAdvToggled);
   connect(m_cbNpnWrite,  &QCheckBox::toggled, this, &ScanWindow::onAdvToggled);
   connect(m_cbDiInvert,  &QCheckBox::toggled, this, [this] {
      onAdvToggled();
      onDiInvertToggled(m_cbDiInvert->isChecked());
   });

   return box;
}

/* 八个回零按钮的文字。下标: 0/1 = 找原点开关那两列 (正向 / 反向回零),
 * 2/3 = 找限位开关那两列 (找正限位 / 找负限位)。**建面板与 refresh 都读这一份**: 两处
 * 各写一遍就会出现"按下去了按钮还写着另一个动作"。轴名不在里面 —— 它写在行首那个标签上,
 * 写进按钮文字里这一行就宽得放不下四个。 */
static const char *kHomeBtnText[4] = { "正向回零", "反向回零", "找正限位", "找负限位" };

/* 「原点模式」—— 驱动器自带的正/反向找原点 (6060h = 6)。单开一个框: 它是动作不是参数,
 * 且是本程序里唯一"按下之后滑台会带电自己走"的按钮。回零会先失能再走 (6098h 只能在未
 * 使能时写), 竖直轴在这期间失去保持力矩, 所以"只回我这一根"必须点得出来。
 *
 * 版面 = 一个速度值 + 一个超时值 + 两行按钮 (2026-09-22 加超时)。**一行一根轴, 四个按钮**:
 * 第 0/1 列找原点开关 X0 (方式 24/29), 第 2/3 列找限位开关 (方式 18/17)。八个按钮**共用上面
 * 那两个值** —— 6099h:01/:02 与 609Ah 是同一对参数, 两个速度框会出现"哪个在生效"这种看不出来
 * 的组合; 超时同理, 它压根不是驱动器参数, 是上位机等 6041h bit12 的耐心。
 * 出处只有两处, 都在 tooltip 与顶部那条横幅里 (事前 / 事中), 框里不再写说明文字。 */
QWidget *ScanWindow::buildHomePanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("回零与找限位"), this);
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
   /* 「够不着开关请先把滑台挪近, 不要为了够得着去调高速度」原本写在框里那行小字上, 那行
    * 删了之后搬到这里 —— 它是这个值唯一的一句事后提醒 */
   /* 这个数有两层意思: 表面是速度, 实际是「能找多远」的其中一个因子 (速度 × 超时, 超时是
    * 下面那个框)。够不着开关时该挪滑台 —— 调高速度等于把撞上去的动能一起调高 */
   m_edHomeVel->setToolTip(QStringLiteral(
      "八个回零按钮共用的速度 6099h:01, 返回速度为它的 1/4。\n"
      "单次回零可达距离 = 速度 × 超时。\n"
      "开关超出可达范围时, 请先移动滑台靠近开关。"));

   /* ---- 回零超时 (2026-09-22 加) ----
    * 做成可改是因为原来的常量 30 s 只对缺省速度成立: 速度下限 100 pul/s 时 30 s 只走 0.06 圈。
    * 八个按钮共用。上下限就是 HMI_HOME_TMO_*_S, 工作线程里还夹同一道 (两处夹取必须一致,
    * 自检里有一对断言钉着这件事)。值进 scan.ini 的 ui/home_tmo_s。 */
   m_edHomeTmo = new QSpinBox(box);
   m_edHomeTmo->setRange(HMI_HOME_TMO_MIN_S, HMI_HOME_TMO_MAX_S);
   m_edHomeTmo->setSingleStep(10);
   m_edHomeTmo->setSuffix(QStringLiteral(" s"));
   /* 缺省显式设, 理由同上面那个速度框 (新建的 QSpinBox 是 0, setRange 会把它夹到下限) */
   m_edHomeTmo->setValue(HMI_HOME_TMO_DEF_S);
   /* **tooltip 里一个数字都不写**(除了下限那句): 写了必然会过期 —— 原来那句「速度 × 30 s」
    * 就是这么变成假话的。 */
   m_edHomeTmo->setToolTip(QStringLiteral(
      "八个回零按钮共用的超时: 等待 6041h bit12 的上限。\n"
      "与速度共同决定单次回零的可达距离。\n"
      "该值不是停止条件; 触发硬件限位时运动即停止。"));

   g->addWidget(new QLabel(QStringLiteral("速度"), box), 0, 0);
   g->addWidget(m_edHomeVel, 0, 1, 1, 2);   /* 占 1~2 列 */

   g->addWidget(new QLabel(QStringLiteral("超时"), box), 1, 0);
   g->addWidget(m_edHomeTmo, 1, 1, 1, 2);   /* 与速度同一列, 一眼看出是两个同类的数 */

   /* 门控行 [编辑][保存][取消] 就摆在右边那一格(2026-09-21 改, 原先是框底单独一行)。
    * 这一框里的参数就是速度和超时两个, 那三个按钮管的就是它们俩 —— 摆在两行旁边一眼能
    * 看出"这三个按钮管的是这两个数"; 单独一行时中间隔着两行按钮, 且白占一行高。
    *
    * **靠右**: 传 Qt::AlignRight 让这一格里的 [编辑] 顶着框的右边界 (不传的话它填满
    * 格子、按钮就贴在速度框后面, 右边空一大块)。那一格是 3~4 列, 宽度够同时放两个
    * 按钮 —— 编辑态里 [编辑] 是藏起来的, 可见的永远最多两个, 所以不会挤出去。 */
   g->addWidget(gateBar(GI_HOME, box), 0, 3, 2, 2, Qt::AlignRight);
   addGate(GI_HOME, box,
           /* 速度与超时是参数; 八个按钮是动作, 不进表 (它们不归编辑态管, 归连接态管) */
           QList<GateItem>{ GateItem{ m_edHomeVel, false, false },
                            GateItem{ m_edHomeTmo, false, false } });

   /* 一行一根轴: 行首一个轴名 (同上面「轴信号」那块表的行标签), 右边四个按钮。
    * 轴名放在行首而不是按钮文字里 —— 四个按钮挤在一行, 每个再带个 "X " 就排不下了,
    * 而"这行是哪根轴"正是错点一下的代价最大的那件事。
    * 四个按钮的文字是唯一的事前标识, 所以 tooltip 里把那句话留着: 会带电移动 / 先失能 /
    * 方向对不对只有试一次才知道 */
   for (int i = 0; i < 2; i++)
   {
      QLabel *nm = new QLabel(QStringLiteral("轴%1").arg(i == 0 ? 'X' : 'Y'), box);
      nm->setStyleSheet(QStringLiteral("color:#9aa3ae;"));
      g->addWidget(nm, i + 2, 0);   /* i + 2: 上面有「速度」「超时」两行 */

      for (int d = 0; d < 2; d++)
      {
         const bool neg = (d == 1);

         /* ---- 找原点开关 X0 (方式 24 / 29) ---- */
         {
            const int meth = ecatcmd::home_method_for(neg);

            m_btnHome[i][d] = new QPushButton(
               QString::fromUtf8(kHomeBtnText[d]), box);
            m_btnHome[i][d]->setObjectName(QStringLiteral("danger"));
            m_btnHome[i][d]->setToolTip(QStringLiteral("轴%1: 6098h = %2, 以原点开关 (X0) 为原点, 先向%3高速寻找。\n"
                                                       "该轴由驱动器驱动, 启动前先失能; 按「停止」可立即中止。\n"
                                                       "运动方向未经本程序验证。").arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y")).arg(meth).arg(QString::fromUtf8(ecatcmd::home_dir_text(neg))));

            connect(m_btnHome[i][d], &QPushButton::clicked, this,
                     [this, i, d] { onHomeClicked(i, d, false); });

            g->addWidget(m_btnHome[i][d], i + 2, 1 + d);
         }

         /* ---- 找限位开关 (方式 18 / 17, 手册叫"以限位开关为原点") ----
          * 与上面那两列**共用同一个速度**: 6099h:01/:02 是同一对参数。 */
         {
            const int meth = ecatcmd::home_lim_method_for(neg);

            m_btnLim[i][d] = new QPushButton(
               QString::fromUtf8(kHomeBtnText[2 + d]), box);
            m_btnLim[i][d]->setObjectName(QStringLiteral("danger"));
            m_btnLim[i][d]->setToolTip(QStringLiteral("轴%1: 6098h = %2, 以%3开关为原点。\n"
                                                      "开关未触发时: 先向%4高速寻找, 触发后减速停止并反向低速退出。\n"
                                                      "开关已触发时: 直接向%5低速退出。\n"
                                                      "坐标 0 落在开关的释放点; 该轴启动前先失能, 触发限位属本动作的预期结果。").arg(i == 0 ? QStringLiteral("X") : QStringLiteral("Y")).arg(meth).arg(QString::fromUtf8(ecatcmd::home_method_short(meth))).arg(QString::fromUtf8(ecatcmd::home_method_first_dir(meth, false))).arg(QString::fromUtf8(ecatcmd::home_method_first_dir(meth, true))));

            connect(m_btnLim[i][d], &QPushButton::clicked, this,
                     [this, i, d] { onHomeClicked(i, d, true); });

            g->addWidget(m_btnLim[i][d], i + 2, 3 + d);
         }
      }
   }

   /* 四列按钮等分本行宽度 (行首那列只放轴名, 不参与拉伸) */
   for (int c = 1; c <= 4; c++)
      g->setColumnStretch(c, 1);

   return box;
}

QWidget *ScanWindow::buildParamPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("扫描参数"), this);
   QFormLayout *f = new QFormLayout(box);
   f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
   f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

   /* 区域与分辨率这两个长度量的单位是 **mm** (2026-09-22 起界面上这么写)。
    * 代码内部、以及 CSV 表头里的 `area_x_unit` / `pulses_per_unit` 仍旧叫 "unit" ——
    * 那是**文件格式**, 改名会让已有的 CSV 再也接不上 (见 docs/scan_sweep.md §3) */
   m_edAreaX = new QDoubleSpinBox(box);
   m_edAreaX->setRange(0.1, 500.0);
   m_edAreaX->setDecimals(3);
   m_edAreaX->setSingleStep(1.0);
   m_edAreaX->setSuffix(QStringLiteral(" mm"));
   m_edAreaX->setToolTip(QStringLiteral("扫描区域的 X 边长 (mm), 以原点为中心, 范围 ±(X/2)。"));

   m_edAreaY = new QDoubleSpinBox(box);
   m_edAreaY->setRange(0.1, 500.0);
   m_edAreaY->setDecimals(3);
   m_edAreaY->setSingleStep(1.0);
   m_edAreaY->setSuffix(QStringLiteral(" mm"));

   m_edRes = new QDoubleSpinBox(box);
   m_edRes->setRange(0.001, 50.0);
   m_edRes->setDecimals(3);
   m_edRes->setSingleStep(0.1);
   m_edRes->setSuffix(QStringLiteral(" mm"));
   m_edRes->setToolTip(QStringLiteral("分辨率 (mm); 点数 = (floor(区域 / 分辨率) + 1)²。"));

   m_edPpu = new QDoubleSpinBox(box);
   m_edPpu->setRange(100.0, 1000000.0);
   m_edPpu->setDecimals(0);
   m_edPpu->setSingleStep(1000.0);
   m_edPpu->setSuffix(QStringLiteral(" pul"));
   m_edPpu->setToolTip(QStringLiteral(
      "每 1 mm 对应的 pul (脉冲当量)。\n"
      "缺省 50000 为 2400h 实测值, 即丝杠一圈走 1 mm。\n"
      "导程非 1 mm 的机器必须按实际丝杠修改; 该值错误会导致实际行程与显示不符。"));

   m_edSpeed = new QSpinBox(box);
   m_edSpeed->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_edSpeed->setSingleStep(1000);
   m_edSpeed->setSuffix(QStringLiteral(" pul/s"));
   m_edSpeed->setToolTip(QStringLiteral("扫描速度, 在「开始扫描」时下发一次, 运行中不可更改。"));

   /* 手动速度: 点画布 /「全部回中」时用。扫描中不生效 (那时速度归 ScanController 管),
    * 扫描一结束/中止会自己把手动速度推回去 */
   m_edManSpeed = new QSpinBox(box);
   m_edManSpeed->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_edManSpeed->setSingleStep(1000);
   m_edManSpeed->setSuffix(QStringLiteral(" pul/s"));
   /* 提示里不提「重测选中点」—— 它走扫描状态机 (armRun), 用的是「扫描速度」 */
   m_edManSpeed->setToolTip(QStringLiteral("画布点动与「全部回中」的速度。扫描过程使用「扫描速度」。"));

   m_edDwell = new QSpinBox(box);
   m_edDwell->setRange(0, 60000);
   m_edDwell->setSingleStep(50);
   m_edDwell->setSuffix(QStringLiteral(" ms"));
   m_edDwell->setToolTip(QStringLiteral("到位稳定后, 再停留此时长开始采样。"));

   m_edSettle = new QSpinBox(box);
   m_edSettle->setRange(0, 10000);
   m_edSettle->setSingleStep(20);
   m_edSettle->setSuffix(QStringLiteral(" ms"));
   m_edSettle->setToolTip(QStringLiteral("到位信号需连续成立此时长, 方判定为到位。"));

   m_edSamples = new QSpinBox(box);
   m_edSamples->setRange(1, 100);
   m_edSamples->setToolTip(QStringLiteral("每点连续采样的次数, 取平均值; 采样时间相应增加。"));

   m_cbDir = new QComboBox(box);
   m_cbDir->addItem(QStringLiteral("X 正向 (+X)"));
   m_cbDir->addItem(QStringLiteral("X 负向 (-X)"));

   m_cbMode = new QComboBox(box);
   m_cbMode->addItem(QStringLiteral("蛇形 (逐行往返)"));
   m_cbMode->addItem(QStringLiteral("每行同向") );

   m_edCsv = new QLineEdit(box);
   m_edCsv->setPlaceholderText(QStringLiteral("scan_out/scan_YYYYmmdd_HHMMSS.csv"));
   /* 成员而非局部量: 门控表要按它算可用性 (见 GateItem) */
   m_btnCsv = new QPushButton(QStringLiteral("…"), box);
   m_btnCsv->setFixedWidth(28);
   connect(m_btnCsv, &QPushButton::clicked, this, &ScanWindow::onBrowseCsv);
   QHBoxLayout *csvRow = new QHBoxLayout;
   csvRow->setContentsMargins(0, 0, 0, 0);
   csvRow->addWidget(m_edCsv, 1);
   csvRow->addWidget(m_btnCsv);

   /* 门控行。QFormLayout 有 insertRow, 插到第 0 行 */
   f->insertRow(0, gateBar(GI_PARAM, box));

   f->addRow(QStringLiteral("区域 X"), m_edAreaX);
   f->addRow(QStringLiteral("区域 Y"), m_edAreaY);
   f->addRow(QStringLiteral("分辨率"), m_edRes);
   f->addRow(QStringLiteral("1 mm ="), m_edPpu);
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
      m_btnDef->setToolTip(QStringLiteral("将本组各项恢复为程序缺省值。CSV 输出路径不变。"));
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
    * 像那么回事, 而扫描区域其实只有 0.1 mm。必须在下面那些 connect 之前, 否则每 set 一个
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

   /* 门控成员表 (不含上面那行三个按钮)。
    * lock_running = true 的都是"这一趟怎么走 / 往哪写": 跑到一半改掉, 落进 CSV 的 (ix,iy)
    * 就跟滑台实际站的地方对不上了 —— 那张表是按扫描开始时定的几何算出来的 */
   addGate(GI_PARAM, box,
           QList<GateItem>{
              GateItem{ m_edAreaX,    true,  false },   /* 区域 X */
              GateItem{ m_edAreaY,    true,  false },   /* 区域 Y */
              GateItem{ m_edRes,      true,  false },   /* 分辨率 */
              GateItem{ m_edPpu,      true,  false },   /* 1 mm = N 脉冲 */
              GateItem{ m_edSpeed,    false, false },   /* 扫描速度: 下一次 start 才下发 */
              GateItem{ m_edManSpeed, false, false },   /* 手动速度: 手工对位用, 与这趟无关 */
              GateItem{ m_edDwell,    false, false },
              GateItem{ m_edSettle,   false, false },
              GateItem{ m_edSamples,  false, false },
              GateItem{ m_cbDir,      true,  false },   /* 起始方向: 改的是轨迹 */
              GateItem{ m_cbMode,     true,  false },   /* 扫描方式: 同上 */
              GateItem{ m_edCsv,      true,  false },   /* 输出路径: 跑着的时候换文件没意义 */
              GateItem{ m_btnCsv,     true,  false },
              GateItem{ m_btnDef,     true,  false },   /* 恢复默认: 一按就是几何全变 */
           });

   return box;
}

QWidget *ScanWindow::buildScanPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("扫描控制"), this);

   m_btnStart = new QPushButton(QStringLiteral("开始扫描"), box);
   m_btnStart->setObjectName(QStringLiteral("go"));
   connect(m_btnStart, &QPushButton::clicked, this, &ScanWindow::onStartClicked);

   m_btnPause = new QPushButton(QStringLiteral("暂停"), box);
   m_btnPause->setToolTip(QStringLiteral("立即锁定当前位置并保持力矩。「继续」将重新走完当前点并重新采样。"));
   connect(m_btnPause, &QPushButton::clicked, this, &ScanWindow::onPauseClicked);

   m_btnResume = new QPushButton(QStringLiteral("继续"), box);
   connect(m_btnResume, &QPushButton::clicked, this, &ScanWindow::onResumeRunClicked);

   m_btnAbort = new QPushButton(QStringLiteral("中止"), box);
   m_btnAbort->setObjectName(QStringLiteral("danger"));
   m_btnAbort->setToolTip(QStringLiteral("锁定并结束本轮扫描。已采数据保留在 CSV 中, 之后可续扫。"));
   connect(m_btnAbort, &QPushButton::clicked, this, &ScanWindow::onAbortClicked);

   m_btnRetest = new QPushButton(QStringLiteral("重测选中点"), box);
   m_btnRetest->setToolTip(QStringLiteral("在画布上左键选中一个点后可用。"));

   connect(m_btnRetest, &QPushButton::clicked, this, &ScanWindow::onRetestClicked);

   m_btnOpen = new QPushButton(QStringLiteral("打开 CSV 续扫"), box);
   m_btnOpen->setToolTip(QStringLiteral("读回已有数据, 仅补采未完成的点, 继续追加同一文件。"));
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

/*
 * 参数栏里的「功率计」框 —— **一整个面板, 一直露在外面**, 而且不进门控。
 *
 * 这一块是"单独接一下功率计看看数"要用的东西: 选源 / 真机那三项 / 读一次 / 按间隔连续读数 /
 * 曲线 / 统计 / 存 CSV。它**一个 EtherCAT 帧都不发** (功率计走 Ophir 的 COM 接口), 所以不插
 * 滑台、不连网卡时整块照常能用 —— 这正是当初要它的原因。
 *
 * 两条决定这一块形状的约束 (见 docs/scan_sweep.md §23):
 *
 *  1. **不能有第二个 OphirMeter**。Ophir 表头是独占的 (ophirmeter.cpp:308, 第二个实例打开
 *     同一个头报 0x80040201), 所以真机那一路只有 m_ophir 这一个实例, 这里只是它的界面。
 *  2. **同一时刻只允许一个未决请求** (powermeter.h:28-31)。这一块里有两处会发请求 ——
 *     「读一次」和连续读数 (MeterLog) —— 加上跑扫描的 ScanController 就是三方。谁的请求归谁,
 *     靠 refresh() 一处仲裁, 靠 setHold / m_readPending 两个开关推过来; 灰不灰在
 *     refreshMeterPanel() 一处算。
 *
 * 2026-09-22 曾经把这一整块搬进一个**独立窗口**、靠菜单栏点开, 参数栏只留一行取样源。
 * 当天又搬回来了: 那个窗口必须先点菜单才看得见, 而"外露"是当初提的要求。
 */
QWidget *ScanWindow::buildMeterPanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("功率计"), this);
   QVBoxLayout *v = new QVBoxLayout(box);
   v->setSpacing(6);

   /* ---------------- 取样源 ---------------- */

   /* 下标 0..3 = 四个源, 顺序就是 onMeterChanged 里那个 switch 的分支顺序 ——
    * 错一个就是在换源的时候悄悄换到别的设备上 */
   m_cbMeter = new QComboBox(box);
   m_cbMeter->addItem(m_manual->kind());
   m_cbMeter->addItem(m_random->kind());
   m_cbMeter->addItem(m_script->kind());
   m_cbMeter->addItem(m_ophir->kind());
   m_cbMeter->setCurrentIndex(1);          /* 默认随机源: 一按开始就有数据可看 */
   m_cbMeter->setToolTip(QStringLiteral(
      "扫描与连续读数共用的取样源, 不需要连接总线。\n"
      "选择真机需已安装 Ophir StarLab。"));
   connect(m_cbMeter, &QComboBox::currentIndexChanged, this, &ScanWindow::onMeterChanged);

   /* 真机这一项不可用之前不让人选, 但**照样列出来**: 不列的话操作员会以为这程序没有真机
    * 这条路。灰的是**表里那一项**, 不是整个下拉框 —— 三个模拟源照旧能选。
    *
    * 两种坏法不一样, 说给操作员的话也就不一样 (isRegistered 是"装没装", isAvailable 是
    * "装机的那份 typelib 读不读得出来"), 而第二种在界面上原本只会以"打开失败"的样子出现 */
   {
      QString why;
      if (!OphirCom::isRegistered())
         why = QStringLiteral("未找到 OphirLMMeasurement COM 对象。"
                              "请安装 Ophir StarLab (含 PD300R + Juno+ 驱动)。");
      else if (!OphirCom::isAvailable())
         why = QStringLiteral("COM 对象已注册, 但 typelib 读取失败 "
                              "(0x8002801D TYPE_E_LIBNOTREGISTERED)。"
                              "StarLab 安装损坏, 请重新安装。");

      if (!why.isEmpty())
      {
         auto *m = qobject_cast<QStandardItemModel *>(m_cbMeter->model());
         if (m != nullptr && m->item(3) != nullptr)
         {
            m->item(3)->setEnabled(false);
            m->item(3)->setToolTip(why);
         }
      }
   }

   QHBoxLayout *srcRow = new QHBoxLayout;
   srcRow->setSpacing(6);
   srcRow->addWidget(new QLabel(QStringLiteral("取样源"), box));
   srcRow->addWidget(m_cbMeter, 1);
   v->addLayout(srcRow);

   m_lMeter = new QLabel(box);
   m_lMeter->setWordWrap(true);
   m_lMeter->setStyleSheet(QStringLiteral("color:#7b8391;"));
   /* 这一句里全是设备给的字 (表头/探头型号、序列号), 明写 PlainText: QLabel 默认 AutoText,
    * 字里有个 `<` 就会被当 HTML 解析 */
   m_lMeter->setTextFormat(Qt::PlainText);
   v->addWidget(m_lMeter);

   /* ---------------- 各源自己那几个参数 ----------------
    * 选到谁只露谁那一行。都摊开的话一半的控件永远是灰的, 而这一列本来就不宽 */

   m_manualRow = new QWidget(box);
   {
      QHBoxLayout *h = new QHBoxLayout(m_manualRow);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(6);
      h->addWidget(new QLabel(QStringLiteral("手填值"), m_manualRow));
      m_edManualV = new QDoubleSpinBox(m_manualRow);
      m_edManualV->setRange(-1e9, 1e9);
      m_edManualV->setDecimals(6);
      m_edManualV->setValue(1.0);
      connect(m_edManualV, &QDoubleSpinBox::valueChanged, this, &ScanWindow::onManualValueChanged);
      h->addWidget(m_edManualV, 1);
   }

   /* 随机源的两个旋钮。**初值都从源自己那儿取** (base()/noise()): 缺省只有一份 —— 在这一列里
    * 再写一遍 1.0 / 0.05, 就是两处缺省, 迟早对不上 */
   m_randomRow = new QWidget(box);
   {
      QFormLayout *f = new QFormLayout(m_randomRow);
      f->setContentsMargins(0, 0, 0, 0);

      m_edRandomBase = new QDoubleSpinBox(m_randomRow);
      m_edRandomBase->setRange(-1e9, 1e9);
      m_edRandomBase->setDecimals(6);
      m_edRandomBase->setValue(m_random->base());
      connect(m_edRandomBase, &QDoubleSpinBox::valueChanged, this,
              [this](double x) { m_random->setBase(x); });
      f->addRow(QStringLiteral("基值"), m_edRandomBase);

      m_edRandomN = new QDoubleSpinBox(m_randomRow);
      m_edRandomN->setRange(0.0, 1e6);
      m_edRandomN->setDecimals(4);
      m_edRandomN->setValue(m_random->noise());
      connect(m_edRandomN, &QDoubleSpinBox::valueChanged, this,
              [this](double x) { m_random->setNoise(x); });
      f->addRow(QStringLiteral("噪声"), m_edRandomN);
   }

   m_scriptRow = new QWidget(box);
   {
      QHBoxLayout *h = new QHBoxLayout(m_scriptRow);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(6);
      h->addWidget(new QLabel(QStringLiteral("脚本"), m_scriptRow));
      m_edScript = new QLineEdit(m_scriptRow);
      m_edScript->setPlaceholderText(QStringLiteral("每行一个数值"));
      /* 框里的文本与真正读的那份表是两份东西, 同步点只有 pushScriptPath() 一处。这一框
       * 没有编辑态, 所以敲完立刻推 —— 这里没有"保存"那个时机可用 */
      connect(m_edScript, &QLineEdit::textEdited, this, [this] { pushScriptPath(); });
      h->addWidget(m_edScript, 1);
      m_btnScript = new QPushButton(QStringLiteral("…"), m_scriptRow);
      m_btnScript->setFixedWidth(28);
      connect(m_btnScript, &QPushButton::clicked, this, &ScanWindow::onBrowseScript);
      h->addWidget(m_btnScript);
   }

   /* 模拟往返延迟。真机一次往返可能上百毫秒, 而模拟源默认 20ms —— 不把它调大, 就没法在
    * 没有真机的时候看时序: 「读一次」那个往返时延、连续读数为什么是回话驱动、把延迟调过
    * 看门狗 (kTimeoutMs) 时超时那一路长什么样。两个模拟源共用这一个数 */
   m_simRow = new QWidget(box);
   {
      QHBoxLayout *h = new QHBoxLayout(m_simRow);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(6);
      h->addWidget(new QLabel(QStringLiteral("模拟延迟"), m_simRow));
      m_edSimDelay = new QSpinBox(m_simRow);
      m_edSimDelay->setRange(0, MeterLog::kTimeoutMs * 4);
      m_edSimDelay->setSuffix(QStringLiteral(" ms"));
      m_edSimDelay->setValue(m_random->delayMs());   /* 两个模拟源的缺省是同一个数 */
      m_edSimDelay->setToolTip(QStringLiteral(
         "模拟的往返延迟: 源收到请求后延时此时长返回 (真机往返可达数百毫秒)。\n"
         "用于在无真机条件下验证往返时延与 %1 ms 超时判据。\n"
         "随机源与脚本源共用; 手动源无此项。").arg(MeterLog::kTimeoutMs));
      connect(m_edSimDelay, &QSpinBox::valueChanged, this, [this](int ms) {
         m_random->setDelayMs(ms);
         m_script->setDelayMs(ms);
      });
      h->addWidget(m_edSimDelay, 1);
   }
   v->addWidget(m_simRow);

   v->addWidget(m_manualRow);
   v->addWidget(m_randomRow);
   v->addWidget(m_scriptRow);

   /* ---------------- 真机那一块 ----------------
    * 选项表由设备给, 一个都不写死 (探头不同, 能选的波长与量程就不同)。三行连着小标签一起
    * 收进 m_devBox, 切到模拟源时整块 setVisible(false) —— 露不露归 refreshMeterPanel() */

   m_devBox = new QWidget(box);
   {
      QFormLayout *f = new QFormLayout(m_devBox);
      f->setContentsMargins(0, 0, 0, 0);

      auto addDevRow = [&](const QString &name, QComboBox **cb) {
         *cb = new QComboBox(m_devBox);
         (*cb)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
         /* 不在这里 setEnabled(false): 灰不灰归 refreshMeterPanel() 一处管 (它每拍重算) */
         connect(*cb, &QComboBox::currentIndexChanged, this, &ScanWindow::onMeterCfgChanged);
         f->addRow(name, *cb);
      };
      addDevRow(QStringLiteral("波长"), &m_cbWl);
      addDevRow(QStringLiteral("量程"), &m_cbRange);
      addDevRow(QStringLiteral("模式"), &m_cbMeasMode);

      m_lDevInfo = new QLabel(m_devBox);
      m_lDevInfo->setWordWrap(true);
      m_lDevInfo->setTextFormat(Qt::PlainText);   /* 同样是设备给的字 */
      m_lDevInfo->setStyleSheet(QStringLiteral("color:#7b8391;"));
      f->addRow(m_lDevInfo);
   }
   v->addWidget(m_devBox);

   /* ---------------- 读一次 ---------------- */

   m_btnRead = new QPushButton(QStringLiteral("读一次"), box);
   m_btnRead->setToolTip(readOnceTip());
   connect(m_btnRead, &QPushButton::clicked, this, &ScanWindow::onReadOnceClicked);
   v->addWidget(m_btnRead);

   m_lReadout = new QLabel(box);
   m_lReadout->setWordWrap(true);
   m_lReadout->setStyleSheet(QStringLiteral("color:#7b8391;"));
   /* 这一格会显示设备来的字 (探头报的过量程原因之类)。明写 PlainText: QLabel 默认 AutoText,
    * 字里有个 `<` 就会被当 HTML 解析 */
   m_lReadout->setTextFormat(Qt::PlainText);
   m_lReadout->setText(QStringLiteral("—"));
   v->addWidget(m_lReadout);

   /* 「读一次」的兜底。接口约定"恰好回一次"是源那边的义务, 源不回话时这个动作会永远卡住
    * —— 而它卡住会连带把连续读数的「开始」压死 (三方仲裁里"手动读在飞"是让位条件之一),
    * 所以兜底必须留着 */
   m_readTimer = new QTimer(this);
   m_readTimer->setSingleShot(true);
   connect(m_readTimer, &QTimer::timeout, this, [this] {
      if (!m_readPending)
         return;
      m_readPending = false;
      const double took = (double)(m_clock.elapsed() - m_readSentMs);
      m_lReadout->setText(QStringLiteral("读一次: 无响应 (已等待 %1 s), "
                                         "取样源未返回数据。")
                             .arg(took / 1000.0, 0, 'f', 1));
      refresh();
   });

   /* ---------------- 连续读数 ---------------- */

   auto *sep = new QFrame(box);
   sep->setFrameShape(QFrame::HLine);
   sep->setFrameShadow(QFrame::Sunken);
   v->addWidget(sep);

   QHBoxLayout *ivRow = new QHBoxLayout;
   ivRow->setSpacing(6);
   ivRow->addWidget(new QLabel(QStringLiteral("间隔"), box));

   m_edMtrInterval = new QSpinBox(box);
   m_edMtrInterval->setRange(MeterLog::kMinIntervalMs, MeterLog::kMaxIntervalMs);
   m_edMtrInterval->setValue(MeterLog::kDefaultIntervalMs);
   m_edMtrInterval->setSuffix(QStringLiteral(" ms"));
   m_edMtrInterval->setToolTip(QStringLiteral(
      "两次请求之间的最小间隔。\n"
      "实际间隔 = 本值 + 单次往返耗时; 同一时刻只允许一个未完成请求。"));
   connect(m_edMtrInterval, &QSpinBox::valueChanged, this, &ScanWindow::onMtrIntervalChanged);
   ivRow->addWidget(m_edMtrInterval, 1);

   m_btnMtrStart = new QPushButton(QStringLiteral("开始"), box);
   m_btnMtrStart->setObjectName(QStringLiteral("go"));
   connect(m_btnMtrStart, &QPushButton::clicked, this, &ScanWindow::onMtrStartClicked);
   ivRow->addWidget(m_btnMtrStart);

   m_btnMtrStop = new QPushButton(QStringLiteral("停止"), box);
   connect(m_btnMtrStop, &QPushButton::clicked, this, &ScanWindow::onMtrStopClicked);
   ivRow->addWidget(m_btnMtrStop);

   m_btnMtrClear = new QPushButton(QStringLiteral("清空"), box);
   m_btnMtrClear->setToolTip(QStringLiteral("仅清除曲线与统计; 已写入 CSV 的数据不变。"));
   connect(m_btnMtrClear, &QPushButton::clicked, this, &ScanWindow::onMtrClearClicked);
   ivRow->addWidget(m_btnMtrClear);

   v->addLayout(ivRow);

   /* 一次采样平均几个读数。与扫描参数里那个「每点采样」是同一件事 —— 那边少了它, 噪声大的
    * 源就没法读稳; 这里少了它, 同一条曲线上扫描那趟与手采那趟的噪声水平就对不上 */
   QHBoxLayout *avgRow = new QHBoxLayout;
   avgRow->setSpacing(6);
   avgRow->addWidget(new QLabel(QStringLiteral("每次平均"), box));
   m_edMtrAvg = new QSpinBox(box);
   m_edMtrAvg->setRange(1, MeterLog::kMaxAverage);
   m_edMtrAvg->setValue(1);
   m_edMtrAvg->setSuffix(QStringLiteral(" 次"));
   m_edMtrAvg->setToolTip(QStringLiteral(
      "每次采样连续读取的读数个数 (与扫描的「每点采样」相同)。\n"
      "1 = 每次读取一个读数; 增大可降低噪声, 代价是每次采样占用 N 次往返。\n"
      "N 次中任一次失败, 该样本即作废 (ok=false)。\n"
      "这 N 个读数连续请求, 因此「间隔」是两次采样之间的间隔。"));
   connect(m_edMtrAvg, &QSpinBox::valueChanged, this, &ScanWindow::onMtrAvgChanged);
   avgRow->addWidget(m_edMtrAvg, 1);
   v->addLayout(avgRow);

   m_lMtrCount = new QLabel(box);
   m_lMtrCount->setStyleSheet(QStringLiteral("color:#7b8391;"));
   v->addWidget(m_lMtrCount);

   m_lMtrLast = new QLabel(box);
   m_lMtrLast->setTextFormat(Qt::PlainText);
   {
      QFont big = m_lMtrLast->font();
      big.setPointSizeF(big.pointSizeF() + 4.0);
      m_lMtrLast->setFont(big);
   }
   v->addWidget(m_lMtrLast);

   m_lMtrStats = new QLabel(box);
   m_lMtrStats->setWordWrap(true);
   m_lMtrStats->setTextFormat(Qt::PlainText);
   m_lMtrStats->setStyleSheet(QStringLiteral("color:#7b8391;"));
   v->addWidget(m_lMtrStats);

   m_curve = new MeterCurve(box);
   m_curve->setLog(m_mlog);
   m_curve->setPlaceholder(QStringLiteral("按「开始」后显示曲线"));
   v->addWidget(m_curve);

   /* 输出文件。与扫描那份 CSV 同一个目录 (scan_out), 一眼能看出是同一条产线的东西 */
   QHBoxLayout *csvRow = new QHBoxLayout;
   csvRow->setSpacing(6);
   csvRow->addWidget(new QLabel(QStringLiteral("CSV"), box));
   m_edMtrCsv = new QLineEdit(box);
   csvRow->addWidget(m_edMtrCsv, 1);
   m_btnMtrCsv = new QPushButton(QStringLiteral("…"), box);
   m_btnMtrCsv->setFixedWidth(28);
   connect(m_btnMtrCsv, &QPushButton::clicked, this, &ScanWindow::onMtrBrowseCsv);
   csvRow->addWidget(m_btnMtrCsv);
   v->addLayout(csvRow);

   m_btnMtrExport = new QPushButton(QStringLiteral("导出当前缓冲"), box);
   m_btnMtrExport->setToolTip(QStringLiteral(
      "将当前缓冲中的点导出为新文件。\n"
      "与「开始」写入的文件无关 (该文件为边采边写)。"));
   connect(m_btnMtrExport, &QPushButton::clicked, this, &ScanWindow::onMtrExportClicked);
   v->addWidget(m_btnMtrExport);

   m_lMtrWritten = new QLabel(box);
   m_lMtrWritten->setWordWrap(true);
   m_lMtrWritten->setStyleSheet(QStringLiteral("color:#7b8391;"));
   v->addWidget(m_lMtrWritten);

   QLabel *note = new QLabel(
      QStringLiteral("「开始」按上方路径写入: 文件为空或不存在时写入表头, 已有内容则追加, "
                     "不覆盖。文件名留空时按时间戳自动生成。"),
      box);
   note->setWordWrap(true);
   note->setStyleSheet(QStringLiteral("color:#5f6875;"));
   v->addWidget(note);

   /* 这两个数**记进 scan.ini** (间隔与输出路径是"这台机器怎么采", 不是"上趟数据的范围")。
    * 读在这里而不是 loadSettings() 里: 那一句在 buildParamPanel() 里就调了, 比这一框建得早
    * (见 buildUi 顶上那段注释里的先后次序) */
   {
      const Prefs pf = prefsLoad(prefsPath());
      m_edMtrInterval->setValue(pf.meter_interval_ms);   /* 越界的值控件自己夹回量程内 */
      m_edMtrCsv->setText(QDir::toNativeSeparators(pf.meter_csv));

      /* 用**控件里**的值下推, 不用 ini 里那个: 上面那一句可能刚夹过 (手改坏的 ini 不该让
       * 采集用一个没验过的节奏) */
      if (m_mlog != nullptr)
         m_mlog->setInterval(m_edMtrInterval->value());
   }

   m_manualRow->setVisible(false);      /* 一开始选的是随机源。之后每拍由 refreshMeterPanel 定 */
   m_randomRow->setVisible(true);
   m_scriptRow->setVisible(false);
   m_devBox->setVisible(false);

   onMeterInfoChanged();      /* 一开始选的是模拟源 -> 真机那三行不用填 */
   return box;
}

QWidget *ScanWindow::buildShadePanel()
{
   QGroupBox *box = new QGroupBox(QStringLiteral("色标"), this);
   QFormLayout *f = new QFormLayout(box);

   const double lo = m_canvas->shadeLo();
   const double hi = m_canvas->shadeHi();

   /* 下限就是 0: 功率没有负的。**下限也钉在 0**, 于是"最小 >= 0"这条不用在槽里再判一次 ——
    * 敲 -5 会被控件自己夹成 0, valueChanged 拿到的一直是合法值 */
   const QString kShadeTip = QStringLiteral(
      "色标上下限, 单位随取样源。\n"
      "任一端越过另一端时另一端随之移动, 跨度保持不变; 取值范围为非负数, 且最小 < 最大。");

   m_edShadeLo = new QDoubleSpinBox(box);
   m_edShadeLo->setRange(0.0, 1e12);
   m_edShadeLo->setDecimals(6);
   m_edShadeLo->setValue(lo);
   m_edShadeLo->setToolTip(kShadeTip);

   m_edShadeHi = new QDoubleSpinBox(box);
   m_edShadeHi->setRange(0.0, 1e12);
   m_edShadeHi->setDecimals(6);
   m_edShadeHi->setValue(hi);
   m_edShadeHi->setToolTip(kShadeTip);

   connect(m_edShadeLo, &QDoubleSpinBox::valueChanged, this, &ScanWindow::onShadeLoChanged);
   connect(m_edShadeHi, &QDoubleSpinBox::valueChanged, this, &ScanWindow::onShadeHiChanged);

   m_btnFit = new QPushButton(QStringLiteral("按数据定标"), box);
   m_btnFit->setToolTip(QStringLiteral("以已采数据的最小值 / 最大值作为色标上下限。仅在按下时执行一次。"));
   connect(m_btnFit, &QPushButton::clicked, this, [this] {
      if (!m_canvas->fitShadeToData())
      {
         hint(QStringLiteral("尚无已采数据, 无法定标。"), false);
         return;
      }
      syncShadeEdits();
   });

   m_cbShadeAuto = new QCheckBox(QStringLiteral("自动跟随数据"), box);
   m_cbShadeAuto->setToolTip(QStringLiteral(
      "色标上下限每帧跟随已采数据的最小值 / 最大值。\n"
      "开启时图上的颜色变化部分来自色标自身的变化, 对比多张图时应关闭。\n"
      "本项记录到 scan.ini。"));
   connect(m_cbShadeAuto, &QCheckBox::toggled, this, [this](bool on) {
      m_canvas->setAutoFit(on);
      applyShadeAutoUi(on);
   });

   /* 两套说明按模式显隐。合成一句做不到: 两边的取舍正好相反 (锁定那句说"不跟着变"是优点,
    * 自动那句得承认它变) */
   m_lblLocked = new QLabel(QStringLiteral(
      "色标锁定, 上下限固定, 不随数据变化。\n"
      "颜色仅表示读数在固定区间内的位置。\n"
      "超出范围的格子按端点着色, 数值仍可在悬停提示中读取。"), box);
   m_lblAuto = new QLabel(QStringLiteral(
      "色标跟随数据: 上下限 = 已采数据的最小值 / 最大值, 每次出现新极值时重算。\n"
      "开启时上方两个数为只读显示。\n"
      "超出范围的格子按端点着色, 数值仍可在悬停提示中读取。"), box);
   for (QLabel *l : { m_lblLocked, m_lblAuto })
   {
      l->setWordWrap(true);
      l->setStyleSheet(QStringLiteral("color:#6b7480;"));
      l->setVisible(l == m_lblLocked);
   }

   f->insertRow(0, gateBar(GI_SHADE, box));

   f->addRow(QStringLiteral("最小"), m_edShadeLo);
   f->addRow(QStringLiteral("最大"), m_edShadeHi);

   /* 单位一行。色阶画的是取样源的读数, 而"色标 (功率)"这个名字里就写着一个单位 —— 这一行
    * 是真正算数的那个 (每拍由 refreshMeterReadout 跟着源改) */
   m_lShadeUnit = new QLabel(box);
   m_lShadeUnit->setWordWrap(true);
   m_lShadeUnit->setTextFormat(Qt::PlainText);
   m_lShadeUnit->setStyleSheet(QStringLiteral("color:#7b8391;"));
   f->addRow(m_lShadeUnit);

   f->addRow(m_cbShadeAuto);
   f->addRow(m_btnFit);
   f->addRow(m_lblLocked);
   f->addRow(m_lblAuto);

   /* 这一框没有"运行中锁住"的项 (改色阶不会把跑起来的那一趟弄歪), 也没有要等设备的项 */
   addGate(GI_SHADE, box,
           QList<GateItem>{
              GateItem{ m_edShadeLo,   false, false },
              GateItem{ m_edShadeHi,   false, false },
              GateItem{ m_cbShadeAuto, false, false },
              /* 定标按钮在自动跟随时灰掉 (need_manual), 这个勾本身**不能灰**: 关掉它得按得动 */
              GateItem{ m_btnFit,      false, true },
           });
   return box;
}

/* 「最小」「最大」是一对, 任何时候都得 min < max (否则色带没法定标: 色阶是拿这两个数做线性
 * 映射的, 反着的区间除零)。规则是**推着走**: 被改的那一头越过另一头, 就把另一头一起推过去,
 * 跨度保持不变 —— 想把整段量程往上挪 (0..1 → 5..6) 时最顺手。夹住改的那一头 (把最小按住不让
 * 超过最大) 也合法, 但那样想挪量程得先改另一个, 绕一圈, 而且敲进去的数会被改掉, 看着像吞键。
 *
 * 推的那一下**屏蔽信号**: 不屏蔽的话被推的那个框会再发一次 valueChanged, 它拿到的一半是自己
 * 刚被写的新值、一半是这边还没落定的值, 两拍里会闪一次错的量程。
 *
 * 跨度取的是"上一次生效的那一份" (画布上还留着的那对值, 这两个槽是唯一的写入方)。两个框的
 * 读数与画布的色阶一直是一致的, 所以它就是要保留的跨度。
 *
 * **这两个槽也会被回灌走到**: 「取消」与"点别块框的编辑、这一框的改动被丢弃"都是把快照写回
 * 控件, 而不挂信号屏蔽 (回灌要顺带重新下推)。那两处写回的是一对合法值, 但如果中途经过一次
 * "先写 lo、此时 hi 还是新值" 的错配, 就会白推一下 —— 值错不了 (紧接着那一项就把 hi 写回),
 * 只是会弹一条**根本不是操作员干的**横幅。所以横幅只在编辑态里发: 那两个框平时是禁用的,
 * 编辑态 != 操作员在敲。 */
void ScanWindow::onShadeLoChanged(double lo)
{
   double hi = m_edShadeHi->value();
   if (hi <= lo)
   {
      const double span = m_canvas->shadeHi() - m_canvas->shadeLo();
      hi = lo + ((span > 0.0) ? span : 1.0);
      QSignalBlocker b(m_edShadeHi);
      m_edShadeHi->setValue(hi);
      if (m_gates[GI_SHADE].gate.editing)
         hint(QStringLiteral("最小值超过最大值, 最大值已调整至 %1 (跨度不变)。")
                 .arg(QString::number(hi, 'g', 6)), false);
   }
   m_canvas->setShadeRange(lo, hi);
}

void ScanWindow::onShadeHiChanged(double hi)
{
   double lo = m_edShadeLo->value();
   if (hi <= lo)
   {
      const double span = m_canvas->shadeHi() - m_canvas->shadeLo();
      lo = hi - ((span > 0.0) ? span : 1.0);
      /* 下限非负, 0 就是地板。压到 0 还不够 (hi 自己也在 0 附近) 就只好掉头把 hi 抬回去:
       * 色阶可以很窄, 但**不能反过来**, 那是一条死规矩 */
      if (lo < 0.0)
         lo = 0.0;
      if (lo >= hi)
         hi = lo + ((span > 0.0) ? span : 1.0);
      {
         QSignalBlocker b(m_edShadeLo);
         m_edShadeLo->setValue(lo);
      }
      {
         QSignalBlocker b(m_edShadeHi);
         m_edShadeHi->setValue(hi);
      }
      if (m_gates[GI_SHADE].gate.editing)
         hint(QStringLiteral("最大值低于最小值, 最小值已调整至 %1 (跨度不变, 下限 0)。")
                 .arg(QString::number(lo, 'g', 6)), false);
   }
   m_canvas->setShadeRange(lo, hi);
}

/* 自动跟随开着的时候, 两个输入框是**读数**而不是输入: 值由画布按数据算, 打字进去下一拍就被
 * 覆盖, 不如直接只读。「按数据定标」也就没必要按了 —— 它做的事其实每帧都在做 (它的**可用性**
 * 走 refreshEditability 的 need_manual, 不在这儿写 setEnabled: 那处才是唯一写点)。 */
void ScanWindow::applyShadeAutoUi(bool on)
{
   m_edShadeLo->setReadOnly(on);
   m_edShadeHi->setReadOnly(on);
   m_lblLocked->setVisible(!on);
   m_lblAuto->setVisible(on);

   if (on)
      syncShadeAuto();
}

void ScanWindow::syncShadeAuto()
{
   const double lo = m_canvas->shadeLo();
   const double hi = m_canvas->shadeHi();

   /* 没变就一句话都不做。这一句不是省事: 本函数由 refresh() 每拍 (30Hz) 调用, 而下面那句
    * gateRebase 会重写框标题 —— 每拍重设一次标题就是每拍重排一次版面。 */
   if (m_edShadeLo->value() == lo && m_edShadeHi->value() == hi)
      return;

   /* **必须屏蔽信号**: 不屏蔽的话先设 lo 会触发 valueChanged, 而它拿的是还没更新的
    * m_edShadeHi->value() —— 色阶会被一个陈值盖一下, 下一拍才纠正回来 (看着像闪)。 */
   {
      QSignalBlocker bl(m_edShadeLo), bh(m_edShadeHi);
      m_edShadeLo->setValue(lo);
      m_edShadeHi->setValue(hi);
   }

   /* 同上: 这两个数现在是**程序在写**, 快照跟上, 否则「未保存」标记会一直亮着 ——
    * 而这一框真正要保存的只有「自动跟随」那个勾 */
   gateRebase(GI_SHADE, m_edShadeLo);
   gateRebase(GI_SHADE, m_edShadeHi);
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

   /* 「高级选项」那三个勾的缺省。Prefs 那几个字段的初值是缺省的唯一定义处 (scanprefs.h),
    * 这里照它填 —— 「恢复默认」必须把它们也带回来, 否则那个按钮对这三个勾就是句空话。
    * 触发的是 onAdvToggled: 此刻工作线程还没连接, 推过去只是记着。 */
   const Prefs pd;
   m_cbWantDigIn->setChecked(pd.want_dig_in);
   m_cbNpnWrite ->setChecked(pd.npn_write_drive);
   m_cbDiInvert ->setChecked(pd.npn_sw_invert);
   m_cbShadeAuto->setChecked(pd.shade_auto);   /* 色标自动跟随 ("恢复默认"也回到这一档) */

   /* 回零速度与回零超时刻意不在这里: 「恢复默认」会把 applyDefaults 再跑一遍, 会把为试回零
    * 特意压小的速度、或者特意调长的超时抬回去。这两个的缺省设在 buildHomePanel 里,
    * 之后由 loadSettings 覆盖。 */
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

   /* 「高级选项」三个勾。prefsLoad 是**显式带缺省**读的 (见 scanprefs.h): QSettings 对缺项
    * 给无效 QVariant, toBool() 一律 false —— 不这样, 一个旧的 scan.ini 会把"默认开"读成
    * "用户关掉了", 于是 2300h 不再被修、60FDh 也不补, 而界面上那两个勾看着还像没动过。 */
   m_cbWantDigIn->setChecked(pf.want_dig_in);
   m_cbNpnWrite ->setChecked(pf.npn_write_drive);
   m_cbDiInvert ->setChecked(pf.npn_sw_invert);

   /* 色标自动跟随。这是**模式**不是数值, 所以它记 (上下限那两个数不记, 见 scanprefs.h):
    * 记不住的话每次开程序都要重新勾一遍 */
   m_cbShadeAuto->setChecked(pf.shade_auto);

   /* 回零速度: -1 = 没记过, 越界的夹回量程内 —— 一个被手改坏的 ini 不该让回零用一个
    * 没验过的速度 (夹取规则在 ecatcmd::home_vel_from_pref, 被自检钉着) */
   m_edHomeVel->setValue((int)ecatcmd::home_vel_from_pref(pf.home_vel));

   /* 回零超时: 同一套 (-1 = 没记过 → HMI_HOME_TMO_DEF_S; 越界的夹回 [MIN_S, MAX_S])。
    * **现存的那个 scan.ini 不需要迁移**: 缺这一项就是 -1, 落到 120 s, 正是要的缺省 */
   m_edHomeTmo->setValue(ecatcmd::home_tmo_s_from_pref(pf.home_tmo_s));

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
   pf.home_vel     = m_edHomeVel->value();
   pf.home_tmo_s   = m_edHomeTmo->value();
   pf.want_dig_in     = m_cbWantDigIn->isChecked();
   pf.npn_write_drive = m_cbNpnWrite->isChecked();
   pf.npn_sw_invert   = m_cbDiInvert->isChecked();
   pf.shade_auto      = m_cbShadeAuto->isChecked();

   /* 功率计那两格也跟着落盘 (它不进门控, 所以是"改了就在这儿被记下", 没有「保存」可点)。
    * 取样源本身**不记** —— 那是设备自己的状态, 理由见 scanprefs.h */
   pf.meter_interval_ms = m_edMtrInterval->value();
   pf.meter_csv         = m_edMtrCsv->text().trimmed();

   prefsSave(prefsPath(), pf);
   m_savedNic = pf.nic;

   /* 这一次落盘是「连接」/ 关窗顺手做的, 不是操作员点的「保存」—— 正在编辑、还没点保存的
    * 那一框, 它的当前值刚刚被一起写进去了。快照跟上、标记清掉: 否则「取消」会退回一份已经
    * 不在 ini 里的"上次保存值", 而那个按钮承诺的就是 ini 里那一份。 */
   for (int gi = 0; gi < m_gates.size(); gi++)
   {
      if (!m_gates[gi].gate.editing)
         continue;
      gateSnapshot(gi);
      m_gates[gi].gate.dirty = false;
      gateTitle(gi);
   }
}

void ScanWindow::onRestoreDefaults()
{
   /* 扫描中锁着这些控件, 这里再判一次: 恢复默认会重建网格, 几何改到一半的扫描落进
    * CSV 的 (ix,iy) 与实际位置就对不上了 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中, 参数不可修改。"), true);
      return;
   }

   applyDefaults();     /* 每一 setValue 都会经 pushParams 重算一遍, 幂等 */

   const Params d;
   hint(QStringLiteral("扫描参数已恢复为缺省值 (区域 %1 × %2 mm, 分辨率 %3 mm, "
                       "1 mm = %4 pul, 速度 %5 pul/s)。CSV 输出路径未变。")
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
      m_lGrid->setText(QStringLiteral("网格 %1 × %2 = %3 点   ±%4 mm\n"
                                      "超过上限 %5, 网格未建立 (画布为空, "
                                      "「开始扫描」不可用)")
                          .arg(nx).arg(ny).arg(total)
                          .arg(p.area_x_unit / 2.0, 0, 'f', 3)
                          .arg(kMaxPlanPoints));
   else
      m_lGrid->setText(QStringLiteral("网格 %1 × %2 = %3 点   ±%4 mm")
                          .arg(nx).arg(ny).arg(total)
                          .arg(p.area_x_unit / 2.0, 0, 'f', 3));

   /* 预估是线性的, 实际一定更长 (每次移动的进近段都要减速), 这一句必须写出来 */
   m_lEst->setText(QStringLiteral("每点 ≈ %1 ms   全程 ≈ %2\n(线性估计, 实际用时更长)").arg(estimatePerPointMs(p)).arg(fmtDur(m_ctl->estimateTotalMs())));

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
   /* **必须屏蔽信号**: 两个框现在是一对 (改一个会推另一个), 不屏蔽的话先设 lo 会拿着还没更新的
    * hi 去判"顶没顶到头", 中间经过一次错的量程, 还可能把 hi 白推一下再被下一句改回来。
    * 值本身不用经信号下推 —— 走到这儿的时候画布的色阶已经是这一对了 (按数据定标定的)。 */
   {
      QSignalBlocker bl(m_edShadeLo), bh(m_edShadeHi);
      m_edShadeLo->setValue(m_canvas->shadeLo());
      m_edShadeHi->setValue(m_canvas->shadeHi());
   }

   /* 这是**程序自己**按数据定的标, 不是操作员改的。快照得跟上: 不然编辑态里按一下「按数据
    * 定标」再按「取消」, 会把色阶滚回定标之前那一份 */
   gateRebase(GI_SHADE, m_edShadeLo);
   gateRebase(GI_SHADE, m_edShadeHi);
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

   /* 这是程序自己起的名, 不是操作员改的 (同 syncShadeEdits 的理由): 快照跟上, 否则正在
    * 编辑「扫描参数」时按一下「开始扫描」, 再按「取消」, 会把路径退成空的 */
   gateRebase(GI_PARAM, m_edCsv);
}

void ScanWindow::onBrowseCsv()
{
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中, 输出文件不可更改。"), false);
      return;
   }

   const QString start = m_edCsv->text().trimmed().isEmpty()
                            ? QDir::current().filePath(QString::fromLatin1(kOutDir))
                            : m_edCsv->text().trimmed();

   const QString f = QFileDialog::getSaveFileName(
      this, QStringLiteral("扫描数据输出文件"), start,
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_edCsv->setText(QDir::toNativeSeparators(f));
   m_last_dir = QFileInfo(f).absolutePath();

   /* setText 不发 textEdited (那是"程序改的"), 所以手动补一次点脏 —— 否则从这里换的输出
    * 路径不会在标题上留「未保存」, 点「取消」时也看不出它会被退回去 */
   gateDirty(GI_PARAM);
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
      hint(QStringLiteral("未选择网卡。列表为空时请点击「刷新网卡」, "
                          "仍为空表示未安装 Npcap。"), false);
      return;
   }

   /* 不弹确认框, 进来就发。它写什么由按钮上的 tooltip 一直写着: 进 OP 开始发帧、
    * 覆盖生效映射里主站拥有的项, 但不发使能 (电机不带电) */
   hint(QStringLiteral("正在连接… (需执行 SDO 配置, 可能需要一段时间)"), false);

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
      hint(QStringLiteral("扫描进行中, 故障复位不可用。"), true);
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
      who = QStringLiteral("未检测到轴故障 (6041h bit3 均为 0), 本次操作不写入驱动器。");
   }
   else
   {
      /* 名字后面带上 603Fh: **复位只是清故障位, 不消除原因** —— 这点信息正好在操作员
       * 就要动手的那一帧给他 (遥测里那份码要还是"还没读到", 这一句就照实这么说) */
      QStringList names;
      for (int k = 0; k < ntodo && k < EM_MAX_AXES; k++)
         names << ecatcmd::fault_axis_text(todo[k], t.ax[todo[k]].fault_code);
      who = QStringLiteral("复位轴 %1。复位成功后该轴停在未使能状态, "
                           "需重新「使能」方可继续。复位仅清除故障位, 不消除故障原因。").arg(names.join(QStringLiteral("、")));
   }

   hint(who, false);

   m_thr->postFaultReset();
}

void ScanWindow::onCenterAllClicked()
{
   m_thr->postCenterAll();
   hint(QStringLiteral("两根轴移动到显示坐标 0 (区域中心)。"), false);
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
      hint(QStringLiteral("正在中止回零… (收尾: 失能、切回 CSP、重新使能, 数秒内完成)"), false);
      return;
   }

   /* 非回零时与从前一字不差 (hmi 那边这个按钮仍然直连 postStop, 共用同一份 ecatworker.cpp) */
   m_thr->postStop();
}

/* 「正向/反向回零」与「找正/负限位」—— 全程序最危险的八个按钮: 按下之后滑台自己
 * 带电朝开关走, 朝哪走、什么时候停、撞不撞开关全由驱动器按 6098h 决定, 软件拦不住它撞开关。
 * 不弹确认框, 事前的话常驻在按钮 tooltip 里 (轴 / 方向 / 方式号 / 该轴会先失能)。
 * 限位判据已成立由 refresh 挂红横幅说, 回零中按「停止」= 立即中止由顶部横幅说。
 *
 * 找限位 (17/18) 与找原点共用这一条路, 差别只有方式号与文案 —— 两道否决 (60FDh 读不到 /
 * 两侧同时有效) 与"这一趟走 a 还是 b"的预告都在工作线程里做, 因为判据要读驱动器自己那两位。 */
void ScanWindow::onHomeClicked(int axis, int dir, bool find_limit)
{
   if (axis < 0 || axis > 1 || dir < 0 || dir > 1)
      return;

   /* 扫描中一律拦住。这是第二道 —— 按钮在扫描期间本来就是灰的 */
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中, 回零不可用。"), true);
      return;
   }

   const bool     neg  = (dir == 1);
   const int      meth = find_limit ? ecatcmd::home_lim_method_for(neg)
                                    : ecatcmd::home_method_for(neg);
   const uint32_t vel  = ecatcmd::home_vel_clamp(m_edHomeVel->value());
   /* 超时同样夹一道再传 (工作线程里还有一道, 两处读同一对宏 —— 自检里有一对断言钉着这件事) */
   const int      tmo  = ecatcmd::home_tmo_s_clamp(m_edHomeTmo->value());
   const QString  ax   = (axis == 0) ? QStringLiteral("X") : QStringLiteral("Y");
   /* 动作名: 找限位那两个方式号本身就带方向, 找原点要把方向补进去才分得清 */
   const QString  act  = find_limit
      ? QString::fromUtf8(ecatcmd::home_method_short(meth))
      : QStringLiteral("%1回零").arg(QString::fromUtf8(ecatcmd::home_dir_text(neg)));

   /* **这里不再手工推进零点世代**(2026-09-22 改)。回零确实会搬零点, 但"这次到底搬没搬"只有
    * 工作线程知道: 它那道闸可能把命令拦掉, 命令也可能一直躺在队列里。在 GUI 猜是猜不准的,
    * 而且猜错的方向不对称 —— 多发一代最多让续扫多问一次, 漏发一代却会把回零前后的坐标静默
    * 拼在一起。现在世代由工作线程在**真写 m_origin[] 的那三处**维护, 界面在 refresh() 里从
    * 遥测同步 (只许往前, 见 ecatcmd::origin_epoch_sync)。 */

   m_thr->postHome(axis, meth, vel, tmo);

   /* 这一句只在**命令没被那道闸接住**时才留得住 (真开始回零的话, 最多 33ms 之后
    * refresh 就会用"轴X 正在回零…"那条**状态**横幅把它盖掉 —— 那是设计如此)。 */
   hint(QStringLiteral("轴 %1 的 %2 已下发 (方式 %3, 速度 %4 pul/s)。按「停止」可立即中止。").arg(ax, act).arg(meth).arg(vel), false);
}

void ScanWindow::onZeroHereClicked()
{
   if (m_ctl->running())
   {
      hint(QStringLiteral("扫描进行中, 零点不可重设。"), true);
      return;
   }

   /* 零点一动, 同一个显示坐标指的就不是同一个物理位置了 —— 世代由工作线程自己 +1, 界面在
    * refresh() 里从遥测同步 (只许往前)。这里不再手工推, 也不再把那个数报给操作员:
    * 它现在是异步跟上的, 说一个当场就过期的数不如不说 (§26 已经把世代从界面上拿掉了)。 */

   m_thr->postZeroHere(0);
   m_thr->postZeroHere(1);

   hint(QStringLiteral("当前位置已设为显示坐标 0 (区域中心), 断开重连后仍沿用。"), false);
   m_canvas->update();
}

/* ---------------------------------------------------------------- 功率计 */

void ScanWindow::onMeterChanged(int idx)
{
   if (m_ctl->running())
   {
      /* 扫描中途换源 = 同一张图上的数据来自两个不同的东西, 禁掉。
       * 回退时必须挡掉信号, 否则 setCurrentIndex 会再进来一次, 两个下标之间来回弹 */
      hint(QStringLiteral("扫描进行中, 取样源不可更改。"), false);
      QSignalBlocker b(m_cbMeter);
      m_cbMeter->setCurrentIndex(m_cbMeter->findText(m_meter->kind()));
      return;
   }

   /* 同理, 连续读数正在跑时也不能换: 那条曲线上会是两个不同的东西采的数。
    * 下拉框本身在 refreshMeterPanel() 里已经灰了, 这里是兜底 (键盘/程序设值也能到这儿) */
   if (m_mlog != nullptr && m_mlog->running())
   {
      hint(QStringLiteral("连续读数进行中"), false);
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

   /* 换了源, 上一个未决请求就作废: 回话即使来了也不该再算数 —— 两个读一次槽都靠
    * m_readPending 与 sender() 认出这一点 */
   if (m_readTimer != nullptr)
      m_readTimer->stop();
   m_readPending = false;

   /* open() 对真机是阻塞的 (枚举 USB → 开设备 → 读探头 → 开流), 上限 12s */
   QString err;
   const bool ok = m_meter->open(&err);

   /* 必须告诉控制器, 否则它还在听旧的源 (旧源还活着, 会照常出数) */
   m_ctl->setMeter(m_meter);

   /* 连续读数器跟着换。它自己会把未决的请求作废、缓冲清掉 (见 MeterLog::setSource 的说明),
    * 那边是"一条曲线只画一个源" */
   m_mlog->setSource(m_meter);

   /* 换了源, 「读一次」上次那行读数就是上一个东西的了 —— 清掉, 免得留在那里被当成新的。
    * 曲线上那一份由 MeterLog::setSource 一起清 */
   if (m_lReadout != nullptr)
      m_lReadout->setText(QStringLiteral("—"));

   if (!err.isEmpty())
      hint(QStringLiteral("功率计打开失败: ") + err, true);
   else if (ok && m_meter == m_ophir)
   {
      const OphirInfo i = m_ophir->info();
      hint(i.summary.isEmpty()
              ? QStringLiteral("取样源已切换为「%1」。").arg(m_meter->kind())
              : QStringLiteral("功率计已连接: ") + i.summary,
           false);
   }
   else
      hint(QStringLiteral("取样源已切换为「%1」。").arg(m_meter->kind()), false);

   refresh();
}

/* 设备信息回来了: 真机那三行填设备的选项表与当前选中项。
 * **灰不灰与露不露都不归这里管** —— 那是 refreshMeterPanel() 每拍算的 (空选项表也在那边判)。
 * 只在 infoChanged 时跑, 不放进 30Hz 的 refresh(): 那个频率下重填下拉框会跟操作员正在点的
 * 那一下抢, 而且每帧重建选项是白烧 CPU。 */
void ScanWindow::onMeterInfoChanged()
{
   if (m_cbWl == nullptr)
      return;                    /* 还没建出来 (构造期不会有 infoChanged, 这里只是兜底) */

   const bool is_ophir = (m_meter == m_ophir);

   const OphirInfo i = m_ophir->info();

   /* 两行设备事实 + 一行版本 (诊断用)。版本那两行取不到就不占地方 —— 它们是驱动层给的东西
    * (getVersion / getDriverVersion), 摆出来是为了出事时能一眼说清"装的是哪一版" */
   QString dev = i.valid
      ? QStringLiteral("%1 / %2   序列号 %3 (表头 %4)\nROM %5   探头类型 %6")
           .arg(i.device_name, i.sensor_name, i.sensor_serial, i.device_serial,
                i.rom_version, i.sensor_type)
      : QStringLiteral("设备信息未读取");

   QStringList vers;
   if (!i.com_version.isEmpty())
      vers << QStringLiteral("对象 %1").arg(i.com_version);
   if (!i.driver_version.isEmpty())
      vers << QStringLiteral("驱动 %1").arg(i.driver_version);
   if (!vers.isEmpty())
      dev += QStringLiteral("\n版本: ") + vers.join(QStringLiteral("   "));

   m_lDevInfo->setText(dev);

   if (!is_ophir)
      return;

   /* 填的时候挡掉信号, 否则每 addItem 一次都会被当成操作员改配置 (一连串 stop/set/start) */
   m_meterCfgQuiet = true;

   auto fill = [&](QComboBox *cb, const QStringList &opts, int cur) {
      cb->clear();
      cb->addItems(opts);
      if (cur >= 0 && cur < cb->count())
         cb->setCurrentIndex(cur);
      /* 探头没有这一项 (手册: options 为空 / index 为 -1) 是正常的 -> refreshMeterPanel
       * 那边看到空表就不会放开这个框 */
   };
   fill(m_cbWl,       i.wavelengths, i.wl_index);
   fill(m_cbRange,    i.ranges,      i.range_index);
   fill(m_cbMeasMode, i.modes,       i.mode_index);

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

/* ---------------------------------------------------------------- 脚本源 */

void ScanWindow::onBrowseScript()
{
   const QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("选择脚本文件 (每行一个数值)"), m_last_dir,
      QStringLiteral("文本 (*.txt *.csv *.dat);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_edScript->setText(QDir::toNativeSeparators(f));
   m_last_dir = QFileInfo(f).absolutePath();
   pushScriptPath();
}

/* 框里的文本和实际用的脚本是两份东西: 文本是给眼睛看的, 真正读数是 m_script 里那份。
 * 两者唯一的同步点就在这里。 */
bool ScanWindow::pushScriptPath()
{
   const QString f = m_edScript->text().trimmed();
   if (f == m_script->path())
      return true;           /* 没变就不重读文件 */

   QString err;
   if (!m_script->setPath(f, &err))
   {
      hint(QStringLiteral("脚本读取失败: ") + err, true);
      return false;
   }

   /* 这个源正被用着: 关一次再开, 否则读的还是旧那份表 */
   if (m_meter == m_script)
   {
      m_script->close();
      m_script->open(&err);
      if (!err.isEmpty())
         hint(QStringLiteral("脚本源打开失败: ") + err, true);
   }
   return true;
}

/* ---------------------------------------------------------------- 读一次 */

/* 发一次普通请求, 走的是扫描用的同一条路, 所以它通了 = 采集那条路也通了。
 *
 * 硬闸: PowerMeter 约定 (powermeter.h) 调用方保证同一时刻只有一个未决请求。扫描跑着时
 * "调用方"是 ScanController, 连续读数跑着时是 MeterLog —— 任一个在场, 这里按下去
 * 就是两个请求撞在同一个源上, 于是 CSV 里会悄悄少一个点或者错一个点, 且不报错。
 * refreshMeterPanel() 里那条 setEnabled 是闸门, 这里再兜一次底。
 *
 * 两个槽里要比一次 sender(): 换取样源时未决请求作废, 但旧源的回话可能已排在事件队列里,
 * 只有当前源的回话算数。四个源都是本窗口的子对象、一样长寿, 所以 sender() 是安全的。 */
void ScanWindow::onReadOnceClicked()
{
   if (m_meter == nullptr || !m_meter->isOpen() || m_ctl->running() || m_readPending)
      return;
   if (m_mlog != nullptr && m_mlog->running())
   {
      hint(QStringLiteral("连续读数进行中"), false);
      return;
   }

   /* 顺序有讲究: 先把 m_readPending / 起始时刻 / 兜底定时器都摆好, 最后才发请求 ——
    * 没打开时三个模拟实现是直接 emit readingFailed (在 requestReading() 的调用栈里就回来
    * 了, 见 powermeter.cpp), 倒过来就是在别人的调用栈里改自己的状态。 */
   m_readPending = true;
   m_readSentMs  = m_clock.elapsed();
   m_readTimer->start(kReadOnceTimeoutMs);
   m_lReadout->setText(QStringLiteral("读一次 [%1]: 读取中…").arg(m_meter->kind()));
   refresh();                       /* 立刻把按钮灰掉, 免得连按出两个请求 */

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
   /* 单位跟源头走, 不硬写 W (powermeter.h 的 unit()): 这一行是"拿这台仪器量出来的一个数",
    * 单位说错了比不说更坏 */
   m_lReadout->setText(QStringLiteral("读一次 [%1]: %2 %3   (往返 %4 ms)")
                          .arg(m_meter->kind(), fmtWatts(watts), unitLabel(m_meter))
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
   m_lReadout->setText(QStringLiteral("读一次 [%1]: 失败, %2   (往返 %3 ms)")
                          .arg(m_meter->kind(), err)
                          .arg(took, 0, 'f', 0));
   refresh();
}

/* ---------------------------------------------------------------- 连续读数 */

/* 开始。与扫描互斥靠的是 refresh() 那一处的仲裁: 扫描在跑时这里**照样能按**, 按下去是
 * 「跟随」—— 曲线画扫描采回来的那些点, 自己一个请求都不发, 也不另开文件 (那些点本来就写在
 * 扫描那份 CSV 里)。 */
void ScanWindow::onMtrStartClicked()
{
   if (m_mlog == nullptr || m_mlog->running())
      return;

   if (m_meter == nullptr || !m_meter->isOpen())
   {
      hint(QStringLiteral("取样源未打开"), true);
      return;
   }

   const int interval = m_edMtrInterval->value();
   m_mlog->setInterval(interval);

   /* ---- 扫描在跑: 进「跟随」, 不开文件 ----
    * 这一路不发请求 (仲裁在 refresh()), 点是从扫描那边推过来的。这里再开一个文件只会得到
    * 一份空表头 —— 一个"看起来在记录"的空文件比不记录更坏。 */
   if (m_ctl->running())
   {
      QString err;
      if (!m_mlog->start(interval, &err))
      {
         hint(err, true);
         return;
      }
      hint(QStringLiteral("扫描进行中, 已切换为跟随: 曲线显示扫描采到的点, "
                          "不发送请求, 不写文件。"),
           false);
      refresh();
      return;
   }

   applyMtrCsvDefaultName();

   /* 文件头上那几行 `#` (仪器/探头/波长/量程/模式/单位/间隔/平均次数) —— 在开文件**之前**
    * 推过去, 因为新文件的表头是 beginRecord 里一次写完的 */
   pushMeterMeta();

   /* 先开文件再开采集: 开不了就不采 —— 让人守着一个"以为在存"的记录是最坏的一种
    * (与扫描那边 append 失败即自动中止同一个道理) */
   QString err;
   if (!m_mlog->beginRecord(m_edMtrCsv->text().trimmed(), &err))
   {
      hint(QStringLiteral("CSV 打开失败, 未开始采集: ") + err, true);
      return;
   }

   if (!m_mlog->start(interval, &err))
   {
      m_mlog->endRecord();
      hint(err, true);
      return;
   }

   hint(QStringLiteral("连续读数开始: 间隔 %1 ms, 数据写入 %2。")
           .arg(interval)
           .arg(QDir::toNativeSeparators(m_mlog->recordPath())), false);
   refresh();
}

void ScanWindow::onMtrStopClicked()
{
   if (m_mlog == nullptr)
      return;

   const int n = m_mlog->written();
   m_mlog->stop();
   m_mlog->endRecord();

   hint(QStringLiteral("连续读数已停止 (本次写入 %1 行)。").arg(n), false);
   refresh();
}

void ScanWindow::onMtrClearClicked()
{
   if (m_mlog == nullptr)
      return;

   /* 采集跑着时清空 = 曲线从头开始, 但**已经写进 CSV 的那些不动** —— 文件是追加的,
    * 缓冲只是"眼前这一段"。要说清楚, 否则会以为把文件也清了 */
   m_mlog->clear();
   refresh();
}

void ScanWindow::onMtrIntervalChanged(int ms)
{
   if (m_mlog != nullptr)
      m_mlog->setInterval(ms);
}

void ScanWindow::onMtrAvgChanged(int n)
{
   if (m_mlog != nullptr)
      m_mlog->setAverage(n);

   /* meta 跟着变: 平均次数直接决定这些数是怎么来的 (几倍于原始读数的稳定度), 换文件时
    * 必须记下**当时**那个数 —— 见 pushMeterMeta() */
   pushMeterMeta();
}

/* 把"这几行是哪个仪器什么配置采的"推给连续读数器, 它建文件时写进去 (meterlog.h 的 setMeta)。
 * 调用的三个时机: 开始、导出、改平均次数 —— 前两个是"马上要写文件了", 第三个是元数据本身
 * 变了。做在这里而不是每拍推一次: 每拍都要拼一遍字符串, 而它只在写文件的那一刻有用。 */
void ScanWindow::pushMeterMeta()
{
   if (m_mlog == nullptr)
      return;

   QStringList lines = meterMetaLines(m_meter);
   /* 采样节奏也是数据的一部分: 间隔决定时间分辨率, 平均次数决定每个点的噪声 */
   lines << QStringLiteral("meter_interval_ms=%1").arg(m_edMtrInterval->value());
   lines << QStringLiteral("meter_avg=%1").arg(m_mlog->average());
   m_mlog->setMeta(lines);
}

void ScanWindow::onMtrBrowseCsv()
{
   const QString start = m_edMtrCsv->text().trimmed().isEmpty()
                            ? QDir::currentPath()
                            : QFileInfo(m_edMtrCsv->text().trimmed()).absolutePath();

   const QString f = QFileDialog::getSaveFileName(
      this, QStringLiteral("连续读数输出文件"),
      QDir(start).filePath(QStringLiteral("meter_%1.csv")
                              .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd_HHmmss")))),
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_edMtrCsv->setText(QDir::toNativeSeparators(f));
}

/* 文件名留空就按时间戳起一个, 落在扫描那份输出同一个目录里 */
void ScanWindow::applyMtrCsvDefaultName()
{
   if (!m_edMtrCsv->text().trimmed().isEmpty())
      return;

   const QString name = QStringLiteral("meter_%1.csv")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
   m_edMtrCsv->setText(QDir::toNativeSeparators(
      QDir::current().filePath(QString::fromLatin1(kOutDir) + QLatin1Char('/') + name)));
}

void ScanWindow::onMtrExportClicked()
{
   if (m_mlog == nullptr || m_mlog->count() == 0)
   {
      hint(QStringLiteral("无数据点"), false);
      return;
   }

   const QString f = QFileDialog::getSaveFileName(
      this, QStringLiteral("导出当前缓冲"), m_edMtrCsv->text().trimmed(),
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   /* 导出的也是**一个完整的文件**, 头几行照写 (这份是新文件, 每次都要写) */
   pushMeterMeta();

   QString err;
   if (!m_mlog->saveBuffer(f, &err))
   {
      hint(QStringLiteral("导出失败: ") + err, true);
      return;
   }

   hint(QStringLiteral("已导出 %1 个点至 %2。")
           .arg(m_mlog->count())
           .arg(QDir::toNativeSeparators(f)), false);
}

/* ---------------------------------------------------------------- 扫描 */

void ScanWindow::onStartClicked()
{
   if (m_ctl->running())
      return;

   /* 「读一次」在飞时不能起扫。按钮上那条条件挡不住这条缝: 「读一次」按下之后到回话
    * 回来之间, 按钮一直是可用的 (那期间只有 m_readPending 变了, 而"能不能开始"原先没看它)。
    * 两个请求撞在同一个源上不报错, 只会让 CSV 悄悄少一个点。 */
   if (m_readPending)
   {
      hint(QStringLiteral("「读一次」请求未完成"),
           true);
      return;
   }

   applyCsvDefaultName();

   const QString path = m_edCsv->text().trimmed();
   if (path.isEmpty())
   {
      hint(QStringLiteral("请先指定 CSV 输出文件。"), false);
      return;
   }

   /* 不弹确认框 —— 这一趟的范围在按之前就全在眼前了 (参数栏填的数、扫描栏那两行、
    * 旁边的「开不了」红字)。这里只留一句"出发了": 区域、点数、时长、数据写到哪 */
   const Params p = currentParams();

   /* 连续读数让位。**就在这儿、就在 m_ctl->start() 前一行** —— 这是"让位"这件事唯一
    * 需要做的动作, 因为仲裁点是 GUI 线程里这十几条指令, 中间没有事件循环, 不存在
    * "那边正好在同一时刻发了一个请求"这种缝。scanwindow.h 里说 m_ctl 与 m_mlog 都归
    * 这一个窗口管, 就是为的这个。
    *
    * 让位而不是拒绝: 一开始要的行为就是"扫描跑着时功率计那一路跟着扫描走" (见 meterlog.h),
    * 所以扫描起来 = 那边进 hold, 曲线改画扫描采到的点, 数据不丢 */
   if (m_mlog != nullptr)
      m_mlog->setHold(true);

   QString err;
   if (!m_ctl->start(path, &err))
   {
      hint(QStringLiteral("启动失败: ") + err, true);
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
   hint(QStringLiteral("扫描开始: 区域 %1 × %2 mm (±%3), 共 %4 × %5 = %6 点, "
                       "预计全程 %7; 取样源 %8, 数据写入 %9。")
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
      this, QStringLiteral("打开未完成的扫描 CSV"), start,
      QStringLiteral("CSV (*.csv);;所有文件 (*)"));
   if (f.isEmpty())
      return;

   m_last_dir = QFileInfo(f).absolutePath();
   m_edCsv->setText(QDir::toNativeSeparators(f));

   QString err, why;
   if (m_ctl->resume(f, false, &err, &why))
   {
      m_banner->setVisible(false);
      hint(QStringLiteral("续扫: 已读入 %1。").arg(QDir::toNativeSeparators(f)), false);
      refresh();
      return;
   }

   if (!err.isEmpty())
   {
      hint(QStringLiteral("无法接续: ") + err, true);
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
      hint(QStringLiteral("仍然无法接续: ") + (err.isEmpty() ? why : err), true);
      return;
   }

   hint(QStringLiteral("续扫: %1\n%2")
           .arg(QDir::toNativeSeparators(f), mismatch), true);
   refresh();
}

void ScanWindow::onPauseClicked()
{
   m_ctl->pause();
   hint(QStringLiteral("已暂停, 保持力矩。"), false);
   refresh();
}

void ScanWindow::onResumeRunClicked()
{
   m_ctl->resumeRun();
   refresh();
}

void ScanWindow::onAbortClicked()
{
   m_ctl->abort(QStringLiteral("操作员中止"));
   hint(QStringLiteral("已中止。已采数据位于 %1。")
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
      hint(QStringLiteral("请先在画布上用左键选中一个点。"), false);
      return;
   }

   QString err;
   if (!m_ctl->retest(ix, iy, &err))
   {
      hint(QStringLiteral("重测失败: ") + err, true);
      return;
   }

   m_banner->setVisible(false);
   hint(QStringLiteral("重测 (%1, %2), 将向同一 CSV 追加一行。").arg(ix).arg(iy), false);
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

/* 6061h 那条不再进界面 (2026-09-20 起「原点模式」框里只有速度与八个按钮): 它现在只在
 * 回零收尾那条控制台结论句里打 (见 EcatThread::doHome 的 note)。**遥测字段与工作线程
 * 那次读留着** —— 那条结论句就是它的消费者, 且它说的是"驱动器此刻按哪种模式解释 607Ah",
 * 是回零出问题时唯一能回看的证据。 */

/* 一根轴的五盏灯 + 状态栏那对。参数栏与状态栏都从这里出 (分开算就会两边说的不一样)。
 * 撞限位的判定只有一处定义: ecatcmd::limit_hit, 在 EcatThread::publish() 里算好,
 * 这里只读 a.limit_active。 */
void ScanWindow::refreshAxisSignals(const BusTelem &t)
{
   /* 正在这一根上找限位 (方式 17/18)。**这一位会让下面那条红横幅闭嘴** —— 找限位就是要去
    * 撞那个开关, 限位信号置起是这一趟的**目的**而不是出了事; 而且那条红横幅与"正在找限位"
    * 那条状态横幅争同一个 m_banner, 争赢的结果是唯一一句"按「停止」可立即中止"被顶掉。 */
   const bool finding_limit = t.homing && ecatcmd::home_method_is_limit(t.homing_method);

   for (int i = 0; i < 2; i++)
   {
      const AxisTelem &a = t.ax[i];

      /* "不知道"的判据跟状态机完全一致 (ScanController 自动中止那一段用的就是
       * !valid || !mirror_ok): 判得比状态机宽的话, 会出现灯说"正常"而程序已经因为
       * 丢了过程数据帧中止了的不一致 */
      const bool known = m_connected && a.valid && a.mirror_ok;
      const bool lim   = known && a.limit_active;

      /* ---- 「轴信号」那两盏: 只有一个"亮成什么样", 没有字 ----
       * 想知道灯亮说的是什么, 挂在灯上的 tooltip 里写着 (kTip + kLampRule),
       * 以及表头上那一列的名字 */
      setSignalCell(m_axGrid, i, AX_ENABLED, known, a.enabled, Lamp::Ok);
      /* 「故障」用 ecatcmd::axis_alarm 而不是 a.fault: 它 = bit3 **或** 603Fh 读到非 0 的码。
       * 通讯报警 (0xFF06) 不保证把 bit3 立起来, 只看 bit3 就会出现"驱动器报警了而故障灯不亮" ——
       * 那正是这一轮要修的那件事。**只是显示放宽**: 「故障复位」挑哪根轴仍按 bit3
       * (ecatcmd::axis_needs_reset), 那条路会真的写 6040h = 0x0000 卸力 */
      setSignalCell(m_axGrid, i, AX_FAULT, known,
                    ecatcmd::axis_alarm(a.fault, a.fault_code), Lamp::Bad);
      /* 「通讯」是**上位机自己看到的**帧够不够 (t.comm_bad 是总线级, 两根轴同一件事)。
       * 判据刻意**不用上面那个 known**: 帧不够到一定程度 mirror_ok 就降 0, 用 known 的话
       * 这盏灯会在事情变糟的那一刻从红变成灰 —— 恰好相反。这里只要求"连着且有这根轴",
       * 而 comm_bad 本身就是"我们知道出事了" */
      const bool live = m_connected && a.valid;
      setSignalCell(m_axGrid, i, AX_COMM, live, t.comm_bad, Lamp::Bad);

      /* ---- 「限位开关」那三盏: 三个开关本身压着没有 (60FDh) ----
       * known 要再与 a.dig_known: 少了它, 读不到 60FDh 时那三位是 0, 界面会显示"三个都没
       * 压住" —— 一个看起来完全正常的结论。原点用 Ok (绿亮 = 正在压着, 位置信息), 正负限位
       * 用 Bad; 绿色在这里不是"没事", 灯亮一律表示这件事正在发生。 */
      const bool dk = known && a.dig_known;
      setSignalCell(m_limGrid, i, LIM_HOME, dk, a.dig_home, Lamp::Ok);
      setSignalCell(m_limGrid, i, LIM_POS, dk, a.dig_pos, Lamp::Bad);
      setSignalCell(m_limGrid, i, LIM_NEG, dk, a.dig_neg, Lamp::Bad);

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
         lb->setText(ax + QStringLiteral(" 有效"));
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
                        "原点 (bit2)=%d 正限位 (bit1)=%d 负限位 (bit0)=%d\n",
                        i, t.di_invert ? 1 : 0, (unsigned)a.sw, a.dig_home ? 1 : 0,
                        a.dig_pos ? 1 : 0, a.dig_neg ? 1 : 0);
         else
            std::printf("[scan] 轴%d 限位判据置起: 反转=%d  sw=0x%04X  "
                        "60FDh 不在生效映射里 (三个开关的状态无从得知)\n",
                        i, t.di_invert ? 1 : 0, (unsigned)a.sw);
         std::fflush(stdout);

         /* ★ 找限位期间**只压横幅**, 上面那行 stdout 证据与限位灯/状态栏那格照旧 ——
          * 它们说的是"这个信号此刻有效", 是事实; "这是个故障"才是这里不让说的话。
          * m_limShown[i] 上面已经置 true 了, 所以这一趟不会反复重算;
          * m_limBanner[i] 刻意**不写** —— 下面那个清理分支是按"这一位掉下去"清的,
          * 掉下去时 m_limShown 归 false, 于是找完限位之后压着限位启扫, 该响的还是响。 */
         if (finding_limit && i == t.homing_axis)
            continue;

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

         /* 这一位掉了, 横幅就该跟着走: hint(s,true) 不自动消失, 而"触发"是个状态,
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

   /* ---- 「通讯」那条红横幅 (总线级, 在轴的循环之外) ----
    * 照限位那条现成的"持续状态"范式: 上升沿弹一次 (hint 的 fault 那一支会
    * m_bannerTimer->stop(), 也就是**不自动消失**), 下降沿按原文比对撤掉自己那一条。
    * 为什么值得不自动消失: 用户报的就是"驱动器报警了而我没看见" —— 一条会自动消失的
    * 提示正好会在没人看着屏幕的那几分钟里消失。 */
   if (t.comm_bad && !m_commShown)
   {
      m_commShown = true;

      /* stdout 留证据: 30Hz 的灯与横幅会一闪而过, 而"当时最长多久没发帧"是这一整件事
       * 唯一能对账的数 —— 现场回来只看这一个数就知道该查线还是查这台 PC */
      std::printf("[scan] 过程数据帧连续不足: WKC=%d/期望 %d  最长 %d ms 没发帧"
                  " (超 %d ms 共 %d 次)  AL: %s\n",
                  t.wkc, t.expected_wkc, t.max_gap_ms, HMI_GAP_WARN_MS, t.gaps_over_ms,
                  t.al_checked
                     ? ecatcmd::al_code_text(t.al_state, t.al_code).toUtf8().constData()
                     : "(没读到)");
      std::fflush(stdout);

      /* 措辞全在 ecatcmd::comm_banner_text 里 (**(B) 家族**: 讲的是"我这边的帧不够",
       * 与驱动器自报的 0xFF06 那套 fault_code_action 分开) */
      m_commBanner = ecatcmd::comm_banner_text(t.wkc, t.expected_wkc, t.bad_wkc_run,
                                               t.max_gap_ms, t.gaps_over_ms);
      hint(m_commBanner, true);
   }
   else if (!t.comm_bad)
   {
      m_commShown = false;

      if (!m_commBanner.isEmpty() && m_banner->isVisible()
          && m_banner->text() == m_commBanner)
      {
         m_banner->setVisible(false);
         m_bannerTimer->stop();
      }
      m_commBanner = QString();
   }
}

/* 一格 = 一盏灯, 状态全在这四种样子里 (一列一个 lit 颜色: 使能/原点用 Ok 绿, 故障/限位
 * 用 Bad 红)。**没有字** —— "已使能"那类说明原来写在灯旁边, 是同一件事说两遍;
 * 现在只有灰/灭/绿亮/红亮四种样子, 灰 = 不知道 (连不上 / 遥测丢了)。 */
void ScanWindow::setSignalCell(LampGrid &g, int i, int s, bool known, bool on, Lamp lit)
{
   Lamp l = Lamp::Unknown;

   if (!known)
      l = Lamp::Unknown;
   else if (on)
      l = lit;
   else
      l = Lamp::Off;

   /* 只在真变了才动控件: 30Hz 每帧重设一遍 styleSheet 会把重绘刷爆。
    * 一张表上五盏灯各自记着自己上一次画的是什么, 就是为这一句 */
   if (g.lampLast[i][s] != l)
   {
      g.lampLast[i][s] = l;
      paintLamp(g.lamp[i][s], l);
   }
}

void ScanWindow::refresh()
{
   /* 状态机先走一格, 再读遥测 —— 顺序反过来的话, 界面上显示的永远是上一拍的状态 */
   m_ctl->tick(m_clock.elapsed());

   const BusTelem t = m_thr->telemetry();

   /* 零点世代的唯一同步点 (2026-09-22 改)。工作线程在**真写 m_origin[] 的那三处**各 +1
    * (第一次取零点 / 回零 / 设为区域中心), 界面只在这里跟一发。
    *
    * **只许往前** (ecatcmd::origin_epoch_sync): 倒回去等于给下一次续扫发一张假的"世代对不上"
    * 红横幅。真会倒的场合只有一个 —— 断开时工作线程把 teardown 跑了, 但那条路必经连接,
    * 而连接要么沿用(不加世代)要么重取(+1), 所以实测不会倒; 这条规矩是防将来改出来的。
    *
    * 为什么不需要再同步别的: 起扫的前提是两轴已使能, 而使能拒绝在零点就绪之前 —— 所以
    * "第一行 CSV 写在零点定下来之前"不可能发生, CSV 里那一代的坐标永远是那一代自己的。
    * 回零与「设为区域中心」在运行中都被拒, 跑起来的那一趟零点不会动。 */
   {
      const int gen = ecatcmd::origin_epoch_sync(m_epoch, t.origin_gen);
      if (gen != m_epoch)
      {
         m_epoch = gen;
         m_ctl->setZeroEpoch(m_epoch);
      }
   }

   /* 按钮形态只看遥测, 不看"点过哪个按钮" —— 后者会与线程的真实状态错开。
    * in_op = 正在发帧 (真连上了), busy = 正在连接或正在收尾; 两者任一为真就是占着总线 */
   const bool onair = t.in_op || t.busy;
   if (onair != m_connected)
      setConnected(onair);

   if (t.in_op)
   {
      QString s = QStringLiteral("WKC %1 / 期望 %2").arg(t.wkc).arg(t.expected_wkc);
      bool    bad = (t.wkc < t.expected_wkc);

      /* 帧够的时候这一截**不出现** —— 状态栏是给异常留的地方, 常态多一串数只是噪声。
       * 出现过一次 (comm_bad 或读到过 AL) 之后就常驻: 用户遇到的是**闩锁**的报警,
       * 报警过去之后那个 AL 码正是最该留在屏幕上的东西。 */
      if (t.comm_bad || t.al_checked)
      {
         s += QStringLiteral("   ");
         s += t.al_checked ? ecatcmd::al_code_text(t.al_state, t.al_code)
                           : QStringLiteral("AL 状态未读取");

         if (t.max_gap_ms > 0)
            s += QStringLiteral(" · 最长 %1 ms 未发帧").arg(t.max_gap_ms);
      }
      if (t.comm_bad)
      {
         s += QStringLiteral(" · 连续 %1 帧 WKC 不足").arg(t.bad_wkc_run);
         bad = true;
      }

      m_lWkc->setText(s);
      m_lWkc->setStyleSheet(bad ? QStringLiteral("color:#ffb020; font-weight:bold")
                                : QStringLiteral("color:#7b8391"));
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

   /* 参数控件的可用性**全部**归 refreshEditability(): 平时只读, 点了这一框的「编辑」才
    * 放开; 几何那几项运行中仍然锁住 (改到一半, 落进 CSV 的 (ix,iy) 就和实际位置对不上)。
    * 原来那张"扫描中锁住"的表已经搬进各框的 GateItem.lock_running。 */
   refreshEditability();

   /* 自动跟随: 色阶是画布在画的时候按数据算的, 这里把两个框的读数跟上 (值没变时是 no-op) */
   if (m_cbShadeAuto->isChecked())
      syncShadeAuto();

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

   /* 八个回零按钮逐轴判, 只看这一根; 另一根带不带电、有没有故障都与它无关。
    * 刻意不看 enabled: 未使能也能回零 (em_home 要求未使能才能写 6098h)。
    * fault / mirror_ok 这里再判一次: 工作线程那道闸才是权威, 但让按钮先按不动更好。
    * 找限位那四个与这四个**同一判据** —— 归"连接态 + 不在运行 + 不在回零"管, 不进编辑门控
    * (它们是动作不是参数)。 */
   for (int i = 0; i < 2; i++)
   {
      const bool    ok   = can_home && t.ax[i].valid && t.ax[i].mirror_ok && !t.ax[i].fault;
      const bool    mine = t.homing && (t.homing_axis == i);
      const bool    mine_lim = mine && ecatcmd::home_method_is_limit(t.homing_method);

      for (int d = 0; d < 2; d++)
      {
         m_btnHome[i][d]->setEnabled(ok);
         m_btnLim[i][d]->setEnabled(ok);

         /* 回零中只改正在动的那一根的按钮, 否则回 X 时 Y 那两个也写着"回零中…"。
          * 而且**两排各自认自己那一趟**: 找限位时左边两列不改字 (那一趟不是找原点),
          * 找原点时右边两列不改字 —— 说反了人会以为"找完还要再找一次"。
          * 文字里不再带轴名 (行首那个标签已经说了), 按下后变短也不会把列宽撑开。 */
         m_btnHome[i][d]->setText((mine && !mine_lim)
                                     ? QStringLiteral("回零中…")
                                     : QString::fromUtf8(kHomeBtnText[d]));
         m_btnLim[i][d]->setText(mine_lim ? QStringLiteral("找限位中…")
                                          : QString::fromUtf8(kHomeBtnText[2 + d]));
      }
   }

   /* 「回零速度」不进上面那张 locked 表, 扫描期间也可改 (它是个值不是动作)。框里那两行
    * 小字 (能找多远 / 6061h) 随它一起删了, 现在这个框里没有按速度重算的文字。 */

   /* 回零横幅: 上升沿起一条, 下降沿只清我们自己写的那条 (原文比对, 同 m_limBanner) */
   if (t.homing)
   {
      const int     m      = t.homing_method;
      const bool    is_lim = ecatcmd::home_method_is_limit(m);
      const QString nm     = (t.homing_axis == 0) ? QStringLiteral("X") : QStringLiteral("Y");
      const int     ai     = (t.homing_axis >= 0 && t.homing_axis < 2) ? t.homing_axis : 0;

      QString s;
      if (is_lim)
      {
         /* 找限位: **不在这句里声称它现在朝哪走**。"这一趟走 a) 还是 b)"是发起那一刻按
          * 驱动器自己那两位定下来的, 而这里手上只有界面反相之后的值 —— 「上位机侧取反」
          * 开着时两者正好相反, 说成"正在反向退开"会恰好说反。分支预告在控制台里 (那句
          * 是工作线程按驱动器自己的读数打的); 这里只报**信号此刻有效**这件事实, 与限位灯
          * 同一份量、同一个措辞。 */
         s = QStringLiteral("轴%1 正在%2 (方式 %3), 按「停止」可立即中止。")
                .arg(nm, QString::fromUtf8(ecatcmd::home_method_short(m)))
                .arg(m);

         if (t.ax[ai].dig_known &&
             ecatcmd::home_lim_target_active(m, t.ax[ai].dig_pos, t.ax[ai].dig_neg))
            s += QStringLiteral(" [%1信号有效]")
                    .arg(QString::fromUtf8(ecatcmd::home_lim_switch_name(m)));
      }
      else
      {
         s = QStringLiteral("轴%1 正在回零 (方式 %2, 先向%3高速寻找), 按「停止」可立即中止。")
                .arg(nm).arg(m)
                .arg(QString::fromUtf8(ecatcmd::home_method_first_dir(m, false)));
      }

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
                                      : QStringLiteral("复位"));

   /* 高级选项那三个勾的可用性归 refreshEditability(), 它不跟 onair 走 (理由在
    * buildAdvPanel 的注释里)。这里只重申那条**不能回灌勾选状态**的规矩:
    * setNpnWriteDrive / setDiInvert 是立刻写, 而 publish 每周期才拷一次 —— 中间那一拍
    * 回灌会让勾自己跳回去。措辞那边走 BusTelem::di_invert (那是真值)。 */

   const bool meter_ok = (m_meter != nullptr) && m_meter->isOpen();
   const bool params_ok = m_ctl->paramsError().isEmpty();
   /* 回零中不能起扫: 回零把轴留在使能, 扫描半路撞上既不会自动中止也等不到插补 ——
    * 状态机会以为到了点, 其实一格没动。纵深防御, armRun() 里那道才是权威。
    *
    * **!m_readPending 是 2026-09-22 补上的**: 原先没有它, 于是"点「读一次」→ 立刻点「开始
    * 扫描」"这一个动作序列会让两个请求撞在同一个源上 (回话还没到, 按钮已经是可用的),
    * 而 powermeter.h 说这种情况不报错、只会静默分错数。 */
   m_btnStart->setEnabled(!running && m_connected && meter_ok && params_ok
                          && !t.homing && !m_readPending);

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
         extra = QStringLiteral("  (等待稳定 %1 ms)").arg(m_ctl->stateMs());

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

   /* ---- 功率计那一框的字 ---- */
   refreshMeterReadout();

   /* ---- 三个请求发起方的仲裁。**全仓库只此一处** ----
    *
    * powermeter.h:28-31 定死了: 同一时刻只允许一个未决请求, 违约不报错、只会静默分错数。
    * 三个发起方: ① ScanController (扫描中) ② 「读一次」 ③ 连续读数 (MeterLog)。
    * 优先级就是上面这个顺序 —— 前一个在场, 后一个让位。
    *
    * 让位不是"拒绝", 是"不开新的口"：每个发起方本来就是一问一答, 已经发出去的那一个照旧
    * 回收 (各家的 sender()/pending 比对会把不属于自己的那份丢掉)。所以这里只需要每拍把
    * 两个开关摆成此刻该有的样子 —— 与别处一样, 不记"谁先按的"。 */
   if (m_mlog != nullptr)
   {
      /* 扫描在跑、或手动读在飞 -> 进 hold: 不发请求, 但**不算停止** (曲线上照旧能画扫描
       * 采回来的点, 见 pointLogged 那个槽) */
      m_mlog->setHold(m_ctl->running() || m_readPending);
      m_mlog->tick(m_clock.elapsed());
   }

   /* 故障横幅 **分两拍**, 说的都是 ecatcmd::fault_banner_text(t) (那一条模板同时管着
    * "码还没读到"与"码到了"两种说法, 见它的注释):
    *   第一拍 —— 上升沿。这一刻 603Fh 还没读回来 (那条 SDO 在下一圈), 横幅只能说"还没读到"。
    *   第二拍 —— 码到了。**「驱动器故障要给出故障码」的重点就在这一拍**: 只弹第一拍的话,
    *             操作员看到的就是一句"出故障了", 而该查什么全在那四个十六进制数字里。
    * 两拍都只在"要说的话变了"时才弹: 30Hz 每帧都设会把重绘刷爆, 也会把刚点掉的提示顶回来。 */
   if (t.fault && !m_faultShown)
   {
      m_faultShown = true;
      hint(ecatcmd::fault_banner_text(t), true);

      /* 这一句里已经把**此刻**那份码写进去了 (可能已经有值, 也可能还是"还没读到")。
       * 记下来, 免得下面第二拍同一帧再弹一遍同样的字。
       * 判据用 alarm 与 fault_banner_text 一致 —— 只挑报警的轴 */
      for (int i = 0; i < 2; i++)
         if (t.ax[i].valid && t.ax[i].mirror_ok
             && ecatcmd::axis_alarm(t.ax[i].fault, t.ax[i].fault_code))
            m_faultCodeShown[i] = t.ax[i].fault_code;
   }
   else if (!t.fault)
   {
      m_faultShown = false;

      /* 故障没了: 把"横幅上说过哪个码"也忘掉 —— 留着的话下一次故障读到的若是同一个码,
       * 第二拍就永远不会弹 (这一句就是那条 bug 的闸) */
      for (int i = 0; i < 2; i++)
         m_faultCodeShown[i] = HMI_FAULT_CODE_UNREAD;
   }

   /* 第二拍: 逐轴比"横幅上已经说过的那一个码"。**只有报故障的轴参与**, 且 UNREAD 不算
    * "变了" (那还是第一拍的状态), FAIL 算 —— "读不到"是这一趟的结论, 该让操作员知道 */
   if (t.fault)
   {
      for (int i = 0; i < 2; i++)
      {
         const AxisTelem &a = t.ax[i];

         if (!m_connected || !a.valid || !a.mirror_ok
             || !ecatcmd::axis_alarm(a.fault, a.fault_code))
            continue;
         if (a.fault_code == m_faultCodeShown[i])
            continue;

         m_faultCodeShown[i] = a.fault_code;

         if (a.fault_code != HMI_FAULT_CODE_UNREAD)
            hint(ecatcmd::fault_banner_text(t), true);
      }
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
   /* **这里不再 +1**(2026-09-22 改)。从前"连接会把零点重设为当时所在的位置"(tryInitOrigin
    * 无条件重取), 所以每次连接都得算换了一代; 现在 scan 开着 setKeepOrigin, 连接**沿用**上次
    * 那份零点, 显示坐标跨重连连续 —— 连接本身不再动零点, 也就不该推世代。
    * 真动了零点的那几处 (回零 / 设为区域中心 / 第一次取零点 / 沿用不了只好重取) 由工作线程
    * 各自 +1, 界面在 refresh() 里同步。 */
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
   /* 换了句话 -> 折行高度可能变了, 而它不在布局里, 大小得自己重算 (顺带 raise: 它一直是
    * 从隐藏的状态回来的, 不抬一次会被后建的分隔条盖住) */
   placeBanner();

   /* 故障不自动消失: 无人值守的一趟扫下来, 一闪而过的提示等于没提示 */
   if (!fault)
      m_bannerTimer->start(8000);
   else
      m_bannerTimer->stop();
}

void ScanWindow::showFault(const QString &why)
{
   hint(QStringLiteral("扫描已自动中止: ") + why, true);

   /* 自动中止还要弹模态 (横幅之外, 无人值守时不会错过), 用 singleShot 挪出这一帧再弹,
    * 免得在 30Hz 的槽里嵌套事件循环 */
   if (m_autoStop > 0)
      return;
   m_autoStop = 1;
   QTimer::singleShot(0, this, [this, why] {
      m_autoStop = 0;
      QMessageBox::critical(this, QStringLiteral("扫描已自动中止"),
         QStringLiteral("%1\n\n"
                        "已采数据均已写入 CSV (%2)。\n\n")
            .arg(why, QDir::toNativeSeparators(m_ctl->csvPath())));
   });
}

void ScanWindow::warnMaybeLive()
{
   QMessageBox::critical(this, QStringLiteral("电机可能仍带电"),
      QStringLiteral(
         "收尾时未能确认所有轴已失能 (控制台退出码 10)。\n\n"));
}

/* ---------------------------------------------------------------- 收尾 */

void ScanWindow::disconnectAndStop()
{
   /* 回零进行中先掐掉它: 这条断开路径是同步等的 (下面 12 秒), 而回零单次能阻塞到**整个回零
    * 超时**(缺省 120 s, 上限 600 s) —— 不掐的话 12 秒空转到底, 最后 QThread 会在 em_home 还在
    * 泵帧时被拆掉。
    * requestMotionStop() 只往一个标志里存 1, 与「停止」按钮直呼的是同一个。
    * 必须先于下面的 postDisconnect: 命令排队, 队列要等回零退出来才轮到。 */
   if (m_thr->isRunning() && m_thr->telemetry().homing)
      m_thr->requestMotionStop();

   /* 先中止扫描再断总线: 反过来状态机会把"掉出 OP"当异常自动中止, 正常收尾变成红色告警 */
   if (m_ctl->running())
      m_ctl->abort(QStringLiteral("总线已断开"));

   if (!m_thr->isRunning())
      return;

   /* 断开 = 让工作线程自己走 teardown (失能 → 还原映射 → 降 PRE_OP → 关网卡)。
    * 用队列命令而不是 ::terminate: 收尾这几步不能被打断 */
   m_thr->postDisconnect();

   /* 等它真的收完: 条件是"不在发帧且不在忙", 不是"点过断开" —— teardown 排队执行,
    * 从投递到开跑之间有一小段, 只看 in_op 会在那一段误判成收完了。
    * 每根轴的失能确认与状态机迁移都有超时, 所以给到 12 秒。
    *
    * **那一道"先掐掉"有一个窄缝**(2026-09-22 补): 上面判 homing 时 CMD_HOME 可能还在队列里,
    * 于是 homing 还是 false、标志没置上, 而线程随后要阻塞**整个回零超时**才轮到
    * postDisconnect —— 超时调到 600 s 时就是在这里卡 10 分钟外加一个"工作线程 15 s 没退出来"
    * 的模态框。所以在等待循环里补一次: 一旦 homing 真的变真就再掐一下。
    * 这时 requestMotionStop 落在 doHome 直接写 m_telem.homing = true **之后**, 也就是
    * em_clear_stop() 之后, 所以不会被清掉。这样这条路的时长就与超时值无关了。 */
   for (int i = 0; i < 600; i++)
   {
      const BusTelem t = m_thr->telemetry();
      if (!t.in_op && !t.busy)
         break;
      if (t.homing)
         m_thr->requestMotionStop();   /* 幂等: 只往标志里存 1 */
      QThread::msleep(20);
   }

   setConnected(false);
}

void ScanWindow::closeEvent(QCloseEvent *e)
{
   /* 扫描在跑也不弹确认框: 关窗 = 中止 + 断开, 已经采到的点都已经落在 CSV 里,
    * 之后「打开 CSV 续扫」能接着跑。要留住这一趟就别关窗 */

   /* 连续读数先收摊 (停 + 关 CSV), **早于下面的总线收尾**: 它会一直向源要数, 而真机那条源
    * 在断开时是要收线程的 —— 收线程的同时还有请求在飞, 那个请求就永远不会有人认领。
    * 它那份 CSV 同理要在这时候落盘并关掉 (stop() 里做) */
   if (m_mlog != nullptr)
      m_mlog->stop();

   if (m_thr->isRunning())
   {
      disconnectAndStop();

      m_thr->requestQuit();
      if (!m_thr->wait(15000))
      {
         QMessageBox::warning(this, QStringLiteral("收尾超时"),
            QStringLiteral("工作线程在 15 s 内未退出, 收尾可能未完成。\n"
                           "关闭窗口后请断开驱动器动力电源。"));
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
