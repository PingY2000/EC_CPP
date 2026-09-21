#include "mapcanvas.h"

#include "scancontroller.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QRadialGradient>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace scan {

/* ---------------------------------------------------------------- 配色 */

/* 与 hmi 那套是同一组颜色 */
static const QColor C_BG     ("#15181e");   /* 画布底色, 比面板 #1f232a 再深一点 */
static const QColor C_AREA   ("#3c434e");   /* 扫描区域框 */
static const QColor C_GRID   ("#23272f");   /* 网格线, 要很轻 */
static const QColor C_AXIS   ("#6b7480");   /* 零线 / 刻度 */
static const QColor C_TEXT   ("#c8ced8");
static const QColor C_MUTED  ("#6b7480");
static const QColor C_WANT   ("#ffb020");   /* 点击目标 (与 hmi 同义) */
static const QColor C_TGT    ("#33d17a");   /* 本周期下发的插值目标 (与 hmi 同义) */
static const QColor C_CUR    ("#ffffff");   /* 实测位置: 见下 */
static const QColor C_FAIL   ("#d03b3b");   /* 出错的红: 超量程的框 + 那行字 */
/* 「扫了但没采到数」的那一格。**故意不用上面那个红**: 色标最高档就是红的, 再来一块纯红
 * 就分不出"这一格没采到数"和"这一格功率最大"了。紫红在整个色带之外, 一眼能分。 */
static const QColor C_NODATA ("#c026d3");
static const QColor C_SEL    ("#c8ced8");   /* 左键查看选中的格 */
/* 撞限位。故意不复用 C_NODATA: 那个紫红的意思是"这一格没采到数", 这个是机械压在开关上。
 * (2026-09-21 之前这里写的是 C_FAIL —— 那时"没采到数"就是那个红; 现在那件事归 C_NODATA 了) */
static const QColor C_LIMIT  ("#ff5f5f");

/* 实测位置用白色 (hmi 里是蓝色): 这块画布上蓝色已是热力图的数据色, 位置标记再用蓝会分不清 */
static const QColor C_POS    ("#ffffff");

/* ---------------------------------------------------------------- OKLab */

/* Björn Ottosson 的 OKLab。用它插值: sRGB 直插在中段会发灰, 色带看着像分成几节。 */
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
 * 冷到暖的 13 级: 蓝 -> 青 -> 绿 -> 黄 -> 红, 值越大越红 (第 0 项最冷 = 最小)。
 *
 * 第 0 项特意压得很暗, 因为底色也是暗的 —— 近零要退回底色。
 * 换成彩虹色带之后明度不再单调 (黄色最亮, 红色反而比黄色暗), 这是彩虹色带的代价,
 * 换来的是"一眼能读出冷热"; 数值本身一直有数字兜底, 见 drawScaleBar 那行说明。
 *
 * 相邻两级色相只差十几度, 再在 OKLab 里插值, 于是色带是连续的 —— 不分成几块色卡。
 */
static const char *RAMP_HEX[] = {
   "#0b2a5c", "#124a94", "#1668c4", "#1a86d6", "#1ba3cf", "#17b894", "#22b95c",
   "#5cc02c", "#9cc71a", "#d4c913", "#f0a80d", "#ef7109", "#e42b12"
};
static const int RAMP_N = (int)(sizeof(RAMP_HEX) / sizeof(RAMP_HEX[0]));

/* ---------------------------------------------------------------- 构造 */

MapCanvas::MapCanvas(QWidget *parent) : QWidget(parent)
{
   /* 下限 240: 画布是跟着窗口长的那一半, 右边那列自己会滚 */
   setMinimumSize(240, 240);
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

bool MapCanvas::dataShadeRange(double *lo, double *hi) const
{
   if (m_ctl == nullptr || lo == nullptr || hi == nullptr)
      return false;

   if (!m_ctl->wattsRange(lo, hi))
      return false;

   if (*hi <= *lo)
   {
      /* 全都一样 (含"只采到一个点") —— 给一个以它为中心的小窗口,
       * 免得整片都是同一个颜色看不出结构 */
      const double pad = (std::fabs(*lo) > 1e-9) ? std::fabs(*lo) * 0.05 : 0.5;
      *lo -= pad;
      *hi += pad;
   }

   /* 色阶下限非负 —— 与 ScanWindow 那两个输入框同一条规矩 (下限钉在 0)。**两处必须一致**:
    * 自动跟随开着时那两个框显示的就是这里的数, 这里放一个负数出来, 框里会被夹成 0,
    * 于是"界面上的量程"和"画图用的量程"对不上。
    * 真采到全为负的数据: 加宽之后仍是负的, 那就退成 [0, 0.5] —— 那些格子一律显示成最冷
    * 那一档 (它们确实"低于量程"), 数值本身在悬停里照样读得到。 */
   if (*lo < 0.0)
   {
      *lo = 0.0;
      if (!(*hi > *lo))
         *hi = 0.5;
   }
   return true;
}

bool MapCanvas::fitShadeToData()
{
   double lo = 0.0, hi = 0.0;
   if (!dataShadeRange(&lo, &hi))
      return false;

   setShadeRange(lo, hi);
   return true;
}

void MapCanvas::setAutoFit(bool on)
{
   if (m_auto == on)
      return;

   m_auto = on;
   applyAutoFit();      /* 打开就立刻跟一次, 不等下一帧 */
   update();
}

void MapCanvas::applyAutoFit()
{
   if (!m_auto)
      return;

   double lo = 0.0, hi = 0.0;
   if (!dataShadeRange(&lo, &hi))
      return;           /* 一点数据都没有: 保持现有色阶 (也是不除零的那条路) */

   /* 值没变就不动。**这条不能省**: setShadeRange 里是 update(), 而本函数在 paintEvent
    * 里被调 —— 每帧都调一次 update() 就成了画布自己给自己排重画, 一直空转。 */
   const double eps = 1e-12 * std::max(1.0, std::fabs(hi));
   if (std::fabs(lo - m_shade_lo) <= eps && std::fabs(hi - m_shade_hi) <= eps)
      return;

   setShadeRange(lo, hi);
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

/*
 * 画布视野固定 30×30 单位 (每边 ±15 = kViewHalfUnits=16 减 1 单位余量), 不跟区域参数缩放:
 * 1 单位恒等于固定的一格, 区域框成了图里的一个量 (默认 27×27)。网格线每 5 单位一条。
 */
static const double kViewHalfUnits = 16.0;

/*
 * 画图区四周的留白 (像素)。标尺 / 色标条 / 超量程红字各占一条, 四个数写在一处 ——
 * 要往框外画东西就得从这儿拿宽度, 散在各处会画到框上去。
 */
static const double kPadL = 38.0;   /* 左边: Y 标尺的刻度与数字 */
static const double kPadR = 74.0;   /* 右边: 色标条 + 它的数字 */
/* 上边两段: 贴顶那一段是**空带**, 给浮在画布上的横幅 (见 MapCanvas::kBannerBand);
 * 它下面那 20px 才是 "区域超出量程" 那行红字与 HUD 头几行落脚的地方。 */
static const double kPadT = 20.0 + MapCanvas::kBannerBand;
static const double kPadB = 38.0;   /* 下边: X 标尺的刻度与数字, 再加那行"视野"说明 */

double MapCanvas::viewHalfUnits() const
{
   return kViewHalfUnits;
}

QRectF MapCanvas::plotRect() const
{
   /* 色标条占右边 kPadR; 剩下的取正方形, 免得 x/y 比例不一样 (那会让距离骗人) */
   const double w = (double)width()  - kPadL - kPadR - 8.0;
   const double h = (double)height() - kPadT - kPadB;
   const double side = std::min(w, h);
   if (side < 40.0)
      return QRectF(0, 0, 0, 0);

   /* 居中: 窗口一宽, 钉在左上角会让正方形画布右边剩一大块空白 */
   const double x = kPadL + std::max(0.0, (w - side) / 2.0);
   const double y = kPadT + std::max(0.0, (h - side) / 2.0);
   return QRectF(x, y, side, side);
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
            m_img.setPixelColor(ix, row, C_NODATA);   /* 扫了但没采到数 */
      }
   }
}

void MapCanvas::rebuildPath()
{
   /* 存的是单位, 不是像素 (像素由 drawPath 现算): 单位是扫描点的本来样子, 与窗口大小无关 */
   m_path = QPainterPath();
   if (m_ctl == nullptr || m_ctl->totalPoints() <= 0)
      return;

   const Point *p0 = m_ctl->pointAt(0);
   if (p0 == nullptr)
      return;

   m_path.moveTo(p0->x_unit, p0->y_unit);
   for (int k = 1; k < m_ctl->totalPoints(); k++)
   {
      const Point *p = m_ctl->pointAt(k);
      if (p != nullptr)
         m_path.lineTo(p->x_unit, p->y_unit);
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

   /* 色阶: 自动跟随模式下先把它拉到已采数据的范围, 再画 —— 一帧里图和色标条用的是同一个 */
   applyAutoFit();

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
   drawRulers(p);
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

   /* 每格是 res×res 的方块, 以网格点为中心 —— 整块比网格点跨距两头各多出半格 */
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

   /* 框下那行"视野"归 drawRulers 画 —— 它要跟刻度数字排在同一条带子里, 两处各画会叠上 */

   if (clipped)
   {
      p.setPen(C_FAIL);
      p.drawText(QRectF(r.left(), r.top() - 2, r.width(), 14),
                 Qt::AlignHCenter | Qt::AlignBottom,
                 QStringLiteral("区域超出量程 —— 边缘会被静默夹掉"));
   }
}

/*
 * 坐标标尺: 下边是 X, 左边是 Y。每 1 单位一个小刻度, 每 5 单位一个带数字的,
 * 与网格线同一组位置。手动定位是按坐标下发的, 读不出坐标就只能靠试。
 */
void MapCanvas::drawRulers(QPainter &p)
{
   static const double kTick     = 5.0;   /* 大刻度线长 */
   static const double kTickFine = 3.0;   /* 小刻度线长 */
   static const double kGap      = 2.0;   /* 刻度线到数字 */

   const QRectF r = plotRect();
   const double half = viewHalfUnits();
   const double k = r.width() / (2.0 * half);      /* 一单位多少像素 */
   if (!(k > 0.0))
      return;

   QFont f = p.font();
   f.setPointSizeF(7.0);
   p.setFont(f);

   const int  n    = (int)std::floor(half / kTick);   /* 3 -> ±15 */
   const bool fine = (k >= 7.0);   /* 1 单位的小刻度: 太挤就不画 (画布能被拖到 240px) */

   /* 小刻度先画 —— 它是背景, 不该压在大刻度和数字上 */
   if (fine)
   {
      p.setPen(C_GRID);
      const int m = (int)std::floor(half);
      for (int i = -m; i <= m; i++)
      {
         if (i % (int)kTick == 0)
            continue;                    /* 5 的倍数归大刻度 */
         const QPointF a = pxOf((double)i, 0.0);
         const QPointF b = pxOf(0.0, (double)i);
         p.drawLine(QPointF(a.x(), r.bottom()),
                    QPointF(a.x(), r.bottom() + kTickFine));
         p.drawLine(QPointF(r.left(), b.y()),
                    QPointF(r.left() - kTickFine, b.y()));
      }
   }

   for (int i = -n; i <= n; i++)
   {
      const double  u    = (double)i * kTick;
      const bool    zero = (i == 0);           /* 零点跟别的刻度分开: 它是显示坐标的原点 */
      const QPointF a = pxOf(u, 0.0);
      const QPointF b = pxOf(0.0, u);

      p.setPen(zero ? C_TEXT : C_AXIS);
      p.drawLine(QPointF(a.x(), r.bottom()), QPointF(a.x(), r.bottom() + kTick));
      p.drawLine(QPointF(r.left(), b.y()), QPointF(r.left() - kTick, b.y()));

      /* 画布小到"5 单位还不到一个数字宽"时 (k < 8), 数字隔一个画一个; 刻度线一根不少 */
      if (k >= 8.0 || i % 2 == 0)
      {
         const QString s = QString::number(u, 'f', 0);
         p.setPen(zero ? C_TEXT : C_MUTED);
         p.drawText(QRectF(a.x() - 22.0, r.bottom() + kTick + kGap, 44.0, 12.0),
                    Qt::AlignHCenter | Qt::AlignTop, s);
         p.drawText(QRectF(r.left() - kTick - 30.0, b.y() - 6.0, 30.0, 12.0),
                    Qt::AlignRight | Qt::AlignVCenter, s);
      }
   }

   /* 单位写在两根标尺交会的那个角上 —— 光有数字说不清是毫米还是脉冲 */
   p.setPen(C_MUTED);
   p.drawText(QRectF(r.left() - kTick - 30.0, r.bottom() + kTick + kGap, 30.0, 12.0),
              Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("单位"));

   /* 视野固定, 这行永远写同一个数; 区域多大写在图里那个框上 */
   p.drawText(QRectF(r.left(), r.bottom() + kTick + kGap + 14.0, r.width(), 14.0),
              Qt::AlignHCenter | Qt::AlignTop,
              QStringLiteral("视野 ±%1 单位 (固定)").arg(half, 0, 'f', 1));
}

void MapCanvas::drawPath(QPainter &p)
{
   if (m_path.isEmpty() || m_ctl->totalPoints() < 2)
      return;

   const QRectF r = plotRect();
   const double k = r.width() / (2.0 * viewHalfUnits());
   if (!(k > 0.0))
      return;

   /* 单位 -> 像素, 现算。跟 pxOf 是同一条式子, 只是包成了变换 */
   QTransform t;
   t.translate(r.center().x(), r.center().y());
   t.scale(k, -k);                      /* Y 轴向上, 屏幕坐标向下 */

   p.save();
   p.setTransform(t, true);

   /* 预览线要很淡: 它的作用是"告诉你待会儿怎么走", 不该压过热力图 */
   QPen pen(QColor(107, 116, 128, 150), 1);
   /* cosmetic 不能省: 世界变换会把线宽一起放大 k 倍 (k ≈ 20), 1px 细线会变成盖掉热力图的灰带 */
   pen.setCosmetic(true);
   p.setBrush(Qt::NoBrush);
   p.setPen(pen);
   p.drawPath(m_path);

   p.restore();
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

   /* 左键查看选中的格 */
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

      QFont f = p.font();
      f.setPointSizeF(8.0);
      p.setFont(f);

      /* 数值: 没采到也写出来 (写"未采集") —— 空着会跟"这里根本没选中"分不清 */
      const bool has = m_ctl->cellHasValue(m_sel_ix, m_sel_iy);
      p.setPen(has ? C_TEXT : C_MUTED);
      p.drawText(QRectF(q.x() - 70, q.y() - h - 18, 140, 16), Qt::AlignCenter,
                 has ? QStringLiteral("%1").arg(m_ctl->cellValue(m_sel_ix, m_sel_iy), 0, 'g', 6)
                     : QStringLiteral("未采集"));

      /* 索引 + 坐标: 「重测选中点」按的是**索引**, 得让人看见自己选中的是第几格 */
      p.setPen(C_MUTED);
      p.drawText(QRectF(q.x() - 90, q.y() + h + 2, 180, 15), Qt::AlignCenter,
                 QStringLiteral("[%1, %2]  (%3, %4) 单位")
                    .arg(m_sel_ix).arg(m_sel_iy)
                    .arg(xu, 0, 'f', 2).arg(yu, 0, 'f', 2));
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

      /* 限位判据成立 (默认 = 6041h bit11「硬件限位信号有效」): 红圈套在位置点上 + 写清是哪根轴。
       * 读的是已经算好的字段 (由 ecatcmd::limit_hit 一处算出), 不在这里再判一次 bit11。 */
      const bool lim_x = t.ax[0].limit_active;
      const bool lim_y = t.ax[1].limit_active;
      if (lim_x || lim_y)
      {
         p.setBrush(Qt::NoBrush);
         p.setPen(QPen(C_LIMIT, 2));
         p.drawEllipse(q, R + 7, R + 7);

         /* 措辞与状态栏一致: 「有效」而非「撞」—— bit11 是那路信号的电平, 不是已发生的碰撞 */
         const QString s = (lim_x && lim_y) ? QStringLiteral("X / Y 轴限位有效")
                         : lim_x           ? QStringLiteral("X 轴限位有效")
                                           : QStringLiteral("Y 轴限位有效");
         QFont f = p.font();
         f.setPointSizeF(9.0);
         f.setBold(true);
         p.setFont(f);
         p.setPen(C_LIMIT);
         p.drawText(QRectF(q.x() - 90, q.y() + R + 8, 180, 16),
                    Qt::AlignCenter, s);
      }
   }
}

/*
 * 刻度数字的步长: 从 1 / 2 / 5 × 10^k 里挑一个, 让色阶大约分成 want 段。
 * 挑"整数"是为了让人一眼读到 0.2 / 0.5 / 100 这种数 —— 按 span/5 直接切会得出
 * 0.037 之类读不出来的值, 数字一多反而更看不懂。
 */
static double niceStep(double span, int want)
{
   if (!(span > 0.0) || want < 1)
      return 0.0;

   const double raw = span / (double)want;
   const double p   = std::pow(10.0, std::floor(std::log10(raw)));
   const double m   = raw / p;             /* 落在 [1, 10) */

   const double f = (m <= 1.0) ? 1.0 : (m <= 2.0) ? 2.0 : (m <= 5.0) ? 5.0 : 10.0;
   return f * p;
}

void MapCanvas::drawScaleBar(QPainter &p)
{
   const QRectF r = plotRect();
   const int x0 = (int)r.right() + 14;
   /* 高度封在 320: 比原来的 240 高, 但**不必**跟画图区一样高 —— 图很高的时候整条拉满,
    * 色标反而长过了头。画布矮了就跟着矮, 上下居中。
    * 两头各留出一点: 两端那两个数字要写在条子外面, 顶格写会伸进画布顶上那条**横幅带**里。
    * 画布还没成型时 plotRect() 是空矩形, 直接不画 —— 数字没地方放, 条子也会是负高。 */
   const int h  = std::min((int)r.height() - 16, 320);
   const int y0 = (int)r.top() + ((int)r.height() - h) / 2;
   const int w  = 14;
   if (h < 20)
      return;

   /* 从红到蓝往下排 —— 上面是大的值 */
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

   /* 数字必须有: 色标最冷那一端跟底色只有 1.3:1, 光看颜色分不出"最小"和"没数据"。
    * 两端必标 (它们就是锁定的色阶), 中间按整数步长补 —— 于是 0..1 会出 6 个数,
    * 而不是原来那 3 个 (两端 + 中点)。 */
   const double span = m_shade_hi - m_shade_lo;

   QVector<double> marks;
   marks.append(m_shade_hi);      /* [0] 高值端 */
   marks.append(m_shade_lo);      /* [1] 低值端 —— 前两个固定是两端, 见下面的 i < 2 */
   if (span > 0.0)
   {
      const double step = niceStep(span, 5);

      bool any = false;
      /* 条子太矮就不补了: 数字会挤成一团, 那时只留两端 + 中点 (见下面的 !any) */
      if (h >= 140 && step > 0.0)
      {
         const long long k0 = (long long)std::ceil(m_shade_lo / step);
         const long long k1 = (long long)std::floor(m_shade_hi / step);
         if (k1 - k0 <= 24)
            for (long long k = k0; k <= k1; k++)
            {
               const double v = (double)k * step;
               if (v <= m_shade_lo || v >= m_shade_hi)
                  continue;            /* 两端由上面那两条管, 别画两遍 */
               marks.append(v);
               any = true;
            }
      }
      /* 一个整数刻度都落不进来 (量程特别偏/特别窄), 退回标中点 —— 有总比没有强 */
      if (!any)
         marks.append((m_shade_lo + m_shade_hi) / 2.0);
   }

   QFont f = p.font();
   f.setPointSizeF(7.5);
   p.setFont(f);

   for (int i = 0; i < marks.size(); i++)
   {
      const bool end = (i < 2);
      double t = (span > 0.0) ? (marks[i] - m_shade_lo) / span : 1.0;
      t = std::max(0.0, std::min(1.0, t));
      const int y = y0 + (int)std::lround((1.0 - t) * (double)h);

      /* 端点写得显眼 (它们是锁定色阶的那两个数), 中间刻度淡一档, 免得抢了数据本身 */
      p.setPen(end ? C_TEXT : C_MUTED);
      if (!end)
         p.drawLine(x0 + w + 1, y, x0 + w + 4, y);
      p.drawText(QRect(x0 + w + 5, y - 7, 62, 14), Qt::AlignLeft | Qt::AlignVCenter,
                 QStringLiteral("%1").arg(marks[i], 0, 'g', 4));
   }

   p.setPen(C_MUTED);
   p.drawText(QRect(x0 - 20, y0 + h + 6, 70, 14), Qt::AlignLeft | Qt::AlignTop,
              QStringLiteral("功率(锁定)"));
}

void MapCanvas::drawHud(QPainter &p)
{
   /* HUD 贴的是画图区, 不是窗口边: 按 height() 摆会压在 X 标尺的刻度数字上,
    * 宽度越出画图区则会压到右边的色标条 */
   const QRectF r = plotRect();
   const int    x = (int)r.left() + 8;
   const int    w = (int)(r.right() - x - 6);
   if (w < 40)
      return;

   QFont f = p.font();
   f.setPointSizeF(8.0);
   p.setFont(f);

   /* 左上: 扫描进度 */
   p.setPen(C_TEXT);
   const QString prog = QStringLiteral("%1 / %2 点   剩 %3")
                           .arg(m_ctl->completedPoints())
                           .arg(m_ctl->totalPoints())
                           .arg(m_ctl->pendingPoints());
   p.drawText(QRect(x, (int)r.top() + 6, 260, 15), Qt::AlignLeft | Qt::AlignVCenter, prog);

   p.setPen(C_MUTED);
   p.drawText(QRect(x, (int)r.top() + 21, w, 15), Qt::AlignLeft | Qt::AlignVCenter,
              m_ctl->stateText());

   /* 第三行: 区域尺寸 (视野固定后, 图里那个框多大不再是"画布多大")。
    * 接在状态行下面而不是右上角: 画布能被拖到 240px 宽, 右对齐会压到左边的进度 */
   p.setPen(C_MUTED);
   const Params &q = m_ctl->params();
   p.drawText(QRect(x, (int)r.top() + 36, w, 15), Qt::AlignLeft | Qt::AlignVCenter,
              QStringLiteral("区域 %1 × %2 单位")
                 .arg(q.area_x_unit, 0, 'f', 2)
                 .arg(q.area_y_unit, 0, 'f', 2));

   /* 下沿: 悬停读数 (单位 + 脉冲) */
   if (m_hover && m_ctl != nullptr)
   {
      const double ppu = m_ctl->params().pulses_per_unit;
      const QString s = QStringLiteral("(%1, %2) 单位 = (%3, %4) pul")
                           .arg(m_hover_xu, 0, 'f', 2).arg(m_hover_yu, 0, 'f', 2)
                           .arg((long long)std::llround(m_hover_xu * ppu))
                           .arg((long long)std::llround(m_hover_yu * ppu));
      p.setPen(C_TEXT);
      p.drawText(QRect(x, (int)r.bottom() - 34, w, 15),
                 Qt::AlignLeft | Qt::AlignVCenter, s);
   }

   /* 再下面一行: 操作提示。扫描中那句照实说 —— 查看还能用, 不能用的是手动定位 */
   p.setPen(m_manual_ok ? C_MUTED : C_WANT);
   const QString hint = m_manual_ok
      ? QStringLiteral("左键 = 查看该格    Shift+左键 = 手动定位")
      : QStringLiteral("扫描中 —— 查看随便点; 手动定位要先「中止」");
   p.drawText(QRect(x, (int)r.bottom() - 18, w, 15),
              Qt::AlignLeft | Qt::AlignVCenter, hint);
}

/* ---------------------------------------------------------------- 交互 */

/* 查看那一格: 选中最近的网格点, 显示它的数值 / 索引 / 坐标。只读, 不动滑台,
 * 所以扫描中照样能用; 窗口那边只把 m_manual_ok 用在手动定位那条路上。 */
void MapCanvas::pickCell(double xu, double yu)
{
   const Params &q = m_ctl->params();
   const double res = q.res_unit;
   const int nx = m_ctl->gridNx();
   const int ny = m_ctl->gridNy();
   if (!(res > 0.0) || nx <= 0 || ny <= 0)
      return;

   const double span_x = ((double)nx - 1.0) * res;
   const double span_y = ((double)ny - 1.0) * res;

   /* lround 取的就是最近的格点 (|round(t) - t| <= 0.5), 不必再判"落在半格之内" */
   const int ix = (int)std::lround((xu + span_x / 2.0) / res);
   const int iy = (int)std::lround((yu + span_y / 2.0) / res);

   if (ix < 0 || ix >= nx || iy < 0 || iy >= ny)
   {
      /* 点在网格外面 (区域框外那圈余量): 取消选中 —— 否则选错的那个框会一直赖在图上 */
      clearSelection();
      return;
   }

   m_sel_ix = ix;
   m_sel_iy = iy;
   update();
   emit cellPicked(ix, iy);
}

void MapCanvas::mousePressEvent(QMouseEvent *e)
{
   if (m_ctl == nullptr || e->button() != Qt::LeftButton)
      return;

   const QPoint px = e->position().toPoint();

   double xu = 0.0, yu = 0.0;
   unitAt(px, &xu, &yu);

   const double half = viewHalfUnits();
   if (std::fabs(xu) > half || std::fabs(yu) > half)
      return;

   /* 左键 = 查看。**它排在扫描那道闸前面** —— 只读的动作没有理由被闸住 */
   if (!(e->modifiers() & Qt::ShiftModifier))
   {
      pickCell(xu, yu);
      return;
   }

   /* Shift + 左键 = 手动定位。扫描中一律吞掉: 手动插一脚的话, 那一点的数据说不清是哪来的 */
   if (!m_manual_ok)
      return;

   const double ppu = m_ctl->params().pulses_per_unit;
   if (!(ppu > 0.0))
      return;

   /*
    * 夹在实际生效的量程之内: 画布上有一圈是扫描区外、也超出量程的地方, 在那一圈点一下
    * 就是一次走到量程尽头的长动作。量程还没读到 (range <= 0) 时不夹。
    */
   double lim = half;
   if (m_bus != nullptr)
   {
      const BusTelem t = m_bus->telemetry();
      if (t.range > 0)
         lim = std::min(lim, (double)t.range / ppu);
   }
   xu = std::max(-lim, std::min(lim, xu));
   yu = std::max(-lim, std::min(lim, yu));

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
