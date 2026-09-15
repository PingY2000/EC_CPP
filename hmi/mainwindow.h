/*
 * hmi/mainwindow.h —— 顶栏 + 每轴一个面板 + 状态栏
 *
 * 主窗口只做三件事: 收集操作、把操作转成 EcatThread 的 post 系列 / set 系列、30Hz 刷遥测。
 * **它不持有 em_bus_t, 也不 include SOEM**; 连 em_csp_set_target 都是经 EcatThread 走的。
 */
#pragma once

#include <QMainWindow>
#include <QVector>

#include "ecatworker.h"

class AxisPanel;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QTimer;

class MainWindow : public QMainWindow
{
   Q_OBJECT

public:
   explicit MainWindow(QWidget *parent = nullptr);
   ~MainWindow() override;

protected:
   /* 关窗 = 断开: 失能 -> 收尾(还原映射 / 降 PRE_OP / 关网卡) -> 必要时弹"可能仍带电" */
   void closeEvent(QCloseEvent *e) override;

private:
   void buildUi();
   void rebuildPanels(int n);
   void setConnected(bool on);
   void hint(const QString &s, bool fault);
   void disconnectAndStop();     /* 断开 + 收尾; 可重入 */

   void onConnectClicked();
   void onEnableClicked();
   void onDisableClicked();
   void onStopClicked();
   void onCenterAllClicked();
   void onAdapters(const QStringList &names, const QStringList &descs);
   void onNotify(const QString &s);
   void onTarget(int axis, int want);
   void onSpeed(int axis, uint32_t vel);
   void refresh();
   void warnMaybeLive();         /* 收尾未确认失能时的模态告警 */

   EcatThread *m_thr = nullptr;

   QComboBox   *m_nic       = nullptr;
   QPushButton *m_btnNic    = nullptr;
   QPushButton *m_btnConn   = nullptr;
   QPushButton *m_btnEnable = nullptr;
   QPushButton *m_btnStop   = nullptr;
   QPushButton *m_btnDis    = nullptr;
   QPushButton *m_btnCenter = nullptr;

   QLabel *m_banner = nullptr;
   QLabel *m_lNote  = nullptr;
   QLabel *m_lWkc   = nullptr;

   QWidget     *m_axisHost = nullptr;
   QHBoxLayout *m_axisLay  = nullptr;
   QVector<AxisPanel *> m_panels;

   QTimer *m_tick        = nullptr;
   QTimer *m_bannerTimer = nullptr;

   /* 全部从遥测推出来, 不用"点过哪个按钮"来推 —— 那种推法会和线程的真实状态错开 */
   bool m_connected = false;
   bool m_faultShown = false;
   bool m_warnedLive = false;
};
