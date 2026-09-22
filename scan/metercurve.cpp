#include "metercurve.h"

#include "axisutil.h"
#include "meterlog.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace scan {

/* 与 MapCanvas 同一个色系 (那边是 mapcanvas.cpp 顶上那一块)。两边分开定义是有意的:
 * 那是"两张图各自的用色", 抽出来只会多一层间接 */
namespace
{
const char *C_BG    = "#15181e";
const char *C_GRID  = "#23272f";
const char *C_AXIS  = "#6b7480";
const char *C_TEXT  = "#c8ced8";
const char *C_MUTED = "#6b7480";
const char *C_LINE  = "#33d17a";   /* 有效读数 */
const char *C_GAP   = "#d03b3b";   /* ok=false 的那一段 */
}   /* namespace */

MeterCurve::MeterCurve(QWidget *parent) : QWidget(parent)
{
   /* 它排在参数栏里 (那一列最窄 320), 所以高度要小 —— 但矮于 130 就只剩一条线了 */
   setMinimumHeight(150);
   setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MeterCurve::setLog(MeterLog *log)
{
   m_log = log;
   update();
}

void MeterCurve::setPlaceholder(const QString &s)
{
   m_placeholder = s;
   update();
}

void MeterCurve::setSourceName(const QString &s)
{
   m_src_name = s;
   update();
}

void MeterCurve::setUnit(const QString &u)
{
   if (u == m_unit)
      return;

   m_unit = u;

   /* 「单位不明」那句话只说在 tooltip 里: 图顶上那一行很窄 (参数栏就 320 宽), 而一个 "?"
    * 已经够让人停下来问一句"这是什么单位"了 —— 那正是要的效果 */
   setToolTip(u.isEmpty()
      ? QStringLiteral(
           "纵轴的单位**不明**。\n"
           "真机报的是 W 还是 J, 由探头与测量模式决定, 而 Ophir 的 COM 接口一个字段都不给: "
           "打开设备后是按设备自己报的探头类型与模式名判的, 判不出来就画 「?」 (见 "
           "powermeter.h 的 unit())。\n"
           "模拟源 (手动 / 随机 / 脚本) 按定义是 W。")
      : QStringLiteral("纵轴: %1").arg(u));

   update();
}

QSize MeterCurve::sizeHint() const
{
   return QSize(300, 170);
}

/*
 * 一张读数折线。横轴是"相对第一笔的秒数", 纵轴自动 (值域只看 ok 的那些)。
 * 纵轴那个单位字由外面给 (setUnit): 真机报的可能是 J, 认不出来时画 "?"。
 */
void MeterCurve::paintEvent(QPaintEvent *)
{
   QPainter p(this);
   p.setRenderHint(QPainter::Antialiasing, true);
   p.fillRect(rect(), QColor(C_BG));

   QFont small = p.font();
   small.setPointSizeF(7.5);
   p.setFont(small);

   /* 左边留给纵轴数字。这一格是按**这一列的宽度**算的, 不写死 —— 参数栏最窄 320, 拉宽了也
    * 只到 400 上下, 而量程从 nW 到 W, 数字位数差得很远 ("1e-09" 与 "1"). 18% 宽加 34 的下限
    * 在两头都够用, 又不会把绘图区挤成一条 */
   const double gutter = std::min(62.0, std::max(34.0, width() * 0.18));
   const QRectF r(gutter, 20.0, width() - gutter - 10.0, height() - 20.0 - 24.0);

   if (r.width() < 40.0 || r.height() < 30.0)
      return;

   const QVector<MeterLog::Sample> empty;
   const QVector<MeterLog::Sample> &v = (m_log != nullptr) ? m_log->samples() : empty;

   if (v.size() < 2)
   {
      p.setPen(QColor(C_MUTED));
      p.drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter,
                 m_placeholder.isEmpty() ? QStringLiteral("还没有数") : m_placeholder);
      return;
   }

   /* 值域只看 ok 的那些 —— 失败那一笔没有 watts, 拿它参与定标会把整条线压平 */
   double lo = 0.0, hi = 0.0;
   bool   any = false;
   for (const MeterLog::Sample &s : v)
   {
      if (!s.ok)
         continue;
      if (!any)
      {
         lo = hi = s.watts;
         any = true;
         continue;
      }
      lo = std::min(lo, s.watts);
      hi = std::max(hi, s.watts);
   }

   if (!any)
   {
      p.setPen(QColor(C_MUTED));
      p.drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter,
                 QStringLiteral("这一段一个数都没读回来"));
      return;
   }

   const int64_t t0 = v.first().ms;
   const int64_t t1 = v.last().ms;
   const double  span_t = (double)(t1 - t0);

   /* 纵轴上下各留 8% 余量: 一条平线贴着边框画出来像是坏了 */
   double pad = (hi - lo) * 0.08;
   if (!(pad > 0.0))
      pad = (std::fabs(hi) > 1e-12) ? std::fabs(hi) * 0.08 : 1e-9;
   lo -= pad;
   hi += pad;

   const double span_v = hi - lo;

   auto px = [&](int64_t ms) {
      return r.left() + (span_t > 0.0 ? (double)(ms - t0) / span_t : 0.0) * r.width();
   };
   auto py = [&](double w) {
      return r.bottom() - (w - lo) / span_v * r.height();
   };

   /* ---- 网格与刻度 ---- */
   const double vstep = niceStep(span_v, 4);
   if (vstep > 0.0)
   {
      for (double w = std::ceil(lo / vstep) * vstep; w <= hi; w += vstep)
      {
         const double y = py(w);
         p.setPen(QColor(C_GRID));
         p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
         p.setPen(QColor(C_MUTED));
         p.drawText(QRectF(0, y - 8, gutter - 5, 16), Qt::AlignRight | Qt::AlignVCenter,
                    QString::number(w, 'g', 3));
      }
   }

   const double tstep = niceStep(span_t / 1000.0, 4);   /* 秒 */
   if (tstep > 0.0)
   {
      for (double s = 0.0; s <= span_t / 1000.0 + 1e-9; s += tstep)
      {
         const double x = r.left() + (span_t > 0.0 ? (s * 1000.0) / span_t : 0.0) * r.width();
         p.setPen(QColor(C_GRID));
         p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
         p.setPen(QColor(C_MUTED));
         p.drawText(QRectF(x - 26, r.bottom() + 3, 52, 16), Qt::AlignCenter,
                    QString::number(s, 'g', 3));
      }
   }

   p.setPen(QColor(C_AXIS));
   p.drawLine(QPointF(r.left(), r.top()), QPointF(r.left(), r.bottom()));
   p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.bottom()));

   p.setPen(QColor(C_MUTED));
   p.drawText(QRectF(r.right() - 60, r.bottom() + 3, 60, 16), Qt::AlignRight,
              QStringLiteral("秒"));
   if (!m_src_name.isEmpty())
      p.drawText(QRectF(r.left(), 1, r.width(), 16), Qt::AlignLeft | Qt::AlignVCenter,
                 QStringLiteral("%1   纵轴: %2")
                    .arg(m_src_name, m_unit.isEmpty() ? QStringLiteral("?") : m_unit));

   /* ---- 折线 ---- */
   p.setClipRect(r);

   QPainterPath path;
   bool started = false;
   for (const MeterLog::Sample &s : v)
   {
      if (!s.ok)
      {
         /* 断一格: 没读到的地方不连线 (连过去等于编一个数出来) */
         started = false;
         continue;
      }

      const QPointF q(px(s.ms), py(s.watts));
      if (!started)
      {
         path.moveTo(q);
         started = true;
      }
      else
      {
         path.lineTo(q);
      }
   }

   p.setPen(QPen(QColor(C_LINE), 1.4));
   p.drawPath(path);

   /* 失败的那些画成底边上的红刻度 —— 断点本身看不出来 (两条线之间的空当可能是采样间隔) */
   p.setPen(QPen(QColor(C_GAP), 1.0));
   for (const MeterLog::Sample &s : v)
      if (!s.ok)
         p.drawLine(QPointF(px(s.ms), r.bottom() - 6), QPointF(px(s.ms), r.bottom()));

   /* 最后一个点标出来, 与下面那行"最近"对得上 */
   for (int i = v.size() - 1; i >= 0; i--)
      if (v[i].ok)
      {
         p.setPen(Qt::NoPen);
         p.setBrush(QColor(C_TEXT));
         p.drawEllipse(QPointF(px(v[i].ms), py(v[i].watts)), 2.6, 2.6);
         break;
      }

   p.setClipping(false);
}

}   /* namespace scan */
