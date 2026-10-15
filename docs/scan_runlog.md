# scan 异常日志 (`scan_runlog.log`) —— 长时间稳定性调查用

**它要回答的问题是**:连着跑十几个小时之后,是什么时候开始不对的。

**为什么要它**:现场报过一次「采集:功率计 1800 ms 未出新数据」,而同一句话同时覆盖两种成因
完全不同的情况(设备真的停流了 / 水位线卡住了);总线那一侧同样只有界面横幅,关窗即忘。
出事那一刻的数、以及"没爆任何错但节拍在慢慢变差"的趋势,`scan` 从前一个字都不落盘。

**它记两样东西**:

- 出事那一刻的**结构化现场**(谁、什么判据、当时的数是多少);
- 每隔 10 s 一行的**健康摘要** —— 因为最常见的坏法不是"爆了一个错",而是**什么错都没爆**,
  节拍却在慢慢变差。

代码在 [`scan/runlog.h`](../scan/runlog.h) / [`scan/runlog.cpp`](../scan/runlog.cpp)。
写进去的那些点分别在 `hmi/ecatworker.cpp`(总线那一侧)与 `scan/ophirmeter.cpp`(功率计那一侧)。

---

## 1. 文件在哪、多大、什么时候写

- **路径**:`bin/scan_out/scan_runlog.log`(与两份 CSV 同一个目录,由 `scan/scanwindow.cpp` 的
  `outDirPath()` 决定)。目录不存在会自己建。
- **固定文件名、一直追加**(这是选定的做法,不生成第二个文件)。每次启动插一段
  `# ===== <时间> 启动 (追加) =====` 与几行启动现场(`exe` / `built` / `qt` / `log` 路径),
  于是"这一份文件里哪几段是哪一趟"一眼看得出来。
- **文件是新建的**才会先写两行格式说明。
- **上限 64 MiB,到顶裁头保尾**(留最近 48 MiB):文件名不变,最前面插一行
  `# trimmed <丢掉的字节数> B at <时间>`。丢弃只发生在最旧的头部,而**每次都有标记**,
  不是静默的。粗估两路摘要合计约 12 行/分钟,64 MiB 够数周到数月。
- **打不开**时:`scan` 弹一条红横幅(原话见 [`scan_messages.md` §19](scan_messages.md)),
  控制台写系统给的原因;之后照常运行,只是这一次不记日志(`line()` 静默丢弃并计数)。

## 2. 行格式

```
<ts> | <TAG> | <event> | <k=v k=v ...> | <自由文本(可选)>
```

```
2026-10-15 14:03:22.123 | ECAT | comm_bad_begin | started_ms_ago=28 wkc=5 exp=6 badrun=14 gap_max=812 gap_self=0 period_avg=2 al_state=0x0004 al_code=0x001b al_ok=1 frames=1049288 badf=14 badpct=0.001 cbad=3 faults=0 recov_total=1 conn=1 gaps_over=2 gaps_over_self=1
2026-10-15 14:03:25.900 | ECAT | summary | up_s=3612 wkc=6 exp=6 badrun=0 comm=0 period_avg=2 last_gap=2 gap_win=601 gap_max=812 gap_self=0 frames=1802340 badf=22 badpct=0.001 cbad=3 faults=0 recov_total=1 conn=1 gaps_over=2 gaps_over_self=1 a0_fault=0 a0_code=0x0000 a0_mirror=1 a1_fault=0 a1_code=0x0000 a1_mirror=1
2026-10-15 14:05:11.004 | METER | meter_stale | n=0 wm=88421 tsmax=-1 waited_ms=1820 stale_ms=1800 kind=empty wm_ahead=-1 ok=12844 getdata_fail=0 status_bad=0 notif=0 stale_empty=1 stale_old=0 fail=1 consec=1 consec_max=1 | 功率计 1800 ms 未出新数据 (请确认设备在出数、量程是否正确)。
```

- **`ts`** 定宽 `yyyy-MM-dd HH:mm:ss.zzz`,字典序 = 时间序。
- **`TAG`** ∈ `LIFE` | `ECAT` | `METER`。
- **`event`** 稳定的小写下划线词,**不中英混写** —— 便于 `grep` 与画图。
- **字段区**空格分隔的 `k=v`;`v` 是纯 ASCII、无空格、无 `|`;没有的量写 `-`;一个字段都没有时
  整个字段区写 `-`。
- **自由文本**是最后一段,可含中文。它是**给人和给 grep 的原话**,机器**只解析到第 4 段**。
- **解析**:`scan::splitRunLogLine()`(`runlog.h`)。`#` 开头的是内务行,不是日志行。

## 3. `LIFE` —— 进程这一层

| event | 什么时候 | 说明 |
|---|---|---|
| `start` | 进程起来、日志打开成功 | 与启动分隔行一起,标出"这一段是哪一趟" |
| `runlog_dropped` | 背压丢过行(计数涨了) | `lost=` 本次丢了几行,`total=` 累计。**看到它就说明那一段有缺口** |

## 4. `ECAT` —— 总线这一侧

### 4.1 事件

| event | 什么时候 | 关键字段 |
|---|---|---|
| `connect_ok` | 进了 OP(连接成功) | `naxis` / `exp`(期望工作计数器) |
| `connect_fail` | 任一步没连上,或等了 15 s 没退出 | `reason`(`bus_new` / `open` / `too_many` / `setup` / `enter_op`) |
| `comm_bad_begin` | **连续 WKC 不足 ≥ 5 帧**那一下 | `started_ms_ago` / `wkc` / `exp` / `badrun` / `gap_max` / `period_avg` / `al_state` / `al_code` / `al_ok` |
| `comm_bad_end` | 坏帧之后**连续好帧 ≥ 5 帧**(或掉出 OP、或收尾) | `badrun`(这一段坏帧的峰值) / `dur_ms` / `end=teardown`(收尾时补的那一条) |
| `axis_fault_begin` / `axis_fault_end` | `6041h` bit3 或 `603Fh` 报故障的**两个沿** | 逐轴的 `aN_fault` / `aN_code` / `aN_mirror` |
| `limit_begin` / `limit_end` | 逐轴限位**边沿**(它一直触发着的那几个小时不刷屏) | `axis` / `home` / `pos` / `neg` / `known` |
| `recover_try` | 自动重请求 OP 动手之前 | `try` / `last_gap` / `ok_run` / `badrun` |
| `recover_done` | 那一趟回来之后 | `rc` / `refused` / `need` / `ok` / `fail` / `gone` / `nofit` / `mixed` / `stop` |
| `recover_master` | 判定"**这一帧**迟了 `HMI_GAP_WARN_MS` 以上" | `last_gap` / `ok_run` |
| `recover_giveup` | 本会话动手次数到顶,不再自动动手 | `tries` |
| `stop` | 收尾完成 | `maybe_live`(断电前有没有确认失能) / `up_s` |

`recover_master` / `recover_giveup` 两条受既有那道冷却闸限制(同一句话 30 s 只说一次),
所以它们不会刷屏。

> **`recover_master` 那一条只证明"这一帧迟了"。** 判据是 `recover_verdict()` 里的
> `cur_gap_ms > gap_limit_ms`,量的是本机两次发帧之间隔了多久 —— 从站好不好它一个字都没查
> (AL 只在 `comm_bad` 的上升沿读一次,而且只读第一台从站)。屏幕上那句措辞因此也只说
> 「停顿在本机这一侧,未动从站的 AL」,**不再说"不在从站"**(2026-10-15 去掉)。
> 想知道那一刻从站在什么状态,看同框的 `comm_bad_begin`:那上面有 `al_state` / `al_code` / `al_ok`。

### 4.2 为什么突发要**两头**防抖

只防开头是不够的:一条坏/好逐帧翻转的线会以"5 帧坏 + 1 帧好"的周期反复来回,光靠"开头要连续
5 帧坏"能刷出上百对 `begin`/`end` 每秒,而那正好把真正要看的东西埋掉。所以**收口也要连续 5 帧好**。

代价:短于 5 帧的抖动**一行都不记**。它不算丢 ——累计量 `badf` / `badpct` 照样把它们数进去。

### 4.3 跨连接只增的那几个数

`frames` / `badf` / `badpct` / `cbad` / `faults` / `recov_total` / `conn` /
`gaps_over` / `gaps_over_self` 是**累计量**,**断开重连不归零**。这一点是**故意的**,
而且是这一族存在的理由:

> 界面上那些计数器(`BusTelem` 里那一批)每次连接都清零 —— 它答的是"这一次连上之后怎么样"。
> 而重连一次就归零的话,日志看上去会像"变好了",**那恰好就是现场报的那个现象**。

所以 `ecatworker.cpp` 的 `doConnectInner()` 那个显式清零块与 `teardown()` **都不许碰它们**。

`gaps_over` 是"累计卡顿次数",`gaps_over_self` 是其中**自家造成**的那一份(复位/使能/回零期间
SOEM 的 SDO 事务一帧都不发,那几百毫秒是我方按下去的,与"这台 PC 卡不卡"无关)。
**它是子集,不是并列的一份** —— 加了它 `gaps_over` 照样涨。判据落在
`ecatcmd::busNoteGap()`(`ecatworker.h`):**严格大于** 50 ms 才算一次。

> ★ 这两个数在 2026-10-15 首跑里**全是 0**:`gaps_over_total` 当时声明了、摘要里也打了,
> 而全仓库没有任何一处 `++`。自检当时直接手赋 `= 7` 验格式,所以一直是绿的。
> **加计数字段时,断言要落在"它有没有数"上,不是"它好不好看"上** —— 那个纯函数就是为了
> 让自检能喂几个 gap 进去看它涨不涨(`test_bus_runstat`)。

## 5. `METER` —— 功率计这一侧

### 5.1 事件

| event | 什么时候 | 关键字段 |
|---|---|---|
| `meter_open_ok` | 打开成功(含流已启动) | `serial` / `wl` / `range` / `mode` / `filter` + 自由文本里的设备名与单位 |
| `meter_open_fail` | 打开阶段任一步失败 | 自由文本 = 屏幕上那句原因 |
| `meter_config_ok` | 改配置(停流 → 改 → 重开流)成功 | `wl` / `range` / `mode` / `filter` / `add_wl` |
| `meter_config_fail` | 改配置失败 | `restarted`(流开回去了没有) / `wm` / 各请求项 |
| `meter_getdata_fail` | `GetData` 自己失败(COM 层) | 自由文本 = COM 原话 |
| `meter_status_bad` | 取到数但 status 说它不可用 | `st` / `wm` |
| `meter_stale` | 等新数等到超时(屏幕上那句 1800 ms) | 见下面 5.2 |
| `meter_recover` | **失败之后第一次成功** | `consec_fail` |
| `meter_req_closed` | 功率计没打开就来要读数 | 无 |
| `meter_thread_leak` | 采集线程收不掉(5 s / 15 s),被故意泄漏 | `where=close` / `where=dtor` |
| `meter_stop` | 采集线程收摊 | `up_s` / `wm` |

读数**成功不逐次记** —— 一个点一个数,几分钟就是几万行,只有噪音。`meter_recover`
(失败之后第一次成功)才是"它自己好起来了没有"这个问题的答案。

### 5.2 `meter_stale` 那一行为什么字段这么多

它是**本轮最要紧的一行**:屏幕上那句「功率计 1800 ms 未出新数据」把两种成因完全不同的情况
说成了一句话,而这一行把它们分开。

| 字段 | 意思 |
|---|---|
| `n` | 本批 `GetData` 取回来的**项数**。`0` = 设备手上一个数都没有 |
| `tsmax` | 本批**全部**时间戳里的最大值(**含被水位线挡掉的那些**)—— 设备手上最新的一条有多新 |
| `wm` | 请求**之前**的水位线 |
| `kind` | `empty` / `all_old` / `fresh`,由 `scan::staleKind()` 判 |
| `waited_ms` | 这一次请求等了多久 |
| `stale_ms` | 判超时的那条线(`k_stale_ms` = 1800) |
| `wm_ahead` | `wm - tsmax`,也就是**水位线卡住的深度**;有一头没量到就是 `-1` |

判据:

- `n = 0` → **`kind=empty`**:设备没在出数。查线 / 供电 / 驱动 / StarLab。
- `n > 0` 且 `tsmax <= wm` → **`kind=all_old`**:有数,但一条都不比水位线新。
  **这是我们自己的状态机**,不是设备。
- 边界 `tsmax == wm` **算 `all_old`** —— 过滤那一条写的是 `data.timestamps[i] <= watermark`,
  相等就是"不比它新"、就是要被丢掉的那一条。

> ★ 自检 `test_stale_kind` 钉的就是这条边界。它要是判反了,屏幕上明明没出数而日志说"有新的",
> 整个调查就被带偏了。

### 5.3 已知的可疑点(截至 2026-10-15 **故意没修**)

`runSession()` 里"改配置"那条路有两支不对称:

- **成功**那一支写着 `watermark = -1.0;`(注释:新流 = 新时间戳,水位线作废);
- **失败**那一支重新开了流(**时间戳从此重来**),而水位线**留在旧流那个高位上**。

若设备的时间戳确实随 `StartStream` 从头开始,那么此后每一项都会被那句 `<= watermark` 挡掉,
表现就是屏幕上一句「1800 ms 未出新数据」一直挂着,直到操作员**手动换个设置**(走成功那一支)
才恢复 —— 与现场报的现象(切一下设置就有读数了)对得上。

**没修的理由**:修了就看不到它了。要先由日志证实(`meter_config_fail` 的 `restarted=1 wm=<大数>`
配上紧随其后的 `meter_stale kind=all_old`),再改。改的时候这一处日志点要留着 ——
它是这条路的唯一见证。

## 6. 摘要那一行(`summary` / `meter_summary`)

每 10 s 一行,分别在 `ECAT` 与 `METER` 两个 TAG 下。这是长稳调查的**主表**:

```bash
grep '| ECAT | summary |' scan_runlog.log | awk -F'period_avg=' '{split($2,a," "); print a[1]}' \
  | sed -n '1~10p'      # 把 period_avg 抽出来看趋势
```

- 总线侧:`period_avg`(帧周期均值) / `gap_win`(上一次摘要以来最长的一次停顿) /
  `badpct` / `conn` / `up_s`(会话秒数)。
- **`gap_win` 才是"这一分钟本机最长停过多久"** —— 发完摘要就清零,所以它答的是"刚才那一段"。
  `gap_max` / `gap_self` 是**会话最大值**,一旦被某一次大的钉住(启动头几秒经常就有一个上千
  毫秒的)就再也不动了:**看这两个要看它涨不涨,不是看它的值**。首跑就是这么吃的亏,见 §10。
- `last_gap` 是**本帧**的间隔,本机正常在 2--4 ms(实测圈期 2.5 ms 级,不是标称的 2 ms)。
  它一跳就是你正好抓到了一帧迟的。
- ★ **`period_avg` 是整数截断的**(`m_period_avg = (int)(sum / n)`),所以 `2` 表示的是
  **2.0--2.9 ms**,不是 2.0 ms。2026-10-15 第二趟实跑就是 2.58 ms 全程印成 `2`
  (`period_avg_ms` 是 `int`,界面那几条告警判据也都按 `int` 走,所以这里不为日志另立一份数)。
  **要精确的节拍,用同一行里的 `frames / up_s` 算** —— `frames / up_s` 就是帧/秒,
  取倒数乘 1000 就是毫秒。73 分钟那一趟是 387 帧/秒 = 2.58 ms。
  它粗但**够看趋势**:真劣化到 4 ms、5 ms,这一格会跟着变(`HMI_PERIOD_WARN_MS` = 6 ms)。
- 功率计侧:`consec_max`(连续失败峰值) / 两个 `stale_*` 计数 / `ok` / `fail`。
- **`up_s` 是单调的会话秒数**(进程起来多久,不是这次连接多久)。墙钟被 NTP 或夏令时拨过之后
  靠它对账 —— 所以它是 `QElapsedTimer`,不是 `QDateTime`。

摘要写在 `publish()` 的尾部(总线侧):那个函数是**每帧唯一的汇聚点**,而且长阻塞命令期间也经
`BlockTick` 被调 —— 那一刻主循环根本没在转,摘要要是挂在循环里就永远发不出来,而那正是最需要
一行心跳的时候。

## 7. 它是怎么接上去的(改之前先读这一节)

```
工作线程 (EcatThread 的 2 ms 循环 / OphirMeter 的 10 ms 轮询)
      │  sink(tag, body) —— std::function,只许入队
      ▼
   RunLog::line()  ── 取时间戳 + 拼一行 + 短锁入队 (绝不碰磁盘)
      │
      ▼  1 s 一把,只在 GUI 线程
   RunLog::drain() ── 写盘 + flush + 到上限裁头
```

三条硬规矩(定义在 `runlog.h` 顶部,这里只重复最要紧的):

1. **生产者不写盘。** 往 2 ms 一圈的控制循环里塞一次可能被磁盘(或杀毒)拖住上百毫秒的写盘,
   是拿被测对象换日志。
2. **队列有界**(4 MiB),超了丢**最旧**的整行并计数 —— 生产者永不阻塞、内存不会涨爆。
3. **失败不抛、不刷屏。** `bool` + `QString *err`,与 `ScanLog` 同一条约定。

两处注入点(`EcatThread::setLogSink` / `OphirMeter::setLogSink`)**都必须在启动之前设一次**,
之后工作线程只读。`hmi` 从头到尾不设 sink —— 空回调,一条新路径都不走,
**行为与加这个功能之前逐字一样**(`hmi/CMakeLists.txt` 一个字没改)。

`RunLog` 对象是**进程寿命的、永不 delete**,与窗口寿命故意不一样:功率计那条线程收不掉时是被
**故意泄漏**的,它还在跑、还会回调;窗口析构时把日志一起删掉,那条线程就会踩在已释放的对象上 ——
那正是本功能要记录的异常,却会让程序在记录它的过程中崩掉。收尾只调 `close()`
(排空 + flush + 关文件),之后再来的 `line()` 静默丢弃并计数。

关窗的顺序(`scanwindow.cpp` 的 `closeEvent`)不能错:
`m_mlog->stop()` → `m_ophir->close()` → `disconnectAndStop()` → `m_thr->wait()` →
`saveSettings()` → `drainRunLog()` → `m_runlog->close()` → 摘 sink。

## 8. 怎么用它(验收方法)

```bash
grep '| ECAT | ' scan_runlog.log | tail -50        # 总线最近出了什么事
grep '| METER | meter_stale' scan_runlog.log | tail   # 那一句话到底是哪一种病
grep 'kind=all_old' scan_runlog.log                # 水位线卡住过几次
grep 'trimmed\|runlog_dropped' scan_runlog.log     # 日志本身有没有缺口
```

1. **空置跑 30–60 min**(不必动轴):确认每约 10 s 一条 `ECAT | summary`;把
   `period_avg` / `gap_max` / `badf` 抽出来画趋势 —— 这就是"没爆错但节拍变差"那张表。
2. 打开 PD300R/Juno+,开连续读数:确认 `METER | meter_summary` 每 10 s 一行。
3. **制造功率计异常**(拔插 USB,或让流停):确认 `meter_stale` 的
   `n=` / `tsmax=` / `wm=` / `kind=` 能把"空数组"与"全是旧时间戳"分开。
4. **拔插 EtherCAT 网线**:确认 `comm_bad_begin` / `comm_bad_end` / `recover_*` 出现,
   而且**重连之后 `frames` / `badf` / `cbad` / `recov_total` 继续只增、不回零**。
5. 运行中 `taskkill /F`,再开:确认文件是**追加**的,而且有新的启动分隔行。

## 9. 自检覆盖了什么、没覆盖什么

**钉住了**(`scan/selftest.cpp`,不需要硬件):

| 用例 | 钉的是 |
|---|---|
| `test_runlog_write_parse` | 写→排空→读回来的往返;行首格式的正则;自由文本里的 `|` 与换行必须被折掉;追加而不是覆盖;没打开时不崩 |
| `test_runlog_backpressure` | 队列封在上限内、丢的是**最旧**的、进程不卡 |
| `test_runlog_trim` | 超上限裁头保尾;行边界对齐;**首行是裁剪标记**;裁完还能接着用 |
| `test_stale_kind` | `empty` / `all_old` / `fresh` 三支与**边界 `tsmax == wm`** |
| `test_bus_runstat` | `onFrame` 的真值表、`badPct()` 的"没量到给 -1"、拼接的字段能被解析器拆开、**`busNoteGap` 真的会涨**(边界在 50 ms 上、self 是子集)、字段数(摘要 10 + 累计 9 + 每轴 3) |
| `test_meter_summary_fields` | 四种坏法分开记、合计与连续次数、通知不算失败 |

**没覆盖的**(报告时必须如实说):`ecatworker.cpp` 与 `scanwindow.cpp` 里的 **sink 接线与调用点**
都不在自检编译范围内(`SCAN_COMMON_SRC` 不含它们),**只能真机验**。所以"日志里真的出现了那一行"
这件事,得按 §8 跑一遍才算验过 —— 不许拿"编译过了"当"验过了"。

## 10. 第一趟实跑记录(2026-10-15,82 min)—— 这一趟教会了我们什么

`bin/scan_out/scan_runlog.log`,14:05:12 → 15:27:05,`conn=1`(一次都没重连),
492 条 `ECAT | summary` / 492 条 `METER | meter_summary`。

**日志本身是好的**:摘要间隔全程 10.00 s(最大 10.01 s),一拍没漏;`drop` / `trimmed` 一条都没有。
(当时我说"节拍全程 2 ms、没有劣化"是**读错了那一格**:`period_avg` 截断成整数,`2` 说的只是
"不到 3 ms"。第二趟量到真值是 2.58 ms —— 见 §6 那条,以及下面第二趟那一节。)

**功率计侧零异常**:`ok=35444`,`fail=0`,`getdata_fail=0`,`status_bad=0`,两个 `stale_*` 都是 0,
`consec_max=0`,每 10 s 的读数增量稳定在 7.2/s、一次都没掉到 0。现场那句
「1800 ms 未出新数据」**这一趟没复现**(§5.3 那条可疑点因此继续留着不修:它要的证据没出现)。

**总线侧一次真事故**(13 次 `comm_bad` 里 12 次是 `wkc=-1 al_ok=0` 的几毫秒抖动):

| 时刻 | 事件 | 要点 |
|---|---|---|
| 14:50:58.407 | `comm_bad_begin` | `wkc=2 exp=6`,`al_state=0x0008`(**OP**),`al_ok=1` |
| 14:51:00.571 | `recover_master` | `last_gap=601` |
| 14:51:02.523 | `summary` | `wkc=2 badrun=757`,`a0/a1_mirror=0` |
| 14:51:03.337 | `recover_try` | `ok_run=1000`(坏帧期间本机仍按时发帧) |
| 14:51:03.400 | `axis_fault_begin` | 两轴 `fault=1 code=0x0000` |
| 14:51:03.407 | `comm_bad_end` | `badrun=1023 dur_ms=5010` |
| 14:51:03.474 | `recover_done` | `rc=0 need=2 ok=2` —— **过程数据救回来了** |
| 14:51:12 起 | 每条 `summary` | 两轴 `fault=1`,到日志结束 36 min 没有 `axis_fault_end` |

两件事**不是 bug**、不需要改代码:

1. **锁故障 36 分钟**是设计 + 缺一个人工动作。`em_recover_op` 只把 AL 拉回 OP,**一个 SDO 都不做、
   不写 6040h bit7**;故障归 `em_fault_reset()`,那条路有人工闸。屏幕当时就说了
   (「有故障请先「故障复位」」)。
2. **`bit3=1` 配 `603Fh=0x0000`** 是记过的组合(CiA402 的 bit3 是**闩锁**的,码可以已经清掉),
   处置就是「故障复位」。SM 看门狗把从站踢出 OP 之后就是这个样子。

**答不出来、而这一趟恰恰卡在这里的一件**:

> 14:50:58 那一瞬间,是**本机先停**还是**从站先掉**?

当时的日志没有这个能力,两条原因都已在这次修掉:

- **摘要里的 `gap_max` / `gap_self` 是会话最大值**,被 14:05:17 启动那一次的 **1643 ms** 一钉到底
  —— 于是 14:51 那次**真实的 601 ms 停顿在 492 条摘要里一个数都没留下**,只活在
  `recover_master` 那一行的 `last_gap=` 里,而那是**最后一次**停顿,不是最早那次。
  → 现在有 `gap_win`(窗口化,发完摘要清零),那种停顿会在**紧接着的那一条摘要**上现形。
- **`gaps_over_total` 是死字段**(没有任何一处 `++`),492 条摘要一条不落地写着 `gaps_over=0`,
  而这一趟明明有两次 >50 ms 的停顿。
  → 现在记账搬进 `ecatcmd::busNoteGap()`,自检喂数进去钉它涨不涨。

**读法**(这一节的用处):一趟跑完先看 `gap_win` 有没有非 2 ms 的尖、`gaps_over` 涨不涨;
两者都动过的那一段再去翻 `comm_bad_*` / `recover_*` / `axis_fault_*`。
`gap_max` 只要不是 0 就说明"这一趟卡过",但它**说明不了是什么时候卡过** —— 别拿它定位时刻。

### 10.1 第二趟实跑(2026-10-15 16:27:29 → 17:43:27,76 min)

**这一趟就是上面那三个改动的验收** —— 二进制 `built=Oct 15 2026 15:54:46`,行里已经有
`last_gap` / `gap_win` / `gaps_over_self` 三个新字段。一次 `conn=1`,正常收尾
(`stop maybe_live=0`)。456 条 `ECAT | summary` / 456 条 `METER | meter_summary`,10.0 s 一拍没漏。

**(a) 新字段确实在干活**:

- `gap_win=1627` 只出现在启动后第一条摘要(16:27:39),之后 75 分钟一直是 **4--13 ms**
  —— 窗口机制成立,发完就清零,没有退化成会话最大值。
- `gaps_over=1 gaps_over_self=1` **全程不变** —— 死字段修好了(首跑那 492 条全是 0),
  而且它数到的那唯一一次(启动的 1627 ms 静默)被正确归到"自家造成"。
- `last_gap` 全程 2--4 ms。

⇒ **这一趟 76 分钟里,上位机一次都没有迟到。** 连最凶的那一段(每秒 10--15 个坏帧)也没有。

**(b) 坏帧是"一阵一阵"的,而且日志把它定位到分钟**:基线约 0.36 坏帧/秒(`badpct` ≈ 0.09%),
但有几段明显抬高 ——

| 时段 | 坏帧速率 | 备注 |
|---|---|---|
| 16:43:59 -- 16:46:29(2.5 min) | 3.5--8.4/s | 20 次 `comm_bad` 里有 12 次挤在这 2.5 分钟。**当时没人碰任何东西** |
| 17:20:20 -- 17:23:40 | 10--15/s(最凶) | 17:22:20 开始跑一次扫描 |
| 17:37:30 -- 17:37:40、17:42:50 -- 17:43:20 | 2--10/s | 扫描期间 / 收尾前 |
| 两段之间 | 0.1--0.8/s | 回到基线附近 |

20 次 `comm_bad` 全是 `wkc=-1`(**一个答话都没有**),每次 13--54 ms 自己就回来了,
`badrun` 峰值 5--10 帧;前 10 次连 AL 都没读上来(`al_ok=0`),后 10 次读到了 ——
`al_state=0x0008` 是 **OP**。全程 `faults=0` / `recov_total=0`,两根轴一直 `fault=0`,
**首跑那次 601 ms 停顿 + 双轴锁故障没有复现**。

**(c) 这一趟量到的真节拍是 2.58 ms**(387 帧/秒,455 段里 454 段都在 386--391),而 `period_avg`
全程印的是 `2` —— 本机正常值(§6 那条)。它**没有**随时间漂:76 分钟一条平线。

**(d) 功率计侧又是零**:456 条摘要的 `fail` / `consec_max` / 两个 `stale_*` / `getdata_fail` /
`status_bad` **全是 0**,读数 7.19 次/秒。除 `meter_open_ok` 与 `meter_stop` 外没有任何 METER 事件。

> 这一趟的结论比首跑硬:**功率计与水印那条路无嫌疑;上位机自己也无嫌疑(`gaps_over` / `gap_win`
> 一条平线)。剩下的只有总线本身** —— `wkc=-1` 而主站按点发帧、从站还在 OP,那是"帧发出去、
> 一个答话都没回来"的形态,即物理层(线缆 / 接头 / 屏蔽 / 伺服主回路的干扰),而且它是
> **一阵一阵**的、不随时间累积 —— 与"跑久了才坏"这个印象不符。
> **注意 16:44 那一阵没有任何操作**,而 17:22 那一阵有扫描在跑:触发条件怕是不止一个。

**(e) 两处要留个心**:① `stop` 在 17:43:27.670 与 .690 各写了一行,内容一字不差 ——
收尾被喊了两次,不害事但该看一眼;② 首跑那份日志**不在了**(这个文件开头有格式说明行、
也没有 `# trimmed`,说明它是**新建**的而不是追加),所以两趟之间只能对已经摘出来的数。
