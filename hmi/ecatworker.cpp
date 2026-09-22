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
      /* 同一个坑: 603Fh 的 0x0000 是"无错误"这个真实读数 */
      m_fault_code[i] = HMI_FAULT_CODE_UNREAD;
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

void EcatThread::postHome(int axis, int method, uint32_t vel_fast, int tmo_s)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_HOME; c.axis = axis; c.method = method;
   c.value = (int32_t)vel_fast;
   c.tmo_s = tmo_s;
   m_cmds.enqueue(c);
}

/* 只有 scan/ 会调。默认 false 就是本类自己的老行为 (连接那一刻即零点), 所以 hmi 不开这个
 * 开关时行为逐字节不变。理由全在头文件里。 */
void EcatThread::setKeepOrigin(bool on) { m_keep_origin = on; }

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

/* ---- BlockTick: 阻塞命令期间让遥测继续流动 (理由全在头文件里) ----
 *
 * 回调体 = publish(), 就是那一圈本来要调的东西。区别只是**谁在多线程里跑**: 这是同一个
 * 工作线程 (em_home 就在它里面跑), 所以 m_ax / m_tgt 这些"只有工作线程碰"的成员照旧。
 * 两处要注意:
 *   · **不许在持有 m_mtx 的作用域里构造本对象** —— publish() 自己要拿这把锁, 而它是
 *     非递归锁, 同线程再进一次当场自锁;
 *   · 回调每帧一次 (2ms), 与平时那一圈的节拍一致 —— publish() 本来就是按这个频率写的。 */
EcatThread::BlockTick::BlockTick(EcatThread *t) : m_t(t)
{
   if (m_t != nullptr && m_t->m_bus != nullptr)
      em_set_cycle_hook(m_t->m_bus, &EcatThread::BlockTick::tick, m_t);
}

EcatThread::BlockTick::~BlockTick()
{
   if (m_t != nullptr && m_t->m_bus != nullptr)
      em_set_cycle_hook(m_t->m_bus, nullptr, nullptr);
}

void EcatThread::BlockTick::tick(void *user, int wkc)
{
   static_cast<EcatThread *>(user)->publish(wkc);
}

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
         /* 故障码 603Fh: publish() 只挂牌子, SDO 读在这里做 (见 serviceFaultCodeReads)。
          * 摆在插补之前 —— 牌子是上一圈 publish() 挂的, 而"故障"这件事比"这一帧的目标"
          * 更急。这一趟读到之前, 界面看到的 fault_code 是 UNREAD, 它自己会说"正在读"。
          *
          * **刻意不套 BlockTick** (曾经套过, 2026-09-22 撤掉): 它的 tick 只在 em__cycle
          * 收完一帧时被调, 而 SDO 事务期间**一帧都不发** —— 那个钩子一次都不会响, 套了等于
          * 没套, 却让人以为这块有保护。真正管这件事的是 serviceFaultCodeReads 里那三条
          * (映射里有码就不发 / 短超时 60ms / 帧不健康就不发), 见头文件 HMI_OP_SDO_TMO_US。 */
         serviceFaultCodeReads();

         qint64 now = clk.elapsed();
         uint32_t dt = (uint32_t)(now - last);
         last = now;

         /* dt 用**实测值** (Windows 不是实时系统); 卡顿按 100ms 封顶 */
         if (dt == 0)  dt = 1;
         if (dt > 100) dt = 100;

         if (m_origin_ready)
            interpolate(dt);

         /* 发帧前后各取一次表: 相邻两次的间隔就是"多久没发过帧"。
          * 它把上面那条 SDO、被阻塞的命令、以及本机的调度延迟**全都算进去** ——
          * 这正是要量的东西 (见 BusTelem::max_gap_ms) */
         const qint64 svc_t0 = clk.elapsed();

         int wkc = em_service(m_bus);   /* 每周期都要发帧: 断了驱动器会掉出 OP */

         const qint64 svc_t1 = clk.elapsed();

         if (m_svc_prev_ms >= 0)
         {
            const qint64 gap = svc_t0 - m_svc_prev_ms;

            if (gap > m_max_gap_ms)
               m_max_gap_ms = (int)gap;
            if (gap > HMI_GAP_WARN_MS)
               m_gaps_over++;
         }
         m_svc_prev_ms = svc_t1;

         if (!m_origin_ready)
            tryInitOrigin();

         publish(wkc);
      }
      else
      {
         last = clk.elapsed();
         m_svc_prev_ms = -1;   /* 没在发帧, 别把"连接前的空档"算成一个帧间隔 */

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

         /* 连接也会阻塞几秒, 但**不挂 BlockTick**: 那几秒里就是"什么都没有" (灯全灰、
          * 没有轴), 而 m_ax / m_naxis 正在被重建 —— 从那些帧里 publish 出去的是半成品。 */
         case CMD_CONNECT:    doConnect(c.text); break;

         case CMD_DISCONNECT: teardown(); break;

         /* ---- 会阻塞的四条: 挂 BlockTick, 让那些帧里也发遥测 (理由见头文件) ---- */
         case CMD_ENABLE:
            { BlockTick tk(this); doEnable(); }
            break;

         case CMD_DISABLE:
            if (m_bus != nullptr && m_in_op)
            {
               BlockTick tk(this);
               if (em_disable_all(m_bus) == EM_EXIT_OK)
                  note(QStringLiteral("已失能, 电机已释放"));
               else
                  note(QStringLiteral("失能未完全: 有轴未退干净, 电机可能仍带电 (轴号见控制台)。"));
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

         /* 下面四条不阻塞 (只是改几个成员): 那一圈自己会 publish, 不用挂 */
         case CMD_STOP:       doStop(); break;
         case CMD_ZERO:       doZero(c.axis); break;
         case CMD_CENTER:     doCenter(c.axis); break;
         case CMD_RANGE:      doRange(c.value); break;

         case CMD_FAULT_RESET:
            /* 逐轴阻塞, 每轴最多 1 秒 —— 复位期间让界面能看见"正在复位…" */
            { BlockTick tk(this); doFaultReset(); }
            break;

         case CMD_HOME:
            /* 本程序里最长的一次阻塞 (回零超时那个值 + 收尾; 缺省 120 s, 上限 600 s):
             * 使能灯、三个开关灯、位置、状态栏都在这一段里要跟着动。找限位时尤其 ——
             * 那一趟的目的就是去压那个开关, 灯不跟着亮就没有任何东西能说明它压上了 */
            { BlockTick tk(this); doHome(c.axis, c.method, (uint32_t)c.value, c.tmo_s); }
            break;
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
      note(QStringLiteral("未找到网卡。请安装 Npcap, 并以管理员身份重新运行。"));

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
      note(QStringLiteral("初始化总线失败 (em_bus_new)"));
      return;
   }

   std::printf("\n==== hmi 连接 %s ====\n", ifname.toUtf8().constData());
   std::fflush(stdout);

   QByteArray ifn = ifname.toUtf8();
   int n = em_open(m_bus, ifn.constData());
   if (n <= 0)
   {
      note(QStringLiteral("网卡打开失败。请关闭其他占用该网卡的程序 (Npcap 为单进程), "
                          "并以管理员身份重新运行。"));
      em_bus_free(m_bus);
      m_bus = nullptr;
      return;
   }

   if (n > EM_MAX_AXES)
   {
      note(QStringLiteral("总线从站数 %1 超过本接口上限 %2, 未连接。")
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

   /* 603Fh 也在 setup 之前设, 但**默认就是开**(em_bus_new 里置的), 这里只是把话说出来。
    * 刻意不做成一个新的设置项: 它是一个只读监视量 (与 60FDh 同类), 而这一轮要修的正是
    * "上位机对驱动器报警只有一扇窗" —— 再加一个默认关的开关等于把同一件事再关上一次。
    * 控制台会打出它到底补上没有 (em_setup 里那段) */
   em_require_err_code(m_bus, 1);

   /* 这一趟的帧间隔统计从这里重新开始 (上一个连接的数不该混进来) */
   m_svc_prev_ms = -1;
   m_max_gap_ms  = 0;
   m_gaps_over   = 0;
   m_bad_wkc_run = 0;
   m_comm_bad    = false;
   m_al_state    = 0;
   m_al_code     = 0;
   m_al_checked  = false;

   if (em_setup(m_bus, cfg, n, /*allow_remap=*/1) != 0)
   {
      note(QStringLiteral("总线配置失败 (em_setup)。具体原因逐条写在控制台输出中。"));
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
               "2300h 写入失败, 有轴仍为原极性: 未触发被读成触发, 限位判据随之出错, "
               "扫描可能无法启动。请勾选「上位机侧取反」, 原因见控制台。");
         em_allow_param_write(m_bus, 0);
      }
   }

   if (em_enter_op(m_bus, /*use_dc=*/0, HMI_CYCLE_US) != 0)
   {
      note(QStringLiteral("进入 OP 失败。请查看控制台输出后重试。"));
      teardown();
      return;
   }

   m_in_op = true;

   /* ---- 从这里往下, 每一条 SDO 读都在**停过程数据** ----
    * SOEM 的 SDO 事务期间一帧过程数据都不发 (ecx_SDOread 走邮箱轮询, 见 public 头
    * em_sdo_read 那段说明), 所以"超时"就是"这次读最多把总线静默多久"。默认 700ms 是
    * 配置期的值; 进了 OP 就该压到正常应答的几倍 —— 否则驱动器刚报警、最可能不应答的那一刻
    * 上位机正好静默它 700ms, 自己把看门狗喂掉一次 (2026-09-22 实机量到一次 1638ms 的静默,
    * 就是两根轴各 700ms)。下面 readModeDisp (6061h) 与那三个同步对象都走这条超时。
    * 配置期 (em_setup / 写映射 / 使能之前那些读) 不受影响 —— 那时没有过程数据可静默。 */
   em_set_sdo_timeout(m_bus, HMI_OP_SDO_TMO_US);

   m_naxis = em_axis_count(m_bus);
   for (int i = 0; i < m_naxis; i++)
      m_ax[i] = em_axis(m_bus, i);
   m_origin_ready = false;
   m_fault_latched = false;

   /* 刚进 OP 时读一次 6061h —— 读到的是驱动器上电后自己认的模式 */
   for (int i = 0; i < m_naxis; i++)
      readModeDisp(i);

   /* ---- 只读报一次"同步方式"相关的三个对象 ----
    * 为什么值得查: 本程序**不上 DC** (上面 em_enter_op 的 use_dc=0), 而驱动器那边
    * 2217h「同步帧阈值」= 20 是手册里的一行字 —— **从没在真机上读过**。如果它被配成等
    * SYNC0, 那个计数就是按时钟自己走的, 与上位机在干什么无关, 于是
    * 「挂着没动也报通讯报警」「要断电/按故障复位才清」两件事同时对上。
    * 1C32h:01 才是作数的那个数: 0 自由运行 / 1 SM 同步 / 2 DC 同步。
    * **一个字节都不写**, 只报; 这一轮不加开 DC 的开关。
    * 只读轴 0 —— 同一台机器上驱动器型号与配置相同, 三根轴读三遍是三次往返换一个重复的答案。 */
   if (m_naxis > 0 && m_ax[0] != nullptr)
   {
      static const struct
      {
         uint16_t    idx;
         uint8_t     sub;
         const char *name;
      } kSync[] = {
         { 0x1C32, 0x01, "1C32h:01 同步方式 (0 自由运行 / 1 SM 同步 / 2 DC 同步)" },
         { 0x1C32, 0x02, "1C32h:02 同步周期 (ns)" },
         { 0x2217, 0x00, "2217h    同步帧阈值 (手册 V2.4 p84 附近)" },
      };
      const int slave = em_axis_slave(m_ax[0]);

      for (int k = 0; k < (int)(sizeof(kSync) / sizeof(kSync[0])); k++)
      {
         uint32_t v  = 0;
         int      sz = 0;

         if (em_rd_any(m_bus, slave, kSync[k].idx, kSync[k].sub, &v, &sz) == 0)
            consoleNote(QStringLiteral("同步: %1 = %2 (0x%3, %4 字节)")
                           .arg(QString::fromUtf8(kSync[k].name))
                           .arg(v)
                           .arg(v, 0, 16)
                           .arg(sz));
         else
            consoleNote(QStringLiteral("同步: %1 读不到 (驱动器不支持这个对象?)")
                           .arg(QString::fromUtf8(kSync[k].name)));
      }

      consoleNote(QStringLiteral(
         "同步: 本程序不上 DC (SM 同步 / 自由运行)。上面 1C32h:01 若报 2, "
         "说明驱动器在等 SYNC0 —— 那类报警与上位机发不发帧无关, 需要另配驱动器"));
   }

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
   note(QStringLiteral("已进入 OP, %1 根轴。电机未带电。")
           .arg(m_naxis));

   /* 2300h 没写成就覆盖掉上面那一句: 状态栏只留得下一条, 而这条更要紧 */
   if (!di_fail.isEmpty())
      note(di_fail);
}

void EcatThread::doEnable()
{
   if (m_bus == nullptr || !m_in_op)
   {
      note(QStringLiteral("未连接总线"));
      return;
   }
   if (!m_origin_ready)
   {
      note(QStringLiteral("未收到完整过程数据帧, 位置未知, 拒绝使能。"));
      return;
   }

   for (int i = 0; i < m_naxis; i++)
   {
      if (em_is_enabled(m_ax[i]))
         continue;
      if (em_set_mode(m_ax[i], EM_MODE_CSP) != EM_EXIT_OK)
      {
         note(QStringLiteral("轴%1: 切换到 CSP 模式失败, 已中止使能。原因见控制台。")
                 .arg(i));
         return;
      }
   }

   if (em_enable_all(m_bus) != EM_EXIT_OK)
   {
      note(QStringLiteral("使能失败。请查看控制台输出, 并确认 6041h 故障位与限位状态。"));
      return;
   }

   /* 使能走完, 6061h 应当就是上面刚设的 CSP, 读一次为界面留一份 */
   for (int i = 0; i < m_naxis; i++)
      readModeDisp(i);

   /* 使能成功了。把界面侧的目标值也钉在"现在这里", 于是**使能那一帧不会产生任何运动**。
    *
    * **这一行是"零点跨重连保留"能不能成立的关键, 而且它故意不夹取**(与 interpolate() 每周期
    * 把 m_tgt 夹进 ±m_range 那一道正相反)。保留零点之后 m_origin[] 可以离当前位置任意远,
    * 而这里写的 `pos - origin` 不管多远都照写 —— 于是"使能那一帧原地不动"仍然成立。
    * 要是这里也夹一道, 滑台一使能就会朝零点方向窜回来。**改这一处之前先读 ecatcmd::origin_keep_ok
    * 的注释**: 不夹取的安全性靠的是"只在与量程相容时才沿用零点"那个前置条件, 不是靠这里。 */
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
      {
         m_tgt[i]  = em_pos(m_ax[i]) - m_origin[i];
         m_want[i] = m_tgt[i];
      }
   }

   note(QStringLiteral("已使能 %1 根轴, 带保持力矩。")
           .arg(m_naxis));
}

/* 故障复位 (6040h bit7 上升沿), 阻塞最多 1 秒/轴。
 * em_fault_reset() 先写 6040h = 0x0000 (卸力) 打十帧、才抬 bit7, 而"本来就没故障"是到
 * 函数末尾才报的 —— 对一根健康的轴做这件事会**真的卸力**, 所以判据必须在调用之前。 */
void EcatThread::doFaultReset()
{
   if (m_bus == nullptr || !m_in_op)
   {
      note(QStringLiteral("未连接总线"));
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
      /* 这一句是重点: **真的一个字节都没写**。后半句是"为什么不能拿它当万用清零" ——
       * 复位的第一件事是 6040h = 0x0000 (卸力), 对健康的轴做等于松开它的保持力矩。 */
      note(QStringLiteral("无轴报故障 (6041h bit3 均为 0), 未写入驱动器。"
                          "故障复位会先卸力, 对未报故障的轴执行会松开其保持力矩。"));
      return;
   }

   /* ---- 2. 逐轴复位, 阻塞 (每轴最多 EM_STEP_TMO_MS = 1000ms) ----
    * 下面那两次**加锁直写 m_telem.resetting** 与 BlockTick 是两件事, 现在都能让界面看见
    * "正在复位…": 直写负责"第一个字节之前就写上", BlockTick 负责"阻塞期间一直刷新"
    * (见 drainCommands 的 CMD_FAULT_RESET)。直写留着 —— 它不依赖那条回调挂没挂,
    * 而 publish() 从 m_resetting 拷的那一份要等下一帧才出去。复位**不可中断**。 */
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
      {
         ok << nm;
      }
      else
      {
         /* 失败的轴上带一份**复位之前**读到的 603Fh —— 复位都没清掉的那个故障,
          * 码就是下一步要查的东西。此时它多半已经有值了 (故障沿之后服务工作线程读过) */
         bad << ecatcmd::fault_axis_text(i, m_fault_code[i]);
      }
   }

   m_resetting = false;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.resetting = false;
   }

   /* ---- 3. 一次说完: note() 是**覆盖写**, 逐轴各 note 一句的话前一句会被冲掉 ---- */
   QString s;

   if (!ok.isEmpty())
      s = QStringLiteral("%1 故障已清除。该轴停在未使能 (复位最后写入 6040h = 0x0000)。"
                         "请重新「使能」后继续。").arg(ok.join(QStringLiteral("/")));

   if (!bad.isEmpty())
   {
      if (!s.isEmpty())
         s += QStringLiteral("   ");
      s += QStringLiteral("%1 复位失败。请先按故障码查明原因 (6041h 实测值见控制台)。")
              .arg(bad.join(QStringLiteral("; ")));
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
void EcatThread::doHome(int axis, int method, uint32_t vel_fast, int tmo_s)
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
      note(QStringLiteral("回零方式 %1 不在允许范围内 (仅 24/29 找原点, 18/17 找限位), 未写入驱动器。")
              .arg(method));
      return;
   }

   const char *why = ecatcmd::home_refusal(bus_ready && ax != nullptr, m_origin_ready,
                                           mirror_ok, fault, any_moving);
   if (why != nullptr)
   {
      note(QStringLiteral("%1 回零未发起, 驱动器未写入。%2")
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
         note(QStringLiteral("%1 %2 未发起, 驱动器未写入。%3")
                 .arg(nm, QString::fromUtf8(ecatcmd::home_method_short(method)),
                      QString::fromUtf8(no)));
         return;
      }

      note(QString::fromUtf8(ecatcmd::home_lim_branch_text(method, tgt)));
   }

   /* ---- 宣告"正在回零"。**必须在第一个阻塞调用之前** ----
    * 下面那两次加锁直写让界面**从第一个字节之前**就看得见 (界面靠它把「停止」换成立即中止);
    * 之后这一整段之所以一直在刷新, 靠的是 CMD_HOME 上那个 BlockTick (见头文件)。
    * 两件事都要: 直写不依赖回调挂没挂, 而回调那一份要等下一帧才出去。 */
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

   /* 回零超时的第二道夹取 (第一道是界面那个 spin box 的 setRange), 顺手把秒换算成毫秒。
    * **全程序唯一一次 `* 1000`** —— 单位在 ini/界面/Cmd 里一律是秒, 换算点只有这一处。 */
   const int      tmo_s2 = ecatcmd::home_tmo_s_clamp(tmo_s);
   const uint32_t tmo_ms = (uint32_t)tmo_s2 * 1000u;

   /* ★ 先失能。6098h/6099h/609Ah/607Ch **只在未使能时可写** —— 这一刻该轴失去保持力矩,
    * 竖直轴可能下滑, 这件事躲不掉。 */
   int rc_disable = 0;
   if (em_is_enabled(ax))
      rc_disable = em_disable(ax);

   /* 0 = 到位 / 1 = 被停止请求中止 / 负 = 失败。比字面量, 不比 EM_R_OK (那是内部宏) */
   const int rc_home = em_home(ax, &cfg, tmo_ms);

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
   /* 零点搬了 —— 世代 +1。**无条件**: 这一步在上面那几条失败路上也跑 (超时/bit3/bit13
    * 都是举着 bit4 返回的), 而无论成败, m_origin[] 确实换了一个值。
    * 同时把"这份零点归本次运行所有"重新盖一次章: 回零之后任何时候断开重连, 沿用的都是它。 */
   m_origin_gen++;
   m_origin_kept  = true;
   m_origin_naxis = m_naxis;

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
                      : QStringLiteral(" <<< 当前不是 CSP(8): 607Ah 会按其他模式解释, 请勿继续下发位置。"));
   }

   s += QStringLiteral(" [6099h:01 = %1, :02 = %2 pul/s, 609Ah = %3, 上限 %4 s]")
           .arg(cfg.vel_fast).arg(cfg.vel_slow).arg(cfg.acc).arg(tmo_s2);

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

/* 故障码 603Fh。**6041h bit3 只说"有故障", 说不了是哪一个** —— 过流/过压/欠压/动力线/
 * 通讯/传感器在界面上长得一模一样, 而处置办法完全不同, 所以必须把 603Fh 读出来。
 *
 * 分工: publish() 每帧扫 bit3, 看见某个轴报故障就给它挂一块牌子 (m_fault_read_want);
 * 真正那条 SDO 读在**这里**做 —— publish() 是每帧跑的, 里面不许有 SDO。
 *
 * ⚠️ 这条 SDO 的代价**不是"几毫秒"**(2026-09-22 更正; 原来这里就是这么写的, 写错了):
 * SOEM 的 SDO 事务期间**完全不发过程数据帧** —— ecx_SDOread 走 ecx_mbxreceive
 * (SOEM/src/ec_coe.c:117 -> ec_main.c:1600), 那里只有邮箱轮询 + osal_usleep, 超时是
 * EC_TIMEOUTRXM = 700ms。所以它不是"让圈期变长", 是**把圈停掉**; 驱动器不应答时尤其糟,
 * 而"驱动器不应答"正是它刚报警时的常态。
 *
 * **2026-09-22 实机复现了这条推论**: 界面报 `最长 1638ms 没发出一帧`, 正好是这条 SDO 在
 * 两根轴上各自超时 700ms (加邮箱发送上限)。也就是说驱动器报警之后, 上位机自己又把它
 * 按了一次 —— 这就是「隔一阵子就通讯报警」里"上位机"那一半的成因。为此三条措施:
 *   ① 603Fh 进 TxPDO 之后**根本不发这条 SDO** (publish() 每帧就有码, 见 run()/doConnectInner);
 *   ② 真要发时用短超时 HMI_OP_SDO_TMO_US (60ms), 不是 700ms;
 *   ③ **过程数据帧不健康时一条 SDO 都不发** —— 牌子留着, 帧回来了再说。
 * 曾经以为"调用点套 BlockTick 就行", 那是错的: BlockTick 的 tick 只在 em__cycle 收完
 * 一帧时被调, 而 SDO 事务期间**根本没有帧**, 那个钩子一次都不会响。
 *
 * **一个故障回合只读一次**: 读到就存值, 读不到就存 FAIL, 然后摘牌。故障每 2ms 重挂一次
 * 的话就成了每 2ms 一条 SDO。bit3 掉了之后 publish() 会把 m_fault_code 置回 UNREAD,
 * 于是下一次故障会重新读。
 *
 * **安全性**: 走到这里 bit3 已经是 1, 驱动器自己早就退电了, 没有运动需要维持; 这一条读
 * 与 doConnect/doEnable 里的那些 SDO 读同序 (同一根从站、同一个邮箱通道, 只有本线程用)。
 * 阻塞命令 (em_home 等) 期间跑不到这里 —— 那些帧里 publish() 只挂得上牌子, 读要等它们
 * 回来。实测意义: 找限位途中报故障, 界面上故障横幅立刻有, 故障码晚一次命令的时间。 */
void EcatThread::serviceFaultCodeReads()
{
   for (int i = 0; i < m_naxis; i++)
   {
      if (!m_fault_read_want[i])
         continue;

      if (m_ax[i] == nullptr)
      {
         m_fault_read_want[i] = false;
         continue;
      }

      /* 闸门是**静态**的"603Fh 在不在生效映射里" —— **不能用 em_err_code_known**:
       * 那个还要 mirror_ok, 而链路一坏 mirror_ok 就降 0, 于是这个闸门正好在**最不该
       * 发 SDO** 的一刻打开 (上面 1638ms 那一次就是这么来的)。映射里就有码 -> 每帧
       * 已经拿到, 一个字节都不必发; 牌子照摘, 留着下一圈还得再走到这里判一次 */
      if (em_err_code_offset(m_ax[i]) >= 0)
      {
         m_fault_read_want[i] = false;
         continue;
      }

      /* ③ 帧已经不健康: **一条 SDO 都不发**, 牌子留着 (帧回来了下一圈再走)。
       * 读不到的可能性最高的时候正是静默最伤的时候 —— 驱动器那边多半已经在报通讯
       * 报警, 再静默几十上百毫秒就是把它按实。宁可拿不到码: 界面那句"还没读到"配着
       * 「通讯」灯与横幅, 已经说清楚发生了什么。 */
      if (em_mirror_ok(m_ax[i]) == 0 || m_bad_wkc_run > 0)
         continue;

      /* 牌子摘掉再读: 无论成败都只试一次 (理由见上面"一个回合只读一次") */
      m_fault_read_want[i] = false;

      /* ② 短超时 —— 这条 SDO 阻塞多久 = 过程数据被静默多久。超时是**进 OP 时**统一压短的
       * (见 doConnectInner 里的 em_set_sdo_timeout), 这里把耗时量出来打到控制台:
       * 那个数就是"这一下把总线静默了多久", 下一次实跑的证据 */
      uint16_t v = 0;

      QElapsedTimer t;
      t.start();
      const int rc   = em_rd_u16(m_bus, em_axis_slave(m_ax[i]), EM_OID_ERROR_CODE, 0, &v);
      const qint64 took = t.elapsed();

      consoleNote(QStringLiteral("[603Fh] 轴%1 回退 SDO (映射里没有) 耗时 %2 ms -> %3")
                      .arg(i).arg(took)
                      .arg(rc == 0 ? QStringLiteral("拿到码") : QStringLiteral("失败")));

      if (rc == 0)
      {
         m_fault_code[i] = (int)v;
         note(ecatcmd::fault_code_line(i, (int)v));
      }
      else
      {
         /* 与 0x0000 (无错误) 区分开: 读不到 ≠ 没故障 —— bit3 还立在那儿呢 */
         m_fault_code[i] = HMI_FAULT_CODE_FAIL;
         note(QStringLiteral("%1。6041h bit3 仍置位, 故障码未读到。")
                 .arg(ecatcmd::fault_axis_text(i, HMI_FAULT_CODE_FAIL)));
      }
   }
}

void EcatThread::doStop()
{
   /* 停止 = 把目标冻在当前插值点上。**不写 6040h=0x0000**: 那是卸力, 滑台会自由滑 */
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < EM_MAX_AXES; i++)
         m_want[i] = m_tgt[i];
   }

   note(QStringLiteral("已停止: 目标冻结在当前位置, 保持力矩未撤。"));
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
      note(QStringLiteral("轴%1 仍在运动, 请停稳后再设零。")
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
   /* 零点搬了 —— 世代 +1 (只搬了一根轴也算: 世代说的是"这一套显示坐标还作不作数") */
   m_origin_gen++;
   m_origin_kept  = true;
   m_origin_naxis = m_naxis;

   note(QStringLiteral("轴%1: 当前位置已设为 0 点, 物理目标未改变。").arg(axis));
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
      note(QStringLiteral("量程已从 ±%1 改为 ±%2 pul。")
              .arg(old).arg(range));
      return;
   }

   /* 缩小 —— 逐轴确认装得下。m_tgt 是工作线程独占的, 这里读它不需要锁。
    * 装不下就整条拒绝, 不夹目标: 夹一下等于凭空下发一次运动。 */
   for (int i = 0; i < m_naxis; i++)
   {
      if (m_tgt[i] > range || m_tgt[i] < -range)
      {
         note(QStringLiteral("量程改小被拒绝: 轴%1 当前下发目标 %2 超出新量程 ±%3。"
                             "请先把滑台移回新量程内, 或使用「回中」。")
                 .arg(i).arg(m_tgt[i]).arg(range));
         return;
      }
   }

   m_range.store(range);
   note(QStringLiteral("量程已从 ±%1 改为 ±%2 pul").arg(old).arg(range));
}

void EcatThread::tryInitOrigin()
{
   if (m_naxis < 1)
      return;

   /* 等**全部**轴都收到过完整帧 —— 一次完整帧才有可用的 6064h */
   for (int i = 0; i < m_naxis; i++)
      if (!em_mirror_ok(m_ax[i]))
         return;

   /* 每根轴**只读一次** 6064h: 下面既要拿它判"能不能沿用", 又要拿它算显示坐标, 读两次就是让
    * 这两件事看两个不同的位置。(而且判据那一路算的是 int64 差, 直接 int32 相减会回绕。) */
   int32_t pos[EM_MAX_AXES];
   for (int i = 0; i < m_naxis; i++)
      pos[i] = em_pos(m_ax[i]);

   /* 量程只在**循环外**读一次, 理由同 publish() 那条 di_invert: 同一个决定不许看两个量程。 */
   const int32_t rng = m_range.load();

   /* 沿用上一份零点的全部条件。m_keep_origin 缺省 false —— 那正是 hmi 保持原样的机制:
    * 它不开这个开关, keep 永远是 false, 下面走的和改动前逐字节相同。 */
   bool keep = m_keep_origin && m_origin_kept && m_naxis == m_origin_naxis;
   for (int i = 0; i < m_naxis && keep; i++)
      if (!ecatcmd::origin_keep_ok(pos[i], m_origin[i], rng))
         keep = false;

   if (keep)
   {
      /* 沿用: **m_origin[] 一个字节都不动** —— 这就是本次改动的全部内容。
       * m_tgt / m_want 要重新对齐一次 (零点没变、位置变了), 做法与失能那条尾巴同一套:
       * m_tgt 不持锁、m_want 持锁。这里 `pos - m_origin` 不会溢出 —— origin_keep_ok 刚验过
       * 它落在 ±rng 里。 */
      int32_t d[EM_MAX_AXES];
      for (int i = 0; i < m_naxis; i++)
      {
         d[i]     = pos[i] - m_origin[i];
         m_tgt[i] = d[i];
      }
      {
         QMutexLocker lk(&m_mtx);
         for (int i = 0; i < m_naxis; i++)
            m_want[i] = d[i];
      }

      m_origin_ready = true;
      /* **不加世代**: 零点没搬, 前面那些 CSV 里的坐标仍然作数。 */

      QString disp;
      for (int i = 0; i < m_naxis; i++)
         disp += (i ? QStringLiteral(", ") : QString()) + QString::number(d[i]);
      note(QStringLiteral("沿用上次零点 (本次连接未重设, 界面中心仍对应上次那个物理位置)。"
                          "滑台当前显示在 (%1) pul。").arg(disp));
      return;
   }

   for (int i = 0; i < m_naxis; i++)
   {
      m_origin[i] = pos[i];
      m_tgt[i]    = 0;
   }
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
         m_want[i] = 0;
   }

   m_origin_ready = true;
   m_origin_kept  = true;
   m_origin_naxis = m_naxis;
   m_origin_gen++;          /* 零点真搬了 */

   /* 重取这一支有两个成因, 文案必须分开 —— 它们说的是两件事:
    *   (a) 以前就没定过零点(或这不是 scan): 常规, 说清"界面正中 = 现在这里"就够了;
    *   (b) 有旧零点但不敢沿用(滑台跑出量程 / 轴数变了): 这是关于**机器状态**的一句警告 ——
    *       屏幕上的 0 换了一个物理位置, 而操作员按老习惯会以为它还是刚才那个。
    * hmi 永远走 (a): 它 m_keep_origin 是 false, "连接即零点"本来就是它要的, 不是意外。
    * 这句里**不出现 50000 pul/rev** —— scan 的脉冲当量操作员可改, 写死就是一句迟早会假的话。 */
   if (m_keep_origin && m_origin_kept)
      note(QStringLiteral("重新取零点: %1。界面中心 = 当前位置, 可点范围 ±%2 pul。")
              .arg(m_naxis != m_origin_naxis
                      ? QStringLiteral("轴数已变化, 上一份零点作废。")
                      : QStringLiteral("滑台已超出上次零点所在量程, 该零点不再沿用。"))
              .arg(rng));
   else
      note(QStringLiteral("零点取自连接时的位置: 界面中心 = 该位置, 可点范围 ±%1 pul。").arg(rng));
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
   /* 回零同一套, 而且它更长 (回零能跑满整个回零超时; 缺省 120 s, 可改到 600 s) */
   t.homing        = m_homing;
   t.homing_axis   = m_homing_axis;
   t.homing_method = m_homing_method;
   t.naxis        = m_naxis;
   t.wkc          = wkc;
   t.expected_wkc = (m_bus != nullptr) ? em_expected_wkc(m_bus) : 0;
   t.range        = m_range.load();
   /* 零点世代。从成员拷而不是从电文里攒: teardown() 会把 m_telem 清成默认值, 而这里是
    * 每圈重算的 —— 断开之后这个值仍然新鲜, 界面也就不会漏发下一代。 */
   t.origin_gen   = m_origin_gen;
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
      a.bad_frames = em_bad_frames(ax);
      a.sw        = em_sw(ax);
      a.state     = QString::fromUtf8(em_sw_state_str(a.sw));
      a.enabled   = em_is_enabled(ax) != 0;
      a.fault     = (a.sw & EM_SW_FAULT) != 0;
      /* 603Fh: **两条路挑哪一条只由 err_code_from() 一处决定** ——
       * ① 在生效 TxPDO 里 (默认就是) -> em_err_code() 那一帧的读数, 与 bit3 同帧;
       * ② 不在 -> 搬 m_fault_code[i] (SDO 那条路读到的"上次值"), 且只在 bit3 立着时作数。
       * 这里都不做 SDO 读 (理由见 serviceFaultCodeReads)。
       *
       * err_code_mapped 问的是**静态**的"映射里有没有" (em_err_code_offset), **不是**
       * em_err_code_known —— 后者还要求 mirror_ok, 链路一断就变 false, 于是同一件事
       * 在界面上会从"0x0000 无错误"翻成"读不到", 而下面挂不挂牌子也跟着翻:
       * 那正是 1638ms 静默那次的成因 (牌子在最不该发 SDO 的一刻挂上)。 */
      a.err_code_mapped = em_err_code_offset(ax) >= 0;
      a.fault_code = ecatcmd::err_code_from(a.err_code_mapped, em_err_code(ax),
                                           m_fault_code[i], a.fault);
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

      /* 报警 (含只有码、bit3 没立起来的那一种)。**冻结目标与红横幅都走这一个判据** ——
       * 只看 a.fault 的话, 一个不置 bit3 的报警在界面上仍然不存在 */
      if (ecatcmd::axis_alarm(a.fault, a.fault_code))
         t.fault = true;

      if (a.fault)
      {
         /* 挂"还欠它一次 603Fh 读"的牌子。**只有不在过程数据里时才挂**: 在的话上面已经
          * 每帧读到码了, 再挂就是每圈白跑一趟 serviceFaultCodeReads。**一挂一次**:
          * 摘牌由那次读负责, 牌子挂着的意思永远是"还没读到", 不会退化成每 2ms 一条 SDO。
          *
          * 判据是**静态**的 err_code_mapped (§ 上面那一段): 用 em_err_code_known 的话,
          * 链路一坏这个条件就成真 —— 恰好在最不该发 SDO 的一刻把牌子挂上。 */
         if (!a.err_code_mapped && m_fault_code[i] == HMI_FAULT_CODE_UNREAD)
            m_fault_read_want[i] = true;
      }
      else
      {
         /* 故障没了就把 SDO 那条路的码也放下 (下一次故障重新读)。**留着旧的才是坑**:
          * 一根健康的轴旁边挂着一句 "0xFF02 (过压)" 等于在说它现在过压。
          * 要留痕看控制台 —— note() 那条在那儿, 不会被这里擦掉。 */
         m_fault_code[i]       = HMI_FAULT_CODE_UNREAD;
         m_fault_read_want[i]  = false;
      }
   }

   /* ---- (B) 帧够不够: 计数在工作线程里, 判据在 ecatcmd::comm_bad_from 一处 ----
    * 只在**进了 OP** 时才数: 连接期 publish(0) 与 not-in-op 那一支的 wkc 恒为 0,
    * 拿它当"不足帧"会把每次连接都记成一串坏帧。 */
   if (m_in_op && t.expected_wkc > 0 && wkc < t.expected_wkc)
      m_bad_wkc_run++;
   else
      m_bad_wkc_run = 0;

   t.bad_wkc_run  = m_bad_wkc_run;
   t.comm_bad     = ecatcmd::comm_bad_from(wkc, t.expected_wkc, m_bad_wkc_run,
                                           HMI_BAD_WKC_LIMIT);
   t.max_gap_ms   = m_max_gap_ms;
   t.gaps_over_ms = m_gaps_over;
   t.al_state     = m_al_state;
   t.al_code      = m_al_code;
   t.al_checked   = m_al_checked;

   if (t.comm_bad && !m_comm_bad)
   {
      /* **上升沿这一次**才读 AL 状态: em_al_status 内部是 ecx_readstate (BRD 广播读,
       * 不走邮箱), 一次往返 —— 但它也是额外流量, 健康时不发才是对的。
       * 这里在写锁外, 与 doConnect 里那些 SDO 读同序 (只有本线程用邮箱通道)。 */
      int st = 0, alcode = 0, slave = 1;

      if (m_ax[0] != nullptr)
         slave = em_axis_slave(m_ax[0]);

      m_al_state   = 0;
      m_al_code    = 0;
      m_al_checked = false;

      if (m_bus != nullptr && em_al_status(m_bus, slave, &st, &alcode) == 0)
      {
         m_al_state   = st;
         m_al_code    = alcode;
         m_al_checked = true;
      }

      /* note 是**覆盖写**, 会顶掉先前那条 —— 与 fault_code_line 同一个用法。
       * 措辞走 (B) 家族: 这条讲的是"我这边的帧不够", 不是驱动器自报的 0xFF06。 */
      QString s = QStringLiteral("过程数据帧连续 %1 帧不足 (工作计数器 %2/%3): 位置与状态为陈旧值, 目标已冻结。")
                     .arg(m_bad_wkc_run).arg(wkc).arg(t.expected_wkc);

      if (m_al_checked)
         s += QStringLiteral("  ") + ecatcmd::al_code_text(m_al_state, m_al_code)
              + QStringLiteral("。");

      s += QStringLiteral(" 本程序最长 %1 ms 未发出帧。")
              .arg(m_max_gap_ms);
      note(s);
   }
   m_comm_bad = t.comm_bad;

   if (t.fault && !m_fault_latched)
   {
      m_fault_latched = true;
      {
         QMutexLocker lk(&m_mtx);
         for (int i = 0; i < EM_MAX_AXES; i++)
            m_want[i] = m_tgt[i];
      }
      /* 不写 0x0000: 故障时驱动器自己会退电, 这里只停止下发新目标。
       * 这一句只是"先占住状态栏" —— 码要是走 SDO 那条路, 得下一圈才读得回来, 读到后
       * serviceFaultCodeReads 会用带码的那一句把它顶掉 (note 是覆盖写)。 */
      note(QStringLiteral("驱动器自报故障 (6041h bit3 或 603Fh), 目标已冻结。"
                          "请按故障码处理后「故障复位」。"));
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
   /* **故意不清 m_origin[] / m_origin_kept / m_origin_naxis / m_origin_gen** —— 断开→连接用的是
    * 同一个 EcatThread 对象(每个程序只 new 一次), 所以"零点丢了吗"从来不是对象没了, 而是
    * tryInitOrigin() 每次把它覆盖掉。这份零点留在内存里是免费的, 重连时才有东西可沿用。
    * (m_origin_gen 更必须留着: 下面把 m_telem 清成默认值, 世代计数要是也住在电文里就会跟着归零,
    * 界面那侧"只许往前"的同步就会**漏掉**下一次真正的零点变更 —— 正是危险的那个方向。) */
   m_fault_latched = false;
   m_resetting     = false;   /* 连接断了, "正在复位"这个状态跟着一起没了 */
   m_homing        = false;   /* 同上。真在回零时走到这里, 调用方应当先 requestMotionStop() */
   m_homing_axis   = -1;
   m_homing_method = 0;
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      m_ax[i]     = nullptr;
      m_tgt[i]    = 0;
      /* 603Fh 跟着一起清: 换了台驱动器还挂着上一台读到的码, 是最难查的那种假象。
       * 牌子也要摘 —— 摘不掉的话下一轮的 serviceFaultCodeReads 会拿着空 m_ax 去读 */
      m_fault_code[i]      = HMI_FAULT_CODE_UNREAD;
      m_fault_read_want[i] = false;
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
