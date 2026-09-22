#include "meterlog.h"

#include "powermeter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace scan {

MeterLog::MeterLog(QObject *parent) : QObject(parent)
{
   m_v.reserve(1024);
}

MeterLog::~MeterLog()
{
   /* 不调 endRecord(): 它会 emit stateChanged, 而析构里发信号没有意义
    * (接收方可能已经死在自己前面了)。文件照旧要还回去 */
   if (m_f != nullptr)
   {
      m_f->close();
      delete m_f;
      m_f = nullptr;
   }
}

/* ---------------------------------------------------------------- 源 */

void MeterLog::setSource(PowerMeter *m)
{
   if (m == m_src)
      return;

   if (m_src != nullptr)
      disconnect(m_src, nullptr, this, nullptr);

   m_src = m;

   if (m_src != nullptr)
   {
      connect(m_src, &PowerMeter::readingReady,  this, &MeterLog::onReady);
      connect(m_src, &PowerMeter::readingFailed, this, &MeterLog::onFailed);
   }

   /* 缓冲一起清掉。**这是有代价的取舍**: 换源会丢掉已经采到的那一段。换掉的理由是
    * 留着更坏 —— 曲线顶上写着源的名字, 一半的点来自另一个源而看不出来, 那就是一条
    * 会骗人的线。宁可少一段可见的历史 (真要紧的那份在 CSV 里), 也不要一条说不清来源的线。
    *
    * 注意调用方是"没在跑的时候才换得了源": 窗口那边连续读数中会灰掉下拉框, 主窗口
    * onMeterChanged 也挡了一道 */
   m_v.clear();
   m_t0 = 0;

   /* 未决的请求跟着作废。**不置 m_pending = false 之外的动作**: 旧源那一份回话还可能
    * 排在事件队列里, 它的 sender() 已经不是 m_src 了, 在槽里会被丢掉 (见 onReady)。
    * 下一次由 tick 从"现在"重排 —— 换源这一下不该被当成欠了一拍 */
   m_pending   = false;
   m_timed_out = false;
   m_due_ms    = m_now_ms;

   emit sampleAdded();      /* 曲线立刻变空 */
   emit stateChanged();
}

/* ---------------------------------------------------------------- 起停 */

int MeterLog::clampInterval(int ms)
{
   return std::min(kMaxIntervalMs, std::max(kMinIntervalMs, ms));
}

void MeterLog::setInterval(int ms)
{
   const int v = clampInterval(ms);
   if (v == m_interval)
      return;

   m_interval = v;

   /* 不打断已在飞的那一个请求。只把"下一次"往后推的那一截按新间隔重算 —— 往前挪会让
    * 下一拍立刻再发一次, 那就等于把请求提前, 与"改间隔"是两回事 */
   if (m_run && !m_pending)
      m_due_ms = m_now_ms + m_interval;

   emit stateChanged();
}

bool MeterLog::start(int interval_ms, QString *err)
{
   if (m_src == nullptr || !m_src->isOpen())
   {
      if (err != nullptr)
         *err = QStringLiteral("取样源没打开 —— 连续读数要采数, 不能没有源");
      return false;
   }

   m_interval = clampInterval(interval_ms);

   m_run       = true;
   m_pending   = false;
   m_timed_out = false;
   /* 第一笔不空等一个间隔: 按下去就该看见数, 否则会以为没生效 */
   m_due_ms    = m_now_ms;

   emit stateChanged();
   return true;
}

void MeterLog::stop()
{
   if (!m_run && !m_pending)
      return;

   m_run       = false;
   m_pending   = false;
   /* 这一句同时是"从超时卡住里出来"的那条路 (见 tick): 作废掉那个未决请求, 下一次 start
    * 就能重新发。代价是可能有一个迟到的回话被丢掉 —— 那正是"停止"要的 */
   m_timed_out = false;

   /* 未决请求不撤回 (撤不了): 它回来时 m_pending 已经是 false, 槽里的比对会把它丢掉。
    * 代价只是源那边白跑一次 */
   emit stateChanged();
}

void MeterLog::setHold(bool hold)
{
   if (hold == m_hold)
      return;

   m_hold = hold;

   /* 放开的那一刻从"现在"重排。**不补采 hold 期间欠下的那些** —— 那段时间源根本不归我们,
    * 补出来的是编的 */
   if (!m_hold)
      m_due_ms = m_now_ms;

   emit stateChanged();
}

/* ---------------------------------------------------------------- 节拍 */

void MeterLog::tick(int64_t now_ms)
{
   m_now_ms = now_ms;

   if (!m_run)
      return;

   /* 看门狗: 一个请求没在 kTimeoutMs 内回话, 记一笔"没采到"并报一次。
    *
    * **但这一拍不重新发请求, m_pending 也留着** —— 这是刻意的, 也是本类唯一一处不显然的
    * 选择。理由: 一个已经超时的请求并没有被谁撤回, 源迟早还会把它那一份回话投递回来。
    * 要是这里另发一个, 那个迟到的回话回来时 m_pending 又已经是 true 了, 它会**冒充**新请求
    * 的答案 —— 一个几百毫秒前的旧数配上新时刻, 而且一个字节的错都不会报。
    * 那正是 powermeter.h 那句话说的"静默分错数"。
    *
    * 于是这里宁可停住: 采集停在"等那一个回话"上 (计数不再涨, 界面上看得见, failed 也已经
    * 喊过), 回话一到就照常记下来、照常续采 —— 那是一个真读数, 记的时刻是"我们知道它的时刻"。
    * 真回不来了 (源彻底卡死) 就一直是停的: 这正是要让人看见的那种状态, 不是要悄悄绕过去的。
    * 「停止」后再「开始」, 或者换一个源, 都能从这里出来。 */
   if (m_pending)
   {
      if (m_timed_out || m_now_ms - m_sent_ms <= kTimeoutMs)
         return;

      m_timed_out = true;

      Sample s;
      s.ms = m_now_ms;
      s.ok = false;
      record(s);

      const QString why = QStringLiteral("等了 %1 ms 没有回话 —— 采集停在这儿, 等那一个回话 "
                                         "(或者按「停止」重来)").arg(kTimeoutMs);
      m_err = why;
      emit failed(why);
      return;
   }

   if (m_hold)
      return;
   if (m_src == nullptr || !m_src->isOpen())
      return;      /* 源被关掉了: 不算错误, 也不排下一次 —— 重新打开后由 setSource/start 复位 */
   if (m_now_ms < m_due_ms)
      return;

   m_sent_ms = m_now_ms;
   m_pending = true;

   /* 接口约定"恰好回一次"。但源没打开时三个模拟实现是**同步** emit readingFailed ——
    * 那会在 requestReading() 的调用栈里就回到 onFailed, 所以 m_pending/m_sent_ms 必须
    * 先摆好 (与 ScanWindow::onReadOnceClicked 同一个理由) */
   m_src->requestReading();
}

/* ---------------------------------------------------------------- 收数 */

void MeterLog::onReady(double watts)
{
   /* 不是当前源的、或者已经不要了的, 一个字都别动。换源与停止都靠这一句兜住迟到的那一份。
    * **超时之后的那一份照收** (m_timed_out 不清 m_pending): 它是个真读数, 见 tick 的说明 */
   if (!m_pending || sender() != m_src)
      return;

   m_pending   = false;
   m_timed_out = false;

   Sample s;
   s.ms    = m_now_ms;
   s.watts = watts;
   s.ok    = true;
   record(s);
}

void MeterLog::onFailed(const QString &err)
{
   /* 同 onReady 的说明: 停止之后回来的一份要丢掉 */
   if (!m_pending || sender() != m_src)
      return;

   m_pending   = false;
   m_timed_out = false;

   Sample s;
   s.ms = m_now_ms;
   s.ok = false;
   record(s);

   m_err = err;
   emit failed(err);
}

void MeterLog::record(const Sample &s)
{
   if (m_v.size() >= kCapacity)
      m_v.removeFirst();            /* 环形: 丢最旧的 */

   if (m_v.isEmpty())
      m_t0 = s.ms;

   m_v.append(s);

   /* 写不进去不打断采集 (读数照旧在曲线上), 但**必须喊出来** —— 一份没人守着的连续记录
    * 悄悄停止落盘, 是这套东西最容易骗人的一种坏法 */
   if (!appendCsv(s))
      emit failed(m_err);

   /* 回话驱动排下一次, 不是固定节拍 —— 一个未决请求的约束下, 固定节拍会越堆越多。
    * 实测节奏里本来就含着往返时间, 这是诚实的记法 */
   m_due_ms = m_now_ms + m_interval;

   emit sampleAdded();
}

void MeterLog::addFollowSample(int64_t ms, double watts)
{
   /* 跟随模式: 点是扫描采的, 只是借这里的缓冲画出来。**不写 CSV** (扫描那份已经写了),
    * 也不算进 pending/due —— 这条路上根本没有我们的请求 */
   Sample s;
   s.ms    = ms;
   s.watts = watts;
   s.ok    = true;

   if (m_v.size() >= kCapacity)
      m_v.removeFirst();
   if (m_v.isEmpty())
      m_t0 = s.ms;

   m_v.append(s);
   emit sampleAdded();
}

void MeterLog::clear()
{
   m_v.clear();
   m_t0 = 0;
   emit sampleAdded();
}

MeterLog::Stats MeterLog::stats() const
{
   Stats st;
   double sum = 0.0;
   bool   first = true;

   for (const Sample &s : m_v)
   {
      if (!s.ok)
         continue;

      if (first)
      {
         st.min = st.max = s.watts;
         first = false;
      }
      else
      {
         st.min = std::min(st.min, s.watts);
         st.max = std::max(st.max, s.watts);
      }
      st.last = s.watts;
      sum += s.watts;
      st.n++;
   }

   if (st.n == 0)
      return st;

   st.mean = sum / (double)st.n;

   if (st.n >= 2)
   {
      double acc = 0.0;
      for (const Sample &s : m_v)
         if (s.ok)
         {
            const double d = s.watts - st.mean;
            acc += d * d;
         }
      st.sd = std::sqrt(acc / (double)(st.n - 1));   /* 样本标准差 */
   }

   return st;
}

/* ---------------------------------------------------------------- CSV */

QString MeterLog::csvHeaderLine()
{
   return QStringLiteral("unix_ms,elapsed_ms,watts,ok\n");
}

QString MeterLog::csvRowLine(const Sample &s, int64_t t0)
{
   /* watts 用 'g',9 —— 量程从 nW 到 W, 与 scanplan.cpp 的 csvRowLine 的 %.9g 同一个口径。
    * QString::number 是 C 区域, 不随系统的小数点/千位设置变 (整份 CSV 的列宽才稳定) */
   return QStringLiteral("%1,%2,%3,%4\n")
      .arg((qlonglong)s.ms)
      .arg((qlonglong)(s.ms - t0))
      .arg(s.ok ? QString::number(s.watts, 'g', 9) : QString())
      .arg(s.ok ? 1 : 0);
}

/* 文件最后一个字节是不是换行。空文件算 true (它没有残行) */
static bool endsWithNewline(const QString &path)
{
   QFile f(path);
   if (!f.open(QIODevice::ReadOnly))
      return true;

   const qint64 n = f.size();
   if (n <= 0)
      return true;

   return f.seek(n - 1) && f.read(1) == QByteArray("\n");
}

bool MeterLog::beginRecord(const QString &path, QString *err)
{
   endRecord();

   if (path.isEmpty())
   {
      if (err != nullptr)
         *err = QStringLiteral("没给文件名");
      return false;
   }

   /* 父目录不存在先建出来 (与 ScanLog::beginNew 一样: 路径打错时抱怨在第一次写, 不是现在) */
   const QDir dir = QFileInfo(path).absoluteDir();
   if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
   {
      if (err != nullptr)
         *err = QStringLiteral("建不了目录 %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
      return false;
   }

   /* 文件已经有内容就**接着写**, 不截断: 一次连续记录停了再按「开始」, 不该把前面那些数
    * 抹掉 —— 抹掉是没法撤销的, 而"两个文件"回到文件夹里一眼能看出来。
    * 空文件/新文件才写表头。
    *
    * 附带的坑: 上一次若是被强杀在 write 中间, 文件尾巴可能差一个换行。补一个,
    * 否则新的一行会跟残行粘成一行 (字段个数不对, 读的人只会当成文件坏了)。 */
   const bool fresh = QFileInfo(path).size() == 0;

   auto *f = new QFile(path);
   const QIODevice::OpenMode mode =
      QIODevice::WriteOnly | QIODevice::Text |
      (fresh ? QIODevice::Truncate : QIODevice::Append);

   if (!f->open(mode))
   {
      if (err != nullptr)
         *err = QStringLiteral("%1 (在 %2)").arg(f->errorString(),
                                                 QDir::toNativeSeparators(path));
      delete f;
      return false;
   }

   QByteArray head;
   if (fresh)
      head = csvHeaderLine().toUtf8();
   else if (!endsWithNewline(path))
      head = QByteArray("\n");

   if (!head.isEmpty() && (f->write(head) < 0 || !f->flush()))
   {
      if (err != nullptr)
         *err = QStringLiteral("写文件头失败: %1").arg(f->errorString());
      f->close();
      delete f;
      return false;
   }

   m_f       = f;
   m_path    = path;
   m_written = 0;
   m_err.clear();

   emit stateChanged();
   return true;
}

void MeterLog::endRecord()
{
   if (m_f == nullptr)
      return;

   m_f->close();
   delete m_f;
   m_f = nullptr;
   m_path.clear();

   emit stateChanged();
}

bool MeterLog::recording() const
{
   return m_f != nullptr;
}

QString MeterLog::recordPath() const
{
   return m_path;
}

bool MeterLog::appendCsv(const Sample &s)
{
   if (m_f == nullptr)
      return true;

   if (m_f->write(csvRowLine(s, m_t0).toUtf8()) < 0)
   {
      m_err = QStringLiteral("写 CSV 失败: %1").arg(m_f->errorString());
      return false;      /* 不打断采集: 读数还在曲线上, 只是这一份文件不再可信 */
   }

   /* 每行 flush —— 崩了/断电只丢最后一个数 (ScanLog::append 同一个取舍) */
   if (!m_f->flush())
   {
      m_err = QStringLiteral("刷 CSV 失败: %1").arg(m_f->errorString());
      return false;
   }

   m_written++;
   return true;
}

bool MeterLog::saveBuffer(const QString &path, QString *err) const
{
   const QDir dir = QFileInfo(path).absoluteDir();
   if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
   {
      if (err != nullptr)
         *err = QStringLiteral("建不了目录 %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
      return false;
   }

   QFile f(path);
   if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
   {
      if (err != nullptr)
         *err = QStringLiteral("%1 (在 %2)").arg(f.errorString(),
                                                 QDir::toNativeSeparators(path));
      return false;
   }

   QString text = csvHeaderLine();
   for (const Sample &s : m_v)
      text += csvRowLine(s, m_t0);

   if (f.write(text.toUtf8()) < 0 || !f.flush())
   {
      if (err != nullptr)
         *err = QStringLiteral("写失败: %1").arg(f.errorString());
      return false;
   }

   return true;
}

}   /* namespace scan */
