/*
 * hmi/axispanel.h —— 一根轴的面板
 *
 * 两个类:
 *   SlideCanvas —— 自绘的滑台轨道, 点哪里就把 want 设成哪里
 *   AxisPanel   —— 标题 + 轨道 + 速度滑块 + 读数 + 「回中 / 设为 0」按钮
 *
 * **这里一个 motor_api 函数都不调, 也不 include ec_motor.h 以外的任何总线头。**
 * 面板只认 AxisTelem (一份显示坐标的快照) 和四个信号; 谁去碰总线是 mainwindow/ecatworker 的事。
 *
 * 不引 Qt Charts / QCustomPlot (都是 GPL) —— 一个矩形加几条线用 paintEvent 就够了。
 */
#pragma once

#include <QWidget>

#include "ecatworker.h"

class QLabel;
class QSlider;
class QSpinBox;
class QPushButton;

/* ---------------------------------------------------------------- 轨道 */

class SlideCanvas : public QWidget
{
   Q_OBJECT

public:
   explicit SlideCanvas(QWidget *parent = nullptr);

   void setPos  (int32_t p) { m_pos   = p; update(); }
   void setTgt  (int32_t t) { m_tgt   = t; update(); }
   void setWant (int32_t w) { m_want  = w; update(); }

   /* 未连接 / 未使能 / 无有效帧时画成灰的, 并把点击吞掉:
    * 让"点了没反应"看起来像**功能没开**, 而不是像程序坏了 */
   void setLive(bool live)      { m_live  = live;  update(); }
   void setMovable(bool movable){ m_movable = movable; update(); }

   /* 越界点击被夹到边界时置位, 画一个小提示 —— 夹了要说出来, 不能默默吃掉 */
   void noteClamped();

   QSize sizeHint() const override { return QSize(560, 120); }
   QSize minimumSizeHint() const override { return QSize(320, 96); }

signals:
   /* want 是**显示坐标** (已夹在 ±HMI_RANGE 内) */
   void targetRequested(int want);

protected:
   void paintEvent(QPaintEvent *e) override;
   void mousePressEvent(QMouseEvent *e) override;
   void mouseMoveEvent(QMouseEvent *e) override;
   void timerEvent(QTimerEvent *e) override;

private:
   void pick(const QPoint &pt);
   int  xOf(int32_t v) const;      /* 显示坐标 -> 像素 */
   int32_t vOf(int x) const;       /* 像素 -> 显示坐标 (不夹, 调用方夹) */

   int32_t m_pos  = 0;
   int32_t m_tgt  = 0;
   int32_t m_want = 0;
   bool    m_live    = false;
   bool    m_movable = false;
   int     m_clamped_ms = 0;
   int     m_timer = 0;
};

/* ---------------------------------------------------------------- 面板 */

class AxisPanel : public QWidget
{
   Q_OBJECT

public:
   explicit AxisPanel(int axis, QWidget *parent = nullptr);

   int axis() const { return m_axis; }

   /* 由主窗口 30Hz 喂进来 */
   void refresh(const AxisTelem &t);

   uint32_t speed() const;
   void     setSpeed(uint32_t vel);

signals:
   void targetRequested(int axis, int want);
   void zeroRequested  (int axis);
   void centerRequested(int axis);
   void speedChanged   (int axis, uint32_t vel);

private:
   int          m_axis = 0;
   SlideCanvas *m_canvas  = nullptr;
   QSlider     *m_vel     = nullptr;
   QSpinBox    *m_velBox  = nullptr;
   QPushButton *m_zero    = nullptr;
   QPushButton *m_center  = nullptr;

   QLabel *m_lPos   = nullptr;
   QLabel *m_lTgt   = nullptr;
   QLabel *m_lWant  = nullptr;
   QLabel *m_lState = nullptr;
   QLabel *m_lBus   = nullptr;
};
