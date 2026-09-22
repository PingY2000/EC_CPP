/*
 * scan/mapcanvas.h —— 二维面板: 区域框 / 热力图 / 滑台位置 / 点击手动定位
 *
 * 标记: 实心点 = 6064h 实测位置, 橙色 = want (点击给的), 绿色虚线 = tgt (本周期下发的)。
 * 热力图: 一格的颜色 = 该点采到的功率。
 *
 * 色标: 冷到暖的顺序色标 (蓝 → 青 → 绿 → 黄 → 红, 值越大越红), 色阶在 OKLab 里插值而成。
 * 色阶**默认锁定**, 要「按数据定标」或改两个数字才变; 开了 setAutoFit 则跟着数据走 (见那里)。
 * 超出范围的格子夹到两端。
 * 格子三态: 未扫到 = 只有底色; 读数失败 = 紫红实心块 (C_NODATA, 避开色标那头的红); 有数 = 顺序色标。
 * 右侧色标条高度封顶 320px, 两端 + 中间整数刻度都写数字 (drawScaleBar)。
 *
 * 视野: 固定 32×32 单位 (每边 ±kCanvasHalfUnits = ±16: 标尺画到 ±15, 再各留 1 单位余量),
 * 量程是常数, 窗口改变只改这块方形的像素边长。所有图元按单位存, 绘制时才换算成像素。
 * 那个常量定义在 scanplan.h —— 软量程的下限也是它 (见 autoRangePul), 只有一份。
 * 下边 X 标尺、左边 Y 标尺: 每 1 单位小刻度, 每 5 单位带数字。
 *
 * 交互: 左键 = 查看那一格 (只读, 扫描中可用); Shift+左键 = 手动定位到该点, 走显示坐标,
 * 夹在当前生效量程内。扫描进行中手动定位一律吞掉 (查看不受影响)。
 */
#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

#include "busview.h"

namespace scan {

class ScanController;

class MapCanvas : public QWidget
{
   Q_OBJECT

public:
   explicit MapCanvas(QWidget *parent = nullptr);

   /* 两个都不归它管, 只借来读。传 nullptr 也能画 (只画区域框与网格) */
   void setController(ScanController *c);
   void setBus(BusView *bus);

   /* 锁定的色阶 (功率单位)。
    * 调用方保证 lo >= 0 且 lo < hi (ScanWindow 那两个输入框就是这么夹的); 真给了一对反的也
    * 不会出事 —— hi <= lo 时一律用一条中间色, 不除零, 色标上就只有两端那两个数。 */
   void setShadeRange(double lo, double hi);
   double shadeLo() const { return m_shade_lo; }
   double shadeHi() const { return m_shade_hi; }
   /* 按当前已采数据的最小/最大定标。**只在人按了按钮时调** */
   bool fitShadeToData();

   /* 色标自动跟随数据。开着 = 色阶每帧跟着**已采数据的最小/最大**走 (一点数据都没有时
    * 保持不动, 不除零); 关着 = 今天这样, 两个数锁死, 只有人来改。
    *
    * 注意它和"色阶锁定"那条规则是冲突的, 只能二选一立: 自动跟随省掉了按按钮,
    * 代价是新采到一个更极端的值时整张图重排颜色 —— 图上的"变化"有一部分是色阶自己在动。 */
   void setAutoFit(bool on);
   bool autoFit() const { return m_auto; }

   /* 左键查看时选中的格; 没有选中时都是 -1 */
   int selectedIx() const { return m_sel_ix; }
   int selectedIy() const { return m_sel_iy; }
   void clearSelection();

   /* 扫描中/跨区域时由窗口置位。**它只管手动定位那一条路** —— 查看是只读的, 不吞 */
   void setManualAllowed(bool on) { m_manual_ok = on; }

   /* 画布顶上那条**空带**(像素): 留给 ScanWindow 那条**浮在画布上**的横幅 (它不进布局,
    * 见 scanwindow.cpp 的 placeBanner)。画布不认识横幅, 但它知道自己顶上得空出这么高 ——
    * 不空的话横幅一出现就盖住 HUD 的头两行 (进度 / 状态)。
    *
    * 算式 (都在下面的注释里量过): 横幅占 6..34, 画图区顶 = 20 + 这个数 = 40,
    * HUD 第一行从画图区顶 +6 = 46 起 —— 中间剩 12px。**下限是 8**: 再小横幅就压住第一行。
    * 横幅那 6 与 28 在 scanwindow.cpp 的 kBannerInset 与那三条样式表里, 改了要连这个一起改。 */
   static const int kBannerBand = 20;

signals:
   /* Shift+左键点了一下: 请把滑台移到这个显示坐标。窗口负责转成 setTarget */
   void manualMove(int32_t x_pul, int32_t y_pul);
   /* 左键查看选中了某一格 */
   void cellPicked(int ix, int iy);

protected:
   void paintEvent(QPaintEvent *) override;
   void mousePressEvent(QMouseEvent *) override;
   void mouseMoveEvent(QMouseEvent *) override;
   void leaveEvent(QEvent *) override;

private:
   /* ---- 坐标换算 (单位 <-> 像素) ---- */
   double viewHalfUnits() const;      /* 可视半宽 (单位) = scan::kCanvasHalfUnits, 见那个常量的注释 */
   QRectF plotRect() const;           /* 画图区 (已经是正方形) */
   QPointF pxOf(double xu, double yu) const;
   void    unitAt(const QPoint &p, double *xu, double *yu) const;
   void    pickCell(double xu, double yu);   /* 左键: 选中最近的格 (只读) */

   /* ---- 画 ---- */
   void  rebuildColors();             /* OKLab 插值出 256 级查找表 */
   void  rebuildImage();              /* nx×ny 的热力图 (一格一像素) */
   void  rebuildPath();
   void  drawGrid(QPainter &p);
   void  drawHeat(QPainter &p);
   void  drawPath(QPainter &p);
   void  drawRulers(QPainter &p);     /* X/Y 坐标标尺 (每 1 单位小刻度, 每 5 单位带数字) */
   void  drawMarkers(QPainter &p);
   void  drawScaleBar(QPainter &p);
   void  drawHud(QPainter &p);
   QColor shadeOf(double watts) const;

   /* 已采数据的功率范围 (「只有一个数」时加宽成一个窗口)。false = 一点都没采到 */
   bool dataShadeRange(double *lo, double *hi) const;
   /* 自动跟随模式下把色阶拉过去。在 paintEvent 里调 (数据变了本来就要重画) */
   void applyAutoFit();

   ScanController *m_ctl = nullptr;
   BusView        *m_bus = nullptr;

   double m_shade_lo = 0.0;
   double m_shade_hi = 1.0;
   bool   m_auto     = false;

   int m_sel_ix = -1;
   int m_sel_iy = -1;
   bool m_manual_ok = true;

   /* 悬停读数 */
   bool     m_hover = false;
   QPoint   m_hover_px;
   double   m_hover_xu = 0.0, m_hover_yu = 0.0;

   /* 缓存 */
   QColor              m_ramp[256];
   QImage              m_img;
   QPainterPath        m_path;
   int                 m_c_nx = -1, m_c_ny = -1, m_c_pts = -1;
   bool                m_geo_done = false;
};

}   /* namespace scan */
