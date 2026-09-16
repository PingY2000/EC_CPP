/*
 * hmi/ecatworker.h —— EtherCAT 工作线程
 *
 * **整个程序里只有这个线程碰 em_bus_t 与总线相关的 motor_api。** GUI 线程一个 motor_api
 * 函数都不调, 也不 include SOEM 的任何头文件 —— 界面侧与总线侧之间只有下面这几个
 * post 系列 / set 系列 与 telemetry()。这样 "ecx_* 必须串行" 与 "网卡单进程独占" 两条约束是
 * **结构上**成立的, 不靠调用纪律。
 *
 * 唯一的例外是 main() 里那句 em_console_init(): 它只设控制台代码页, 不碰总线、不碰网卡,
 * 而它的注释要求"main() 的第一句就调"(否则第一次打日志之前的汉字是乱码)。
 *
 * 坐标有两套, 不要混:
 *   驱动器坐标 = 6064h 的原始值 (掉电清零, 每次上电从驱动器自己的 0 开始)
 *   显示坐标   = 驱动器坐标 - 软件零点 origin。界面上那个"中间 0"就是它。
 * 对外 (postZeroHere / setTarget / telemetry) 一律用**显示坐标**。
 */
#pragma once

#include <QMutex>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>

#include "ec_motor.h"

/* ---------------------------------------------------------------- 参数 */

/* 工作范围**默认值**: ±500000 脉冲。50000 pul/圈 => ±10 圈 (2400h 实测 = 50000)。
 *
 * 它只是缺省 —— 真正的量程是 EcatThread::m_range, 可以经 postRange() 改。
 * 加这一层是为了 scan/ (蛇形扫描采集): 它的区域默认 27 单位 = ±675000 脉冲,
 * 比这里大。而量程在 interpolate() 里是**夹取**用的, 差一点就会把区域边缘悄悄削掉 ——
 * 那种 bug 不报错, 只是永远扫不到边, 所以必须让它跟着区域参数走。
 *
 * hmi 自己从不调 postRange, 于是它拿到的永远是下面这个值, 行为与从前完全一致。 */
#define HMI_RANGE       500000

#define HMI_VEL_MIN       1000   /* pul/s, 约 0.02 圈/秒 */
#define HMI_VEL_MAX     100000   /* pul/s, 约 2 圈/秒 */
#define HMI_VEL_DEF      20000

#define HMI_CYCLE_US      2000   /* 过程数据周期 (µs), 与 motor_test 缺省一致, 不上 DC */
#define HMI_LOOP_MS          2   /* 循环里的让步节拍 */
#define HMI_STOP_MS        300   /* 进近段留出的刹停时间: 减速度 = v / 0.3s */

/* ---------------------------------------------------------------- 遥测 */

/* 一根轴的一帧快照。全部是**显示坐标**。 */
struct AxisTelem
{
   bool     valid     = false;
   bool     mirror_ok = false;   /* 收到过完整帧 (否则下面的 sw/pos 是陈值) */
   bool     enabled   = false;   /* 6041h bit2 = 电机带电 */
   bool     fault     = false;   /* 6041h bit3 */
   bool     at_target = false;   /* 插值目标已到 want */
   int32_t  pos       = 0;       /* 6064h - origin */
   int32_t  want      = 0;       /* 点击给出的目标 */
   int32_t  tgt       = 0;       /* 本周期真正下发的插值目标 */
   uint32_t vel       = HMI_VEL_DEF;
   uint16_t sw        = 0;
   uint32_t frames    = 0;
   QString  state;               /* em_sw_state_str(sw) 的中文名 */
};

struct BusTelem
{
   bool     connected = false;
   bool     in_op     = false;
   /* 正在做连接/收尾 (SDO、状态机迁移, 会阻塞几秒)。界面靠它决定按钮形态 ——
    * **不要用"点过连接"来推**, 那会和实际的线程状态错开 */
   bool     busy      = false;
   bool     fault     = false;
   int      naxis     = 0;
   int      wkc       = 0;
   int      expected_wkc = 0;
   /* 当前生效的量程 (脉冲)。默认 HMI_RANGE; scan/ 会经 postRange() 改成跟它的区域匹配,
    * 界面靠它画量程、也靠它判断"我要的目标会不会被夹" */
   int32_t  range     = HMI_RANGE;
   QString  note;                /* 最后一条给操作员看的话 */
   AxisTelem ax[EM_MAX_AXES];
};

/* ---------------------------------------------------------------- 线程 */

class EcatThread : public QThread
{
   Q_OBJECT

public:
   explicit EcatThread(QObject *parent = nullptr);
   ~EcatThread() override;

   /* ---- GUI 线程调用: 生命周期。排队给工作线程执行, 不阻塞 ---- */
   void postListAdapters();            /* 结果走 adaptersListed() 信号 */
   void postConnect(const QString &ifname);
   void postDisconnect();
   void postEnable();
   void postDisable();
   void postStop();                    /* 冻在当前位置, **保持使能** */
   void postZeroHere(int axis);        /* 把当前位置设为显示坐标 0 */
   void postCenter(int axis);          /* 走到显示坐标 0 */
   void postCenterAll();
   void postRange(int32_t range);      /* 改量程 (脉冲)。见 ecatworker.cpp 的 doRange */

   /* ---- GUI 线程调用: 每周期都要用的两个量, 加锁直接写 ---- */
   void setTarget(int axis, int32_t want_disp);
   void setSpeed (int axis, uint32_t vel);

   /* ---- 遥测 ---- */
   BusTelem telemetry() const;

   /* 收尾时未能确认失能 (**CLI 的退出码 10 就是它**) —— 必须在界面上弹模态告警 */
   bool maybeLive() const { return m_maybe_live; }

   /* 请求工作线程退出并做收尾 (关网卡)。调用方随后 wait() */
   void requestQuit();

signals:
   /* 给操作员看的一次性消息: 连接失败 / 使能失败 / 故障。别拿它做逐周期刷新 */
   void notify(const QString &text);

   /* 网卡清单 (names[i] 就是 postConnect 要的字符串)。走这里而不是让界面自己调
    * em_list_adapters: 界面侧要**一个 motor_api 函数都不调**, 那条边界才守得住 */
   void adaptersListed(const QStringList &names, const QStringList &descs);

protected:
   void run() override;

private:
   enum CmdType
   {
      CMD_LIST, CMD_CONNECT, CMD_DISCONNECT, CMD_ENABLE, CMD_DISABLE,
      CMD_STOP, CMD_ZERO, CMD_CENTER, CMD_RANGE
   };
   struct Cmd
   {
      CmdType type  = CMD_STOP;
      int     axis  = -1;
      int32_t value = 0;      /* CMD_RANGE 用 */
      QString text;
   };

   /* 以下全部在工作线程里跑 */
   void drainCommands();
   void doListAdapters();
   void doConnect(const QString &ifname);
   void doEnable();
   void doStop();
   void doZero(int axis);
   void doCenter(int axis);
   void doRange(int32_t range);
   void tryInitOrigin();
   void interpolate(uint32_t dt_ms);
   void publish(int wkc);
   void teardown();

   void note(const QString &s);   /* 记进遥测 + 发给界面 */

   mutable QMutex m_mtx;                 /* 保护 m_cmds / m_want / m_vel / m_telem */
   QQueue<Cmd>    m_cmds;
   int32_t        m_want[EM_MAX_AXES] = {0};
   uint32_t       m_vel [EM_MAX_AXES] = {0};
   BusTelem       m_telem;
   QString        m_note;

   /* 以下只有工作线程碰, 不需要锁 */
   em_bus_t  *m_bus   = nullptr;
   em_axis_t *m_ax[EM_MAX_AXES] = {nullptr};
   int        m_naxis = 0;
   bool       m_in_op = false;
   bool       m_busy  = false;      /* 见 BusTelem::busy */
   bool       m_origin_ready = false;
   int32_t    m_origin[EM_MAX_AXES] = {0};
   int32_t    m_tgt   [EM_MAX_AXES] = {0};
   bool       m_fault_latched = false;

   /* 当前量程 (脉冲)。**不是普通成员**: postRange 在工作线程里改它, 而 setTarget 在
    * GUI 线程里读它 (夹取用), interpolate 又在工作线程里读 —— 所以用原子量, 不另加锁。
    * 默认 HMI_RANGE, 于是 hmi 自己的行为一个字都不变。 */
   std::atomic<int32_t> m_range{HMI_RANGE};

   std::atomic<bool> m_quit{false};
   bool       m_maybe_live = false;
};
