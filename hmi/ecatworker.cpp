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

/* 单轴那一趟 —— **内部就是两轴那一套**, 只圈一根。老那三个字段照样填 (别的读者还有), 但
 * `hm_mask` 非空, 所以 drainCommands 走的是两轴那条路, 而那一趟的会话大小恰好是 1。
 * 这正是"改造前后单轴行为一致"能被自检钉住的原因 (§33.7 第 7 条)。 */
void EcatThread::postHome(int axis, int method, uint32_t vel_fast, int tmo_s)
{
   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_HOME; c.axis = axis; c.method = method;
   c.value = (int32_t)vel_fast;
   c.tmo_s = tmo_s;

   if (axis >= 0 && axis < EM_MAX_AXES)
   {
      c.hm_mask          = (1u << axis);
      c.hm_method[axis]  = method;
      c.hm_vel[axis]     = vel_fast;
   }

   m_cmds.enqueue(c);
}

/* 一趟几根同时。「回零校准」走这一条 (mask = 0b11)。 */
void EcatThread::postHomeBoth(unsigned mask, const int *method, const uint32_t *vel_fast,
                              int tmo_s)
{
   if (mask == 0)
      return;

   QMutexLocker lk(&m_mtx);
   Cmd c; c.type = CMD_HOME; c.tmo_s = tmo_s;
   c.hm_mask = mask;

   for (int i = 0; i < EM_MAX_AXES; i++)
      if ((mask & (1u << i)) != 0)
      {
         c.hm_method[i] = (method != nullptr) ? method[i] : 0;
         c.hm_vel[i]    = (vel_fast != nullptr) ? vel_fast[i] : 0;
      }

   /* 老那三个字段也填上第一根 —— 它们是"这一趟的默认值"。两轴时 drainCommands 不读它们,
    * 但调试时从队列里看一条 CMD_HOME 应当能一眼看出它瞄的是谁。 */
   for (int i = 0; i < EM_MAX_AXES; i++)
      if ((mask & (1u << i)) != 0)
      {
         c.axis   = i;
         c.method = c.hm_method[i];
         c.value  = (int32_t)c.hm_vel[i];
         break;
      }

   m_cmds.enqueue(c);
}

/* 只有 scan/ 会调。默认 false 就是本类自己的老行为 (连接那一刻即零点), 所以 hmi 不开这个
 * 开关时行为逐字节不变。理由全在头文件里。 */
void EcatThread::setKeepOrigin(bool on) { m_keep_origin = on; }

/* 「停止」在回零期间走这一个 —— **全程序唯一一处 GUI 线程直呼 motor_api**。
 * 安全: em_request_stop() 只往一个 `static volatile sig_atomic_t` 里存 1, 不碰总线/网卡。
 *
 * 改造前非如此不可: 回零把工作线程整根占住, 一条 CMD_STOP 要等它自己退出来才轮到。
 * 现在回零只占每圈的一步, 队列那条路也通了 (drainCommands 会话期间专门放行 CMD_STOP) ——
 * 但这个入口**保留**, 两个理由: 它是"立即"的 (不等那一圈), 而且 scan/ 那个按钮按的是它。 */
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
   if (m_t == nullptr)
      return;

   /* "下面这一段时间里主循环发不出帧, 是我们自己造成的" —— 给帧间隔归因用。
    * **在构造时置起、不在析构时清**: 命令跑完那一刻正是那个间隔被测到的那一刻,
    * 析构先清掉就什么都归不到自己头上了, 于是我们自己一次使能造成的 1 秒静默会被
    * 读成"这台 PC 在卡", 把人支去查电源计划与网卡节能 (见 recover_verdict)。 */
   m_t->m_gap_self_want = true;

   /* **只有最外层那一次真的挂钩子** (理由见头文件里的 m_tick_depth)。
    * 内层再挂一次没有意义 —— 钩子是"一个用户 + 一个函数", 挂第二遍等于覆盖同一份;
    * 真正有害的是内层析构那一下摘掉外层的。 */
   if (m_t->m_tick_depth++ == 0 && m_t->m_bus != nullptr)
      em_set_cycle_hook(m_t->m_bus, &EcatThread::BlockTick::tick, m_t);
}

EcatThread::BlockTick::~BlockTick()
{
   if (m_t == nullptr)
      return;

   /* 计数归 0 才摘 —— 与构造严格对称。少了 -- 那句, 摘不掉就等于一直挂着
    * (那正是做成 RAII 要防的事)。 */
   if (--m_t->m_tick_depth == 0 && m_t->m_bus != nullptr)
      em_set_cycle_hook(m_t->m_bus, nullptr, nullptr);
}

void EcatThread::BlockTick::tick(void *user, int wkc)
{
   static_cast<EcatThread *>(user)->publish(wkc);
}

void EcatThread::run()
{
   /* ---- 起手: 先把 Windows 的省电节流关掉, 再谈别的 ----
    * 被挂上 EcoQoS 的进程不只被判得慢, **连自己那个定时器精度请求都会被丢掉** ——
    * 本机实测 Sleep(2) 就是这么睡成 15.9 ms 的 (一圈慢 8 倍, 而驱动器每 2 ms 就该收到
    * 一帧)。em_sleep_ms 那边换成高精度定时器是这件事的另一半, 两半都要有:
    * 那一半管"我们等得准", 这一半管"我们跑得动"。
    * 进程级、幂等、老系统上办不到也只是返回 -1, 所以不看返回值 —— 它不是判据。 */
   em_reject_power_throttling();

   QElapsedTimer clk;
   clk.start();
   qint64 last = clk.elapsed();

   while (!m_quit.load())
   {
      drainCommands();

      /* ---- 回零会话的每周期一步 ----
       * **位置三条都不能动** (docs/scan_sweep.md §33):
       *  - 在 drainCommands() 之后: 本圈刚发起的会话这一圈就要抬 bit4;
       *  - 在 em_service() 之前: 它写进镜像的东西必须由**本圈**那一帧带出去;
       *  - 在 interpolate() 之前: 有结局时收尾在**同一圈内**做完, 于是插补器永远看不到
       *    "收尾做到一半"的轴。
       * 也不放进下面那个 if (m_bus && m_in_op) 分支里: 总线掉出 OP 时那一支跑不到, 会话
       * 就会一直挂着 —— 队列再也排不动, 界面上的「停止」也一直停在"立即中止"形态上。
       * 那个分支条件由 serviceHoming() 自己判 (它会就地收尾并报出来)。 */
      serviceHoming();

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

            /* 这一段是不是**我们自己**造成的 (刚跑完一条会静默总线的命令, 见 BlockTick)。
             * m_gap_self_want 在这里消费掉 —— 置它的那次命令与本测量之间不会插别的测量。 */
            const bool self_caused = m_gap_self_want;
            m_gap_self_want = false;

            m_last_gap_ms = (int)gap;

            if (gap > m_max_gap_ms)
               m_max_gap_ms = (int)gap;
            if (gap > HMI_GAP_WARN_MS)
               m_gaps_over++;

            /* 上位机的健康状况: 这一帧发得及不及时。**只有我们自己造成的静默不算**
             * —— 那几百毫秒是我们自己在跑复位/使能/回零, 不是本机卡。算进去的话, 每次
             * 使能都要重新等 2 秒才谈得上自动恢复。 */
            if (gap > HMI_GAP_WARN_MS && !self_caused)
               m_gap_ok_run = 0;
            else if (m_gap_ok_run < 1000000)
               m_gap_ok_run++;

            if (self_caused)
            {
               /* 自成一档: 复位/使能/回零期间 SOEM 的 SDO 事务**一帧都不发**, 那几百毫秒
                * 是我们自己干的, 不是网卡也不是电源管理。 */
               if (gap > m_max_gap_self_ms)
                  m_max_gap_self_ms = (int)gap;
            }
            else if (gap > m_gap_bad_ms)
            {
               /* **自动恢复的触发量只看这一支**: 净的外部停顿。
                * 顺手把上面那条命令造成的也算进去的话, 我们自己一次使能就能把自动恢复
                * 点着 (而那一刻从站好得很), 冷却一到又点一次。 */
               m_gap_bad_ms = (int)gap;
            }
         }

         /* 帧周期 = 这一帧**开始**的时刻 − 上一帧开始的时刻。与上面那个 gap 是两个数:
          * gap 量的是两帧之间的空档, 把 em_service 自己花的时间排除在外; 而驱动器认的是
          * **多久收到一帧**, 那正是这个。攒满一窗算一次平均 —— 单帧抖动不该进这个数,
          * 要看的恰恰是"整条节拍有多快" (见 BusTelem::period_avg_ms)。 */
         if (m_svc_prev_t0 >= 0)
         {
            m_period_sum += svc_t0 - m_svc_prev_t0;
            m_period_n++;

            if (m_period_n >= HMI_PERIOD_WIN_FRAMES)
            {
               m_period_avg = (int)(m_period_sum / m_period_n);
               m_period_sum = 0;
               m_period_n   = 0;
            }
         }

         m_svc_prev_ms = svc_t1;
         m_svc_prev_t0 = svc_t0;

         if (!m_origin_ready)
            tryInitOrigin();

         publish(wkc);

         /* ---- 自动重请求 OP ----
          * **调用点只能在这里, 不许搬进 publish()**: publish() 就是 BlockTick::tick 的
          * 回调体, 在它里面触发会经 em_recover_op -> em__cycle -> tick -> publish 当场递归,
          * 而 BlockTick 的注释已经写明这条。摆在这一圈的最后, 遥测与帧间隔都已经结完账。 */
         serviceAutoRecover(svc_t1);
      }
      else
      {
         last = clk.elapsed();
         m_svc_prev_ms = -1;   /* 没在发帧, 别把"连接前的空档"算成一个帧间隔 */
         m_svc_prev_t0 = -1;
         /* 没在发帧就谈不上节拍。平均值也要清: 留着上一个会话的数, 重连之后屏幕上会先
          * 摆出一个陈旧的数字, 而那个数字恰恰是"本机跟不跟得上"的唯一读数 —— 陈旧的那
          * 一个与"没量到"说的是两件事, 宁可什么都不说。 */
         m_period_sum = 0;
         m_period_n   = 0;
         m_period_avg = 0;
         m_gap_self_want = false;
         m_gap_ok_run    = 0;  /* 没在发帧就谈不上"按时发帧" */
         m_last_gap_ms   = 0;

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
      Cmd  c;
      bool took_stop = false;

      {
         QMutexLocker lk(&m_mtx);

         /* ---- 回零会话活着: 队列**一条都不出队**, 只把「停止」挑出来 ----
          * 这一段是 §33.5(a) 那个洞的补丁, 不是洁癖。下面那句 em_clear_stop() 每条命令都
          * 清一次 g_stop, 而「停止」在回零期间**唯一的通路就是 g_stop** (GUI 直呼
          * requestMotionStop(), 不经队列)。改造前安全, 是因为回零把工作线程整根占住 ——
          * **队列在回零期间根本不会被排空**。改造后队列变活了: 任何一条躺在里面的命令被
          * 出队, 都会把用户刚按下的「停止」悄悄抹掉。按住没反应、两根继续朝开关走, 而屏幕
          * 上那句「按「停止」可立即中止」还挂着 —— 这是这次改造最不能出的错。
          *
          * 也不能"把别的命令排到回零后面"了事: postRange / postZeroHere / postDisconnect
          * 原本都排在回零后面, 改造后它们会插进两根轴都带电的中间 —— 其中 postDisconnect
          * 会在那一刻去收总线。
          *
          * CMD_STOP 是唯一例外: 它必须立刻生效, 所以就地取出来, 而且**不走**下面那句
          * em_clear_stop() —— 它自己就是那个请求, 清掉等于把它自己抹了。 */
         if (m_homing)
         {
            for (int k = 0; k < m_cmds.size(); k++)
               if (m_cmds.at(k).type == CMD_STOP)
               {
                  m_cmds.removeAt(k);
                  took_stop = true;
                  break;
               }

            if (!took_stop)
               return;
         }
         else
         {
            if (m_cmds.isEmpty())
               return;
            c = m_cmds.dequeue();
         }
      }

      if (took_stop)
      {
         em_request_stop();
         return;
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
            /* **只有起手这一段还阻塞** (三道闸 + 失能 + 5 笔 SDO + 启动), 最长几秒。
             * 轮询那一大段现在跑在 run() 主循环的 serviceHoming() 里 —— 那些帧由主循环自己
             * publish, 所以不必也不该套 BlockTick (套了会在会话期间每 2ms 白拷两遍 BusTelem)。
             * 起手这一段仍旧要套: 那几笔 SDO 期间一帧都不发, 使能灯与位置不跟着动就说不过去。
             * 起手就失败时收尾也在这一趟里做完 (startHoming 里那条路), 同样落在钩子内。 */
            if (c.hm_mask != 0)
            {
               /* 两轴那条路 (单轴也从这里过 —— postHome 填的就是 `1 << axis`)。
                * `hm_method` / `hm_vel` 是数组, 所以这里取的是**命令自己那份拷贝**的地址:
                * startHoming 起手这一段是同步的 (它返回时轮询还没开始), 整个调用期内 `c`
                * 都还在这个栈帧上, 指针一直有效。 */
               BlockTick tk(this);
               startHoming(c.hm_mask, c.hm_method, c.hm_vel, c.tmo_s);
            }
            else
            {
               /* `hm_mask == 0` 只可能来自一个**没填新字段的老调用方** (改造期间留下的一条
                * 兜底, 现在没有这种调用方)。照老三个字段凑一趟单轴的, 而不是静默地什么都不做 ——
                * "点了回零却没动"是这里最不该出的错。 */
               const int axis = c.axis;
               if (axis >= 0 && axis < EM_MAX_AXES)
               {
                  int      m[EM_MAX_AXES] = {};
                  uint32_t v[EM_MAX_AXES] = {};
                  m[axis] = c.method;
                  v[axis] = (uint32_t)c.value;

                  BlockTick tk(this);
                  startHoming(1u << axis, m, v, c.tmo_s);
               }
            }
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
   m_svc_prev_t0 = -1;
   m_period_sum  = 0;
   m_period_n    = 0;
   m_period_avg  = 0;
   m_max_gap_ms  = 0;
   m_gaps_over   = 0;
   m_gap_self_want   = false;
   m_max_gap_self_ms = 0;
   m_gap_bad_ms      = 0;
   m_last_gap_ms     = 0;
   m_gap_ok_run      = 0;
   m_recover_warn_ms = -1;
   m_recover_last_ms = -1;   /* 新连接 = 自动恢复从零开始 (冷却与次数都重来) */
   m_recover_tries   = 0;
   m_recover_said.clear();
   m_bad_wkc_run = 0;
   m_good_wkc_run = 0;
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

         /* 最后那个占位符是 %7, **不是 %s**。写成 %s 时 QString::arg 会抛
          * "Argument missing" 并把整句 NPN 极性提示丢掉, 屏幕上留着字面的 "%s" ——
          * 而那句提示正是"极性配反了"唯一的线索 (现场日志里就有这一条)。 */
         consoleNote(QStringLiteral("%1: 2300h = 0x%2 (%3 字节)  X0~X2 = %4/%5/%6%7")
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
   /* ---- 回零会话活着时, 这一族命令一律不动 ----
    * 纵深防御的第二层 (第一层是 drainCommands: 会话期间除 CMD_STOP 外一条都不出队)。
    * **必须有这一层**: 那一层靠的是 m_homing 这个"工作线程自己写的普通 bool", 一旦哪次
    * 改造把它的时序动了 (比如把会话宣告挪到出队之后), 这些命令就会在一根轴正带电找原点的
    * 时候动手 —— doRange 会夹目标、doZero 会搬零点, 两个都是"凭空一次没人按过的运动"。
    * 留这一句比事后查那一次事故便宜得多。下面 doZero / doCenter / doRange 各有一句。 */
   if (m_homing)
      return;

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

   /* ---- 前置闸: 帧不完整就**不要**落到下面那句"请确认 6041h 故障位与限位状态" ----
    * 那一句的前提是"我看得见驱动器状态", 而帧不足时 6041h / 6064h 都是陈值 ——
    * 现场那一次就是被这一句支去看故障位与限位, 真因却是过程数据从来没回来。
    * 只报轴名, 不复述任何 6041h 读数 (它不可信)。 */
   {
      QStringList stale;

      for (int i = 0; i < m_naxis; i++)
      {
         if (em_mirror_ok(m_ax[i]) == 0)
            stale << QString::fromUtf8(ecatcmd::axis_label(i));
      }

      if (!stale.isEmpty())
      {
         note(ecatcmd::enable_stale_text(stale.join(QStringLiteral("、"))));
         return;
      }
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
   /* 帧不完整、判不了的那几根。与 todo 分开存: 两件事的处置完全不同 ——
    * 一个是"写驱动器", 一个是"先把通讯修好"。 */
   QStringList unknown;

   for (int i = 0; i < m_naxis; i++)
   {
      /* 用**刚读到的**状态字, 不用遥测快照 —— 而这一条判断决定要不要卸力 */
      const bool mok = em_mirror_ok(m_ax[i]) != 0;
      const bool flt = (em_sw(m_ax[i]) & EM_SW_FAULT) != 0;

      switch (ecatcmd::reset_gate(true, mok, flt))
      {
         case ecatcmd::RESET_DO:
            todo[ntodo++] = i;
            break;
         case ecatcmd::RESET_UNKNOWN:
            unknown << QString::fromUtf8(ecatcmd::axis_label(i));
            break;
         case ecatcmd::RESET_NOFAULT:
            break;
      }
   }

   if (ntodo == 0)
   {
      /* **"判不了"必须先说, 而且不许说成"没有"** —— 现场那一次把陈旧的 6041h 说成了
       * 「bit3 均为 0」, 与同一块屏幕上刚说过的"陈旧值"直接打脸, 还把人支去查驱动器。
       * 两支都**真的一个字节都没写** (RESET_UNKNOWN 是 axis_needs_reset 的返回 false
       * 那一路, 判据一个字节都没放宽)。 */
      if (!unknown.isEmpty())
         note(ecatcmd::reset_unknown_text(unknown.join(QStringLiteral("、"))));
      else
         note(ecatcmd::reset_nofault_text());
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
      s += QStringLiteral("%1 复位失败。请按故障码查明原因 (6041h 实测值见控制台)。")
              .arg(bad.join(QStringLiteral("; ")));
   }

   /* 同一趟里有帧不完整的轴**: 上面那句只说动了手的那些, 一句都不许盖住这几根 ——
    * 它们的 6041h 是陈值, 所以既没被复位也没被检查, 而"漏了谁"正是最该看见的。 */
   if (!unknown.isEmpty())
   {
      if (!s.isEmpty())
         s += QStringLiteral("   ");
      s += QStringLiteral("%1 本次没有检查也没有复位 (过程数据帧不完整, 6041h 是陈旧值)。")
              .arg(unknown.join(QStringLiteral("、")));
   }

   /* m_fault_latched **不在这里碰**: 它只有一个写者 (publish()), 清除条件就是 "bit3 掉了"。
    * m_tgt / m_want 也不碰 —— 复位后该轴是失能态, interpolate() 跳过失能轴。 */
   note(s);
}

/* 回零起手 —— 驱动器自带的 HM 模式 (6060h = 6)。四个方式: 24/29 = 正/反向找**原点开关**,
 * 18/17 = 找**正/负限位开关** (手册 V2.4 p46~p48, 每个各带 a)/b) 两条分支)。
 *
 * 三段顺序不能动: **闸 (一个字节都不写) -> 宣告 -> 起手两段**, 因为失败路径留下的状态没有
 * 一条可以不管 —— 起手失败时**就地收尾** (本函数最后那一段), 而不是简单 return。
 * 17/18 多一道闸 (两道否决) 与一句分支预告, 位置在两道现有闸之后、宣告之前。
 *
 * **本函数不再等到回零结束**: 它返回时轮询还没开始。整个会话由三处拼成 ——
 * 这里起手、`serviceHoming()` 每圈走一步、`finishHoming()` 收尾, 三者由 m_homing 串起来。
 * 这样一根轴的等待不再占住工作线程, 两轴才可能同时动 (docs/scan_sweep.md §33)。 */
void EcatThread::startHoming(unsigned mask, const int *method, const uint32_t *vel_fast,
                             int tmo_s)
{
   /* 掩码只许圈连上的轴。`postHome` / `postHomeBoth` 填的都是"要哪几根", 而"连上了几根"
    * 是这里的事实 —— 圈到一根不存在的轴, 下面每一步都要为它多写一个特例。 */
   mask &= (m_naxis >= EM_MAX_AXES) ? 0xFFFFFFFFu : ((1u << m_naxis) - 1u);

   if (mask == 0)
      return;                    /* 编程错误, 不是操作员的事 */

   const bool bus_ready = (m_bus != nullptr) && m_in_op;

   /* 「还有轴在走」用工作线程**自己的真相** (m_want 对 m_tgt), 不用遥测 (有滞后);
    * 回零期间 interpolate() 不跑, 另一根轴的目标会停在半途。 */
   bool any_moving = false;
   {
      QMutexLocker lk(&m_mtx);
      for (int i = 0; i < m_naxis; i++)
         if (m_ax[i] != nullptr && m_want[i] != m_tgt[i])
            any_moving = true;
   }

   /* 只放行四个 (24/29 找原点, 18/17 找限位)。em_home() 自己只查 [1,35], 别的方式的方向
    * 语义没验过, 放进来是拿滑台去试 —— 而回零是**软件兜不住**的动作。
    * **逐根先查**: 一根的方式号不合法就必须整体不动, 不能让它走到闸那里才发现。 */
   for (int i = 0; i < EM_MAX_AXES; i++)
      if ((mask & (1u << i)) != 0 && !ecatcmd::home_method_allowed(method[i]))
      {
         note(QStringLiteral("%1 回零方式 %2 不在允许范围内 (仅 24/29 找原点, 18/17 找限位), "
                             "未写入驱动器。")
                 .arg(QString::fromUtf8(ecatcmd::axis_label(i)))
                 .arg(method[i]));
         return;
      }

   /* 用**刚读到的** 6041h: 本函数第一件事 em_disable() 真的会撤掉保持力矩。
    * 逐轴两份 —— 起手闸要**每一根都问一遍**, 而不能只看第一根 (§33.7 第 1 条那条). */
   bool mirror_ok[EM_MAX_AXES] = {};
   bool fault[EM_MAX_AXES]     = {};

   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      if ((mask & (1u << i)) == 0)
         continue;

      const em_axis_t *ax = bus_ready ? m_ax[i] : nullptr;
      mirror_ok[i] = (ax != nullptr) && em_mirror_ok(ax) != 0;
      fault[i]     = (ax != nullptr) && (em_sw(ax) & EM_SW_FAULT) != 0;
   }

   /* ---- 起手闸: **一根不合格就整体不动** (用户选的语义) ----
    * 文案与单轴那条路是同一份 (home_batch_refusal 内部就调 home_refusal), 只是这里要
    * **把不合格的那几根都点名** —— 只说第一根, 操作员修完再撞一次才轮到第二根。 */
   const ecatcmd::HomeGate gate =
      ecatcmd::home_batch_refusal(bus_ready, m_origin_ready, any_moving, mask, mirror_ok, fault);

   if (gate.reason != nullptr)
   {
      /* 点名用 `bad_mask`, 而总线级那几道**不指向任何一根** (bad_mask = 0) —— 那时退回
       * 用 `mask`: 操作员按的是"这几根", 屏幕上就得有这几根的名字。单轴时两条路都以
       * 同一个名字打头, 于是那句话与改造前逐字相同。 */
      note(QStringLiteral("%1 回零未发起, 驱动器未写入。%2")
              .arg(ecatcmd::home_axis_prefix(gate.bad_mask != 0 ? gate.bad_mask : mask),
                   QString::fromUtf8(gate.reason)));
      return;
   }

   const unsigned want = gate.ok_mask;

   /* ---- 找限位 (17/18) 的第二道闸 + 分支预告 ----
    * 位置在这里是量出来的: 早了没状态 (上面那道闸刚放行), 晚了已经卸力 (下面就是
    * em_disable)。判据用**驱动器自己**那两位 (em_di_poslim / em_di_neglim, 即 2300h +
    * 2310h 之后的结果), 不用遥测里反相后的 dig_pos/dig_neg —— 驱动器按它自己的读数
    * 决定怎么走, 上位机反相只改显示。
    *
    * 预告必须打: 手册 a) 与 b) 两条分支的**首段方向是相反的**, 不说一句, 操作员会以为
    * 自己点错了按钮, 而那时电机已经在动。
    *
    * ⚠️ 它必须**逐根都过一遍**再放行: 两根里只要有一根走不了, 这一趟整体不许发起 ——
    * 否则另一根已经在找原点时这一根才报"不行", 而那时它已经动了。 */
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      if ((want & (1u << i)) == 0 || !ecatcmd::home_method_is_limit(method[i]))
         continue;

      em_axis_t *ax = m_ax[i];

      const bool dig_known = em_dig_in_known(ax) != 0;
      const bool pos_lim   = em_di_poslim(ax) != 0;
      const bool neg_lim   = em_di_neglim(ax) != 0;
      const bool tgt       = ecatcmd::home_lim_target_active(method[i], pos_lim, neg_lim);
      const bool other     = ecatcmd::home_lim_other_active(method[i], pos_lim, neg_lim);

      const char *no = ecatcmd::home_lim_refusal(dig_known, tgt, other);
      if (no != nullptr)
      {
         note(QStringLiteral("%1 %2 未发起, 驱动器未写入。%3")
                 .arg(QString::fromUtf8(ecatcmd::axis_label(i)),
                      QString::fromUtf8(ecatcmd::home_method_short(method[i])),
                      QString::fromUtf8(no)));
         return;
      }

      /* **`.arg()` 是必须的** —— home_lim_branch_text 的文案里带一个 `%1` 占位符, 而改造前
       * 这里漏了它, 控制台上打出来的是字面的"轴%1 找正限位 (方式 18)…"。 */
      note(QString::fromUtf8(ecatcmd::home_lim_branch_text(method[i], tgt))
              .arg(QString::fromUtf8(ecatcmd::axis_label(i))));
   }

   /* 回零超时的第二道夹取 (第一道是界面那个 spin box 的 setRange), 顺手把秒换算成毫秒。
    * **全程序唯一一次 `* 1000`** —— 单位在 ini/界面/Cmd 里一律是秒, 换算点只有这一处。
    * 超时是**整个会话共用的**一个值 (界面上只有一个框), 所以它算一次、每根都用它。 */
   const int      tmo_s2 = ecatcmd::home_tmo_s_clamp(tmo_s);
   const uint32_t tmo_ms = (uint32_t)tmo_s2 * 1000u;

   /* ---- 把逐轴的值都填好, **最后**才置 mask ----
    * mask 一置, publish() 就会拿着它去读 m_home_method[] / m_home_done[], 而下面那句
    * em_disable() 的帧里就会跑一次 publish() (BlockTick 那个回调)。反过来的话, 那一帧会
    * 报出一个方式号 0。 */
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      if ((want & (1u << i)) == 0)
         continue;

      em_home_cfg_t cfg;
      em_home_cfg_default(&cfg);
      cfg.method   = method[i];
      cfg.vel_fast = ecatcmd::home_vel_clamp((int32_t)vel_fast[i]);
      cfg.vel_slow = ecatcmd::home_vel_slow(cfg.vel_fast);
      /* **acc 必须跟着速度一起算** (见 home_accel_for); offset 保持 0, 界面上没有它的控件 */
      cfg.acc      = ecatcmd::home_accel_for(cfg.vel_fast);

      /* 存下起手时算好的参数: 结论句在 finishHoming() 里写, 那里离这里有好几秒 */
      m_home_cfg[i]        = cfg;
      m_home_tmo_s[i]      = tmo_s2;
      m_home_method[i]     = method[i];
      m_home_rc[i]         = EM_HM_RUNNING;
      m_home_done[i]       = false;
      m_home_disable_rc[i] = 0;
   }

   /* ---- 宣告"正在回零"。**必须在第一个阻塞调用之前** ----
    * 那次加锁直写让界面**从第一个字节之前**就看得见 (界面靠它把「停止」换成立即中止);
    * 之后那几笔 SDO 期间之所以一直在刷新, 靠的是 CMD_HOME 上那个 BlockTick (见头文件)。
    * 两件事都要: 直写不依赖回调挂没挂, 而回调那一份要等下一帧才出去。 */
   m_homing      = true;
   /* **赋值, 不是 |=**: 一次会话就是"这一趟要的那几根", 而 m_homing 为真时队列一条都不出队
    * (drainCommands), 所以这里不会与上一次会话叠加。 */
   m_homing_mask = want;
   {
      QMutexLocker lk(&m_mtx);

      m_telem.homing = true;

      for (int i = 0; i < EM_MAX_AXES; i++)
         if ((want & (1u << i)) != 0)
         {
            m_telem.ax[i].homing        = true;
            m_telem.ax[i].homing_method = method[i];
         }
   }

   /* ★ 先失能,**逐根**。6098h/6099h/609Ah/607Ch **只在未使能时可写** —— 这一刻该轴失去
    * 保持力矩, 竖直轴可能下滑, 这件事躲不掉。返回码要留着: 结论里那个「失能 %1」是它与
    * 收尾那一次的**第一个非零值**, 与改造前那两句的合成规则一样。 */
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      if ((want & (1u << i)) == 0)
         continue;

      if (em_is_enabled(m_ax[i]))
         m_home_disable_rc[i] = em_disable(m_ax[i]);
   }

   /* ---- 起手两段: 写参数 (纯 SDO, 会静默总线) -> 启动 (有界阻塞, 自泵帧) ----
    * **两轮的次序是硬约束: 所有 prepare 做完, 才轮到任何 start** (§33.3)。反过来的话,
    * X 已经在找原点 (bit4 举着、等帧喂它), 主站却在给 Y 写 SDO —— X 那几秒一帧都收不到,
    * 而那几秒正好够两台驱动器的 SM 看门狗动作。
    *
    * **在这里就结束本函数**: 轮询归 serviceHoming(), 收尾归 finishHoming()。 */
   int fail_at    = -1;
   int fail_phase = 0;      /* 0 = prepare 那一轮, 1 = start 那一轮 */
   int rc         = 0;

   for (int i = 0; i < EM_MAX_AXES && rc == 0; i++)
      if ((want & (1u << i)) != 0 && (rc = em_home_prepare(m_ax[i], &m_home_cfg[i])) != 0)
      {
         fail_at    = i;
         fail_phase = 0;
      }

   for (int i = 0; i < EM_MAX_AXES && rc == 0; i++)
      if ((want & (1u << i)) != 0 && (rc = em_home_start(m_ax[i], tmo_ms)) != 0)
      {
         fail_at    = i;
         fail_phase = 1;
      }

   if (rc == 0)
      return;               /* 起手成功 —— 从这里开始, 每 2 ms 由 serviceHoming() 走一步 */

   /* ---- 起手就失败: **就地收尾, 一条不留** ----
    * 这一条与改造前"em_home() 返回非 0 之后仍旧走那整套收尾"逐行对应, 而它绝不是多余的:
    * em_home_start 可能已经把轴使能了 (em_enable 成功、em__cw_step 超时), 甚至可能已经让
    * 它动起来了 —— 那种轴必须有人把它交回确定状态。
    *
    * 定性**逐根分开**, 不能一律 SETUP:
    *   · 已经轮到过的那几根 —— prepare 过了; 若失败发生在 start 那一轮, 它们**已经启动**,
    *     正在找原点, 那是"被对侧拉停"(PEER), 不是"没跑起来"(SETUP);
    *   · 失败的那一根 —— 0/1/负 三个返回码按老规矩映射;
    *   · 还没轮到的 —— 一个字节都没写, 但它已经被先失能了, 照样要收尾。 */
   if (rc == 1)
   {
      /* ★ **「停止」是全局的, 所以这一支要单独走**: 它一到, 这一趟里就没有谁"错了"。
       * 每一根都记成"被停止" —— 包括已经启动起来的那几根: 拉停它们的是操作员, 不是对侧。
       *
       * 记成 `EM_HM_PEER` 会**点错人**: `home_batch_blame()` 的兜底前提是"非 PEER 的那一根
       * 出了事", 而这里非 PEER 的那一根正是被停止请求拦住的那一根 —— 它没出事。于是批结论
       * 会打出"请查 轴Y", 把操作员按的那个停止说成一根轴的故障。
       * 全部记 ABORTED 之后 `home_batch_end()` 给的正是 `HOME_BATCH_STOPPED` —— 自成一档,
       * 不叫失败 (与"两根都被按停"那一档合流, 那本来就是同一件事)。 */
      for (int i = 0; i < EM_MAX_AXES; i++)
         if ((want & (1u << i)) != 0)
         {
            m_home_rc[i]   = EM_HM_ABORTED;
            m_home_done[i] = true;
         }
   }
   else
   {
      for (int i = 0; i < EM_MAX_AXES; i++)
      {
         if ((want & (1u << i)) == 0)
            continue;

         /* 轮到过的那几根: start 那一轮失败时它们**已经启动**, 正举着 bit4 找原点 ——
          * 那是"被对侧拉停" (PEER)。prepare 那一轮失败时它们只是写好了参数, 一个字都没动。 */
         if (i < fail_at)
            m_home_rc[i] = (fail_phase == 1) ? EM_HM_PEER : EM_HM_SETUP;
         /* 失败的那一根与还没轮到的那些根都是 SETUP —— 前者"起手段就没跑起来", 后者
          * 一个字节都没写。两种说法在屏幕上同一句, 而它们要人做的事也是同一件: 看控制台
          * 弄清是哪一步 (prepare 的 -1 与 start 的 -1 在控制台上是两行不同的日志)。 */
         else
            m_home_rc[i] = EM_HM_SETUP;

         m_home_done[i] = true;
      }
   }

   finishHoming();
}

/* 回零会话的每周期一步。**只能从 run() 主循环调** (位置的三条理由写在 run() 里)。
 *
 * 三件事, 顺序不能动:
 *   1. 逐轴走一步 —— 每一步只写输出镜像 (em_home_step 的契约), 并且**当场记下结局**;
 *   2. 有一根出事 -> 其余的**在同一圈**被拉停 (用户选的"两根一起停");
 *   3. 全部定局 -> 在**同一圈内**做完收尾 —— 于是插补器与别的命令永远看不到"收尾做到
 *      一半"的轴。 */
void EcatThread::serviceHoming()
{
   if (!m_homing)
      return;

   /* ---- 0. 总线没了就先收尾 ----
    * 掉出 OP 或已经断开时, 会话不能就这么挂着: m_homing 一直真的话, 队列再也排不动
    * (drainCommands), 界面上的「停止」也一直停在"立即中止"形态上, 而轴上什么都没有了。
    * 收尾那几级会自己失败并报出来, 这正是要的: 让操作员看见"这一趟没走到头"。 */
   if (m_bus == nullptr || !m_in_op)
   {
      finishHoming();
      return;
   }

   /* ---- 1. 逐轴走一步 ---- */
   bool any_failed = false;
   int  blame      = -1;   /* 第一根**自己**出错的轴 (被拉停的不算, 所以不能事后反推) */

   for (int i = 0; i < m_naxis; i++)
   {
      if ((m_homing_mask & (1u << i)) == 0 || m_home_done[i])
         continue;

      if (m_ax[i] == nullptr)
      {
         m_home_rc[i]   = EM_HM_SETUP;
         m_home_done[i] = true;
         any_failed     = true;
         if (blame < 0)
            blame = i;
         continue;
      }

      const int rc = em_home_step(m_ax[i]);

      if (rc != EM_HM_RUNNING)
      {
         m_home_rc[i]   = rc;
         m_home_done[i] = true;

         /* 到位与"被停止"都不是故障: 它们不该把对侧那一根拉停。
          * 停下那一条尤其要紧 —— 停止请求是**全局**的, 同一圈里每一根都会看到它,
          * 各自写回 0x000F 就是了, 再互相 abort 反而会覆盖掉各自的结局。 */
         if (rc != EM_HM_ATTAINED && rc != EM_HM_ABORTED)
         {
            any_failed = true;
            if (blame < 0)
               blame = i;
         }
      }
   }

   /* ---- 2. 一根出事 -> 其余的同一圈拉停 ----
    * **同一圈**: 对侧可能正在朝开关走, 拖到下一圈就多走一个 2ms 的步长, 而这只是最小
    * 的代价 —— 真正的理由是"两根一起停"是用户在权衡过之后选的语义: 一根出事之后另一根
    * 接着找一个已经不可信的零点, 没有意义。 */
   if (any_failed)
      for (int i = 0; i < m_naxis; i++)
      {
         if ((m_homing_mask & (1u << i)) == 0 || m_home_done[i] || m_ax[i] == nullptr)
            continue;

         em_home_abort(m_ax[i]);
         m_home_rc[i]   = EM_HM_PEER;
         m_home_done[i] = true;
         /* 不许写"请查上一句": note() 是**覆盖写状态栏**, 而出错那根自己的结论句在这一句
          * *之后*才出去 (结论统一在下面第 3 步逐轴说), 按时间顺序它反而是下一句。
          *
          * **这一句留着的理由是它说了别处说不出的一件事 —— 因果关系**: 后面那些结论句只会
          * 说这一根"被另一根轴带停" (home_axis_note 里那一档), 不说元凶是谁; 批结论
          * (home_batch_summary) 会点名元凶, 但不把两根连起来。只有这里同时说出"谁把谁拉停的"。 */
         note(QStringLiteral("%1 回零被拉停: %2 出错。")
                 .arg(QString::fromUtf8(ecatcmd::axis_label(i)),
                      QString::fromUtf8(ecatcmd::axis_label(blame))));
      }

   /* ---- 3. 全部定局 -> 收尾 ---- */
   for (int i = 0; i < m_naxis; i++)
      if ((m_homing_mask & (1u << i)) != 0 && !m_home_done[i])
         return;   /* 还有在跑的 */

   finishHoming();
}

/* 收尾 + 结论。**无条件、顺序不能动** (与改造前逐行对应):
 * 逐轴 [收尾阶梯 -> 重新锚定零点 -> 读 6061h -> 判定并一次说完], 最后一句批结论, 关会话。
 *
 * 为什么**逐轴**做完一根再做下一根: 每条轴在失能窗口里失去保持力矩, 逐步做会让两根的
 * 窗口同时变长; 逐轴还保住了"是哪一根出的问题"这个信息 (docs/scan_sweep.md §33.6)。
 * 代价是总时长变长, 认下来。 */
void EcatThread::finishHoming()
{
   /* ---- 0. 再清一次停止标志 ---- 补 drainCommands() 留下的洞: 那个停止请求瞄的是
    * **运动**, 而收尾的全部职责是抵达一个确定状态。 */
   em_clear_stop();

   /* 先把要收尾的那几根**拍一张快照**: 下面每收完一根就把它的 mask 位清掉 (界面据此把
    * 那一根的"回零中"落下), 而边遍历边改 mask 会漏掉后面那几根。 */
   const unsigned mask = m_homing_mask;

   /* ---- 结论攒在这里, 最后一句说 ---- ★ **每一根都不许被后一根覆盖掉** ----
    * note() 是**覆盖写状态栏**, 所以逐轴 note 之后状态栏上只剩最后一根; 而"是哪一根出的问题"
    * 正是这次改造最要紧的那条信息, 恰好可能被落在前面。两件事一起解决:
    *   · 逐轴的整句照旧逐根 note (控制台与通知里一句不少, 与改造前的单轴逐字相同);
    *   · 收尾之后再补一句**批结论**, 把每一根的结果都念一遍并点出元凶 —— 它最后写, 所以
    *     状态栏上留下的是它 (同 doFaultReset() 那个"点名所有轴"的写法)。
    * `n == 1` 时**不发这一句**: 那一趟必须与改造前逐字节一致 (§33.7 第 7 条)。 */
   ecatcmd::HomeReport rep[EM_MAX_AXES];
   ecatcmd::HomeEnd   ends[EM_MAX_AXES] = {};
   int                step_rc[EM_MAX_AXES] = {};   /* em_home_step 的原始结局, 批结论要用 */
   unsigned           rep_mask         = 0;
   int                nrep             = 0;

   for (int axis = 0; axis < EM_MAX_AXES; axis++)
   {
      if ((mask & (1u << axis)) == 0)
         continue;

      em_axis_t *ax = m_ax[axis];

      /* 轴已经不在了 (掉出 OP / 断开): 收尾阶梯没法走, 但结论**必须报** ——
       * 这一趟没走到头这件事不能因为"总线先没了"就消失。 */
      if (ax == nullptr)
      {
         note(QStringLiteral("%1 回零未完成: 总线已不在。")
                 .arg(QString::fromUtf8(ecatcmd::axis_label(axis))));
         m_home_done[axis] = true;
         m_homing_mask &= ~(1u << axis);
         continue;
      }

      /* 0 = 到位 / 1 = 被「停止」中止 / 负 = 失败。**映射只有一处**
       * (ecatcmd::home_step_rc_legacy, 改造前它就是 em_home() 的返回码本身) */
      const int rc_home  = ecatcmd::home_step_rc_legacy(m_home_rc[axis]);
      const int attained = (m_home_rc[axis] == EM_HM_ATTAINED);

      /* ---- 1..4b. 收尾阶梯 + 重锚 + 读模式 ---- 这一段会静默总线, 所以整段挂 BlockTick。
       *
       * ★ **挂 tick 不是装饰**: 改造前这几步天然在 doHome() 的那个 tick 底下, 拆开之后
       * 没人替它们挂。失能 / 切模式 / 重新使能**每一级都是 SDO**, 而 SDO 事务期间过程数据
       * **一帧都不发** —— 不挂的话界面在这儿停住 1~2 s, 更糟的是那段时间会被 frame-gap
       * 读成"这台 PC 在卡, 去查电源计划与网卡节能" (2026-09-23 抓到的正是这个形态:
       * m_gap_self_want 没人置)。它同时是验收第 8 条那个 max_gap_self_ms 的来源。
       *
       * ★ 1..3 不冗余: bit3 / bit13 / 超时 / 中止那四条路上驱动器**还在找**, 让它停下来的
       * 正是这里。阶梯本身住在 motor_api (em_home_finish) —— 两轴并行时"逐轴做完"需要一个
       * 单元, 而 em_axis_t* 是唯一自然的那一个。
       *
       * 起手就失败那条路 (startHoming 里调进来) 本来就在 CMD_HOME 那个钩子底下, 所以那
       * 一趟是**嵌套**的 —— 嵌套安全靠 BlockTick 的 m_tick_depth (见 ecatworker.h)。 */
      em_home_end_rc_t end_rc;
      int              rc_disable = 0, rc_mode = 0, rc_enable = 0;

      {
         BlockTick tk(this);

         (void)em_home_finish(ax, attained, &end_rc);

         /* 起手那次失能与收尾这一次, 取第一个非零 —— 与改造前那两句的合成规则一样 */
         rc_disable = (m_home_disable_rc[axis] != 0) ? m_home_disable_rc[axis]
                                                     : end_rc.rc_disable;
         rc_mode    = end_rc.rc_mode;
         rc_enable  = end_rc.rc_enable;

         /* ---- 4. 重新锚定显示原点 ---- ★ **这一行漏掉, 就是一次没人按过按钮的全速运动**:
          * m_origin 若还是回零之前的值, 下一次 interpolate() 会把**回零之前的物理位置**当成
          * CSP 目标发出去。**必须在收尾阶梯之后** —— em_arm 钉 607Ah 用"使能那一刻的 6064h"。
          * (em_pos 在 mirror 不健康时会退化成一条 SDO 读, 所以它在 tick 里面。) */
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
      }
      /* tick 到此为止 (它只罩 SDO); 下面判定与结论句不发 SDO。 */

      /* ---- 5. 判定 + 一次说完 ---- */

      /* 判据是**收尾结束这一刻的实测状态**, 不是上面那几个返回码的排列组合 */
      const bool end_enabled = em_is_enabled(ax) != 0;
      const bool fault_now   = (em_sw(ax) & EM_SW_FAULT) != 0;
      const ecatcmd::HomeEnd end =
         ecatcmd::home_end_state(true, fault_now, end_enabled, rc_mode);

      /* 这一趟的整句由 ecatcmd::home_axis_note() 一处出 —— **一个字的正文都不在这边**:
       * 那句话自检抄了一遍钉着 (那是"重构没改行为"唯一的自动证据), 正文散在这边就钉不住。 */
      const em_home_cfg_t &cfg = m_home_cfg[axis];
      ecatcmd::HomeReport &r   = rep[nrep];

      r.axis       = axis;
      r.method     = m_home_method[axis];
      r.rc_home    = rc_home;
      /* 原始结局也带上: `rc_home` 那张三值表把"被对侧带停"与"操作员按了停止"挤在同一格,
       * 只有这个字段分得开 (见 HomeReport::step_rc) */
      r.step_rc    = m_home_rc[axis];
      r.end        = end;
      r.rc_disable = rc_disable;
      r.rc_mode    = rc_mode;
      r.rc_enable  = rc_enable;
      r.mode_disp  = m_mode_disp[axis];
      r.vel_fast   = cfg.vel_fast;
      r.vel_slow   = cfg.vel_slow;
      r.acc        = cfg.acc;
      r.tmo_s      = m_home_tmo_s[axis];

      note(ecatcmd::home_axis_note(r));

      ends[nrep]    = end;
      step_rc[axis] = m_home_rc[axis];
      rep_mask |= (1u << axis);
      nrep++;

      /* 这一根收完了 —— 逐轴落下它的"回零中"。mask 是 publish() 唯一的来源, 清它才是
       * 真的落下 (直写 m_telem 只会被下一帧覆盖掉)。 */
      m_homing_mask &= ~(1u << axis);
   }

   /* 收尾没能确认到"已卸力" -> 走既有的动力电源告警那条路 (ec_shutdown 也是它)。
    * 判据与"哪几根"共用 ecatcmd::home_batch_maybe_live —— 任一根 STRANDED 就算。 */
   if (ecatcmd::home_batch_maybe_live(ends, rep_mask))
      m_maybe_live = true;

   /* ---- 批结论: 每一根的结果念一遍, 并点出元凶 ----
    * 只在**两根以上**时发 (n == 1 那一趟必须与改造前逐字节一致)。
    * 为什么它非有不可: note() 是覆盖写状态栏, 逐轴那几句里只剩最后一句; 而"是哪一根出的问题"
    * 可能落在前面被冲掉 —— 元凶被冲掉的那一半情形, 恰恰是最需要看见的那一半。 */
   if (nrep > 1)
      note(ecatcmd::home_batch_summary(step_rc, rep_mask));

   /* ---- 会话结束 ----
    * **摆在所有轴的结论之后**, 与改造前"先清旗标再 note"的顺序不同: 那一步在单轴时
    * 只在收尾阶梯自己发的帧里差一拍 (界面晚一帧看到旗标落下), 而两轴时必须等两根都说完,
    * 否则第一根的结论会在"另一根还在回零"的那一刻出去。 */
   m_homing      = false;
   m_homing_mask = 0;
   {
      QMutexLocker lk(&m_mtx);
      m_telem.homing = false;
      /* 逐轴那一份也要**当场**落下, 不能只靠 mask 清零: 下一帧 publish() 确实会把它推平,
       * 但这两行之间界面读到的是"本会话在跑 = false, 而这一根在回零 = true" —— 自相矛盾
       * 的一对。startHoming 起手时那两处直写就是为同一个理由存在的。 */
      for (int axis = 0; axis < EM_MAX_AXES; axis++)
         if ((mask & (1u << axis)) != 0)
         {
            m_telem.ax[axis].homing        = false;
            m_telem.ax[axis].homing_method = 0;
         }
   }
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

/* 自动重请求 OP —— 兜底, 不是主修 (主修是 Windows 那几条省电项, 见 README)。
 *
 * 现场那一次的病根是上位机停顿 1.6s, 而**从站跟不回来**才让程序彻底没救: 两台驱动器被
 * SM 看门狗踢出 OP, WKC 恒为 2/6, mirror_ok 降 0 且再也升不回来 (它只在完整帧上置位),
 * 于是复位/使能全被拒, 唯一的回程是"断开->重连"。
 *
 * 这一段要修的就是那个回程。**只管把 AL 拉回 OP, 让各轴停在未使能** —— 不重新给力矩、
 * 不清驱动器故障、不碰仍在 OP 的轴。三条规矩逐条写在 em_recover_op 的注释里。
 *
 * 调用点**只能**在 run() 那一圈的最后 (publish 之后) —— 见 run() 里那段说明。 */
void EcatThread::serviceAutoRecover(qint64 now_ms)
{
   if (m_bus == nullptr || !m_in_op || m_naxis <= 0)
      return;

   /* ---- 闸门: 有任何一条命令在动/在排队就一点都不碰 ----
    * 那些命令正在写 6040h / 6041h / 607Ah, 而恢复会重写从站的 AL 寄存器并把掉出 OP 的
    * 轴的 6040h 压成 0x0000 —— 两个写者同时动同一根轴, 后果不可推演。
    * 回零是阻塞的 (m_homing), 天然跑不到这里; 其余四条 (使能/失能/复位/停止) 靠这三个
    * 标志位 + 队列非空拦住。**队列非空这一条不能省**: drainCommands 每圈取一条,
    * 上一圈取走的那条还在跑时队列是空的, 所以标志位与队列两条都要。 */
   if (m_busy || m_resetting || m_homing)
      return;
   {
      QMutexLocker lk(&m_mtx);
      if (!m_cmds.isEmpty())
         return;
   }
   /* 从站数与建立映射时不一样 = 总线结构变了, 那时只有"断开->重连"是对的
    * (em_recover_op 里 `!bus->mapped` 那一支的同一件事, 这里是界面侧的早退) */
   if (em_slave_count(m_bus) != m_naxis)
      return;

   const ecatcmd::RecoverVerdict v =
      ecatcmd::recover_verdict(m_last_gap_ms, HMI_GAP_WARN_MS,
                               m_gap_ok_run, HMI_RECOVER_ARM_FRAMES,
                               m_recover_tries, HMI_RECOVER_MAX_TRIES);

   if (v == ecatcmd::RECOVER_NO)
      return;

   /* ---- 总线**确实**坏着才谈得上"救"与"试满了" (见 recover_bus_broken) ----
    * 2026-09-23 实跑: 少了这一条, 一条健康的总线在连上约 2 s (ARM_FRAMES) 之后就被点着 ——
    * 第一趟是在"已使能 2 根轴"之后 2 s 触发的, 而它自己紧接着报「所有从站的 AL 都在 OP」。
    * 上限 2 次会在几十秒内烧光, 而真出事那一次就轮不到了。
    *
    * **RECOVER_MASTER 不受这道闸管**: 它说的是"这台 PC 自己在卡", 那是根因告警 ——
    * 总线好不好都要说, 操作员正是靠它去改电源计划与网卡节能 (README 那一节)。 */
   if (v != ecatcmd::RECOVER_MASTER
       && !ecatcmd::recover_bus_broken(m_bad_wkc_run, HMI_RECOVER_ARM_FRAMES))
      return;

   /* 不动手的那两种: 说一次就够了。**说之前先看"这一句是不是就是上一句"** ——
    * 这一条每 2ms 跑一次, 不加这道闸, 同一句话会在状态栏里刷成噪音, 并且把 notify()
    * 弹窗一直顶着。 */
   if (v == ecatcmd::RECOVER_MASTER || v == ecatcmd::RECOVER_GIVEUP)
   {
      const QString s =
         (v == ecatcmd::RECOVER_MASTER)
            ? QStringLiteral("刚才这一帧迟了 %1 ms (超过 %2 ms): 停顿在这台机器自己, "
                             "不在从站, 未动从站的 AL。请检查电源计划与网卡节能。")
                 .arg(m_last_gap_ms).arg(HMI_GAP_WARN_MS)
            : QStringLiteral("本会话已自动重请求 OP %1 次 (上限 %2), 不再自动动手。"
                             "从站没回来请重连总线。")
                 .arg(m_recover_tries).arg(HMI_RECOVER_MAX_TRIES);

      /* 冷却只为**不重复说同一句话**, 与"动手"那个冷却分开算: 两个冷却共用一个成员的话,
       * 报过一次"M 在卡"就会把紧接着该做的那次恢复也按掉 30 秒。 */
      if (s != m_recover_said
          && (m_recover_warn_ms < 0
              || now_ms - m_recover_warn_ms >= HMI_RECOVER_COOLDOWN_MS))
      {
         m_recover_said    = s;
         m_recover_warn_ms = (int)now_ms;
         note(s);
      }
      return;
   }

   /* ---- RECOVER_TRY: 动手 ----
    * 冷却: 本会话第一次动手不受限 (last < 0), 之后要隔够。 */
   if (m_recover_last_ms >= 0
       && !ecatcmd::recover_cooldown_ok((int)(now_ms - m_recover_last_ms),
                                        HMI_RECOVER_COOLDOWN_MS))
      return;

   m_recover_last_ms = (int)now_ms;
   m_recover_tries++;

   /* 动手之前先说出来: 底下这一趟阻塞 1~3s, 界面那 1~3s 里只有 BlockTick 在喂,
    * 操作员看到灯不变而不知道为什么。与 doHome 一样, **宣告要挡在动作前面**。
    *
    * 措辞**只说这里真的查过的那两件事** (本机连续按时发帧够久 + 过程数据帧连续不足够久)。
    * 曾经写的是"从站仍不在 OP"—— 那是**没查过**的 (判据里没有一条读 AL), 而第一趟实跑
    * 就撞上了: 它自己紧接着报「所有从站的 AL 都在 OP」, 与这句当场打脸。 */
   note(QStringLiteral("本机已连续 %1 帧按时发出, 而过程数据帧仍连续 %2 帧不足 —— "
                       "正在自动重请求 OP (只救过程数据交换, 不给力矩)…")
           .arg(m_gap_ok_run).arg(m_bad_wkc_run));

   /* 值初始化: em_recover_op 在**拒绝**那几条早退路径上不填 out (它的 out 是递增填的),
    * 这里不置 0 就会拿栈上的垃圾去组织给操作员的话 */
   em_recover_t r = {};
   int rc;
   {
      /* BlockTick: 1~3 秒里界面继续动 (它的 tick 只在 em__cycle 收完一帧时响, 而
       * em_wait_state 每圈都发帧 —— 与 CMD_HOME 那一条同一个用法) */
      BlockTick tk(this);
      rc = em_recover_op(m_bus, &r);
   }

   /* ---- 恢复这一趟**不算一次帧间隔** ----
    * 不重置的话, 上面那 1~3s 会被下一圈测成一个几秒的 gap, 于是
    *   · m_max_gap_ms 变成一个几秒的数 —— 而那是操作员判断"是不是这台 PC 的锅"唯一
    *     能对账的量 (README 的验收方法就看它), 被自己污染了就再也对不了账;
    *   · 再叠上"本机在卡"那条判据, 下一次自动恢复会被自己按掉。
    * 这一行看起来像多余的清理, 删掉就同时坏掉上面两件事。
    *
    * ⚠️ `m_gap_self_want = false;` **必须与 `m_svc_prev_ms = -1` 成对** —— 2026-09-23 实跑
    * 抓到的那个 bug 就是漏了这一行: 上面 BlockTick 的构造函数把它置了起来, 而置起来之后
    * **唯一的消费点**是 run() 里那段 `if (m_svc_prev_ms >= 0)` 里面的测量 —— 而这里恰好
    * 把那个测量整段跳过了。于是这个标志**留给了下一次真正的测量**, 把一段纯外部的停顿
    * 判成"我们自己的命令造成的"。三件事同时坏掉:
    *   · 横幅说「其中最长的那次是本程序自己的命令造成的, 不是外部卡顿」—— **在说谎**,
    *     而且正好把人从根因 (电源计划 / 网卡节能) 上支开, 这是最坏的方向;
    *   · m_gap_bad_ms 记不到那一次真停顿;
    *   · 真停顿没把 m_gap_ok_run 归零 ⇒ 计数继续涨 ⇒ **2 秒后又触发一次** (日志里那两次
    *     连着来, 中间只隔一句输出)。
    * 一句话: 跳过了一次测量, 就必须同时把"这一次该归给谁"也吃掉。 */
   m_svc_prev_ms    = -1;
   m_svc_prev_t0    = -1;   /* 帧周期那一路同理: 这一趟恢复不是"两帧之间的一段" */
   m_gap_self_want  = false;
   m_gap_ok_run     = 0;
   m_last_gap_ms    = 0;
   m_gap_bad_ms     = 0;

   /* ---- 只对**被动过的那几根**重钉目标 ----
    * 恢复之后它们的位置读数已经变了 (掉出 OP 期间的位置没人知道), 而 m_tgt/m_want 还停在
    * 掉线之前的值 —— 不重钉, 下一次使能就会朝那个旧目标窜过去。
    * 照 CMD_DISABLE 那条尾巴的做法; 仍在 OP 的轴一根都不许碰 (它可能正带着操作员刚下发的
    * 运动, 而恢复本来就没动它)。 */
   if (r.ok > 0)
   {
      QMutexLocker lk(&m_mtx);

      for (int i = 0; i < m_naxis && i < EM_MAX_AXES; i++)
      {
         if ((r.was_out & (1u << i)) == 0)
            continue;
         if ((r.still_out & (1u << i)) != 0)
            continue;   /* 没救回来的那根位置仍不可信, 不动它 */
         if (!m_origin_ready)
            continue;   /* 零点还没建立, 没有可下的目标 */
         m_tgt[i]  = em_pos(m_ax[i]) - m_origin[i];
         m_want[i] = m_tgt[i];
      }
   }

   ecatcmd::RecoverReport rep;
   rep.need       = r.need;
   rep.ok         = r.ok;
   rep.fail       = r.fail;
   rep.gone       = r.gone;
   rep.nofit      = r.nofit;
   rep.mixed      = r.mixed;
   rep.stop       = (rc == 1);            /* EM_R_STOP; EM_R_OK/EM_R_FAIL 都是 0/-1 */

   /* **"拒绝"与"没救全"的 rc 都是 -1**, 分不开 —— 所以按"库填了几个数"来判:
    * 一个都没填 = 它连从站状态都没读就退出来了 (总线没开 / 没建映射 / 没进过 OP)。
    * 这一支一个关于从站的字都不许说 (见 recover_done_text)。 */
   rep.refused    = (rc != 0 && !rep.stop
                     && r.need == 0 && r.ok == 0 && r.fail == 0
                     && r.gone == 0 && r.nofit == 0 && r.mixed == 0);
   rep.max_gap_ms = m_max_gap_ms;

   note(ecatcmd::recover_done_text(rep));
}

void EcatThread::doZero(int axis)
{
   if (m_homing)            /* 同 doEnable 那一句 */
      return;
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
   if (m_homing)            /* 同 doEnable 那一句 */
      return;
   if (axis < 0 || axis >= m_naxis)
      return;
   setTarget(axis, 0);      /* 显示坐标 0 = 界面上那个正中 */
}

/* 改量程。interpolate() 是"先夹 m_tgt、再用 m_tgt 算 CSP 目标"的, 所以**改小**时若滑台
 * 正停在旧量程边缘外, 那一夹会把 m_tgt 拽回来 —— 等于凭空一次没人按过按钮的运动。
 * 放大无条件允许; 缩小只在新范围装得下所有轴当前的 m_tgt 时才允许。 */
void EcatThread::doRange(int32_t range)
{
   if (m_homing)            /* 同 doEnable 那一句 */
      return;

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

      /* 正在回零的这一根也不下发。**光靠上面那个使能位拦不住** —— 回零中的轴正是已使能的,
       * 而它此刻按 HM 解释 6060h: 往 607Ah 写一个 CSP 目标, 轻则被忽略, 重则被当成
       * 回零参数的一部分。改造前这条闸是"天然"的 (回零把整个工作线程占住, 这一圈根本轮
       * 不到); 现在回零只占每圈的一步, 所以必须**明写**。
       *
       * ⚠️ 它拦的是**整个会话**, 不只是"这一根在找"的那几秒: 收尾那一段 (失能 -> 切 CSP ->
       * 使能) 里 m_origin[] 还没重新锚定, 那一刻放过去就是拿**回零之前的物理位置**当目标
       * 发出去 —— 一次没人按过的全速运动。 */
      if ((m_homing_mask & (1u << i)) != 0)
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
   /* 哪几根在回零由下面那个逐轴循环从 m_homing_mask 填 (AxisTelem::homing) ——
    * m_homing_mask 是**唯一**的来源, "会话在跑"与"这一根在跑"就永远不会互相矛盾。 */
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

      /* 这一根自己是不是在回零 (与 t.homing 那个"本会话在跑"分开, 理由见 AxisTelem)。
       * **只问 mask, 不问结局**: 它说的是"这一根被本次会话持有", 从 prepare 之前到它自己
       * 收尾完成为止 —— 一根到位之后另一根还在找, 它仍然是被持有的那一根 (它在等对侧),
       * 界面上要能说出这件事。逐轴旗标的清除在 finishHoming() 里逐轴做。 */
      a.homing        = ((m_homing_mask & (1u << i)) != 0);
      a.homing_method = (a.homing ? m_home_method[i] : 0);

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
   {
      m_bad_wkc_run++;
      m_good_wkc_run = 0;
   }
   else
   {
      m_bad_wkc_run = 0;

      /* 好帧**归零帧间隔的坏账** —— 见 m_gap_bad_ms 的注释: 自动恢复要的正是
       * "停顿已经过去、从站没跟回来"这个状态, 而这个状态只能由"已经好了一阵子"说出口。
       * 不能拿 m_max_gap_ms 顶替: 那是会话最大值, 第一次停顿之后永远回不到这里。 */
      if (m_in_op)
      {
         m_good_wkc_run++;
         m_gap_bad_ms = 0;
      }
   }

   t.bad_wkc_run   = m_bad_wkc_run;
   t.comm_bad      = ecatcmd::comm_bad_from(wkc, t.expected_wkc, m_bad_wkc_run,
                                            HMI_BAD_WKC_LIMIT);
   t.max_gap_ms    = m_max_gap_ms;
   t.gaps_over_ms  = m_gaps_over;
   t.max_gap_self_ms = m_max_gap_self_ms;
   t.period_avg_ms = m_period_avg;
   t.gap_bad_ms    = m_gap_bad_ms;
   t.recover_tries = m_recover_tries;
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

      /* 那最长的一次如果是**我们自己命令造成的**, 必须说出来 —— 否则这句会被读成
       * "这台 PC 在卡", 把人支去查电源计划与网卡节能 (见 comm_banner_text)。 */
      if (m_max_gap_self_ms > 0 && m_max_gap_self_ms >= m_max_gap_ms)
         s += QStringLiteral("其中最长的那次是本程序自己的命令造成的, 不是外部卡顿。");

      /* 节拍慢也在这里说出来 —— **"最长多久没发帧"那个数看不见它**: 那个数只记 > 50 ms 的
       * 离群点, 也不含 em_service 内部的耗时, 所以一圈均匀地慢下来它报不出"每一帧都迟到"
       * 这件事 (那一趟 30 分钟它报的是 79 ms 那一次离群点, 而整趟帧周期是 15.8 ms)。
       * 到了"帧不足"这一刻, 两个数要一起摆在屏幕上, 让人分得开是线缆还是本机。 */
      if (ecatcmd::cadence_slow(m_period_avg))
         s += QStringLiteral(" 本程序") + ecatcmd::cadence_text(m_period_avg)
              + QStringLiteral("。");

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
   /* 同上。**会话的逐轴记录也一起清** —— 留着的话下一次 startHoming 之前,
    * publish() 会拿着上一次的结局去填遥测 (那是上一趟的话)。 */
   m_homing      = false;
   m_homing_mask = 0;
   for (int i = 0; i < EM_MAX_AXES; i++)
   {
      m_home_done[i]       = false;
      m_home_rc[i]         = EM_HM_RUNNING;
      m_home_method[i]     = 0;
      m_home_tmo_s[i]      = 0;
      m_home_disable_rc[i] = 0;
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
