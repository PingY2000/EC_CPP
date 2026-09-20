#include "ecatworker.h"

#include <QElapsedTimer>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <cstdlib>

EcatThread::EcatThread(QObject *parent) : QThread(parent)
{
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      m_vel[i]  = HMI_VEL_DEF;
      m_want[i] = 0;
      /* "还没读过"必须显式写: 0 是一个合法的模式号 (未定义), 不能拿它冒充"没读过" */
      m_mode_disp[i] = HMI_MODE_DISP_UNREAD;
   }
}

EcatThread::~EcatThread()
{
   requestQuit();
   wait(15000);
}

/* 记一句给操作员看的话: 存进遥测 + 打到控制台 + 发给界面弹一次。工作线程调 */
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

/* 只进控制台的一条: 不进状态栏、不发通知。连接那一次要说的事实有好几行 (每根轴一条),
 * 状态栏只留得下一句结论 —— 逐轴的读数走这里。工作线程调 */
static void consoleNote(const QString &s)
{
   std::printf("[hmi] %s\n", s.toUtf8().constData());
   std::fflush(stdout);
}

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

void EcatThread::postHome(int axis, int method, uint32_t vel_fast)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_HOME; c.axis = axis; c.method = method;
   c.value = (int32_t)vel_fast;
   m_cmds.enqueue(c);
}

/* 「停止」在回零期间走这一个 —— **全程序唯一一处 GUI 线程直呼 motor_api**。
 * 安全: em_request_stop() 只往一个 `static volatile sig_atomic_t` 里存 1, 不碰总线/网卡。
 * 非如此不可: 回零阻塞在工作线程里, 一条 CMD_STOP 要等它自己退出来才轮到。 */
void EcatThread::requestMotionStop()
{
   em_request_stop();
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

void EcatThread::setNpnWriteDrive(bool on)
{
   QMutexLocker lk(&m_mtx);
   m_npn_write_drive = on;
}

bool EcatThread::npnWriteDrive() const
{
   QMutexLocker lk(&m_mtx);
   return m_npn_write_drive;
}

/* 「上位机侧取反」—— 运行期参数, 所以不加锁、不进命令队列: 界面勾一下,
 * 下一帧 publish() 就用上了。 */
void EcatThread::setDiInvert(bool on)
{
   m_di_invert.store(on);
}

bool EcatThread::diInvert() const
{
   return m_di_invert.load();
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

         /* dt 用**实测值** (Windows 不是实时系统); 卡顿按 100ms 封顶 */
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

         /* 没进 OP 也要刷遥测: 界面的按钮形态从遥测推出来, 而连接期不发帧 */
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

      /* 每一条命令都从"干净"开始: g_stop 是**进程级**的而且不会自己清, 上一条命令留下的
       * 那个 1 会让下一条在第一次检查处当场中止 (em__cw_step / em_wait_sw / em_set_mode /
       * em_home / em_fault_reset 都读它)。 */
      em_clear_stop();

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
         case CMD_HOME:       doHome(c.axis, c.method, (uint32_t)c.value); break;
      }
   }
}

/* 填网卡下拉框。返回的是**这块网卡在 SOEM 里的名字** (`\Device\NPF_{GUID}`), 不是描述 */
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

/* m_busy 的**唯一一对**写点就在这里 —— 内层有光秃秃的 return (em_bus_new 失败、打不开
 * 网卡): 既没调 teardown() 也没人清 m_busy, 于是 m_busy 永远是 true, 界面把
 * in_op || busy 当成"占着总线", 亮着一片本不存在的按钮。 */
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

   /* 选**总线上全部从站**, 不是"前两台": em_setup 的硬要求 —— 留在 PRE_OP 的从站
    * 不参与过程数据交换, 会让整帧的 WKC 持续偏短。 */
   em_axis_cfg_t cfg[EM_MAX_AXES];
   for (int i = 0; i < n; i++)
   {
      cfg[i].bus_pos = i;
      cfg[i].pos_tol = 0;      /* 0 = 用 EM_POS_TOL_DEF */
   }

   /* 60FDh 那个可选项必须在 em_setup **之前**设 —— 它是个连接期参数。
    * 默认关着: 只绑不补, 不动驱动器的映射。 */
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

   /* 2300h 输入有效电平逻辑 (输入端子 X0~X2 的常开/常闭)。这件事在进 OP 之前做:
    * 极性配反的机器上 6041h bit11 恒为 1, 一进 OP 就是"两个限位都压着"的样子。
    *
    * 无论勾没勾都**先只读探一遍**: 不探就不知道这台机器的原值, 事后说"还原了"没有依据。
    * 失败要等进 OP 之后才报 —— 那条 note() 要覆盖掉"已进 OP"那一句 (见文件末尾)。 */
   QString di_fail;
   {
      bool want_npn;

      {
         QMutexLocker lk(&m_mtx);
         want_npn = m_npn_write_drive;
      }

      for (int i = 0; i < em_axis_count(m_bus); i++)
      {
         em_axis_t *ax = em_axis(m_bus, i);
         uint32_t   v  = 0;
         int        sz = 0;

         if (em_rd_any(m_bus, em_axis_slave(ax), EM_OID_DI_LOGIC, 0, &v, &sz) != 0)
         {
            consoleNote(QStringLiteral("%1: 2300h 读不到 (输入有效电平逻辑)"
                                       " —— 极性未知, 三个灯的含义不可判")
                           .arg(QString::fromUtf8(em_axis_label(ax))));
            continue;
         }

         consoleNote(QStringLiteral("%1: 2300h = 0x%2 (%3 字节)  X0~X2 = %4/%5/%6%s")
                        .arg(QString::fromUtf8(em_axis_label(ax)))
                        .arg(v, 4, 16, QLatin1Char('0'))
                        .arg(sz)
                        .arg((v & 1u) ? QStringLiteral("常闭") : QStringLiteral("常开"))
                        .arg((v & 2u) ? QStringLiteral("常闭") : QStringLiteral("常开"))
                        .arg((v & 4u) ? QStringLiteral("常闭") : QStringLiteral("常开"))
                        .arg(EM_DI_LOGIC_EQ(v, EM_DI_LOGIC_NPN)
                                ? QStringLiteral("   <- NPN 传感器该有的极性")
                                : QStringLiteral("   <- NPN 传感器要的是 0x0007")));
      }

      if (want_npn)
      {
         em_allow_param_write(m_bus, 1);
         if (em_di_set_logic(m_bus, EM_DI_LOGIC_NPN) != 0)
            di_fail = QStringLiteral(
               "2300h 写入失败 (原因见控制台): 有轴仍是原极性 -> 驱动器照旧把「没触发」"
               "读成「触发」, 定位与限位一起错, 扫描可能开不了。"
               "退路: 勾上「高级选项」里的「上位机侧取反」");
         em_allow_param_write(m_bus, 0);
      }
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

   /* 刚进 OP 时读一次 6061h —— 读到的是驱动器上电后自己认的模式 */
   for (int i = 0; i < m_naxis; i++)
      readModeDisp(i);

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

   /* 这里**不写 m_busy** —— 它是外面那个壳一个人的事 (见 doConnect 上面那段) */
   note(QStringLiteral("已进 OP, %1 根轴。电机仍未带电 —— 点「使能」才会带电")
           .arg(m_naxis));

   /* 2300h 没写成就覆盖掉上面那一句: 状态栏只留得下一条, 而这条更要紧 */
   if (!di_fail.isEmpty())
      note(di_fail);
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

   /* 使能走完, 6061h 应当就是上面刚设的 CSP, 读一次为界面留一份 */
   for (int i = 0; i < m_naxis; i++)
      readModeDisp(i);

   /* 使能成功了。把界面侧的目标值也钉在"现在这里", 于是**使能那一帧不会产生任何运动** */
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

/* 故障复位 (6040h bit7 上升沿), 阻塞最多 1 秒/轴。
 * em_fault_reset() 先写 6040h = 0x0000 (卸力) 打十帧、才抬 bit7, 而"本来就没故障"是到
 * 函数末尾才报的 —— 对一根健康的轴做这件事会**真的卸力**, 所以判据必须在调用之前。 */
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
      /* 用**刚读到的**状态字, 不用遥测快照 —— 而这一条判断决定要不要卸力 */
      if (ecatcmd::axis_needs_reset(true,
                                    em_mirror_ok(m_ax[i]) != 0,
                                    (em_sw(m_ax[i]) & EM_SW_FAULT) != 0))
         todo[ntodo++] = i;
   }

   if (ntodo == 0)
   {
      /* 这一句是重点: **真的一个字节都没写** */
      note(QStringLiteral("没有轴报故障 (6041h bit3 都是 0) -> **一个字节都没写**。"
                          "复位只对报故障的轴做: 它的动作是先把 6040h 写成 0x0000 "
                          "(卸力) 再抬 bit7, 对一根健康的轴做这件事会松开它的保持力矩"));
      return;
   }

   /* ---- 2. 逐轴复位, 阻塞 (每轴最多 EM_STEP_TMO_MS = 1000ms) ----
    * 下面那两次**加锁直写 m_telem.resetting** 是界面能看见"正在复位…"的唯一原因
    * (publish() 与本函数同线程); 复位**不可中断**, 界面只能把它按住。 */
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

      /* 比的是 0, 不是 EM_R_OK (那是 ec_motor_internal.h 里的内部宏, 界面侧不 include) */
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

   /* ---- 3. 一次说完: note() 是**覆盖写**, 逐轴各 note 一句的话前一句会被冲掉 ---- */
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

   /* m_fault_latched **不在这里碰**: 它只有一个写者 (publish()), 清除条件就是 "bit3 掉了"。
    * m_tgt / m_want 也不碰 —— 复位后该轴是失能态, interpolate() 跳过失能轴。 */
   note(s);
}

/* 回零 —— 驱动器自带的 HM 模式 (6060h = 6)。四个方式: 24/29 = 正/反向找**原点开关**,
 * 18/17 = 找**正/负限位开关** (手册 V2.4 p46~p48, 每个各带 a)/b) 两条分支)。
 * 三段顺序不能动: **闸 (一个字节都不写) -> 宣告 -> 动作 + 无条件收尾**, 因为 em_home() 的
 * 五条返回路径留下的状态没有一条可以不管 (后三条举着 bit4 返回, 驱动器那一刻还在找)。
 * 17/18 多一道闸 (两道否决) 与一句分支预告, 位置在两道现有闸之后、宣告之前。 */
void EcatThread::doHome(int axis, int method, uint32_t vel_fast)
{
   if (axis < 0 || axis >= EM_MAX_AXES)
      return;                    /* 编程错误, 不是操作员的事 */

   const bool bus_ready = (m_bus != nullptr) && m_in_op;
   em_axis_t *ax = bus_ready ? m_ax[axis] : nullptr;

   /* 「还有轴在走」用工作线程**自己的真相** (m_want 对 m_tgt), 不用遥测 (有滞后);
    * 回零期间 interpolate() 不跑, 另一根轴的目标会停在半途。 */
   bool any_moving = false;
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
         if (m_ax[i] != nullptr && m_want[i] != m_tgt[i])
            any_moving = true;
   }

   /* 用**刚读到的** 6041h: 本函数第一件事 em_disable() 真的会撤掉保持力矩 */
   const bool mirror_ok = (ax != nullptr) && em_mirror_ok(ax) != 0;
   const bool fault     = (ax != nullptr) && (em_sw(ax) & EM_SW_FAULT) != 0;

   /* 只放行四个 (24/29 找原点, 18/17 找限位)。em_home() 自己只查 [1,35], 别的方式的方向
    * 语义没验过, 放进来是拿滑台去试 —— 而回零是**软件兜不住**的动作。 */
   if (!ecatcmd::home_method_allowed(method))
   {
      note(QStringLiteral("回零方式 %1 不在允许的范围内 (只用 24/29 找原点、18/17 找限位) "
                          "-> **一个字节都没写**")
              .arg(method));
      return;
   }

   const char *why = ecatcmd::home_refusal(bus_ready && ax != nullptr, m_origin_ready,
                                           mirror_ok, fault, any_moving);
   if (why != nullptr)
   {
      note(QStringLiteral("%1 回零没有发起: %2 -> **一个字节都没写**")
              .arg(QString::fromUtf8(ecatcmd::axis_label(axis)),
                   QString::fromUtf8(why)));
      return;
   }

   const QString nm = QString::fromUtf8(ecatcmd::axis_label(axis));

   /* ---- 找限位 (17/18) 的第二道闸 + 分支预告 ----
    * 位置在这里是量出来的: 早了没状态 (上面那道闸刚放行), 晚了已经卸力 (下面就是
    * em_disable)。判据用**驱动器自己**那两位 (em_di_poslim / em_di_neglim, 即 2300h +
    * 2310h 之后的结果), 不用遥测里反相后的 dig_pos/dig_neg —— 驱动器按它自己的读数
    * 决定怎么走, 上位机反相只改显示。
    *
    * 预告必须打: 手册 a) 与 b) 两条分支的**首段方向是相反的**, 不说一句, 操作员会以为
    * 自己点错了按钮, 而那时电机已经在动。 */
   if (ecatcmd::home_method_is_limit(method))
   {
      const bool dig_known = em_dig_in_known(ax) != 0;
      const bool pos_lim   = em_di_poslim(ax) != 0;
      const bool neg_lim   = em_di_neglim(ax) != 0;
      const bool tgt       = ecatcmd::home_lim_target_active(method, pos_lim, neg_lim);
      const bool other     = ecatcmd::home_lim_other_active(method, pos_lim, neg_lim);

      const char *no = ecatcmd::home_lim_refusal(dig_known, tgt, other);
      if (no != nullptr)
      {
         note(QStringLiteral("%1 %2没有发起: %3 -> **一个字节都没写**")
                 .arg(nm, QString::fromUtf8(ecatcmd::home_method_short(method)),
                      QString::fromUtf8(no)));
         return;
      }

      note(QString::fromUtf8(ecatcmd::home_lim_branch_text(method, tgt)));
   }

   /* ---- 宣告"正在回零"。**必须在第一个阻塞调用之前** ----
    * 下面那两次加锁直写是"阻塞期间界面还看得见"的唯一原因, 界面靠它把「停止」换成立即中止。 */
   m_homing = true;
   m_homing_axis = axis;
   m_homing_method = method;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.homing = true;
      m_telem.homing_axis = axis;
      m_telem.homing_method = method;
   }

   em_home_cfg_t cfg;
   em_home_cfg_default(&cfg);
   cfg.method   = method;
   cfg.vel_fast = ecatcmd::home_vel_clamp((int32_t)vel_fast);
   cfg.vel_slow = ecatcmd::home_vel_slow(cfg.vel_fast);
   /* **acc 必须跟着速度一起算** (见 home_accel_for); offset 保持 0, 界面上没有它的控件 */
   cfg.acc      = ecatcmd::home_accel_for(cfg.vel_fast);

   /* ★ 先失能。6098h/6099h/609Ah/607Ch **只在未使能时可写** —— 这一刻该轴失去保持力矩,
    * 竖直轴可能下滑, 这件事躲不掉。 */
   int rc_disable = 0;
   if (em_is_enabled(ax))
      rc_disable = em_disable(ax);

   /* 0 = 到位 / 1 = 被停止请求中止 / 负 = 失败。比字面量, 不比 EM_R_OK (那是内部宏) */
   const int rc_home = em_home(ax, &cfg, HMI_HOME_TMO_MS);

   /* 收尾。**无条件, 顺序不能动。** */

   /* ---- 0. 再清一次停止标志 ---- 补 drainCommands() 留下的洞: 那个停止请求瞄的是
    * **运动**, 而收尾的全部职责是抵达一个确定状态。 */
   em_clear_stop();

   /* ---- 1. 失能 ---- ★ 不冗余: em_home() 在 bit3 / bit13 / 超时那三条路上是**举着
    * bit4 返回**的, 驱动器那一刻**还在找**, 让它停下来的正是这里。 */
   if (em_is_enabled(ax))
   {
      const int rc = em_disable(ax);
      if (rc_disable == 0)
         rc_disable = rc;
   }

   /* ---- 2. 切回 CSP: em_set_mode 在已使能时会被拒, 所以必须排在 1 之后。
    * 不切回来, interpolate() 写的 607Ah 会被驱动器按 HM 解释。 */
   const int rc_mode = em_set_mode(ax, EM_MODE_CSP);

   /* ---- 3. 重新使能到 CSP: em_arm 把 607Ah 钉在此刻的 6064h, 使能那一帧原地不动 */
   int rc_enable = -1;
   if (rc_mode == 0)
      rc_enable = em_enable(ax);

   /* ---- 4. 重新锚定显示原点 ---- ★ **这一行漏掉, 就是一次没人按过按钮的全速运动**:
    * m_origin 若还是回零之前的值, 下一次 interpolate() 会把**回零之前的物理位置**当成
    * CSP 目标发出去。**必须在 3 之后** —— em_arm 钉 607Ah 用"使能那一刻的 6064h"。 */
   m_origin[axis] = em_pos(ax);
   m_tgt[axis]    = 0;
   {
      QMutexLocker lk(&m_mtx);
      m_want[axis] = 0;
   }

   /* ---- 4b. 读一次 6061h: 手册 §3.7 把「6061h 读回 6」当作 HM 的前提。
    * 收尾之后应当是 8 (CSP), 不是 8 就得在结论句里喊出来; 读失败 (-1) 也照实写。 */
   readModeDisp(axis);

   /* ---- 5. 判定 + 复位旗标 + 一次说完 ---- */

   /* 判据是**收尾结束这一刻的实测状态**, 不是上面那几个返回码的排列组合 */
   const bool end_enabled = em_is_enabled(ax) != 0;
   const bool fault_now   = (em_sw(ax) & EM_SW_FAULT) != 0;
   const ecatcmd::HomeEnd end =
      ecatcmd::home_end_state(true, fault_now, end_enabled, rc_mode);

   m_homing = false;
   m_homing_axis = -1;
   m_homing_method = 0;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.homing = false;
      m_telem.homing_axis = -1;
      m_telem.homing_method = 0;
   }

   /* 收尾没能确认到"已卸力" -> 走既有的动力电源告警那条路 (ec_shutdown 也是它) */
   if (end == ecatcmd::HOME_END_STRANDED)
      m_maybe_live = true;

   /* 中段那个动作名。找限位的两个方式号**本身就带方向** (正限位/负限位), 再叠一个
    * "正向/反向"是重复的; 找原点的两个方式号同名, 方向必须补进去才分得清。 */
   const QString what =
      ecatcmd::home_method_is_limit(method)
         ? QString::fromUtf8(ecatcmd::home_method_short(method))
         : QStringLiteral("%1%2")
              .arg(QString::fromUtf8(ecatcmd::home_dir_text(
                      method == ecatcmd::home_method_for(true))),
                   QString::fromUtf8(ecatcmd::home_method_short(method)));

   /* note() 是**覆盖写**, 所以这里一次说完 */
   QString s = QStringLiteral("%1 %2 (方式 %3): %4 (rc = %5)。\n%6")
                  .arg(nm, what,
                       QString::number(method),
                       QString::fromUtf8(ecatcmd::home_cause_text(rc_home)),
                       QString::number(rc_home),
                       QString::fromUtf8(ecatcmd::home_end_text(end)));

   if (end == ecatcmd::HOME_END_HOLDING)
      s += QStringLiteral(" (显示坐标已把这里定为 0)");

   if (end != ecatcmd::HOME_END_HOLDING)
      s += QStringLiteral(" [收尾: 失能 %1 / 切 CSP %2 / 使能 %3]")
              .arg(rc_disable).arg(rc_mode).arg(rc_enable);

   /* 6061h 单独一格, 并**把期望值写进去**: 它是"驱动器现在按哪种模式解释 607Ah"的唯一显示器 */
   {
      const int md = m_mode_disp[axis];

      s += QStringLiteral(" [6061h = %1 (%2)%3]")
              .arg(md)
              .arg(QString::fromUtf8(ecatcmd::mode_text(md)),
                   (md == EM_MODE_CSP)
                      ? QString()
                      : QStringLiteral(" <<< 不是 CSP(8): 607Ah 会被按别的模式解释, "
                                       "先别再走"));
   }

   s += QStringLiteral(" [6099h:01 = %1, :02 = %2 pul/s, 609Ah = %3, 上限 %4 秒]")
           .arg(cfg.vel_fast).arg(cfg.vel_slow).arg(cfg.acc).arg(HMI_HOME_TMO_MS / 1000);

   note(s);
}

/* 读一次该轴的实际运行模式 (6061h, SDO) 存进 m_mode_disp[axis] —— 界面与 doHome 那句
 * 结果话都从这里取值。**只许在本来就阻塞/便宜的时刻调**, publish() 与 interpolate() 不许。 */
void EcatThread::readModeDisp(int axis)
{
   if (axis < 0 || axis >= EM_MAX_AXES || m_ax[axis] == nullptr)
      return;

   m_mode_disp[axis] = em_get_mode(m_ax[axis]);
}

void EcatThread::doStop()
{
   /* 停止 = 把目标冻在当前插值点上。**不写 6040h=0x0000**: 那是卸力, 滑台会自由滑 */
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

   /* disp = pos - origin, 要让现在这点变成 0 就取 origin' = origin + disp。
    * 光动 origin 会让 want/tgt 还是老的显示值 —— 那就等于凭空下了一条新指令,
    * 所以 want/tgt 一起平移, **物理目标点一个脉冲都不动**。 */
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

/* 改量程。interpolate() 是"先夹 m_tgt、再用 m_tgt 算 CSP 目标"的, 所以**改小**时若滑台
 * 正停在旧量程边缘外, 那一夹会把 m_tgt 拽回来 —— 等于凭空一次没人按过按钮的运动。
 * 放大无条件允许; 缩小只在新范围装得下所有轴当前的 m_tgt 时才允许。 */
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
   /* 与 m_busy 同一个写法。**它不是界面"正在复位…"能亮起来的原因** —— 真正让界面看到
    * 的是 doFaultReset 里那两次加锁直写 (同线程, 阻塞期间这一句跑不到); 这一句负责自洽。 */
   t.resetting    = m_resetting;
   /* 回零同一套, 而且它更长 (回零能跑满 30 秒) */
   t.homing        = m_homing;
   t.homing_axis   = m_homing_axis;
   t.homing_method = m_homing_method;
   t.naxis        = m_naxis;
   t.wkc          = wkc;
   t.expected_wkc = (m_bus != nullptr) ? em_expected_wkc(m_bus) : 0;
   t.range        = m_range.load();
   /* 界面靠它知道"现在生效的是哪一条判据"。**一次 load 成局部量**: 不能在循环里一轴
    * load 一次, 否则同一次 publish 里会一半轴按老值、一半按新值算。 */
   const bool di_invert = m_di_invert.load();
   t.di_invert = di_invert;

   for (int i = 0; i < m_naxis; i++)
   {
      em_axis_t *ax = m_ax[i];
      AxisTelem &a  = t.ax[i];

      a.valid     = true;
      a.mirror_ok = em_mirror_ok(ax) != 0;
      /* 只是搬一份**上次读到**的 6061h —— 这里不做 SDO 读 */
      a.mode_disp = m_mode_disp[i];
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

      /* 60FDh 三个开关。**dig_known 与那三个 bool 分开算**: 读不到时 em_di_*() 一律返回 0,
       * 而"三个都没压住"在界面上是个看起来完全正常的结论。 */
      a.dig_known = em_dig_in_known(ax) != 0;
      a.dig_home  = em_di_home(ax)   != 0;
      a.dig_pos   = em_di_poslim(ax) != 0;
      a.dig_neg   = em_di_neglim(ax) != 0;

      /* 「上位机侧取反」。**三个一起翻, 不能只翻一个** —— 2300h 配反了是整排一起
       * 反相。读不到 60FDh 时那三个都是 0, 翻完变成"三个都压着", **这个方向是故意的**:
       * 万一有人在别处漏判了 dig_known, 看到的是"压着"(会拦下来)而不是"松开"(会放过去)。 */
      if (di_invert)
      {
         a.dig_home = !a.dig_home;
         a.dig_pos  = !a.dig_pos;
         a.dig_neg  = !a.dig_neg;
      }

      /* 撞限位: **只此一处算**, 控制器 / 参数栏 / 画布都读这个字段 */
      a.limit_active = ecatcmd::limit_hit(a.sw, a.dig_known, a.dig_pos, a.dig_neg,
                                          di_invert);

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
      /* 不写 0x0000: 故障时驱动器自己会退电, 这里只停止下发新目标 */
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
   m_homing        = false;   /* 同上。真在回零时走到这里, 调用方应当先 requestMotionStop() */
   m_homing_axis   = -1;
   m_homing_method = 0;
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

      /* em_shutdown 自己会: 写 6040h=0x0000 -> 250ms 过程数据 -> 用 6041h 确认 bit2 已清
       * -> 还原 PDO 映射 -> 降回 PRE_OP -> 关网卡。这里不再单独调 em_disable_all。 */
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
