/*
 * scan/main.cpp —— 滑台蛇形扫描 + 逐点功率采集
 *
 * 默认只读: 启动后不写总线; 「连接」进 OP 才发帧, 「使能」才带电, 「开始扫描」才动作。
 */

#include <QApplication>
#include <QFont>

#include "ec_motor.h"
#include "scanstyle.h"
#include "scanwindow.h"

int main(int argc, char **argv)
{
   /* 必须是第一句: 把控制台代码页设成 UTF-8, 否则之前的中文日志全成乱码 */
   em_console_init();

   QApplication app(argc, argv);
   app.setApplicationName(QStringLiteral("scan"));
   app.setApplicationDisplayName(QStringLiteral("滑台蛇形扫描采集"));

   /* 不显式设字体族的话, 中文可能落到无汉字的字体上 -> 方框 */
   QFont f(QStringLiteral("Microsoft YaHei UI"), 9);
   f.setStyleStrategy(QFont::PreferAntialias);
   app.setFont(f);

   /* 界面样式表在 exe 旁边那个 **scan.qss** 里, 不在这个文件里 —— 首次运行会自动写出来一份
    * 出厂样式 (见 scanstyle.h)。想改外观就改那个文件, 界面上按 **F5** 重新读一遍, 不用重编译。 */
   app.setStyleSheet(styleLoad());

   scan::ScanWindow w;
   w.resize(1360, 780);
   w.show();

   return app.exec();
}
