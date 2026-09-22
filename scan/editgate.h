/*
 * scan/editgate.h —— 参数框的编辑门控 (纯逻辑, 不依赖 Qt)。
 *
 * 一块参数框平时只读, 点该框的「编辑」才能改。三条规则:
 *   1. 同一时刻最多一块框处于编辑态;
 *   2. 点别的框的「编辑」= 静默丢弃当前框的改动 (不弹框);
 *   3. 「保存」与「取消」都退出编辑态, 区别只在调用方要不要先落盘 / 回滚控件。
 *
 * 状态全在这里, 界面只按结论 setEnabled —— 界面自己不再记任何状态。
 * 界面侧唯一写 setEnabled 的地方是 ScanWindow::refreshEditability()。
 */
#pragma once

namespace editgate {

/* 一块框的门。dirty 只影响标题上那个标记, 不拦「保存」——
 * 值在改的那一刻就已经生效了, 保存只是把它记住 */
struct Gate
{
   bool editing   = false;   /* 在编辑态 */
   bool dirty     = false;   /* 进编辑态之后被改过 */
   bool discarded = false;   /* 刚被静默丢弃过 (标记留到下次进编辑态) */
};

/* 进编辑态。返回 false = 本来就在编辑态 (重复点「编辑」是 no-op, 不重拍快照) */
inline bool begin(Gate *g)
{
   if (g->editing)
      return false;
   g->editing   = true;
   g->discarded = false;     /* 「已丢弃」这个痕迹到下次进编辑态为止 */
   return true;
}

/* 只有编辑态里的改动才算数 —— 载入 ini / 恢复默认时顺手碰到的控件不该把框点脏 */
inline void markDirty(Gate *g)
{
   if (g->editing)
      g->dirty = true;
}

/* 改回原值 = 没有未保存的改动, 标记自己消失。界面侧那个标记是按"逐项与快照比对"算出来的,
 * 所以需要这一条 —— 一个"改过没有"的布尔标记会在改回原值之后继续挂着 */
inline void undirty(Gate *g)
{
   if (g->editing)
      g->dirty = false;
}

/* 静默丢弃, 退出编辑态。返回 true = 刚才**真有改动**, 调用方必须回滚控件并重新下推 */
inline bool drop(Gate *g)
{
   const bool had = g->dirty;
   g->editing   = false;
   g->dirty     = false;
   if (had)
      g->discarded = true;
   return had;
}

inline void save(Gate *g)
{
   g->editing   = false;
   g->dirty     = false;
   g->discarded = false;
}

/* 与 save 的区别只在调用方: 取消还要把控件值滚回快照 */
inline void cancel(Gate *g)
{
   save(g);
}

/* 框标题的后缀 —— 只此一处。空串 = 不拼后缀 (不是拼一个空格) */
inline const char *titleMark(const Gate &g)
{
   if (g.editing && g.dirty)
      return "   ● 未保存";
   if (g.discarded)
      return "   ● 未保存 (已丢弃)";
   return "";
}

/* 「高级选项」那两个勾的互斥判据。返回 nullptr = 没有警告。
 *
 * 双反相**不是**"等于没反", 是更坏: 判据此时是 LIMIT_RULE_INVERT (只看反相后的正/负限位
 * 开关, 见 hmi/ecatworker.h 的 limit_hit_rule), 反相之后那两位空载时都是 1 ->
 * 判据**恒成立** -> 扫描永远开不了。只多报, 不漏报。 */
inline const char *doubleInvertWarning(bool npn_write_drive, bool npn_sw_invert)
{
   if (!(npn_write_drive && npn_sw_invert))
      return nullptr;

   return "双反相: 「写驱动器 2300h」与「上位机侧取反」同时勾选 —— 限位判据恒成立, "
          "扫描无法启动。请关闭「上位机侧取反」。";
}

}   /* namespace editgate */
