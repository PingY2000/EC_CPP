/* hmi/main.cpp —— 两台滑台的 ±500000 脉冲 CSP 点击定位台。
 * 界面侧不碰 SOEM; 默认只读: 「连接」才进 OP 发帧, 「使能」才让电机带电。 */

#include <QApplication>
#include <QFont>

#include "ec_motor.h"
#include "mainwindow.h"

int main(int argc, char **argv)
{
   /* 必须是第一句: 把控制台代码页设成 UTF-8, 晚一步它之前的汉字日志就是乱码。
    * 界面线程唯一一次调 motor_api —— 只设代码页, 不碰总线、不碰网卡。 */
   em_console_init();

   QApplication app(argc, argv);
   app.setApplicationName(QStringLiteral("hmi"));
   app.setApplicationDisplayName(QStringLiteral("滑台 CSP 定位台"));

   /* 不显式设字体的话, 中文可能落到没有汉字的字体上 -> 方框 */
   QFont f(QStringLiteral("Microsoft YaHei UI"), 9);
   f.setStyleStrategy(QFont::PreferAntialias);
   app.setFont(f);

   app.setStyleSheet(QStringLiteral(R"(
      QWidget            { background:#16181d; color:#c8ced8; }
      QWidget#axisPanel  { background:#1f232a; border:1px solid #2c313a; border-radius:6px; }
      QWidget#axisPanel QLabel { background:transparent; }
      QPushButton        { background:#262b33; border:1px solid #3c434e;
                           border-radius:4px; padding:5px 12px; }
      QPushButton:hover  { background:#2f3640; }
      QPushButton:pressed{ background:#20252c; }
      QPushButton:disabled { color:#5b626c; background:#1d2126; border-color:#2a2f37; }
      QPushButton#danger { background:#5a2323; border-color:#8a3535; }
      QPushButton#danger:hover { background:#6b2a2a; }
      QComboBox, QSpinBox { background:#20242b; border:1px solid #3c434e;
                            border-radius:4px; padding:3px 6px; }
      QComboBox:disabled, QSpinBox:disabled { color:#5b626c; }
      QSlider::groove:horizontal { height:6px; background:#2c313a; border-radius:3px; }
      QSlider::handle:horizontal { width:14px; margin:-5px 0; border-radius:7px;
                                   background:#4a9eff; }
      QSlider::sub-page:horizontal { background:#2b4a6e; border-radius:3px; }
      QStatusBar         { background:#12141a; color:#9aa3ae; }
      QStatusBar::item   { border:none; }
      QMessageBox        { background:#1f232a; }
      QLabel#caution    { color:#c0a05a; }
      QToolTip           { background:#20242b; color:#c8ced8; border:1px solid #3c434e; }
   )"));

   MainWindow w;
   w.resize(1180, 560);
   w.show();

   return app.exec();
}
