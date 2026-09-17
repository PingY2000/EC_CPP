#include "ecatworker.h"

#include <QElapsedTimer>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <cstdlib>

/* ---------------------------------------------------------------- 构造 */

EcatThread::EcatThread(QObject *parent) : QThread(parent)
{
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      m_vel[i]  = HMI_VEL_DEF;
      m_want[i] = 0;
   }
}

EcatThread::~EcatThread()
{
   requestQuit();
   wait(15000);
}

/*
 * 记录一句给操作员看的话: 存进遥测 (状态栏一直显示) + 打到控制台 + 发给界面弹一次。
 * 工作线程调, 所以 emit 是跨线程的排队投递 —— 界面还没起事件循环时也不会卡住这里。
 */
void EcatThread::note(const QString &s)
{
   {
      QMutexLocker lk(&m_mtx);
      m_note = s;
   }
   /* 控制台是 UTF-8 (em_console_init), 所以用 toUtf8 而不是 qPrintable (那是本地码页) */
   std::printf("[hmi] %s\n", s.toUtf8().constData());
   std::fflush(stdout);
   emit notify(s);
}

/* ---------------------------------------------------------------- 命令 */

void EcatThread::postListAdapters()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_LIST;
   m_cmds.enqueue(c);
}

void EcatThread::postConnect(const QString &ifname)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_CONNECT; c.text = ifname;
   m_cmds.enqueue(c);
}

void EcatThread::postDisconnect()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_DISCONNECT;
   m_cmds.enqueue(c);
}

void EcatThread::postEnable()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_ENABLE;
   m_cmds.enqueue(c);
}

void EcatThread::postDisable()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_DISABLE;
   m_cmds.enqueue(c);
}

void EcatThread::postStop()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_STOP;
   m_cmds.enqueue(c);
}

void EcatThread::postFaultReset()
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_FAULT_RESET;
   m_cmds.enqueue(c);
}

void EcatThread::setWantDigIn(bool on)
{
   QMutexLocker lk(&m_mtx);
   m_want_dig_in = on;
}

bool EcatThread::wantDigIn() const
{
   QMutexLocker lk(&m_mtx);
   return m_want_dig_in;
}

void EcatThread::postZeroHere(int axis)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_ZERO; c.axis = axis;
   m_cmds.enqueue(c);
}

void EcatThread::postCenter(int axis)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_CENTER; c.axis = axis;
   m_cmds.enqueue(c);
}

void EcatThread::postCenterAll()
{
   QMutexLocker lk(&m_mtx);
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      Cmd c; c.type = CMD_CENTER; c.axis = i;
      m_cmds.enqueue(c);
   }
}

void EcatThread::postRange(int32_t range)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_RANGE; c.value = range;
   m_cmds.enqueue(c);
}

void EcatThread::setTarget(int axis, int32_t want_disp)
{
   if (axis < 0 || axis >= EM_MAX_AXES)
      return;

   /* 夹在工作范围内。界面也会夹一次, 这里是第二道 —— 越界的目标不该只靠界面拦 */
   int32_t range = m_range.load();
   if (want_disp >  range) want_disp =  range;
   if (want_disp < -range) want_disp = -range;

   QMutexLocker lk(&m_mtx);
   m_want[axis] = want_disp;
}

void EcatThread::setSpeed(int axis, uint32_t vel)
{
   if (axis < 0 || axis >= EM_MAX_AXES)
      return;
   if (vel < HMI_VEL_MIN) vel = HMI_VEL_MIN;
   if (vel > HMI_VEL_MAX) vel = HMI_VEL_MAX;

   QMutexLocker lk(&m_mtx);
   m_vel[axis] = vel;
}

BusTelem EcatThread::telemetry() const
{
   QMutexLocker lk(&m_mtx);
   return m_telem;
}

void EcatThread::requestQuit() { m_quit.store(true); }

/* ---------------------------------------------------------------- 主循环 */

void EcatThread::run()
{
   QElapsedTimer clk;
   clk.start();
   qint64 last = clk.elapsed();

   while (!m_quit.load())
   {
      drainCommands();

      if (m_bus != nullptr && m_in_op)
      {
         qint64 now = clk.elapsed();
         uint32_t dt = (uint32_t)(now - last);
         last = now;

         /*
          * dt 用**实测值**, 不假设 2ms —— Windows 不是实时系统。卡了一下就按 100ms 封顶:
          * CSP 下让目标一次跳一大步, 就是一次高速冲刺, 而卡顿本身往往意味着总线不稳。
          * 这个口径与 motor_api 的 em_csp_move_multi 一致。
          */
         if (dt == 0)  dt = 1;
         if (dt > 100) dt = 100;

         if (m_origin_ready)
            interpolate(dt);

         int wkc = em_service(m_bus);   /* 每周期都要发帧: 断了驱动器会掉出 OP */

         if (!m_origin_ready)
            tryInitOrigin();

         publish(wkc);
      }
      else
      {
         last = clk.elapsed();

         /*
          * 没进 OP 也要刷一次遥测。**不能只在发帧时刷**: 界面的按钮形态是从遥测推出来的
          * (in_op || busy), 而"正在连接"和"连接失败"这两件事恰好都发生在不发帧的时候 ——
          * 不刷的话界面就永远停在点下去之前的样子。
          */
         publish(0);
      }

      em_sleep_ms(HMI_LOOP_MS);
   }

   teardown();
}

void EcatThread::drainCommands()
{
   for (;;)
   {
      Cmd c;
      {
         QMutexLocker lk(&m_mtx);
         if (m_cmds.isEmpty())
            return;
         c = m_cmds.dequeue();
      }

      switch (c.type)
      {
         case CMD_LIST:       doListAdapters(); break;

         case CMD_CONNECT:    doConnect(c.text); break;

         case CMD_DISCONNECT: teardown(); break;

         case CMD_ENABLE:     doEnable(); break;

         case CMD_DISABLE:
            if (m_bus != nullptr && m_in_op)
            {
               if (em_disable_all(m_bus) == EM_EXIT_OK)
                  note(QStringLiteral("已失能 (电机释放)"));
               else
                  note(QStringLiteral("失能有轴没退干净 —— **电机可能仍带电**, "
                                      "看控制台里是哪一根"));
            }
            /* 失能后目标跟着实际位置, 免得再使能时把旧目标当成新指令 */
            {
               QMutexLocker lk(&m_mtx);
               for (int i = 0; i < m_naxis; i++)
                  if (m_ax[i] != nullptr)
                  {
                     m_tgt[i]  = em_pos(m_ax[i]) - m_origin[i];
                     m_want[i] = m_tgt[i];
                  }
            }
            break;

         case CMD_STOP:       doStop(); break;
         case CMD_ZERO:       doZero(c.axis); break;
         case CMD_CENTER:     doCenter(c.axis); break;
         case CMD_RANGE:      doRange(c.value); break;
         case CMD_FAULT_RESET: doFaultReset(); break;
      }
   }
}

/* ---------------------------------------------------------------- 生命周期 */

/*
 * 填网卡下拉框。返回的是**这块网卡在 SOEM 里的名字** (`\Device\NPF_{GUID}`), 不是描述 ——
 * 描述只是给人看的, 名字才是 postConnect 要的东西。
 */
void EcatThread::doListAdapters()
{
   em_adapter_t list[32];
   int n = em_list_adapters(list, 32);

   QStringList names, descs;
   for (int i = 0; i < n && i < 32; i++)
   {
      names << QString::fromUtf8(list[i].name);
      descs << QString::fromUtf8(list[i].desc);
   }

   if (n == 0)
      note(QStringLiteral("一块网卡都没找到 —— 多半是 Npcap 没装, 或当前不是管理员"));

   /* 也往控制台打一份。界面上只有描述, 而排查时要看的是那个 `\Device\NPF_{GUID}` 名字 */
   std::printf("[hmi] 找到 %d 块网卡\n", n);
   for (int i = 0; i < names.size(); i++)
      std::printf("    - %s  (%s)\n", names[i].toUtf8().constData(),
                                     descs[i].toUtf8().constData());
   std::fflush(stdout);

   emit adaptersListed(names, descs);
}

/*
 * m_busy 的**唯一一对**写点就在这个壳里 —— 里面那个函数一个都不管。
 *
 * 为什么单拆一层: 里面那个函数有七条出口, 其中**两条是光秃秃的 `return`**
 * (em_bus_new 失败、打不开网卡) —— 既没调 teardown(), 也没有第二个人清 m_busy,
 * 于是 m_busy **永远是 true**。(其余几条走 teardown(), 而 teardown 末尾自己会把它
 * 清掉, 所以那几条本来是好的 —— 修的时候别把它们一起算进去。)
 *
 * 那不只是"按钮形态不对": 界面把 `onair = in_op || busy` 当成"占着总线"
 * (见 ScanWindow::refresh()), 于是 m_connected 也永远为真 —— 「使能」「设为零点」
 * 「开始扫描」全部亮着, 而底下什么都没有。
 * (scan 那个「让 60FDh 进 TxPDO」勾选框从前正是被这个坑卡住的: 它 setEnabled(!onair),
 * 而"连接失败过一次"的人恰恰是最需要勾它的那个 —— 现在那个框不跟着 onair 变灰了,
 * 见 ScanWindow::refresh() 与 onWantDigInToggled()。)
 *
 * ⚠️ 另外要认清一件事: **界面在连接期间其实看不到 busy**。中间那几秒是阻塞的, 而
 * publish() 与本函数同线程 (run() 里 drainCommands 在循环顶、publish 在循环底),
 * 那期间一次都跑不到 —— 所以"界面据此把按钮锁住, 免得点两下"这件事并没有真的发生
 * (docs/scan_sweep.md §13 小瑕疵记着这一条)。这个壳的职责是**把状态收干净**,
 * 不是"让按钮在连接期间变形态"; 真要后者得照 doFaultReset() 那样加锁直写。
 */
void EcatThread::doConnect(const QString &ifname)
{
   m_busy = true;
   doConnectInner(ifname);
   m_busy = false;
}

void EcatThread::doConnectInner(const QString &ifname)
{
   if (m_bus != nullptr)
      teardown();

   m_bus = em_bus_new();
   if (m_bus == nullptr)
   {
      note(QStringLiteral("em_bus_new 失败"));
      return;
   }

   std::printf("\n==== hmi 连接 %s ====\n", ifname.toUtf8().constData());
   std::fflush(stdout);

   QByteArray ifn = ifname.toUtf8();
   int n = em_open(m_bus, ifn.constData());
   if (n <= 0)
   {
      note(QStringLiteral("打不开网卡。多半是**被别的程序独占** (Npcap 单进程), "
                          "或没装 Npcap, 或不是管理员。先确认没有别的东西在用这张卡"));
      em_bus_free(m_bus);
      m_bus = nullptr;
      return;
   }

   if (n > EM_MAX_AXES)
   {
      note(QStringLiteral("总线上有 %1 台从站, 超过本接口的上限 %2 —— 不连")
              .arg(n).arg(EM_MAX_AXES));
      teardown();
      return;
   }

   /*
    * 选**总线上全部从站**, 不是"前两台": em_setup 的硬要求 —— 留在 PRE_OP 的从站不参与
    * 过程数据交换, 会让整帧的 WKC 持续偏短, 与"过程数据没落地"分不开。
    */
   em_axis_cfg_t cfg[EM_MAX_AXES];
   for (int i = 0; i < n; i++)
   {
      cfg[i].bus_pos = i;
      cfg[i].pos_tol = 0;      /* 0 = 用 EM_POS_TOL_DEF */
   }

   /*
    * 60FDh 那个可选项必须在 em_setup **之前**设 —— 它是个连接期参数。
    * 默认关着: 只绑不补, 不动驱动器的映射 (理由见 motor_api 的 em_require_dig_in)。
    */
   {
      QMutexLocker lk(&m_mtx);
      em_require_dig_in(m_bus, m_want_dig_in ? 1 : 0);
   }

   if (em_setup(m_bus, cfg, n, /*allow_remap=*/1) != 0)
   {
      note(QStringLiteral("em_setup 失败 —— 上面有具体原因 (缺映射 / 偏移证不出来 / "
                          "从站不在预期状态)。控制台里每一条都写明了"));
      teardown();
      return;
   }

   if (em_enter_op(m_bus, /*use_dc=*/0, HMI_CYCLE_US) != 0)
   {
      note(QStringLiteral("进 OP 失败 —— 见控制台。不要反复点「连接」, 先看原因"));
      teardown();
      return;
   }

   m_in_op = true;
   m_naxis = em_axis_count(m_bus);
   for (int i = 0; i < m_naxis; i++)
      m_ax[i] = em_axis(m_bus, i);
   m_origin_ready = false;
   m_fault_latched = false;

   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
      {
         m_want[i] = 0;
         m_tgt[i]  = 0;
         if (m_vel[i] < HMI_VEL_MIN || m_vel[i] > HMI_VEL_MAX)
            m_vel[i] = HMI_VEL_DEF;
      }
   }

   /* 这里**不写 m_busy** —— 它是外面那个壳一个人的事 (见 doConnect 上面那段)。
    * 底下也故意不写: 漏一条就是"永远忙", 而那会连带把界面锁死一大片 */
   note(QStringLiteral("已进 OP, %1 根轴。电机仍未带电 —— 点「使能」才会带电")
           .arg(m_naxis));
}

void EcatThread::doEnable()
{
   if (m_bus == nullptr || !m_in_op)
   {
      note(QStringLiteral("还没连上总线"));
      return;
   }
   if (!m_origin_ready)
   {
      note(QStringLiteral("还没收到完整的过程数据帧 -> 位置未知, 拒绝使能"));
      return;
   }

   for (int i = 0; i < m_naxis; i++)
   {
      if (em_is_enabled(m_ax[i]))
         continue;
      if (em_set_mode(m_ax[i], EM_MODE_CSP) != EM_EXIT_OK)
      {
         note(QStringLiteral("轴%1: 切到 CSP 模式失败 -> 中止使能 (原因见控制台)")
                 .arg(i));
         return;
      }
   }

   if (em_enable_all(m_bus) != EM_EXIT_OK)
   {
      note(QStringLiteral("使能失败 —— 控制台里写着是哪一步。"
                          "**不要重复点**: 先看是不是 6041h 报了故障或限位"));
      return;
   }

   /*
    * 使能成功了。把目标钉在"现在这里": em_arm() 在使能前已经把 607Ah 钉在当前实际位置,
    * 这里把界面侧的目标值也对齐 —— 于是**使能那一帧不会产生任何运动**。
    */
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
      {
         m_tgt[i]  = em_pos(m_ax[i]) - m_origin[i];
         m_want[i] = m_tgt[i];
      }
   }

   note(QStringLiteral("已使能 %1 根轴 (有保持力矩)。点画布上的位置就走过去")
           .arg(m_naxis));
}

/*
 * 故障复位 (6040h bit7 上升沿)。
 *
 * 与 doEnable() 同一种形状: 阻塞最多 1 秒/轴, 跑在工作线程里, 期间遥测不刷新。
 *
 * ── 为什么这里要自己再筛一遍"哪根轴真的报了故障" ──────────────────────────────
 *
 * em_fault_reset() 里头确实会看一眼 6041h bit3, 但那是**到函数末尾**才看的, 用来报告
 * "本来就没有故障, 这次是空操作"。而它的动作顺序是**先**写 6040h = 0x0000
 * (Disable voltage = 卸力) 打十帧, **然后**才抬 bit7 —— bit7 是上升沿触发, 不先把 0
 * 压下去就构不成沿, 这个顺序本身是对的。
 *
 * 问题在于: 对**一根没有故障的轴**做这件事, 那一轴会**真的卸力**。竖直滑台会掉下来。
 * 所以"哪根轴该复位"必须在**调用之前**定好, 而判据只能是 6041h bit3。
 *
 * 判据本身在 ecatcmd::axis_needs_reset() (头文件里) —— 放那里的理由是 scan_selftest
 * 不编这个 .cpp, 写在 .cpp 里那条逻辑就永远验不到。
 */
void EcatThread::doFaultReset()
{
   if (m_bus == nullptr || !m_in_op)
   {
      note(QStringLiteral("还没连上总线"));
      return;
   }

   /* ---- 1. 先算"该复位谁"。**这一步之前一个字节都不写** ---- */
   int todo[EM_MAX_AXES];
   int ntodo = 0;

   for (int i = 0; i < m_naxis; i++)
   {
      /*
       * 用**刚读到的**状态字, 不用自己上一轮发布的快照 —— 快照可能已经是 2ms 前的,
       * 那 2ms 里故障可能刚起来, 也可能已经自己清了。而这一条判断决定要不要卸力。
       */
      if (ecatcmd::axis_needs_reset(true,
                                    em_mirror_ok(m_ax[i]) != 0,
                                    (em_sw(m_ax[i]) & EM_SW_FAULT) != 0))
         todo[ntodo++] = i;
   }

   if (ntodo == 0)
   {
      /*
       * 这一句是本函数的重点, 不是"顺便提一句": **真的一个字节都没写**。
       *
       * 不能改成"那就调一下 em_fault_reset 让它自己发现没故障" —— 它照样会先把
       * 那十帧 6040h = 0x0000 (卸力) 打出去, 然后才打印"本来就没有故障, 是空操作"。
       * 那道闸必须在**调用之前**, 这就是它。
       */
      note(QStringLiteral("没有轴报故障 (6041h bit3 都是 0) -> **一个字节都没写**。"
                          "复位只对报故障的轴做: 它的动作是先把 6040h 写成 0x0000 "
                          "(卸力) 再抬 bit7, 对一根健康的轴做这件事会松开它的保持力矩"));
      return;
   }

   /*
    * ---- 2. 逐轴复位。这一段是阻塞的 (每轴最多 EM_STEP_TMO_MS = 1000ms) ----
    *
    * 下面那两次**加锁直写 m_telem.resetting** 是界面能看见"正在复位…"的唯一原因:
    * run() 里 drainCommands 与 publish 是同一根线程上的前后两步, 而这里一阻塞就是
    * 最多两三秒, 期间 publish 一次都跑不到 (详见 publish() 里 t.resetting 那段注释)。
    * 界面线程是独立的 30Hz 轮询, 所以它读得到; 而 resetting 一旦置起就**没有"取消"**
    * —— em_fault_reset 里那个 em_stop_requested() 在 GUI 里是死的 (只有 motor_test
    * 装了 SIGINT), 所以只能让界面看起来被占住 (按钮禁用 + 文案), 不能真去中断它。
    */
   m_resetting = true;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.resetting = true;
   }

   QStringList ok, bad;

   for (int k = 0; k < ntodo; k++)
   {
      const int i = todo[k];
      const QString nm = (i == 0) ? QStringLiteral("轴X") : QStringLiteral("轴Y");

      /*
       * 比的是 0, 不是 EM_R_OK —— 后者是**内部**宏 (在 ec_motor_internal.h 里),
       * 而界面这一侧只 include ec_motor.h。公开头文件给 em_fault_reset 定的约定
       * (见 ec_motor.h:514-519) 就是 "0 = 成功 / -1 = 失败 / 1 = 收到停止请求",
       * 与 em_enable / em_disable 同一套。别为了好看把它 import 进来 —— 那会让界面
       * 侧开始依赖内部头, 而"界面不碰 motor_api 内部"正是这个文件开头那条边界。
       */
      if (em_fault_reset(m_ax[i]) == 0)
         ok << nm;
      else
         bad << nm;
   }

   m_resetting = false;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.resetting = false;
   }

   /*
    * ---- 3. 一次说完 ----
    *
    * note() 是**覆盖写** (它自己会 emit notify), 逐轴各 note 一句的话前一句会被冲掉 ——
    * 两根轴一起复位时操作员只会看到最后一句, 而"哪一根失败了"才是要看的。
    */
   QString s;

   if (!ok.isEmpty())
      s = QStringLiteral("%1 故障已清。该轴现在停在**未使能** —— 复位最后写的是 "
                         "6040h = 0x0000, 要接着走请重新点「使能」").arg(ok.join(QStringLiteral("/")));

   if (!bad.isEmpty())
   {
      if (!s.isEmpty())
         s += QStringLiteral("   ");
      s += QStringLiteral("%1 复位失败 —— **先查清故障原因** (控制台里有 6041h 的实测值, "
                          "603Fh 是故障码), 不要靠反复点硬顶").arg(bad.join(QStringLiteral("/")));
   }

   /*
    * m_fault_latched **不在这里碰**。它只有一个写者 (publish()), 而清除条件就是
    * "bit3 掉了"。手工清是活的危险: 万一上面复位失败、bit3 还举着, 手工清会让
    * publish() 不再冻结目标 —— 于是插补恢复写 607Ah, 而界面说"没故障"。一个所有者。
    *
    * m_tgt / m_want 也不碰: 复位后该轴是失能态, interpolate() 跳过失能轴, 冻着的值
    * 不会产生任何运动; 下一次「使能」会在 doEnable() 里重新对齐。
    */
   note(s);
}

void EcatThread::doStop()
{
   /*
    * 停止 = 把目标冻在当前插值点上。**不写 6040h=0x0000**: 那是卸力, 滑台会自由滑;
    * 带保持力矩停住才是这里要的。要不要撤电由「失能」决定。
    */
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < EM_MAX_AXES; i++)
         m_want[i] = m_tgt[i];
   }

   note(QStringLiteral("已停止: 目标冻在当前位置, 仍带保持力矩(未卸力)"));
}

void EcatThread::doZero(int axis)
{
   if (axis < 0 || axis >= m_naxis)
      return;
   if (!m_origin_ready)
      return;

   int32_t disp = em_pos(m_ax[axis]) - m_origin[axis];

   if (m_want[axis] != m_tgt[axis])
   {
      note(QStringLiteral("轴%1 还在走 -> 等停稳了再设零 (否则零点会落在半路上)")
              .arg(axis));
      return;
   }

   /*
    * 原来的显示坐标: disp = pos - origin。要让**现在这点**变成 0:
    *   origin' = origin + disp   ->  新 disp' = pos - origin' = 0
    * 光动 origin 会让同一个物理位置换一个显示值, 而 want/tgt 还是老的显示值 ——
    * 那就等于凭空下了一条新指令。所以 want/tgt 一起平移, **物理目标点一个脉冲都不动**。
    */
   m_origin[axis] += disp;
   m_tgt[axis]    -= disp;
   {
      QMutexLocker lk(&m_mtx);
      m_want[axis] -= disp;
   }

   note(QStringLiteral("轴%1: 当前位置已设为 0 点 (物理目标未动)").arg(axis));
}

void EcatThread::doCenter(int axis)
{
   if (axis < 0 || axis >= m_naxis)
      return;
   setTarget(axis, 0);      /* 显示坐标 0 = 界面上那个正中 */
}

/*
 * 改量程。**这一步有个必须绕开的陷阱**:
 *
 * interpolate() 是"先夹 m_tgt、再用 m_tgt 算 CSP 目标"的。所以如果把量程**改小**、
 * 而滑台此刻正停在旧量程的边缘外, 那一夹就会把 m_tgt 拽回来 —— 那等于**凭空产生一次
 * 运动, 而且是没人按过任何按钮的运动**。
 *
 * 所以这里的规矩是: 放大无条件允许(放大夹不到东西); 缩小只在新范围装得下所有轴的
 * 当前 m_tgt 时才允许, 装不下就拒绝。**绝不为了迁就新量程去改 want/tgt。**
 */
void EcatThread::doRange(int32_t range)
{
   const int32_t old = m_range.load();

   if (range < 1000)
      range = 1000;                     /* 比 1 圈(50000)还小的量程没有意义, 兜个底 */

   if (range >= old)
   {
      m_range.store(range);
      note(QStringLiteral("量程已从 ±%1 改为 ±%2 脉冲 (只放大, 不产生任何运动)")
              .arg(old).arg(range));
      return;
   }

   /* 缩小 —— 逐轴确认装得下。m_tgt 是工作线程独占的, 这里读它不需要锁 */
   for (int i = 0; i < m_naxis; i++)
   {
      if (m_tgt[i] > range || m_tgt[i] < -range)
      {
         note(QStringLiteral("量程改小被**拒绝**: 轴%1 当前下发目标 %2 超出新量程 ±%3。"
                             "先把滑台走回新量程内 (或点「回中」) —— "
                             "为了迁就新量程去夹一下目标就等于凭空下发一次运动")
                 .arg(i).arg(m_tgt[i]).arg(range));
         return;
      }
   }

   m_range.store(range);
   note(QStringLiteral("量程已从 ±%1 改为 ±%2 脉冲").arg(old).arg(range));
}

void EcatThread::tryInitOrigin()
{
   if (m_naxis < 1)
      return;

   /* 等**全部**轴都收到过完整帧 —— 一次完整帧才有可用的 6064h */
   for (int i = 0; i < m_naxis; i++)
      if (!em_mirror_ok(m_ax[i]))
         return;

   for (int i = 0; i < m_naxis; i++)
   {
      m_origin[i] = em_pos(m_ax[i]);
      m_tgt[i]    = 0;
   }
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
         m_want[i] = 0;
   }

   m_origin_ready = true;
   note(QStringLiteral("零点是**连接时读到的位置**: 界面正中 = 现在这里, 可点范围 ±%1 脉冲 "
                       "(50000 pul/圈 => ±%2 圈)。换个零点用「把当前位置设为 0」")
           .arg(m_range.load())
           .arg(m_range.load() / 50000.0, 0, 'f', 1));
}

void EcatThread::interpolate(uint32_t dt_ms)
{
   int32_t  want[EM_MAX_AXES];
   uint32_t vel [EM_MAX_AXES];
   const int32_t range = m_range.load();

   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < EM_MAX_AXES; i++)
      {
         want[i] = m_want[i];
         vel[i]  = m_vel[i] ? m_vel[i] : HMI_VEL_DEF;
      }
   }

   for (int i = 0; i < m_naxis; i++)
   {
      em_axis_t *ax = m_ax[i];

      /* 没使能就不下发目标: 一个"位置指令"在未使能时是没有任何意义的 */
      if (!em_is_enabled(ax))
         continue;

      int64_t d = (int64_t)want[i] - (int64_t)m_tgt[i];

      if (d != 0)
      {
         double v = (double)vel[i];
         double a = v / (HMI_STOP_MS / 1000.0);              /* 刹停减速度 */
         double allow = std::sqrt(2.0 * a * (double)std::llabs(d));

         if (allow < v)
            v = allow;                                       /* 进近段自动减速, 不冲过头 */

         double stepd = v * (double)dt_ms / 1000.0;
         if (stepd < 1.0)
            stepd = 1.0;                                     /* 快到了也得走一步, 否则到不了 */

         int64_t step = (int64_t)(stepd + 0.5);
         if (step > std::llabs(d))
            step = std::llabs(d);

         m_tgt[i] += (d > 0) ? step : -step;
      }

      /* 显示坐标夹在工作范围内 —— 越界的目标不该只靠点击时那道夹 */
      if (m_tgt[i] >  range) m_tgt[i] =  range;
      if (m_tgt[i] < -range) m_tgt[i] = -range;

      /* 这就是 CSP: 每周期一次, 发的是绝对位置 (驱动器坐标) */
      (void)em_csp_set_target(ax, m_origin[i] + m_tgt[i]);
   }
}

void EcatThread::publish(int wkc)
{
   int32_t  want[EM_MAX_AXES];
   uint32_t vel [EM_MAX_AXES];

   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < EM_MAX_AXES; i++)
      {
         want[i] = m_want[i];
         vel[i]  = m_vel[i];
      }
   }

   BusTelem t;
   t.connected    = (m_bus != nullptr);
   t.in_op        = m_in_op;
   t.busy         = m_busy;
   /*
    * 与 m_busy 同一个写法: 源是工作线程的成员, 每周期向遥测拷贝一次。
    *
    * 但**它不是界面上"正在复位…"能亮起来的原因** —— 这一条要说清, 否则下次改的人
    * 会把真正起作用的那半删掉。publish() 和 doFaultReset() 都在 run() 这一个线程里
    * (drainCommands 在循环顶部, publish 在循环底部), 而 doFaultReset 里那段复位是
    * **阻塞**的 (每轴最多 1 秒): 这期间 run() 整个卡在里面, publish 一次都跑不到,
    * 所以下面这句在复位期间根本没机会执行, 而复位回来时 m_resetting 已经被置回 false
    * —— 只靠这一句, 界面永远看不到 resetting == true。
    *
    * 真正让界面看到的是 doFaultReset 里那两次**加锁直写 m_telem.resetting**: 界面线程
    * 以 30Hz 独立轮询 telemetry(), 工作线程阻塞期间它照样读得到。两处都要:
    * 加锁直写负责"阻塞期间看得见", 这一句负责"循环恢复后整个结构体仍然是自洽的"
    * (也是万一以后把复位改成跨周期状态机时唯一还需要的那半)。
    */
   t.resetting    = m_resetting;
   t.naxis        = m_naxis;
   t.wkc          = wkc;
   t.expected_wkc = (m_bus != nullptr) ? em_expected_wkc(m_bus) : 0;
   t.range        = m_range.load();

   for (int i = 0; i < m_naxis; i++)
   {
      em_axis_t *ax = m_ax[i];
      AxisTelem &a  = t.ax[i];

      a.valid     = true;
      a.mirror_ok = em_mirror_ok(ax) != 0;
      a.frames    = em_mirror_frames(ax);
      a.sw        = em_sw(ax);
      a.state     = QString::fromUtf8(em_sw_state_str(a.sw));
      a.enabled   = em_is_enabled(ax) != 0;
      a.fault     = (a.sw & EM_SW_FAULT) != 0;
      a.pos       = em_pos(ax) - m_origin[i];
      a.tgt       = m_tgt[i];
      a.want      = want[i];
      a.vel       = vel[i];
      a.at_target = (a.want == a.tgt);

      /*
       * 60FDh 三个开关。**dig_known 与那三个 bool 分开算**:
       * 读不到时 em_di_*() 一律返回 0, 而"三个都没压住"在界面上是个看起来完全正常的
       * 结论 —— 不知道和"都没压住"必须分得开, 这里就是分的那一处。
       */
      a.dig_known = em_dig_in_known(ax) != 0;
      a.dig_home  = em_di_home(ax)   != 0;
      a.dig_pos   = em_di_poslim(ax) != 0;
      a.dig_neg   = em_di_neglim(ax) != 0;

      /* 撞限位: **只此一处算**, 控制器 / 参数栏 / 画布都读这个字段 */
      a.limit_active = ecatcmd::limit_hit(a.sw, a.dig_known, a.dig_pos, a.dig_neg);

      if (a.fault)
         t.fault = true;
   }

   if (t.fault && !m_fault_latched)
   {
      m_fault_latched = true;
      {
         QMutexLocker lk(&m_mtx);
         for (int i = 0; i < EM_MAX_AXES; i++)
            m_want[i] = m_tgt[i];
      }
      /* 不写 0x0000: 故障时驱动器自己会退电, 我们只停止下发新目标。
       * 措辞要在 hmi 和 scan **两边都成立** —— 这句话是共用的, 而 scan 那边有「故障复位」。 */
      note(QStringLiteral("6041h bit3 = Fault -> 已冻结目标。查清故障原因 (603Fh 是故障码), "
                          "再用「故障复位」清故障位"));
   }
   else if (!t.fault)
   {
      m_fault_latched = false;
   }

   {
      QMutexLocker lk(&m_mtx);
      t.note = m_note;
      m_telem = t;
   }
}

void EcatThread::teardown()
{
   /* 有东西要收就是"忙": 收尾里有 SDO 与状态机迁移, 界面此时不该让人再点连接 */
   m_busy         = (m_bus != nullptr);

   m_in_op        = false;
   m_naxis        = 0;
   m_origin_ready = false;
   m_fault_latched = false;
   m_resetting     = false;   /* 连接断了, "正在复位"这个状态跟着一起没了 */
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      m_ax[i]     = nullptr;
      m_tgt[i]    = 0;
   }

   if (m_bus != nullptr)
   {
      int live = 0;

      std::printf("\n==== hmi 收尾 ====\n");
      std::fflush(stdout);

      /*
       * em_shutdown 自己会: 给使能中的轴写 6040h=0x0000 -> 打 250ms 过程数据 ->
       * 用 6041h 确认 bit2 已清 -> 还原 PDO 映射 -> 降回 PRE_OP -> 关网卡。
       * 这里不再单独调 em_disable_all, 那只会让每根轴多等一次状态机超时。
       */
      em_shutdown(m_bus, /*restore_mapping=*/1, &live);
      if (live)
         m_maybe_live = true;

      em_bus_free(m_bus);
      m_bus = nullptr;
   }

   m_busy = false;

   {
      QMutexLocker lk(&m_mtx);
      m_telem = BusTelem();
      m_note  = QStringLiteral("已断开");
      for (int i = 0; i < EM_MAX_AXES; i++)
         m_want[i] = 0;
   }
}
