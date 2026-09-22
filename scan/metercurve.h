/*
 * scan/metercurve.h —— 「连续读数」那条时间-功率折线。手画, 不依赖任何画图库。
 *
 * 为什么手画: 这套 MSYS2 Qt 里**没装 Qt6 Charts** (C:/msys64/ucrt64/lib/cmake/ 下没有
 * Qt6Charts*), 而 MapCanvas 已经是现成的手画范例 —— 暗色常量、niceStep 刻度、QPainter 折线
 * 那一套全在 scan/mapcanvas.cpp 里, 这里照着它写, 刻度步长与它**共用同一个**
 * scan/axisutil.h 的 niceStep()。
 *
 * 数据**不抄一份**: paintEvent 直接指着 MeterLog 那块环形缓冲画 (同一个线程, 没人会边画边改)。
 *
 * 它要 Qt6::Widgets, 所以只进 `scan` 这个目标, **不进 SCAN_COMMON_SRC** —— 那条链里的
 * scan_selftest 只链 Qt6::Core, 编不了 QWidget。
 */
#pragma once

#include <QString>
#include <QWidget>

namespace scan {

class MeterLog;

class MeterCurve : public QWidget
{
   Q_OBJECT

public:
   explicit MeterCurve(QWidget *parent = nullptr);

   /* 画谁的数据。传 nullptr = 还没有源 (画占位的那句话) */
   void setLog(MeterLog *log);
   /* 空的时候中间那句话 ("按「开始」就出曲线" / "这一段一个数都没读回来") */
   void setPlaceholder(const QString &s);
   /* 左上角那行说明 (源的名字)。空 = 不画 */
   void setSourceName(const QString &s);

   QSize sizeHint() const override;

protected:
   void paintEvent(QPaintEvent *e) override;

private:
   MeterLog *m_log = nullptr;
   QString   m_placeholder;
   QString   m_src_name;
};

}   /* namespace scan */
