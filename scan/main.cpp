/*
 * scan/main.cpp —— 滑台蛇形扫描 + 逐点功率采集
 *
 * 它跟 hmi/main.cpp 是**同一个骨架**: em_console_init 必须第一句, 然后 QApplication +
 * 中文字体 + 夜间配色。差别只在一个: 那边的主窗口是两根一维轨道 (手动调试台),
 * 这边是二维面板 + 一个扫描状态机。
 *
 * 界面侧**完全不碰 SOEM**: 这个文件只 include scanwindow.h 与 ec_motor.h, 而 ec_motor.h
 * 是纯 C 的公开接口 (只有一个 stdint.h)。所有 ecx_* 都在 EcatThread 里 ——
 * 那是 hmi/ecatworker 原样编进来的一份, 一行没改。
 *
 * 默认是只读的: 启动之后一个字节都不写总线。「连接」才进 OP 开始发帧, 「使能」才让电机带电,
 * 「开始扫描」才自己动。
 */

#include <QApplication>
#include <QFont>

#include "ec_motor.h"
#include "scanwindow.h"

int main(int argc, char **argv)
{
   /*
    * 必须是第一句: 它把控制台代码页设成 UTF-8, 而本程序的中文日志既有我们自己打的
    * (note()), 也有 motor_api 打的 (选轴 / 偏移证明 / 使能阶梯)。晚一步, 那之前的汉字
    * 就全是乱码 (实测把「多轴」打成「澶氳酱」)。
    *
    * 这是界面线程唯一一次调 motor_api —— 它只设代码页, 不碰总线、不碰网卡。
    */
   em_console_init();

   QApplication app(argc, argv);
   app.setApplicationName(QStringLiteral("scan"));
   app.setApplicationDisplayName(QStringLiteral("滑台蛇形扫描采集"));

   /* 不显式设字体的话, 中文在默认族里可能落不到有汉字的字体上 -> 方框 */
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

   /*
    * **启动时不弹任何对话框。** 免责的话写在主窗口里常驻, 而真正危险的三个动作各有自己的
    * 模态确认 (「连接」说清它写什么、「使能」要求人在设备旁、「开始扫描」把区域和时长再报一遍)。
    * 每次启动都要点一次的弹窗只会被条件反射地关掉。
    */
   scan::ScanWindow w;
   w.resize(1360, 780);
   w.show();

   return app.exec();
}
