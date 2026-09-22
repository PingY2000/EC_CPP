/* scan/scanstyle.cpp —— 样式表的读盘与出厂那一份 (理由见 scanstyle.h) */

#include "scanstyle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

/* 出厂样式。**这是"缺省", 不是"备份"**: 只有 exe 旁边没有 scan.qss 时才会被写到那儿去,
 * 文件在的时候一个字节都不动它 —— 所以你在那个文件里改的东西不会被下一次跑程序盖掉。 */
static const char *kDefaultSheet = R"(/* scan.qss —— 本程序的界面样式表。
 *
 * 这个文件是程序第一次跑的时候自己写出来的, 之后**只读不写**: 改它, 界面上按 F5 重新读一遍,
 * 不用重编译。想回到出厂样子就把这个文件删掉, 下次启动会重新写一份。
 *
 * 改这里能改的是"长什么样": 颜色、边框、圆角、内边距、字号、按钮大小、灰掉时的样子。
 * 改不了的是"谁摆在哪儿" —— 控件的位置、行顺序、拉伸比例都在 scan/scanwindow.cpp 里。
 *
 * 两条容易被改坏的地方:
 *   · QPushButton 的 padding 是按框里那些大按钮定的 (上下各 5px -> 按钮自然高度 28px)。
 *     参数框标题那一行只有 18px 高, 所以 QPushButton#panelbar (标题行右端那对保存/取消)
 *     单开了一条把内边距压掉的规则 —— 删掉它, 那两个字会被挤成一条缝。
 *   · QGroupBox 的 margin-top 决定"框顶上给标题留多高", 也就是标题那一行的下沿。改小到
 *     比标题字还矮, 标题会被切掉一截。
 */
   QWidget            { background:#16181d; color:#c8ced8; }
   QGroupBox          { background:#1f232a; border:1px solid #2c313a;
                        border-radius:6px; margin-top:9px; padding:8px 6px 6px 6px; }
   QGroupBox::title   { subcontrol-origin: margin; left:9px; padding:0 4px;
                        color:#9aa3ae; background:#16181d; }
   QGroupBox QLabel   { background:transparent; }
   QPushButton        { background:#262b33; border:1px solid #3c434e;
                        border-radius:4px; padding:5px 12px; }
   /* 参数框标题那一行右端的那两个小按钮 (objectName = panelbar, 见 panelBar())。 */
   QPushButton#panelbar { padding:0 9px; }
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
)";

QString stylePath()
{
   /* exe 旁边 (与 prefsPath() 同一个目录: 取的是 applicationDirPath 而不是工作目录,
    * 从哪儿双击启动都找得到) */
   return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("scan.qss"));
}

QString styleDefault()
{
   return QString::fromUtf8(kDefaultSheet);
}

QString styleLoad()
{
   QFile f(stylePath());
   if (f.open(QIODevice::ReadOnly | QIODevice::Text))
   {
      const QString s = QString::fromUtf8(f.readAll());
      f.close();
      /* 只剩空白 = 当成没有: 空样式表会把整个界面打回系统默认样子, 那不是谁想要的,
       * 多半是存盘存坏了或者刚清空还没写 */
      if (!s.trimmed().isEmpty())
         return s;
      return styleDefault();
   }

   /* 文件不在: 写出厂那一份, 让"第一次跑完就能改"成立。写不出去 (**只读目录 / 没权限**) 也
    * 不算错 —— 照样返回出厂的这一份, 界面正常 */
   QFile out(stylePath());
   if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
      out.write(QString::fromUtf8(kDefaultSheet).toUtf8());
   return styleDefault();
}
