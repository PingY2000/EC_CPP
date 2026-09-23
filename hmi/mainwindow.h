/* hmi/mainwindow.h —— 顶栏 + 每轴一个面板 + 状态栏。主窗口把操作转成 EcatThread 的
 * post / set 系列, 自己不持有 em_bus_t。 */
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
   /* 「重连总线」。断开(先卸力) + 重连(重新进 OP, 各轴未使能)。未连接时等同「连接」 */
   void onReconnectClicked();
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
   /* 「重连总线」—— 断开(先卸力) + 重连(重新进 OP)。AutoRecover 救不回来时的出口 */
   QPushButton *m_btnReconn = nullptr;
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

   bool m_connected = false;
   bool m_faultShown = false;
   /* 故障横幅上**已经说过**的那个 603Fh (一根轴一格)。码是故障沿之后才到的,
    * 到了要重弹一次横幅, 而"弹过了没有"只能按轴存着 —— 否则 30Hz 每帧都弹。
    * 与 scan 侧 ScanWindow::m_faultCodeShown 同一套 (两处说法必须一致) */
   int  m_faultCodeShown[2] = {HMI_FAULT_CODE_UNREAD, HMI_FAULT_CODE_UNREAD};
   bool m_warnedLive = false;
};
