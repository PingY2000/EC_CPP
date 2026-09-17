#include "scanprefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace scan {

/* ---------------------------------------------------------------- 键名 */

/*
 * 键名**只在这里写一遍** —— 存的与读的读同一个字符串。
 *
 * 从前这种对应关系最容易错的方式是: 写的时候手抄一遍、读的时候再手抄一遍,
 * 两边错一个字母**都不报错**, 只是那一项永远记不住。做成常量之后至少不会错开。
 */
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
static const char *serp       = "scan/serpentine";
static const char *start_pos  = "scan/start_positive";
static const char *nic        = "bus/nic";
static const char *manspeed   = "ui/manual_speed";
}

QString prefsPath()
{
   /* exe 旁边。QCoreApplication 已经在别处起过了 (main 里第一个 QApplication),
    * 这里只是读它记下来的路径, 不会再初始化什么 */
   return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("scan.ini"));
}

/* ---------------------------------------------------------------- 读 */

Prefs prefsLoad(const QString &path)
{
   Prefs p;                                   /* 全部按缺省起, 缺项就留在缺省上 */
   QSettings s(path, QSettings::IniFormat);

   /* QSettings 读不到文件 / 没有这一项时给的是一张无效的 QVariant, 而 toDouble()
    * 之流会把它当 0 —— 所以每一项都**显式带上缺省值**, 不用 toDouble() 的返回值赌 */
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

   p.params.serpentine     = s.value(QLatin1String(k::serp),      p.params.serpentine).toBool();
   p.params.start_positive = s.value(QLatin1String(k::start_pos), p.params.start_positive).toBool();

   /* 量程**从不记忆**: 它是从区域算出来的 (autoRangePul), 记一份下来就成了第二份真相 */
   p.params.range_pul = 0;

   p.nic          = s.value(QLatin1String(k::nic)).toString();
   p.manual_speed = s.value(QLatin1String(k::manspeed), -1).toInt();

   return p;
}

/* ---------------------------------------------------------------- 并 */

void prefsMergeParams(Prefs *store, const Params &cur)
{
   if (store == nullptr)
      return;

   /* 判据就是界面开始按钮用的那一条 (validate) —— "能不能扫"。不能扫的不记, 见头文件 */
   if (!validate(cur).empty())
      return;

   Params keep = cur;
   keep.range_pul = 0;
   store->params = keep;
}

/* ---------------------------------------------------------------- 写 */

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

   s.setValue(QLatin1String(k::serp),      p.params.serpentine);
   s.setValue(QLatin1String(k::start_pos), p.params.start_positive);

   s.setValue(QLatin1String(k::nic),       p.nic);
   s.setValue(QLatin1String(k::manspeed),  p.manual_speed);

   /* 显式 sync 一次: 这个对象马上就析构了, 但"写没写进去"不该靠析构时机去赌 */
   s.sync();
}

}   /* namespace scan */
