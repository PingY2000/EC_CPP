#include "mapcanvas.h"

#include "scancontroller.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace scan {

/* ---------------------------------------------------------------- 配色 */

/* 与 hmi/axispanel.cpp / main.cpp 里那套是同一套, 照抄过来而不是 include ——
 * 那是个 .cpp 里的局部变量, 抽出来要动 hmi/, 而这一版的原则是 hmi 一个字不改。 */
static const QColor C_BG     ("#15181e");   /* 画布底色, 比面板 #1f232a 再深一点 */
static const QColor C_AREA   ("#3c434e");   /* 扫描区域框 */
static const QColor C_GRID   ("#23272f");   /* 网格线, 要很轻 */
static const QColor C_AXIS   ("#6b7480");   /* 零线 / 刻度 */
static const QColor C_TEXT   ("#c8ced8");
static const QColor C_MUTED  ("#6b7480");
static const QColor C_WANT   ("#ffb020");   /* 点击目标 (与 hmi 同义) */
static const QColor C_TGT    ("#33d17a");   /* 本周期下发的插值目标 (与 hmi 同义) */
static const QColor C_CUR    ("#ffffff");   /* 实测位置: 见下 */
static const QColor C_FAIL   ("#d03b3b");   /* 采失败的那一格 */
static const QColor C_SEL    ("#c8ced8");   /* Shift 选中的格 */

/*
 * 实测位置在 hmi 里是蓝色的 (#4a9eff)。**这里刻意换成白色。**
 *
 * 因为这块画布上蓝色已经是**数据**了 (热力图就是蓝色顺序色标)。位置标记再用蓝色,
 * 就变成"一个中值的格子和滑台现在在哪儿"分不清 —— 而这两个意思是完全不同的东西。
 * 白色在所有色阶上都最跳, 也正是"现场光标"该有的样子。
 * hmi 那边画布上没有数据, 所以它用蓝色没问题, 不必跟着改。
 */
static const QColor C_POS    ("#ffffff");

/* ---------------------------------------------------------------- OKLab */

/* Björn Ottosson 的 OKLab。用它插值是因为 sRGB 直插在中段会塌下去 (发灰发暗),
 * 一条本该均匀的色带看上去像被分成了几节。 */
struct Lab { double L, a, b; };

static double s2l(double c)
{
   return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

static double l2s(double c)
{
   if (c <= 0.0031308)
      return c * 12.92;
   double v = 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
   return (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
}

static Lab toLab(const QColor &c)
{
   const double r = s2l(c.redF()), g = s2l(c.greenF()), b = s2l(c.blueF());

   const double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
   const double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
   const double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

   const double l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);

   Lab o;
   o.L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
   o.a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
   o.b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
   return o;
}

static QColor fromLab(const Lab &v)
{
   const double l_ = v.L + 0.3963377774 * v.a + 0.2158037573 * v.b;
   const double m_ = v.L - 0.1055613458 * v.a - 0.0638541728 * v.b;
   const double s_ = v.L - 0.0894841775 * v.a - 1.2914855480 * v.b;

   const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;

   const double r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
   const double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
   const double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

   QColor out;
   out.setRgbF(l2s(r), l2s(g), l2s(b));
   return out;
}

/* ---------------------------------------------------------------- 顺序色标 */

/*
 * 蓝色的 13 级 (100 -> 700)。这一组是**校验过的**: 明度严格单调、色相跨度 4°,
 * 也就是"读起来确实是一个从暗到亮的量, 而不是两种颜色混在一起"。
 *
 * 顺序是**由暗到亮**(数组第 0 项最暗), 因为底色是暗的 —— 近零要退回底色去,
 * 大的值才亮起来。色标条上会写死数值, 这就是暗端对比度低 (1.5:1) 的兜底。
 */
static const char *RAMP_HEX[] = {
   "#0d366b", "#104281", "#184f95", "#1c5cab", "#256abf", "#2a78d6", "#3987e5",
   "#5598e7", "#6da7ec", "#86b6ef", "#9ec5f4", "#b7d3f6", "#cde2fb"
};
static const int RAMP_N = (int)(sizeof(RAMP_HEX) / sizeof(RAMP_HEX[0]));

/* ---------------------------------------------------------------- 构造 */

MapCanvas::MapCanvas(QWidget *parent) : QWidget(parent)
{
   setMinimumSize(320, 320);
   setMouseTracking(true);
   setAutoFillBackground(false);
   setFocusPolicy(Qt::StrongFocus);
   rebuildColors();
}

void MapCanvas::rebuildColors()
{
   Lab stops[RAMP_N];
   for (int i = 0; i < RAMP_N; i++)
      stops[i] = toLab(QColor(RAMP_HEX[i]));

   for (int k = 0; k < 256; k++)
   {
      const double t = (double)k / 255.0;
      const double s = t * (double)(RAMP_N - 1);

      int i0 = (int)std::floor(s);
      if (i0 > RAMP_N - 2) i0 = RAMP_N - 2;
      const double f = s - (double)i0;

      Lab v;
      v.L = stops[i0].L + (stops[i0 + 1].L - stops[i0].L) * f;
      v.a = stops[i0].a + (stops[i0 + 1].a - stops[i0].a) * f;
      v.b = stops[i0].b + (stops[i0 + 1].b - stops[i0].b) * f;

      m_ramp[k] = fromLab(v);
   }
}

void MapCanvas::setController(ScanController *c)
{
   m_ctl = c;
   m_geo_done = false;
   update();
}

void MapCanvas::setBus(BusView *bus)
{
   m_bus = bus;
}

void MapCanvas::setShadeRange(double lo, double hi)
{
   m_shade_lo = lo;
   m_shade_hi = hi;
   update();
}

bool MapCanvas::fitShadeToData()
{
   if (m_ctl == nullptr)
      return false;

   double lo = 0.0, hi = 0.0;
   if (!m_ctl->wattsRange(&lo, &hi))
      return false;

   if (hi <= lo)
   {
      /* 全都一样 —— 给一个以它为中心的小窗口, 免得整片都是同一个颜色看不出结构 */
      const double pad = (std::fabs(lo) > 1e-9) ? std::fabs(lo) * 0.05 : 0.5;
      lo -= pad;
      hi += pad;
   }
   setShadeRange(lo, hi);
   return true;
}

QColor MapCanvas::shadeOf(double watts) const
{
   const double span = m_shade_hi - m_shade_lo;
   if (!(span > 0.0))
      return m_ramp[160];                 /* 色阶没定好时不要除零, 给一条中间色 */

   double t = (watts - m_shade_lo) / span;
   if (t < 0.0) t = 0.0;                  /* 超出范围夹到两端; 数值仍可在悬停里读到 */
   if (t > 1.0) t = 1.0;

   return m_ramp[(int)std::lround(t * 255.0)];
}

void MapCanvas::clearSelection()
{
   m_sel_ix = m_sel_iy = -1;
   update();
}

/* ---------------------------------------------------------------- 坐标 */

double MapCanvas::viewHalfUnits() const
{
   double ax = 27.0, ay = 27.0;
   if (m_ctl != nullptr)
   {
      ax = m_ctl->params().area_x_unit;
      ay = m_ctl->params().area_y_unit;
   }
   /* 区域各留 12% 的边, 于是区域框不会贴着画布边 */
   return std::max(ax, ay) * 0.5 * 1.12 + 0.5;
}

QRectF MapCanvas::plotRect() const
{
   /* 色标条占右边 74 px; 剩下的取正方形, 免得 x/y 比例不一样 (那会让距离骗人) */
   const double strip = 74.0;
   const double w = (double)width() - strip - 16.0;
   const double h = (double)height() - 40.0;
   const double side = std::min(w, h);
   if (side < 40.0)
      return QRectF(0, 0, 0, 0);

   return QRectF(8.0, 8.0, side, side);
}

QPointF MapCanvas::pxOf(double xu, double yu) const
{
   const QRectF r = plotRect();
   const double half = viewHalfUnits();
   const double k = r.width() / (2.0 * half);

   return QPointF(r.center().x() + xu * k,
                  r.center().y() - yu * k);   /* Y 轴向上, 屏幕坐标向下 */
}

void MapCanvas::unitAt(const QPoint &p, double *xu, double *yu) const
{
   const QRectF r = plotRect();
   const double half = viewHalfUnits();
   const double k = (r.width() > 0.0) ? (r.width() / (2.0 * half)) : 1.0;

   if (xu != nullptr) *xu = (p.x() - r.center().x()) / k;
   if (yu != nullptr) *yu = (r.center().y() - p.y()) / k;
}

/* ---------------------------------------------------------------- 缓存 */

void MapCanvas::rebuildImage()
{
   const int nx = m_ctl->gridNx();
   const int ny = m_ctl->gridNy();
   if (nx <= 0 || ny <= 0)
   {
      m_img = QImage();
      return;
   }

   m_img = QImage(nx, ny, QImage::Format_ARGB32_Premultiplied);
   m_img.fill(Qt::transparent);

   for (int iy = 0; iy < ny; iy++)
   {
      for (int ix = 0; ix < nx; ix++)
      {
         /* 行 0 是**最下面的**那一行 (y 最小的单位), 而 QImage 第 0 行在最上面 */
         const int row = ny - 1 - iy;

         if (!m_ctl->cellDone(ix, iy))
            continue;                      /* 还没扫到 = 只有底色 */

         if (m_ctl->cellHasValue(ix, iy))
            m_img.setPixelColor(ix, row, shadeOf(m_ctl->cellValue(ix, iy)));
         else
            m_img.setPixelColor(ix, row, C_FAIL);   /* 扫了但没采到数 */
      }
   }
}

void MapCanvas::rebuildPath()
{
   m_path = QPainterPath();
   if (m_ctl == nullptr || m_ctl->totalPoints() <= 0)
      return;

   const Point *p0 = m_ctl->pointAt(0);
   if (p0 == nullptr)
      return;

   m_path.moveTo(pxOf(p0->x_unit, p0->y_unit));
   for (int k = 1; k < m_ctl->totalPoints(); k++)
   {
      const Point *p = m_ctl->pointAt(k);
      if (p != nullptr)
         m_path.lineTo(pxOf(p->x_unit, p->y_unit));
   }
}

/* ---------------------------------------------------------------- 绘制 */

void MapCanvas::paintEvent(QPaintEvent *)
{
   QPainter p(this);
   p.fillRect(rect(), C_BG);

   if (m_ctl == nullptr)
   {
      p.setPen(C_MUTED);
      p.drawText(rect(), Qt::AlignCenter, QStringLiteral("没有扫描控制器"));
      return;
   }

   /* 路径只在几何变时才重建 —— 它跟数据无关 */
   if (!m_geo_done
       || m_c_nx != m_ctl->gridNx() || m_c_ny != m_ctl->gridNy()
       || m_c_pts != m_ctl->totalPoints())
   {
      m_c_nx = m_ctl->gridNx();
      m_c_ny = m_ctl->gridNy();
      m_c_pts = m_ctl->totalPoints();
      m_geo_done = true;
      rebuildPath();
   }

   /* 热力图每帧重建: 3025 个像素, 比一次 QPainter 填充还便宜, 不值得为它做脏标记 */
   rebuildImage();

   const QRectF r = plotRect();
   if (r.width() < 40.0)
      return;

   if (m_shade_hi > m_shade_lo)
      drawHeat(p);
   drawGrid(p);
   drawPath(p);
   drawMarkers(p);
   drawScaleBar(p);
   drawHud(p);
}

void MapCanvas::drawHeat(QPainter &p)
{
   if (m_img.isNull())
      return;

   const int nx = m_ctl->gridNx();
   const int ny = m_ctl->gridNy();
   const double res = m_ctl->params().res_unit;

   /*
    * 每一格是 res×res 的方块, **以网格点为中心** —— 所以整块热力图比网格点跨距
    * 两头各多出半格。和网格线对齐: 网格点画在格子的中心, 而不是格子的角。
    */
   const double half_x = (double)nx * res / 2.0;
   const double half_y = (double)ny * res / 2.0;

   const QPointF tl = pxOf(-half_x,  half_y);
   const QPointF br = pxOf( half_x, -half_y);
   const QRectF dst(tl, br);

   p.setRenderHint(QPainter::SmoothPixmapTransform, false);
   p.drawImage(dst, m_img);     /* 放大用最近邻: 一格就是一个方块, 不该被糊成渐变 */
}

void MapCanvas::drawGrid(QPainter &p)
{
   const Params &q = m_ctl->params();

   /* 零点十字 */
   p.setPen(QPen(C_AXIS, 1, Qt::DashLine));
   const QPointF c = pxOf(0.0, 0.0);
   const QRectF r = plotRect();
   p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
   p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));

   /* 每 5 单位一条淡网格线 */
   p.setPen(QPen(C_GRID, 1));
   const double half = viewHalfUnits();
   for (double u = -std::floor(half / 5.0) * 5.0; u <= half; u += 5.0)
   {
      if (std::fabs(u) < 1e-9)
         continue;
      const QPointF a = pxOf(u, -half), b = pxOf(u, half);
      p.drawLine(QPointF(a.x(), r.top()), QPointF(b.x(), r.bottom()));
      const QPointF d = pxOf(-half, u), e = pxOf(half, u);
      p.drawLine(QPointF(r.left(), d.y()), QPointF(r.right(), e.y()));
   }

   /* 扫描区域框 */
   QRectF area(pxOf(-q.area_x_unit / 2.0,  q.area_y_unit / 2.0),
               pxOf( q.area_x_unit / 2.0, -q.area_y_unit / 2.0));
   area = area.normalized();

   /* 超出量程的部分会被 EcatThread 静默夹掉 —— 标红, 让人一眼看见 */
   bool clipped = false;
   if (m_bus != nullptr)
   {
      const BusTelem t = m_bus->telemetry();
      const double rng_u = (q.pulses_per_unit > 0.0)
                              ? (double)t.range / q.pulses_per_unit : 0.0;
      if (t.range > 0
          && (q.area_x_unit / 2.0 > rng_u || q.area_y_unit / 2.0 > rng_u))
         clipped = true;
   }

   p.setBrush(Qt::NoBrush);
   p.setPen(QPen(clipped ? C_FAIL : C_AREA, clipped ? 2 : 1));
   p.drawRect(area);

   /* 刻度: 角上标单位数 */
   p.setPen(C_MUTED);
   QFont f = p.font();
   f.setPointSizeF(7.5);
   p.setFont(f);
   const QString lab = QStringLiteral("±%1 单位").arg(half, 0, 'f', 1);
   p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), 14),
              Qt::AlignHCenter | Qt::AlignTop, lab);

   if (clipped)
   {
      p.setPen(C_FAIL);
      p.drawText(QRectF(r.left(), r.top() - 2, r.width(), 14),
                 Qt::AlignHCenter | Qt::AlignBottom,
                 QStringLiteral("区域超出量程 —— 边缘会被静默夹掉"));
   }
}

void MapCanvas::drawPath(QPainter &p)
{
   if (m_path.isEmpty() || m_ctl->totalPoints() < 2)
      return;

   /* 预览线要很淡: 它的作用是"告诉你待会儿怎么走", 不该压过热力图 */
   p.setBrush(Qt::NoBrush);
   p.setPen(QPen(QColor(107, 116, 128, 150), 1));
   p.drawPath(m_path);
}

void MapCanvas::drawMarkers(QPainter &p)
{
   const Point *cur = m_ctl->currentPoint();

   /* 当前扫描点: 白色空心方框 + 十字, 位置来自网格 (即"应该到的位置") */
   if (cur != nullptr)
   {
      const QPointF q = pxOf(cur->x_unit, cur->y_unit);
      const double h = std::max(5.0, plotRect().width() / (2.0 * viewHalfUnits())
                                          * m_ctl->params().res_unit * 0.45);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(C_CUR, 1.5));
      p.drawRect(QRectF(q.x() - h, q.y() - h, 2 * h, 2 * h));
   }

   /* Shift 选中的格 */
   if (m_sel_ix >= 0 && m_sel_iy >= 0)
   {
      const double res = m_ctl->params().res_unit;
      const double span = ((double)m_ctl->gridNx() - 1.0) * res;
      const double yspan = ((double)m_ctl->gridNy() - 1.0) * res;
      const double xu = -span / 2.0 + (double)m_sel_ix * res;
      const double yu = -yspan / 2.0 + (double)m_sel_iy * res;

      const QPointF q = pxOf(xu, yu);
      const double h = std::max(6.0, plotRect().width() / (2.0 * viewHalfUnits()) * res * 0.5);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(C_SEL, 2));
      p.drawRect(QRectF(q.x() - h, q.y() - h, 2 * h, 2 * h));

      if (m_ctl->cellHasValue(m_sel_ix, m_sel_iy))
      {
         p.setPen(C_TEXT);
         QFont f = p.font();
         f.setPointSizeF(8.0);
         p.setFont(f);
         p.drawText(QRectF(q.x() - 60, q.y() - h - 18, 120, 16), Qt::AlignCenter,
                    QStringLiteral("%1").arg(m_ctl->cellValue(m_sel_ix, m_sel_iy), 0, 'g', 6));
      }
   }

   if (m_bus == nullptr)
      return;

   const BusTelem t = m_bus->telemetry();
   const double ppu = m_ctl->params().pulses_per_unit;
   if (!(ppu > 0.0))
      return;

   /* 插值目标 tgt: 绿色虚线十字 (与 hmi 的一维轨道同义) */
   {
      const QPointF q = pxOf((double)t.ax[0].tgt / ppu, (double)t.ax[1].tgt / ppu);
      p.setPen(QPen(C_TGT, 1, Qt::DashLine));
      p.drawLine(QPointF(q.x() - 8, q.y()), QPointF(q.x() + 8, q.y()));
      p.drawLine(QPointF(q.x(), q.y() - 8), QPointF(q.x(), q.y() + 8));
   }

   /* 点击目标 want: 橙色小三角朝下 */
   {
      const QPointF q = pxOf((double)t.ax[0].want / ppu, (double)t.ax[1].want / ppu);
      QPolygonF tri;
      tri << QPointF(q.x(), q.y() + 3)
          << QPointF(q.x() - 6, q.y() - 8)
          << QPointF(q.x() + 6, q.y() - 8);
      p.setBrush(C_WANT);
      p.setPen(Qt::NoPen);
      p.drawPolygon(tri);
   }

   /* 实测位置: 白点 + 一圈深色描边, 于是压在任何一档颜色上都看得见 */
   {
      const QPointF q = pxOf((double)t.ax[0].pos / ppu, (double)t.ax[1].pos / ppu);

      const double R = 5.0;
      p.setPen(Qt::NoPen);

      QRadialGradient g(q, R * 2.6);
      g.setColorAt(0.0, QColor(255, 255, 255, 90));
      g.setColorAt(1.0, QColor(255, 255, 255, 0));
      p.setBrush(g);
      p.drawEllipse(q, R * 2.6, R * 2.6);

      p.setPen(QPen(QColor(21, 24, 30, 220), 2));
      p.setBrush(C_POS);
      p.drawEllipse(q, R, R);
   }
}

void MapCanvas::drawScaleBar(QPainter &p)
{
   const QRectF r = plotRect();
   const int x0 = (int)r.right() + 14;
   const int h  = std::min((int)r.height(), 240);
   const int y0 = (int)(r.center().y() - h / 2.0);
   const int w  = 14;

   /* 从亮到暗往下排 —— 上面是大的值 */
   for (int i = 0; i < h; i++)
   {
      const double t = 1.0 - (double)i / (double)(h - 1);
      const int k = (int)std::lround(std::max(0.0, std::min(1.0, t)) * 255.0);
      p.setPen(m_ramp[k]);
      p.drawLine(x0, y0 + i, x0 + w, y0 + i);
   }
   p.setBrush(Qt::NoBrush);
   p.setPen(QPen(C_AREA, 1));
   p.drawRect(QRectF(x0, y0, w, h));

   /*
    * **数字必须有。** 色标暗的那一端跟底色只有 1.5:1, 光看颜色分不出"最小"和"没数据" ——
    * 数字就是这条色标的兜底, 不是装饰。
    */
   QFont f = p.font();
   f.setPointSizeF(7.5);
   p.setFont(f);
   p.setPen(C_TEXT);

   const QString hi_s = QStringLiteral("%1").arg(m_shade_hi, 0, 'g', 4);
   const QString lo_s = QStringLiteral("%1").arg(m_shade_lo, 0, 'g', 4);
   const QString mid  = QStringLiteral("%1").arg((m_shade_lo + m_shade_hi) / 2.0, 0, 'g', 4);

   p.drawText(QRect(x0 + w + 3, y0 - 8, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, hi_s);
   p.drawText(QRect(x0 + w + 3, y0 + h / 2 - 7, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, mid);
   p.drawText(QRect(x0 + w + 3, y0 + h - 6, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, lo_s);
   p.setPen(C_MUTED);
   p.drawText(QRect(x0 - 20, y0 + h + 6, 70, 14), Qt::AlignLeft | Qt::AlignTop,
              QStringLiteral("功率(锁定)"));
}

void MapCanvas::drawHud(QPainter &p)
{
   QFont f = p.font();
   f.setPointSizeF(8.0);
   p.setFont(f);

   /* 左上: 扫描进度 */
   p.setPen(C_TEXT);
   const QString prog = QStringLiteral("%1 / %2 点   剩 %3")
                           .arg(m_ctl->completedPoints())
                           .arg(m_ctl->totalPoints())
                           .arg(m_ctl->pendingPoints());
   p.drawText(QRect(10, 8, 260, 15), Qt::AlignLeft | Qt::AlignVCenter, prog);

   p.setPen(C_MUTED);
   p.drawText(QRect(10, 23, 300, 15), Qt::AlignLeft | Qt::AlignVCenter,
              m_ctl->stateText());

   /* 左下: 悬停读数 (单位 + 脉冲) */
   if (m_hover && m_ctl != nullptr)
   {
      const double ppu = m_ctl->params().pulses_per_unit;
      const QString s = QStringLiteral("(%1, %2) 单位 = (%3, %4) pul")
                           .arg(m_hover_xu, 0, 'f', 2).arg(m_hover_yu, 0, 'f', 2)
                           .arg((long long)std::llround(m_hover_xu * ppu))
                           .arg((long long)std::llround(m_hover_yu * ppu));
      p.setPen(C_TEXT);
      p.drawText(QRect(10, height() - 40, width() - 20, 15),
                 Qt::AlignLeft | Qt::AlignVCenter, s);
   }

   /* 右下: 操作提示。Shift 重测这种不写出来就没人知道 */
   p.setPen(m_manual_ok ? C_MUTED : C_WANT);
   const QString hint = m_manual_ok
      ? QStringLiteral("左键 = 手动定位    Shift+左键 = 选中该格")
      : QStringLiteral("扫描进行中 —— 先「中止」才能手动控制");
   p.drawText(QRect(10, height() - 22, width() - 20, 15),
              Qt::AlignLeft | Qt::AlignVCenter, hint);
}

/* ---------------------------------------------------------------- 交互 */

void MapCanvas::mousePressEvent(QMouseEvent *e)
{
   if (m_ctl == nullptr || e->button() != Qt::LeftButton)
      return;

   const QPoint px = e->position().toPoint();

   /* **扫描中一律吞掉。** 手动插一脚的话, 那一点的数据说不清是哪来的 */
   if (!m_manual_ok)
      return;

   double xu = 0.0, yu = 0.0;
   unitAt(px, &xu, &yu);

   const double half = viewHalfUnits();
   if (std::fabs(xu) > half || std::fabs(yu) > half)
      return;

   if (e->modifiers() & Qt::ShiftModifier)
   {
      /* 就近吸附到网格点。落在半格之外就不选 —— 免得选中一个离得很远的格子 */
      const Params &q = m_ctl->params();
      const double res = q.res_unit;
      const int nx = m_ctl->gridNx();
      const int ny = m_ctl->gridNy();
      if (res > 0.0 && nx > 0 && ny > 0)
      {
         const double span_x = ((double)nx - 1.0) * res;
         const double span_y = ((double)ny - 1.0) * res;

         const int ix = (int)std::lround((xu + span_x / 2.0) / res);
         const int iy = (int)std::lround((yu + span_y / 2.0) / res);

         if (ix >= 0 && ix < nx && iy >= 0 && iy < ny)
         {
            const double gx = -span_x / 2.0 + (double)ix * res;
            const double gy = -span_y / 2.0 + (double)iy * res;
            if (std::fabs(gx - xu) <= res * 0.5 && std::fabs(gy - yu) <= res * 0.5)
            {
               m_sel_ix = ix;
               m_sel_iy = iy;
               update();
               emit cellPicked(ix, iy);
            }
         }
      }
      return;
   }

   const double ppu = m_ctl->params().pulses_per_unit;
   if (!(ppu > 0.0))
      return;

   emit manualMove((int32_t)std::llround(xu * ppu), (int32_t)std::llround(yu * ppu));
}

void MapCanvas::mouseMoveEvent(QMouseEvent *e)
{
   m_hover = true;
   m_hover_px = e->position().toPoint();
   unitAt(m_hover_px, &m_hover_xu, &m_hover_yu);
   update();
}

void MapCanvas::leaveEvent(QEvent *)
{
   m_hover = false;
   update();
}

}   /* namespace scan */
