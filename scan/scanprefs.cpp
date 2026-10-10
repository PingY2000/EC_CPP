#include "scanprefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

/* 头文件那条"只链 Qt6::Core"照旧 (这个 include 不带来任何新的链接依赖, 全是 inline 的
 * 纯判据)。要它只为一件事: 世代那两个哨兵判据住在 ecatcmd 里, 与用它的工作线程同一处 ——
 * 在这里另抄一份就会有一边改了另一边忘的那天, 而这两个判据各自守着一次真实的运动。 */
#include "ecatworker.h"

namespace scan {

/* 键名只在这里写一遍: 存的与读的用同一份常量 */
namespace k
{
static const char *area_x     = "scan/area_x_unit";
static const char *area_y     = "scan/area_y_unit";
static const char *res        = "scan/res_unit";
static const char *ppu        = "scan/pulses_per_unit";
static const char *speed      = "scan/speed_pul_s";
static const char *dwell      = "scan/dwell_ms";
static const char *settle     = "scan/settle_ms";
static const char *samples    = "scan/samples_per_point";
static const char *mode       = "scan/mode";
static const char *start_pos  = "scan/start_positive";
static const char *nic        = "bus/nic";
static const char *manspeed   = "ui/manual_speed";
static const char *homevel    = "ui/home_vel";
static const char *hometmo    = "ui/home_tmo_s";
static const char *homeoffx   = "ui/home_off_x";
static const char *homeoffy   = "ui/home_off_y";
static const char *want_dig   = "adv/want_dig_in";
static const char *npn_wr     = "adv/npn_write_drive";
static const char *npn_sw     = "adv/npn_sw_invert";
static const char *shade_auto = "shade/auto_fit";
static const char *shade_unit = "shade/unit";
/* 功率计那几个键。键名照旧只在这里定义 —— 用它的那边是 load 整个 Prefs、改字段、再 save
 * 回去, 不自己拼键名 (拼第二遍就会有一边改名一边忘的那天) */
static const char *mtr_int    = "meter/interval_ms";
static const char *mtr_window = "meter/window_min";
static const char *mtr_csv    = "meter/csv";
static const char *mtr_serial = "meter/serial";

/* 软件零点那三个键 (见 scanprefs.h)。逐轴那个要拼序号, 所以它由一个函数出 —— 存的与读的
 * 还是同一处生成, 不会有一边改名一边忘 (与上面那一段同一条规矩)。序号从 1 起, 与界面上的
 * 「轴1/轴2」对齐; 数组下标从 0 起, 换算只在这一对函数里。 */
static const char *zero_na = "zero/naxis";
static const char *zero_ep = "zero/epoch";
static QString originKey(int i) { return QStringLiteral("zero/origin_%1").arg(i + 1); }

/* 限位记录那一族 (见 scanprefs.h)。逐轴逐侧一个键, 所以也由函数出 —— 序号同样从 1 起,
 * 侧那一格写 **p / n** 而不是 0 / 1: 手翻 ini 的人看的是"正那侧 / 负那侧",
 * 而 0 与 1 在这里正好是反的 (数组下标 0 = 正), 写成数字迟早有人按反。 */
static const char *lim_epoch = "limit/epoch";
static QString limitPosKey(int axis, int side)
{
   return QStringLiteral("limit/pos_%1%2").arg(axis + 1).arg(side == 0 ? QLatin1Char('p')
                                                                      : QLatin1Char('n'));
}
}

QString prefsPath()
{
   /* exe 旁边 */
   return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("scan.ini"));
}

Prefs prefsLoad(const QString &path)
{
   Prefs p;                                   /* 全部按缺省起, 缺项就留在缺省上 */
   QSettings s(path, QSettings::IniFormat);

   /* 每项都显式带上缺省值: 缺项时 QSettings 给的是无效 QVariant, toDouble() 会当 0 */
   p.params.area_x_unit = s.value(QLatin1String(k::area_x), p.params.area_x_unit).toDouble();
   p.params.area_y_unit = s.value(QLatin1String(k::area_y), p.params.area_y_unit).toDouble();
   p.params.res_unit    = s.value(QLatin1String(k::res),    p.params.res_unit).toDouble();
   p.params.pulses_per_unit =
      s.value(QLatin1String(k::ppu), p.params.pulses_per_unit).toDouble();

   p.params.speed_pul_s =
      (uint32_t)s.value(QLatin1String(k::speed), (uint)p.params.speed_pul_s).toUInt();
   p.params.dwell_ms     = s.value(QLatin1String(k::dwell),   p.params.dwell_ms).toInt();
   p.params.settle_ms    = s.value(QLatin1String(k::settle),  p.params.settle_ms).toInt();
   p.params.samples_per_point =
      s.value(QLatin1String(k::samples), p.params.samples_per_point).toInt();

   /* 扫描方式 (2026-10-11 从 `scan/serpentine` 那个 bool 改成这个数, 见 ScanMode)。
    * 走 modeFromIndex 而不是直接强转: 手改坏的 ini 里可能是一个越界的数, 越界一律回落缺省
    * (「逐行往返」), 不让界面拿到一个说不出的模式。**老 ini 里的 serp 不再读** —— 与 §40
    * 那条"缺项就是缺项的待遇, 不做迁移"同一套。 */
   p.params.mode = modeFromIndex(
      s.value(QLatin1String(k::mode), (int)p.params.mode).toInt());
   p.params.start_positive = s.value(QLatin1String(k::start_pos), p.params.start_positive).toBool();

   /* 量程从不记忆: 它由区域算出 (autoRangePul)。现在是 max(区域推出来的, 画布半宽推出来的),
    * 记下来就等于把画布那条下限**冻结在当时的脉冲当量上** —— 脉冲当量一改它就是个错的数。 */
   p.params.range_pul = 0;

   p.nic          = s.value(QLatin1String(k::nic)).toString();
   p.manual_speed = s.value(QLatin1String(k::manspeed), -1).toInt();
   p.home_vel     = s.value(QLatin1String(k::homevel),  -1).toInt();
   p.home_tmo_s   = s.value(QLatin1String(k::hometmo),  -1).toInt();
   /* 缺省一律写 -1 (= 没记过), **不能写 0**: 0 是"零点就放在落点上"这个合法选择。
    * 读回来之后由界面过 ecatcmd::home_off_from_pref (它才认这个哨兵)。 */
   p.home_off_x   = s.value(QLatin1String(k::homeoffx), -1).toInt();
   p.home_off_y   = s.value(QLatin1String(k::homeoffy), -1).toInt();

   /* 这三项**必须显式带缺省值**: 缺项时 QSettings 给的是无效 QVariant, toBool() 一律返回
    * false —— 不写缺省的话, 旧 ini 会把"默认开"静默读成"用户把它关了" */
   p.want_dig_in     = s.value(QLatin1String(k::want_dig), p.want_dig_in).toBool();
   p.npn_write_drive = s.value(QLatin1String(k::npn_wr),   p.npn_write_drive).toBool();
   p.npn_sw_invert   = s.value(QLatin1String(k::npn_sw),   p.npn_sw_invert).toBool();

   /* 同样显式带缺省值: ini 里没这一项时要回落到"关", 而不是当成读过 */
   p.shade_auto = s.value(QLatin1String(k::shade_auto), p.shade_auto).toBool();

   /* 色标单位: 缺项时 toString() 给空串, 而 shadeUnitFromText("") 就是缺省 (mW) ——
    * 于是"没有这一项"与"写着个认不出来的字"走同一条路, 不用另写一处缺省 */
   p.shade_unit = shadeUnitFromText(s.value(QLatin1String(k::shade_unit)).toString());

   /* 功率计那两项 (「独立窗口」已经不在了, 见 scanprefs.h 上面对这两项的说明)。
    * 间隔同样显式带缺省值 —— 缺项时 toInt() 会给 0, 那是个非法间隔,
    * 会被 MeterLog 夹到下限, 于是"没记过"表现成 20ms 而不是 200ms */
   p.meter_interval_ms = s.value(QLatin1String(k::mtr_int), p.meter_interval_ms).toInt();
   /* 窗口同理: 缺项给 0 -> MeterLog 夹到 1 分钟, 于是"没记过"表现成 1 分钟而不是 5 分钟。
    * 手改坏的 ini (负数 / 一千万) 也都由它夹 —— 曲线不该用一个没验过的窗口 */
   p.meter_window_min  = s.value(QLatin1String(k::mtr_window), p.meter_window_min).toInt();
   p.meter_csv         = s.value(QLatin1String(k::mtr_csv)).toString();
   p.meter_serial      = s.value(QLatin1String(k::mtr_serial)).toString();

   /* 软件零点。世代**独立读、独立过哨兵** —— 它是计数器, 不是那份值的属性: 值读坏了不该让
    * 世代一起丢 (丢了之后落盘那道"只许往前"的闸会让零点再也写不回来)。
    * 哨兵判据用 ecatcmd::origin_epoch_from_pref, 与工作线程种下去时用的是同一个。 */
   p.zero_epoch = ecatcmd::origin_epoch_from_pref(
                     s.value(QLatin1String(k::zero_ep), p.zero_epoch).toInt());

   /* 那份逐轴值: **全有或全无**。轴数越界、或任何一项不是整数字面量, 整份都不采纳
    * (zero_naxis 留 0 = 没记过)。半份记录比没有更危险 —— 缺的那项会按 0 进来, 而 0 是
    * 合法坐标, 于是一根轴悄悄跑到 6064h 的 0 点上, 屏幕上看不出任何异常。
    * 缺项走的就是这条路: 键不在时 value() 给空串, originFromText("") 返回 false。 */
   {
      const int n = s.value(QLatin1String(k::zero_na), 0).toInt();
      int32_t   org[EM_MAX_AXES] = {0};
      bool      ok = ecatcmd::origin_naxis_usable(n);
      for (int i = 0; ok && i < n; i++)
         ok = originFromText(s.value(k::originKey(i)).toString(), &org[i]);

      if (ok)
      {
         p.zero_naxis = n;
         for (int i = 0; i < EM_MAX_AXES; i++)
            p.zero_origin[i] = org[i];
      }
   }

   /* 限位记录 (2026-09-29)。三个规矩:
    *   ① 解析走 originFromText (不许 toInt —— 手改坏的字会变成"记在 0 点上");
    *   ② **逐条独立**: 每一条自己说自己有没有 —— 缺项 / 读坏了那一条就是"没记过", 别的照旧。
    *      **这里与上面那份零点值不是同一条规矩, 是刻意的**: 零点记的是一根轴只许有一个的
    *      坐标, 缺一项就得整份丢 (缺的那项会按 0 进来, 而 0 是合法坐标, 那根轴会悄悄跑掉);
    *      限位记录是**疏的** —— 只记撞过的那几侧, 而且"没有记录"本身就是它的空值 (不像 0),
    *      所以缺一项的正确含义就是"这一侧没撞过", 没有第二种可能。
    *   ③ 与那份零点**同一代**才算数 (lim_epoch 与 zero_epoch 不等 = 两半不是一批写的)。 */
   {
      p.lim_epoch = s.value(QLatin1String(k::lim_epoch), 0).toInt();

      for (int i = 0; i < EM_MAX_AXES; i++)
         for (int side = 0; side < 2; side++)
         {
            int32_t v = 0;
            const bool ok = originFromText(s.value(k::limitPosKey(i, side)).toString(), &v);
            p.lim_have[i][side] = ok;
            p.lim_pos[i][side]  = ok ? v : 0;
         }

      /* 世代对不上: 整份当没记过。位置上记的是**那一个坐标系**里的一个点, 零点换了代就什么
       * 都不是了 —— 留着的唯一后果是拦下一次没人要求过的运动。 */
      if (p.lim_epoch != p.zero_epoch)
      {
         for (int i = 0; i < EM_MAX_AXES; i++)
            for (int side = 0; side < 2; side++)
            {
               p.lim_have[i][side] = false;
               p.lim_pos[i][side]  = 0;
            }
      }
   }

   return p;
}

bool originFromText(const QString &s, int32_t *out)
{
   if (out == nullptr)
      return false;

   /* toLongLong 在 base 10 下: 收首尾空白与正负号, 拒 "7.5" / "abc" / "0x10" / 空串。
    * **不能用 QVariant::toInt()** —— 它对不认识的东西给 0, 而 0 是合法坐标 (理由见头文件)。 */
   bool            ok = false;
   const qlonglong v  = s.trimmed().toLongLong(&ok);
   if (!ok || v > (qlonglong)INT32_MAX || v < (qlonglong)INT32_MIN)
      return false;

   *out = (int32_t)v;
   return true;
}

void prefsMergeParams(Prefs *store, const Params &cur)
{
   if (store == nullptr)
      return;

   /* 判据与界面开始按钮用的那一条一致 (validate) */
   if (!validate(cur).empty())
      return;

   Params keep = cur;
   keep.range_pul = 0;   /* 理由同上: 它是算出来的, 不是操作员设的 */
   store->params = keep;
}

void prefsSave(const QString &path, const Prefs &p)
{
   QSettings s(path, QSettings::IniFormat);

   s.setValue(QLatin1String(k::area_x),    p.params.area_x_unit);
   s.setValue(QLatin1String(k::area_y),    p.params.area_y_unit);
   s.setValue(QLatin1String(k::res),       p.params.res_unit);
   s.setValue(QLatin1String(k::ppu),       p.params.pulses_per_unit);

   s.setValue(QLatin1String(k::speed),     (uint)p.params.speed_pul_s);
   s.setValue(QLatin1String(k::dwell),     p.params.dwell_ms);
   s.setValue(QLatin1String(k::settle),    p.params.settle_ms);
   s.setValue(QLatin1String(k::samples),   p.params.samples_per_point);

   s.setValue(QLatin1String(k::mode),      (int)p.params.mode);
   s.setValue(QLatin1String(k::start_pos), p.params.start_positive);

   s.setValue(QLatin1String(k::nic),       p.nic);
   s.setValue(QLatin1String(k::manspeed),  p.manual_speed);
   s.setValue(QLatin1String(k::homevel),   p.home_vel);
   s.setValue(QLatin1String(k::hometmo),   p.home_tmo_s);
   s.setValue(QLatin1String(k::homeoffx),  p.home_off_x);
   s.setValue(QLatin1String(k::homeoffy),  p.home_off_y);

   s.setValue(QLatin1String(k::want_dig),  p.want_dig_in);
   s.setValue(QLatin1String(k::npn_wr),    p.npn_write_drive);
   s.setValue(QLatin1String(k::npn_sw),    p.npn_sw_invert);

   s.setValue(QLatin1String(k::shade_auto), p.shade_auto);
   /* 存 ASCII 记号 (follow / W / mW / uW), 不存屏幕上那个字: QSettings 把非 ASCII 转义成
    * \xXXXX, 那个键就没法手改了 (见 powermeter.h 的 shadeUnitFromText) */
   s.setValue(QLatin1String(k::shade_unit), shadeUnitToText(p.shade_unit));

   s.setValue(QLatin1String(k::mtr_int),    p.meter_interval_ms);
   s.setValue(QLatin1String(k::mtr_window), p.meter_window_min);
   s.setValue(QLatin1String(k::mtr_csv),    p.meter_csv);
   s.setValue(QLatin1String(k::mtr_serial), p.meter_serial);

   /* 软件零点: **只在一份可用记录时才写**。zero_naxis == 0 时一个键都不写 —— 写一个 0 进
    * 去就是**擦掉一个坐标系**, 那不该是关窗的副作用 (与"网卡清单还没到就别把记住的抹掉"
    * 同一条规矩)。要清就删 bin/scan.ini。 */
   if (ecatcmd::origin_naxis_usable(p.zero_naxis))
   {
      s.setValue(QLatin1String(k::zero_na), p.zero_naxis);
      s.setValue(QLatin1String(k::zero_ep), p.zero_epoch);
      for (int i = 0; i < p.zero_naxis; i++)
         s.setValue(k::originKey(i), (int)p.zero_origin[i]);
   }

   /* 限位记录。**一条有记录才写一条, 没记录的那一条要 remove** ——
    * 写一个 0 进去是把一条限位线**钉在坐标系原点上** (那条线会拦住往那一侧的每一次运动),
    * 而"不写"在这种稀疏记录里还有第二种含义: 那些键上一次写下的值**会留在文件里**
    * (QSettings 是读-改-写, 它不认识"整份重写"; 别的键也一样, 只是它们在界面上永远有值)。
    * 于是"回过零, 线作废"这条路必须**删**掉那些键 —— 留着的话, 下一次真撞上线时新记录
    * 只写它自己那一侧, 陈的那几侧会以**新一代的世代号**被读回来 (世代判据救不了它)。
    *
    * `limit/epoch` 只在真有记录时才写 (没有记录时连它一起删): 一个世代配一份空记录,
    * 下次读回来就是"有世代、没位置" —— 那正是"没记过"想要的样子。 */
   {
      bool any = false;
      for (int i = 0; i < EM_MAX_AXES && !any; i++)
         for (int side = 0; side < 2 && !any; side++)
            any = p.lim_have[i][side];

      for (int i = 0; i < EM_MAX_AXES; i++)
         for (int side = 0; side < 2; side++)
         {
            if (p.lim_have[i][side])
               s.setValue(k::limitPosKey(i, side), (int)p.lim_pos[i][side]);
            else
               s.remove(k::limitPosKey(i, side));
         }

      if (any)
         s.setValue(QLatin1String(k::lim_epoch), p.lim_epoch);
      else
         s.remove(QLatin1String(k::lim_epoch));
   }

   /* 显式 sync: 不靠析构时机保证写盘 */
   s.sync();
}

}   /* namespace scan */
