/*
 * scan/main.cpp —— 滑台蛇形扫描 + 逐点功率采集
 *
 * 默认只读: 启动后不写总线; 「连接」进 OP 才发帧, 「使能」才带电, 「开始扫描」才动作。
 */

#include <QApplication>
#include <QFont>

#include "ec_motor.h"
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

   app.setStyleSheet(QStringLiteral(R"(
      QWidget            { background:#16181d; color:#c8ced8; }
      QGroupBox          { background:#1f232a; border:1px solid #2c313a;
                           border-radius:6px; margin-top:9px; padding:8px 6px 6px 6px; }
      QGroupBox::title   { subcontrol-origin: margin; left:9px; padding:0 4px;
                           color:#9aa3ae; background:#16181d; }
      QGroupBox QLabel   { background:transparent; }
      QPushButton        { background:#262b33; border:1px solid #3c434e;
                           border-radius:4px; padding:5px 12px; }
      QPushButton:hover  { background:#2f3640; }
      QPushButton:pressed{ background:#20252c; }
      QPushButton:disabled { color:#5b626c; background:#1d2126; border-color:#2a2f37; }
      QPushButton#danger { background:#5a2323; border-color:#8a3535; }
      QPushButton#danger:hover { background:#6b2a2a; }
      QPushButton#go     { background:#1f4030; border-color:#2f6b4d; }
      QPushButton#go:hover { background:#255038; }
      QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit {
                           background:#20242b; border:1px solid #3c434e;
                           border-radius:4px; padding:3px 6px; }
      QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
      QLineEdit:disabled { color:#5b626c; background:#1a1e23; border-color:#2a2f37; }
      QStatusBar         { background:#12141a; color:#9aa3ae; }
      QStatusBar::item   { border:none; }
      QMessageBox        { background:#1f232a; }
      QToolTip           { background:#20242b; color:#c8ced8; border:1px solid #3c434e; }
   )"));

   scan::ScanWindow w;
   w.resize(1360, 780);
   w.show();

   return app.exec();
}
