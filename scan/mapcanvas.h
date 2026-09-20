/*
 * scan/mapcanvas.h —— 二维面板: 区域框 / 热力图 / 滑台位置 / 点击手动定位
 *
 * 标记: 实心点 = 6064h 实测位置, 橙色 = want (点击给的), 绿色虚线 = tgt (本周期下发的)。
 * 热力图: 一格的颜色 = 该点采到的功率。
 *
 * 色标: 单色相顺序色标 (蓝, 暗 = 小, 亮 = 大), 色阶锁定, 需「按数据定标」或改两个数字才变;
 * 超出范围的格子夹到两端。色相在 OKLab 里插值而成。
 * 格子三态: 未扫到 = 只有底色; 读数失败 = 红框; 有数 = 顺序色标。
 *
 * 视野: 固定 30×30 单位 (每边 ±15, 再各留 1 单位余量, kViewHalfUnits), 量程是常数,
 * 窗口改变只改这块方形的像素边长。所有图元按单位存, 绘制时才换算成像素。
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

   /* 锁定的色阶 (功率单位)。hi <= lo 时用一条中间色, 不除零 */
   void setShadeRange(double lo, double hi);
   double shadeLo() const { return m_shade_lo; }
   double shadeHi() const { return m_shade_hi; }
   /* 按当前已采数据的最小/最大定标。**只在人按了按钮时调** */
   bool fitShadeToData();

   /* 左键查看时选中的格; 没有选中时都是 -1 */
   int selectedIx() const { return m_sel_ix; }
   int selectedIy() const { return m_sel_iy; }
   void clearSelection();

   /* 扫描中/跨区域时由窗口置位。**它只管手动定位那一条路** —— 查看是只读的, 不吞 */
   void setManualAllowed(bool on) { m_manual_ok = on; }

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
   double viewHalfUnits() const;      /* 可视半宽 (单位) */
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

   ScanController *m_ctl = nullptr;
   BusView        *m_bus = nullptr;

   double m_shade_lo = 0.0;
   double m_shade_hi = 1.0;

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
