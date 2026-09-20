#include "scanprefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

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
static const char *serp       = "scan/serpentine";
static const char *start_pos  = "scan/start_positive";
static const char *nic        = "bus/nic";
static const char *manspeed   = "ui/manual_speed";
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

   p.params.serpentine     = s.value(QLatin1String(k::serp),      p.params.serpentine).toBool();
   p.params.start_positive = s.value(QLatin1String(k::start_pos), p.params.start_positive).toBool();

   /* 量程从不记忆: 它由区域算出 (autoRangePul) */
   p.params.range_pul = 0;

   p.nic          = s.value(QLatin1String(k::nic)).toString();
   p.manual_speed = s.value(QLatin1String(k::manspeed), -1).toInt();

   return p;
}

void prefsMergeParams(Prefs *store, const Params &cur)
{
   if (store == nullptr)
      return;

   /* 判据与界面开始按钮用的那一条一致 (validate) */
   if (!validate(cur).empty())
      return;

   Params keep = cur;
   keep.range_pul = 0;
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

   s.setValue(QLatin1String(k::serp),      p.params.serpentine);
   s.setValue(QLatin1String(k::start_pos), p.params.start_positive);

   s.setValue(QLatin1String(k::nic),       p.nic);
   s.setValue(QLatin1String(k::manspeed),  p.manual_speed);

   /* 显式 sync: 不靠析构时机保证写盘 */
   s.sync();
}

}   /* namespace scan */
