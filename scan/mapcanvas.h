/*
 * scan/mapcanvas.h —— 二维面板: 区域框 / 热力图 / 滑台位置 / 点击手动定位
 *
 * 它是 hmi 那两条一维轨道 (hmi/axispanel.cpp) 的二维版本, 语义照搬:
 * **实心点 = 6064h 实测位置, 橙色 = want (点击给的), 绿色虚线 = tgt (本周期下发的)**。
 * 多出来的一层是热力图 —— 每一格的颜色 = 那一点采到的功率。
 *
 * ── 颜色 ──────────────────────────────────────────────────────────────
 *
 * 色标是**单色相顺序色标** (蓝色, 由暗到亮 = 由小到大), 蓝色取自仓库里既有的强调色
 * `#4a9eff` 那一族, 于是新程序看起来还是同一个程序。取色是**在 OKLab 里插值**的,
 * 不是 sRGB —— sRGB 插值在中段会发灰、发脏, 一眼就能看出色带分节。
 * 校验过的是顺序色标该过的那一关: 明度单调、色相跨度 4° (不分叉成两种颜色)。
 *
 * 暗色底上的方向是 **暗 = 小, 亮 = 大**, 与"近零应该退回底色"是同一条规则:
 * 亮的地方就是功率大的地方, 暗的地方是安静的。代价是最小值那一端跟底色对比度低
 * (1.5:1), 于是**必须有数字兜底** —— 右侧的色标条上写着锁定的最小/最大值,
 * 鼠标移到格子上也出数。热力图永远不能只有颜色。
 *
 * **色阶是锁定的, 不跟着数据实时变。** 这一点是刻意的: 边采边自动缩放的话,
 * 每来一个点整张图的颜色都会重排一遍, 操作员看到的"渐变"其实只是自己在动 ——
 * 那种图不能用来判断任何事。所以是两个数字 (默认 0..1) 加一个「按数据定标」按钮,
 * 要改是**显式**改一次。超出范围的格子夹到两端, 数值仍可在悬停里读到。
 *
 * 三种状态在画面上分得清清楚楚, 不靠深浅去猜:
 *   · 还没扫到 —— 只有底色的空位
 *   · 扫到了、读数失败 —— 红框 (crash 那种红), 因为那才是要人去处理的一格
 *   · 扫到了、有数 —— 顺序色标
 *
 * ── 视野 ──────────────────────────────────────────────────────────────
 *
 * **固定 30×30 单位** (每边 ±15, 再各留 1 单位余量, 见 mapcanvas.cpp 的 kViewHalfUnits) ——
 * 不跟着扫描区域缩放。缩放的话两次不同尺寸的扫描画出来一样大、颜色也看不出疏密,
 * 图与图没法比; 固定之后 1 单位恒等于固定的一格, 区域框成了图里的一个量。
 *
 * 窗口变大变小, 变的是**这块方形的像素边长**, 不是它的量程 —— 量程是常数。
 * 画出来的一切 (热力图 / 蛇形轨迹 / 滑台位置) 都是**按单位存、画的时候才换成像素**的,
 * 所以拉伸窗口是整张图一起缩放, 不会出现"格子跟着长、线还停在原处"。
 * 下边一条 X 标尺、左边一条 Y 标尺 (每 1 单位小刻度, 每 5 单位带数字, 与网格线同一组位置),
 * 于是图上任何一个点离零点多远可以直接读出来 —— 手动定位是按坐标下发的, 这一条要够得着。
 *
 * ── 交互 ──────────────────────────────────────────────────────────────
 *
 * 左键 = **查看那一格** (选中 + 读出数值 / 索引 / 坐标)。**只读**, 扫描中也能用 ——
 *        它是"看一眼", 不动滑台, 所以没有任何理由在扫描时把它关掉。
 * Shift + 左键 = 手动定位到那一点 (与 hmi 的点击同义, 走显示坐标), **夹在当前生效量程内**
 *
 * **要走滑台就得按住 Shift。** 这一条是刻意的: 画布上最常见的动作是"看看这一点采到多少",
 * 而那一下如果同时会把滑台支出去, 就没人敢随便点了 —— 尤其扫描完一屏数据、想逐格读的时候。
 * 动作分给两个键之后, 会动的一个永远要**多做一步**, 顺手点的那一个永远是空手。
 *
 * **扫描进行中手动定位一律吞掉**, 只留一行提示 —— 扫描期间手动插一脚, 采出来的数据就
 * 说不清了。查看不受影响。
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
