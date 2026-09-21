#include "axispanel.h"

#include <QBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimerEvent>

#include <cmath>

static const int HMI_HALF = 1000000;   /* 2 * HMI_RANGE, 免得到处写 */

SlideCanvas::SlideCanvas(QWidget *parent) : QWidget(parent)
{
   setMouseTracking(true);
   setFocusPolicy(Qt::StrongFocus);
   setCursor(Qt::CrossCursor);
   setAutoFillBackground(false);
}

int SlideCanvas::xOf(int32_t v) const
{
   int x0 = 24;
   int x1 = width() - 24;
   if (x1 <= x0)
      return x0;

   double f = (double)((int64_t)v + HMI_RANGE) / (double)HMI_HALF;
   return x0 + (int)(f * (double)(x1 - x0) + 0.5);
}

int32_t SlideCanvas::vOf(int x) const
{
   int x0 = 24;
   int x1 = width() - 24;
   if (x1 <= x0)
      return 0;

   double f = (double)(x - x0) / (double)(x1 - x0);
   return (int32_t)std::llround(f * (double)HMI_HALF - (double)HMI_RANGE);
}

void SlideCanvas::noteClamped()
{
   m_clamped_ms = 1500;
   if (m_timer == 0)
      m_timer = startTimer(100);   /* 1.5 秒后自己灭, 不常驻 */
   update();
}

void SlideCanvas::pick(const QPoint &pt)
{
   /* 未连接 / 未使能时把点击吞掉 */
   if (!m_live || !m_movable)
      return;

   int32_t v = vOf(pt.x());

   if (v >  HMI_RANGE) { v =  HMI_RANGE; noteClamped(); }
   if (v < -HMI_RANGE) { v = -HMI_RANGE; noteClamped(); }

   m_want = v;
   update();
   emit targetRequested(v);
}

void SlideCanvas::mousePressEvent(QMouseEvent *e)
{
   if (e->button() == Qt::LeftButton)
      pick(e->pos());
   else
      e->ignore();
}

void SlideCanvas::mouseMoveEvent(QMouseEvent *e)
{
   /* 按住拖动 = 实时改向 */
   if (e->buttons() & Qt::LeftButton)
      pick(e->pos());
}

void SlideCanvas::timerEvent(QTimerEvent *e)
{
   if (e->timerId() != m_timer)
   {
      QWidget::timerEvent(e);
      return;
   }

   m_clamped_ms -= 100;
   if (m_clamped_ms <= 0)
   {
      m_clamped_ms = 0;
      killTimer(m_timer);
      m_timer = 0;
   }
   update();
}

void SlideCanvas::paintEvent(QPaintEvent *)
{
   QPainter p(this);
   p.setRenderHint(QPainter::Antialiasing, true);

   const int yBand = height() * 3 / 5;
   const int hBand = 14;
   const int yTop  = 30;                 /* 目标三角与文字的上边 */

   QColor cBg     ("#1b1e24");
   QColor cTrack  ("#2c313a");
   QColor cEdge   ("#3c434e");
   QColor cTick   ("#6b7480");
   QColor cCarOn  ("#4a9eff");
   QColor cCarOff ("#555b66");
   QColor cWant   ("#ffb020");
   QColor cTgt    ("#33d17a");
   QColor cText   ("#c8ced8");

   p.fillRect(rect(), cBg);

   if (width() < 80 || height() < 70)
      return;

   QRect band(20, yBand, width() - 40, hBand);
   p.setPen(QPen(cEdge, 1));
   p.setBrush(cTrack);
   p.drawRoundedRect(band, 4, 4);

   QFont f = p.font();
   f.setPointSizeF(8.0);
   p.setFont(f);

   for (int32_t v = -HMI_RANGE; v <= HMI_RANGE; v += 100000)
   {
      int x = xOf(v);
      bool center = (v == 0);

      p.setPen(QPen(center ? cText : cTick, center ? 2 : 1));
      p.drawLine(x, yBand - 6, x, yBand + hBand + 6);

      QString lab = (v == 0) ? QStringLiteral("0")
                             : QStringLiteral("%1k").arg(v / 1000);
      QRect tr(x - 40, yBand + hBand + 9, 80, 14);
      p.setPen(center ? cText : cTick);
      p.drawText(tr, Qt::AlignHCenter | Qt::AlignTop, lab);
   }

   /* 插值目标 (每周期真正下发的那一点): 细线 */
   {
      int x = xOf(m_tgt);
      p.setPen(QPen(cTgt, 1, Qt::DashLine));
      p.drawLine(x, yBand - 14, x, yBand + hBand + 4);
   }

   /* 点击目标 want: 向下的三角, 画在轨道上方 */
   {
      int x = xOf(m_want);
      QPolygon tri;
      tri << QPoint(x, yBand - 4)
           << QPoint(x - 7, yBand - 16)
           << QPoint(x + 7, yBand - 16);
      p.setPen(Qt::NoPen);
      p.setBrush(cWant);
      p.drawPolygon(tri);
   }

   /* 实际位置 (6064h): 滑块本体 */
   {
      int x = xOf(m_pos);
      QRect car(x - 18, yBand - 20, 36, hBand + 10);
      p.setPen(QPen(cEdge, 1));
      p.setBrush(m_movable ? cCarOn : cCarOff);
      p.drawRoundedRect(car, 5, 5);

      p.setPen(QPen(Qt::white, 1));
      p.drawLine(x, car.top() + 3, x, car.bottom() - 3);
   }

   {
      p.setPen(cText);
      QFont nf("Consolas");
      nf.setPointSizeF(9.0);
      nf.setStyleHint(QFont::Monospace);
      p.setFont(nf);

      QString s = QStringLiteral("实际 %1   下发 %2   目标 %3  pul")
                     .arg(m_pos).arg(m_tgt).arg(m_want);
      p.drawText(QRect(20, 2, width() - 40, yTop - 6),
                 Qt::AlignLeft | Qt::AlignVCenter, s);
   }

   if (!m_live)
   {
      p.setPen(QColor("#7b8391"));
      p.drawText(rect(), Qt::AlignCenter,
                 QStringLiteral("无过程数据 —— 先「连接」"));
   }
   else if (!m_movable)
   {
      p.setPen(QColor("#7b8391"));
      p.drawText(rect(), Qt::AlignCenter,
                 QStringLiteral("未使能 —— 点击不动作 (点「使能」后可点)"));
   }
   else if (m_clamped_ms > 0)
   {
      p.setPen(cWant);
      p.drawText(QRect(0, height() - 20, width(), 18),
                 Qt::AlignCenter, QStringLiteral("已到边界: 目标被夹在 ±500000"));
   }
}

AxisPanel::AxisPanel(int axis, QWidget *parent)
   : QWidget(parent), m_axis(axis)
{
   setObjectName(QStringLiteral("axisPanel"));
   /* 普通 QWidget 子类默认不吃样式表里的 background —— 要么自己画, 要么开这个属性 */
   setAttribute(Qt::WA_StyledBackground, true);

   m_canvas = new SlideCanvas(this);

   m_vel = new QSlider(Qt::Horizontal, this);
   m_vel->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_vel->setSingleStep(1000);
   m_vel->setPageStep(5000);
   m_vel->setValue(HMI_VEL_DEF);

   m_velBox = new QSpinBox(this);
   m_velBox->setRange(HMI_VEL_MIN, HMI_VEL_MAX);
   m_velBox->setSingleStep(1000);
   m_velBox->setSuffix(QStringLiteral(" pul/s"));
   m_velBox->setValue(HMI_VEL_DEF);

   connect(m_vel, &QSlider::valueChanged, this, [this](int v)
   {
      if (m_velBox->value() != v)
         m_velBox->setValue(v);
      emit speedChanged(m_axis, (uint32_t)v);
   });
   connect(m_velBox, &QSpinBox::valueChanged, this, [this](int v)
   {
      if (m_vel->value() != v)
         m_vel->setValue(v);
      emit speedChanged(m_axis, (uint32_t)v);
   });

   m_zero = new QPushButton(QStringLiteral("把当前位置设为 0"), this);
   m_zero->setToolTip(QStringLiteral(
      "只改软件的显示零点, 不写驱动器 607Dh / 不写 EEPROM; 物理目标点一个脉冲都不动"));

   m_center = new QPushButton(QStringLiteral("本轴回中"), this);
   m_center->setToolTip(QStringLiteral("走到显示坐标 0 (= 连接时读到的那个位置)"));

   connect(m_zero,   &QPushButton::clicked, this, [this] { emit zeroRequested(m_axis); });
   connect(m_center, &QPushButton::clicked, this, [this] { emit centerRequested(m_axis); });

   connect(m_canvas, &SlideCanvas::targetRequested, this,
           [this](int want) { emit targetRequested(m_axis, want); });

   auto mk = [this](const QString &t)
   {
      QLabel *l = new QLabel(t, this);
      l->setFont(QFont("Consolas"));
      return l;
   };
   m_lPos   = mk(QStringLiteral("—"));
   m_lTgt   = mk(QStringLiteral("—"));
   m_lWant  = mk(QStringLiteral("—"));
   m_lState = mk(QStringLiteral("—"));
   m_lBus   = mk(QStringLiteral("—"));

   QGridLayout *g = new QGridLayout;
   g->addWidget(new QLabel(QStringLiteral("速度"), this), 0, 0);
   g->addWidget(m_vel,                                   0, 1);
   g->addWidget(m_velBox,                                0, 2);
   g->addWidget(m_zero,                                  1, 0);
   g->addWidget(m_center,                                1, 1, 1, 2);
   g->setColumnStretch(1, 1);

   QGridLayout *rd = new QGridLayout;
   const char *names[5] = {"实际", "下发", "目标", "状态", "总线"};
   QLabel *labels[5] = {m_lPos, m_lTgt, m_lWant, m_lState, m_lBus};
   for (int i = 0; i < 5; i++)
   {
      rd->addWidget(new QLabel(QString::fromUtf8(names[i]), this), i / 3, (i % 3) * 2);
      rd->addWidget(labels[i],                                    i / 3, (i % 3) * 2 + 1);
   }

   QVBoxLayout *v = new QVBoxLayout(this);
   QLabel *title = new QLabel(QStringLiteral("轴 %1").arg(m_axis), this);
   QFont tf = title->font();
   tf.setBold(true);
   title->setFont(tf);
   v->addWidget(title);
   v->addWidget(m_canvas, 1);
   v->addLayout(g);
   v->addLayout(rd);

   m_canvas->setLive(false);
   m_canvas->setMovable(false);
}

uint32_t AxisPanel::speed() const { return (uint32_t)m_vel->value(); }

void AxisPanel::setSpeed(uint32_t vel)
{
   m_vel->setValue((int)vel);   /* 滑块变了会连带发 speedChanged */
}

void AxisPanel::refresh(const AxisTelem &t)
{
   bool live    = t.valid && t.mirror_ok;
   bool movable = live && t.enabled;

   m_canvas->setLive(live);
   m_canvas->setMovable(movable);

   if (!live)
   {
      m_lPos->setText(QStringLiteral("—"));
      m_lTgt->setText(QStringLiteral("—"));
      m_lWant->setText(QStringLiteral("—"));
      m_lState->setText(t.valid ? QStringLiteral("等完整帧")
                                : QStringLiteral("—"));
      m_lBus->setText(QStringLiteral("—"));
      return;
   }

   m_canvas->setPos(t.pos);
   m_canvas->setTgt(t.tgt);
   m_canvas->setWant(t.want);

   m_lPos->setText(QString::number(t.pos));
   m_lTgt->setText(QString::number(t.tgt));
   m_lWant->setText(QString::number(t.want));

   /* 走没走完用下发目标判: CSP 下驱动器没有"到位"信号可等 */
   QString st = t.state;
   if (t.fault)
      /* 6041h bit3 只说"有故障", **是哪一种看 603Fh** (过流/过压/欠压/动力线/通讯/传感器
       * 的处置办法完全不搭界)。它是 SDO 读的, 所以故障沿那一拍还没到 —— 那两种情况
       * fault_code_text 自己会说清 ("还没读到" / "读不到"), 不会编一个码出来 */
      st = QStringLiteral("故障: ") + st + QStringLiteral(" · 故障码 ")
             + ecatcmd::fault_code_text(t.fault_code);
   else if (t.enabled && !t.at_target)
      st += QStringLiteral(" · 运动中");
   else if (t.enabled)
      st += QStringLiteral(" · 已到位");
   m_lState->setText(st);

   m_lBus->setText(QStringLiteral("帧 %1").arg(t.frames));
}
