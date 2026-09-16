# Qt 上位机的结构与布置规划

目标：在 `EC_CPP` 里增加一个 **Qt Widgets 工程师调试台**（`hmi/`），
复用现有 SOEM 工具链的判定与护栏，而不是再抄一份。

定位（已确认）：**工程师调试台**——对齐现有 5 个 CLI 的能力，
从站树 / 对象字典 / 过程数据镜像 / 位级状态灯 / PDO 映射全可见，写操作必须手动解除保险。
不是产线触摸屏人机（那是将来在它之上裁出的"操作模式"子集）。

前置决策（已确认）：**先抽 `ecat_core`，再上 Qt**；UI 用 **Qt Widgets**。

---

## 0. 现状（2026-09-15）：先落了"点击定位"这一个界面

本文件规划的是**工程师调试台**（P0~P4：抽样 / 对象字典 / 基线 diff / 抖动面板 / 报文回放）。
实际先落地的是它的一个很小的子集：`hmi/` 里一个**两台滑台的 ±500000 脉冲 CSP 点击定位界面**
（点哪里去哪里 / 开机位置即零点 / 速度可调），见 [README.md](../README.md) 的运行注意。

它与下面的规划有两处**有意的偏离**，将来做 P1 时要收回来：

1. **没有先抽 `ecat_core`。** 这次直接链 `motor_api`（`ec_motor.c` + `ec_motor_motion.c`），
   界面侧完全不 include SOEM —— 所有 `ecx_*` 都关在 [hmi/ecatworker.cpp](../hmi/ecatworker.cpp)
   一个文件、一个工作线程里。所以第 1 节说的"同一个东西存在于多处"这个问题**没有被解决，
   也没有被加重**：`hmi/` 没有新增第三套总线初始化，它只是 `motor_api` 的调用方。
2. **`hmi/` 这个名字与 P1 的目标目录一致，是故意的。** 将来抽 `ecat_core` 时，把
   `ecatworker.cpp` 里那些 `em_*` 换成 core 的接口即可，目录名不用动；
   `axispanel`/`mainwindow` 这两层与总线无关，基本可以原样留用。

**2026-09-17 补**：`hmi/` 旁边又多了一个 [scan/](../scan/) —— 控制滑台**蛇形扫描**一个
矩形区域、逐点读功率计、写 CSV 并画二维热力图的上位机（[docs/scan_sweep.md](scan_sweep.md)）。
它对本节这两处偏离的处理方式是**继续走同一条路，而不是把 P1 提前**：`scan`
**把 `hmi/ecatworker.cpp` 原样编进自己的目标**（同一个 2ms CSP 循环、同一套坐标零点与
收尾护栏，只有一份实现），界面侧同样一个 `ecx_*` 都不碰。所以现在"同一套总线工作线程"
有了**两个**调用方，`ecatworker` 事实上已经承担了 core 的职责 —— 将来抽 `ecat_core`
时它是唯一要动的地方，两个界面都只是它的调用方。

另外本节记录的这条环境事实值得单独记住：**界面用的 Qt 必须与 SOEM 同一套 CRT**。
本机取 **MSYS2 UCRT64 仓库的 Qt 6**（`mingw-w64-ucrt-x86_64-qt6-base`），
**不能用 Qt 官方安装器那个 msvcrt 版 MinGW** —— 理由（工作线程 `printf` 崩在
`msvcrt!_lock` 的完整链路）写在 [README.md](../README.md) 的「上位机 `hmi` (Qt)」一节
和 [CMakePresets.json](../CMakePresets.json) 的 `hmi-qt-ucrt64` 里。
**本文件里凡是提到构建工具链的地方（§11、§12…），一律以这一条为准** ——
§11 原先写的"MSVC 2022 x64 / 别混 MinGW"是被实现推翻的旧设想，已就地标注。
`scan/` 与 `hmi/` 用**同一个 preset**（`EC_BUILD_SCAN` 默认跟随 `EC_BUILD_HMI`），
所以这条约束对它同样成立，且没有第二条 configure 命令需要维护。

---

## 1. 为什么必须先抽 `ecat_core`

现在同一个东西存在于多处：

| 已有实现 | 内容 | 问题 |
|---|---|---|
| [slide_motion/sm_bus.c](../slide_motion/sm_bus.c) (733 行) | 时钟 / SDO 读 / 寄存器读 / 错误栈排空 / 身份判定 | 只读底座，但只有 `slide_motion` 一族在用 |
| [slide_motion/sm_guard.c](../slide_motion/sm_guard.c) (741 行) | **全工程唯一的写入口** | 这条不变量写在 [slide_motion/CMakeLists.txt](../slide_motion/CMakeLists.txt) 里 |
| [slide_motion/sm_baseline.c](../slide_motion/sm_baseline.c) (688 行) | 基线 INI 解析与比对 | — |
| [slide_verify/slide_verify.c](../slide_verify/slide_verify.c) (723 行) | 自己一套总线初始化 + 对象校验表 `sv_objs[]` | **重复实现**，不含 `sm_guard` |
| [slide_motion/test2.c](../slide_motion/test2.c) (1036 行) | 又一套最小实现 | 明确写了"放弃三条保障" |
| [aliasinfo/aliasinfo.c](../aliasinfo/aliasinfo.c) (114 行) | 只读别名 | — |

对象清单与判定规则硬编码在三处以上。**后果在 2026-09-15 已经发生过一次**：
手册更新到 V2.4 后，[ykd2205pe_ci402.md](ykd2205pe_ci402.md) 更正了
6041h 位定义、2401h 默认值、2102h 语义、PDO 默认映射，
而代码里那些断言和注释**没有跟着变**，于是不同地方对同一台设备给出互相矛盾的结论。

Qt 上位机如果直接链 `sm_*.c` 的源文件，就是把这件事放大成第四份抄本，
而且**会把「唯一写入口」这条护栏拆成三份**：

- CLI 的护栏在 `sm_guard.c`
- `test2.c` / `sm_pdo.c` 各有自己的写入口
- Qt 面板如果再自己调 `ecx_SDOwrite`，就再没有单点可审

所以 `ecat_core` 不是"重构洁癖"，它是**让 Qt 不必成为第三个豁免者**的手段。

---

## 2. 目标目录结构

```text
EC_CPP/
├─ CMakeLists.txt              # project(EC_CPP LANGUAGES C CXX) + option(EC_BUILD_HMI OFF)
├─ CMakePresets.json           # 新增 preset (实际叫 hmi-qt-ucrt64; 与 build/ build-mingw 隔离)
├─ SOEM/                       # 不动
│
├─ ecat_core/                  # ★新: 无 Qt 依赖的静态库 (C++17, 内部可继续用 C 风格)
│   ├─ CMakeLists.txt
│   ├─ include/ecat/
│   │   ├─ bus.h               # 打开网卡 / config_init / config_map / 状态检查 / DC / SM
│   │   ├─ identity.h          # YKD 身份集合: 厂商 0x0994, 产品码 {0x2000, 0x3000}
│   │   ├─ sdo.h               # 读 (无限制) + 写 (必经 guard)
│   │   ├─ pdo.h               # 映射读取 / 偏移推导 / 过程数据镜像访问
│   │   ├─ cia402.h            # 6040h/6041h 全位定义 + 状态机判据 (★改正后的判法)
│   │   ├─ od.h                # 对象字典查询 (由 od/ 生成, 不手写)
│   │   ├─ baseline.h          # 基线 INI 解析 / 比对 / 导出
│   │   ├─ guard.h             # ★唯一的写入口 + 授权 + 上限 + 撤防
│   │   └─ verdict.h           # PASS/WARN/FAIL + 退出码契约 (现 sm.h 的 0~10)
│   └─ src/
│       ├─ bus.cpp  sdo.cpp  pdo.cpp  od.cpp  baseline.cpp
│       ├─ guard.cpp           # ★全工程只有这个文件出现 ecx_SDOwrite
│       └─ verdict.cpp
│
├─ od/                         # ★新: 对象字典的单一数据源 (手册 → 数据)
│   ├─ ykd2205pe_od.csv        # 对象/类型/范围/默认值/单位/可写性/危险级/source 列
│   └─ gen_od.py               # → ecat_core 的 C 表 + Qt 的模型数据 + 文档一致性检查
│
├─ tools/                      # (可选, 阶段 4 之后) 现有 CLI 移入, 全部链 ecat_core
│   ├─ aliasinfo/  slide_verify/  slide_motion/     # 含 sm_*.c / test*.c
│
├─ hmi/                        # ★新: Qt Widgets 上位机
│   ├─ CMakeLists.txt
│   ├─ main.cpp
│   ├─ bus/                    # ★唯一持有 ecx_context 的地方
│   │   ├─ bus_backend.h       # 抽象: LiveBus / ReplayBus (见 §5)
│   │   ├─ bus_worker.cpp      # ★唯一 #include "soem/soem.h" 的文件
│   │   ├─ snapshot.h          # 过程数据快照 (双缓冲, 原子换手)
│   │   ├─ cmd_queue.h         # GUI → 总线线程的命令队列
│   │   └─ recorder.h          # CSV 记录 / 回放
│   ├─ model/                  # QAbstractItemModel: 从站树 / 对象字典 / PDO 映射表
│   ├─ safety/
│   │   ├─ arm_controller.cpp  # UI 保险: 各类授权的解除与自动撤防
│   │   └─ state_lamp.cpp      # 状态灯控件 (调 ecat_core 的判定函数)
│   ├─ panels/                 # 一个面板一个文件 (QDockWidget)
│   │   ├─ panel_connect.cpp       # 网卡 / 扫描 / 从站树 / AL 状态
│   │   ├─ panel_pdo_mirror.cpp    # ★过程数据镜像 + 位级标注
│   │   ├─ panel_state_machine.cpp # 6040h/6041h 位灯 + 切换 + 通道指示
│   │   ├─ panel_od_browser.cpp    # 对象字典 (SDO 读; 写需解保险)
│   │   ├─ panel_pdo_map.cpp       # 1C12h/1C13h + 它们指到的那张映射对象 + 推导偏移
│   │   ├─ panel_baseline.cpp      # 基线 diff
│   │   ├─ panel_motion.cpp        # 使能 / 微动 / 回零 (PP)
│   │   ├─ panel_jitter.cpp        # DC 周期抖动 vs 2217h 阈值
│   │   └─ panel_trace.cpp         # 报文 / 日志 / 故障历史
│   └─ util/                   # QSettings 持久化; 当量换算 (受 2201h 决定)
│
├─ docs/  manual/  baseline_ykd2205pe.ini  YAKO_MS_ECAT_V2.5.xml
```

**不搬**现有 `aliasinfo/` `slide_verify/` `slide_motion/`：
README 与两份 docs 里有大量相对链接指向它们，搬迁只换来好看，
代价是所有文档链接失效、`git log` 追溯变麻烦。
要收就等阶段 4 之后用一次独立的 `git mv` 提交做（保留历史）。

---

## 3. 线程与所有权 (最关键的一节)

三条硬约束：

1. **SOEM 不是线程安全的**——所有 `ecx_*` 调用必须串行。
2. **网卡单进程独占**（Npcap）——不能同时跑两个持有网卡的进程。
3. **周期帧有硬时序**（手册: 同步周期 250µs~4000µs），而 **SDO 会阻塞**（SOEM 的
   `EC_TIMEOUTRXM` = 700ms）。

结论：**全程序只有一个 EtherCAT 线程**，它是唯一持有 `ecx_context`、
唯一调用 `ecx_*` 的地方。

```text
 GUI 线程 (Qt)                        EtherCAT 线程
 ─────────────                        ──────────────
 面板 / 模型 / 控件                     while (!stop):
    │                                     ① 写输出镜像 (来自快照)
    │ ① 命令入队 (cmd_queue)              ② ecx_send_processdata()
    │    arm / 写对象 / 切状态 / 微动      ③ ecx_receive_processdata()
    │                                    ④ 更新快照 (双缓冲换手)
    │ ② 30~60Hz QTimer 采样快照           ⑤ 抽执行一条命令队列里的命令
    │    → 刷新控件与曲线                     (SDO / 状态检查 / DC 配置)
    │ ③ 接收信号: 判定结果 / 退出码 / 告警
```

要点：

- **SDO 只在"帧与帧的间隙"执行**（步骤 ⑤），不要单开一个 SDO 线程去抢
  `ecx_context`。这就是 SOEM 单线程模型的正确用法。
- 运动期 SDO 超时压到 **200ms**（沿用 [sm.h](../slide_motion/sm.h) 里的
  `SM_SDO_TMO_MOTION`）；非运动期才用 700ms。否则故障后最长 700ms 才发出停机写。
- **不要每个周期 emit 信号**。1kHz × N 个信号会淹掉 Qt 事件循环。
  改为**一个双缓冲快照**（写满后原子换手指针，GUI 侧只读），
  曲线控件自己维护环形缓冲。这是 DSO/示波器类界面的标准做法。
- 停机必须**协作式**：`std::atomic<bool> stop_requested` + 有序收尾
  （撤防 → 写 6040h=0 → 回 PRE_OP → 关网卡）。**禁止 `QThread::terminate()`**，
  那会在任意指令处杀掉线程，可能留下带电的电机。

---

## 4. 护栏怎么从 CLI 迁到 GUI (安全不变量不许退化)

现有护栏是"命令行开关 + 上限只能收紧、放宽需 `--force-caps`"。
GUI 里逐条对应：

| CLI | GUI |
|---|---|
| `--allow-motion` / `--allow-jog` | `ArmController` 的 `armEnable` / `armMotion`，**必须双步确认，不是持久勾选框** |
| `--allow-pdo` | `armPdoMap`（写 PDO 映射，仅 RAM） |
| `--force-caps` | `armRelaxCaps`（放宽上限，二次确认 + 红色标识） |
| (无) | `armOdWrite`（SDO 写普通对象） |
| (无) | `armEepromWrite`（**2102h 永远不给一键按钮**，放最深菜单 + 二次确认 + 明确"掉电不可逆"） |

**自动撤防**（任一命中即撤防并走 guard teardown 写 6040h 回失能态）：

- 6041h 故障位 bit3 = 1
- 看门狗 / 丢站 / AL 掉出当前状态
- 硬件限位有效（bit11）
- 任何 SDO abort
- 周期抖动超阈值
- 面板切走 / 主窗口失焦 / 程序退出

**退出码契约**（[sm.h](../slide_motion/sm.h) 的 0~10）在 GUI 里变成每次动作的**结论条**：
同一套判定函数返回同一套码，界面**原样显示**"结果: EXIT 10 电机可能仍带电"，
不要翻译成模糊的红/绿。这样 CLI 与 GUI 的诊断能互相印证。

**退出码 10 在 GUI 里必须是模态告警**——收尾后 6041h 仍报 Operation enabled
意味着电机可能还带电，这是最严重的一类失败。

**不许在 GUI 里做"自动进 OP"的默认行为**：进 OP 就会推 6040h，等于给电机上电。
`--dc` / 自由运行也必须显式可见地切换（沿用 CLI 语义，**不要自动 fallback**）。

**不许在 GUI 里试探厂商私有控制字**：6040h 的推送值仍限 0x0000/0x0006/0x0007/0x000F
（外加故障复位 0x0080，且要显式授权）。私有控制字有真的动滑台的风险。

---

## 5. `BusBackend` 抽象：能否并行开发的前提

**现实障碍**：只有一台设备 + 网卡单进程独占 ⇒ 开着 GUI 就没法调试设备，
反之亦然。所以 `bus_backend.h` 从阶段 1 就要有两份实现：

| 实现 | 用途 |
|---|---|
| `LiveBus` | 真机：`ecx_init` → `config_init` → `config_map_group` |
| `ReplayBus` | 回放一次录好的会话（逐周期输入/输出镜像 + 邮箱往返 + AL 状态 + 时间戳） |

让 GUI 全流程（连接、状态机切换、进 OP、微动）都能对着录像开发与回归。
**这不是锦上添花的测试件，它是"能不能并行开发"的前提。**

同时它让**回归可复现**：2026-09-11 与 2026-09-15 那两次运行
（[slide_motion_verify.md](slide_motion_verify.md) §6 记录了 `AL 0x001E` 与
`6041h=0x0210`）如果能录成会话文件，今天就能拿来当回归基线。

---

## 6. 把手册变成数据 (`od/`)

`od/ykd2205pe_od.csv` 的列建议：

`对象 | 子索引 | 名称 | 类型 | 范围 | 默认值 | 单位 | 可写 | 危险级 | source | 实测值 | 备注`

- `source` 列写这条事实来自哪一版（`V1.0` / `V1.1` / `V2.3` / `V2.4` / `ESI` / `实测`）。
  这次更新文档时最费劲的就是"这条默认值到底以哪一版为准"——把它变成数据列，
  以后 GUI 里可以直接显示"这条的依据是 V2.4 YKD 附录，与 ESI 声明不一致"。
- `gen_od.py` 产出三样东西：`ecat_core` 的 C 表、Qt 的 `OdModel` 数据、
  **以及一次对 [ykd2205pe_ci402.md](ykd2205pe_ci402.md) 的交叉校验**
  （CSV 与文档表格不一致就报错）。防的是"文档改了、代码没改"这件事再发生一次。
- 手册里那些**自相矛盾**的值（6091h 分子/分母 10000/4000 vs 4000/10000；
  6502h 手册 `0x00A5` vs ESI `0x01DD`；2400h 默认 `-` vs 50000）不要静默择一，
  CSV 里两行都留、`source` 区分，GUI 显示冲突提示。

---

## 7. `cia402.h`：把这次的更正固化进代码

[ykd2205pe_ci402.md](ykd2205pe_ci402.md) 这次更正的核心是
**「6041h 低 4 位 == 0x0000」不是「未使能」的正确判据**：
手册定义了完整 8 态迁移表，上电自检完成后驱动器合法地停在 `0001`
（Ready to switch on，**电机释放**），此时写 `6040h=0x0000` 也仍是释放态。
正确的「未使能且无故障」判据是 **`(6041h & 0x000C) == 0`**（bit2 未使能、bit3 无故障）。

`cia402.h` 提供：

```c
/* 6040h / 6041h 全 16 位定义 (V2.4 p10/p11) */
enum { EC_CW_SWITCH_ON = 0x0001, EC_CW_ENABLE_VOLTAGE = 0x0002, ... };
enum { EC_SW_READY_TO_SWITCH_ON = 0x0001, EC_SW_SWITCHED_ON = 0x0002,
       EC_SW_OPERATION_ENABLED = 0x0004, EC_SW_FAULT = 0x0008,
       EC_SW_VOLTAGE_ENABLED = 0x0010, EC_SW_QUICK_STOP = 0x0020,
       EC_SW_SWITCH_ON_DISABLED = 0x0040, EC_SW_WARNING = 0x0080,
       EC_SW_REMOTE = 0x0200, ... };

ec_sw_state_t ec_sw_state(uint16_t sw);        /* 8 态枚举, 按 bit6/5/3/2/1/0 */
const char   *ec_sw_name_cn(uint16_t sw);      /* 中文名 */
bool          ec_sw_disabled_no_fault(uint16_t sw);  /* (sw & 0x000C) == 0 ★ */
bool          ec_sw_remote_ok(uint16_t sw);          /* bit9: 0 = 控制字不可操作 */
```

**规则：任何面板都不许自己写 `(sw & 0x000F) == 0` 这类判据，一律调这两个函数。**
状态灯控件（`safety/state_lamp.cpp`）是唯一画状态的地方。

> 判状态时以 **bit2 / bit3** 为准最稳：实测发现从 Ready to switch on 写 `0x0000`
> 后并未按标准迁到 Switch on disabled（仍停在 bit5=1/bit6=0），
> 本机 bit5 是否符合标准 CiA 402 **尚未确认**。详见文档「状态字 6041h」。

---

## 8. 过程数据镜像面板 (`panel_pdo_mirror`) —— 最该先做的面板

按这次的排查经验，卡住的地方全是"看不到原始字节"。这个面板要做：

- 输出/输入镜像的**逐字节 hex + bit 网格**，`Obytes`/`Ibytes`、`outputsWKC`/`inputsWKC` 实时显示。
- **按 `1600h`/`1A00h` 的实读映射做位级标注**：每个字段标出对象名、
  在镜像里的**推导偏移**、当前值、按 `cia402.h` 解出的语义名。
- 顶部**醒目显示推导出的偏移**（`6040h @ +0`、`6041h @ +0`），
  以及映射来源：**手册值 / ESI 声明值 / ESI 字典默认值 / 现场实读**四者不一致时给出警告。

理由：这次实测输出镜像 **23 字节**是奇数，与手册（2 B）、ESI 声明（16 B）、
ESI 字典默认（3 B）**都对不上且无法解释**。唯一可信的是实读映射。
把这个"只能实读、不能假设"的结论**做成界面**，比写在文档里有用。

同一面板还要显示 `1C12h`/`1C13h`/`1600h`/`1A00h` 的当前值，
并在写映射前把快照存下来（对应 CLI 的"默认还原"）。

---

## 9. Windows 不是实时系统 —— 抖动要摆在明面上

手册：同步周期 250~4000µs，`2217h 同步帧阈值` 默认 20。
Windows 上 DC 抖动超阈值会报同步帧错误（`test2 --dc` 那条 `WKC=-1`
很可能与此有关）。

`panel_jitter` 显示周期线程的**实际抖动**（最小/最大/P99/丢帧计数），
并与 `2217h` 阈值对比；默认周期建议 **≥1000µs**；超阈值时给出
"这是主机调度抖动，不是驱动器问题"的明确提示。

---

## 10. 多轴

总线可能挂 2 台（[aliasinfo/aliasinfo.c](../aliasinfo/aliasinfo.c) 的示例输出就是 2 台）。
**一个网卡 → 一个进程 → 一个周期线程驱动所有轴**，DC 是全局的，所以：

- 模型层第一天就要有 `Axis`（= 总线位置，0-based），
  面板按**当前选中轴**工作，**不要做成全局单例**。
- **动作类操作先只允许单轴**（其余轴保持失能），这与 CLI 现状一致。
- 面板标题栏始终显示「轴 0 / 轴 1」与各自的 AL 状态，避免在错误的轴上操作。

---

## 11. 构建与打包

- 顶层改成 `project(EC_CPP LANGUAGES C CXX)`（加 `CXX` 才能引 Qt）。
- **`option(EC_BUILD_HMI "构建 Qt 上位机" OFF)`，默认关** ——
  没装 Qt 的人仍然能构建 CLI，现有工作流与 CI 不受影响。
- ~~Qt 6 LTS + **MSVC 2022 x64**：`SOEM/oshw/win32/wpcap` 里是 MSVC 格式的 `.lib`，
  别在 HMI 上混 MinGW。~~
  **这条已被实现推翻**：实际用的是 **MSYS2 UCRT64 的 MinGW + `hmi-qt-ucrt64`
  preset（Ninja）**，`wpcap.lib`/`Packet.lib` 由 MinGW 直接链接，没有任何问题；
  真正的约束不是"别混 MinGW"，而是**界面用的 Qt 必须与 SOEM 同一套 CRT**
  （见 §0 末段与 [hmi_click_position.md](hmi_click_position.md) §6.1）。
  仍然保留的一半：用 `qt_standard_project_setup()` + `qt_add_executable`。
- 构建隔离：`CMakePresets.json` 加独立 preset（实际叫 **`hmi-qt-ucrt64`**），
  避免与现有 `build/`（VS）和 `build-mingw`（Ninja + MinGW）互相污染。
- 打包：`windeployqt` + Npcap 运行时（`wpcap.dll` / `Packet.dll`）+ 需管理员说明。
  GUI 启动时**自检网卡能否打开**，失败要给出 CLI 退出码 1 那种明确原因
  （网卡打不开 / Npcap 被独占 / 需管理员），不要只弹一句"连接失败"。
- **曲线控件的许可证坑**：Qt Widgets 是 LGPLv3，但 **Qt Charts 是 GPLv3**，
  QCustomPlot 也是 GPL。内部调试台无所谓；若将来要闭源交付，
  自己在 `QWidget::paintEvent` 里画趋势图（位置/速度 vs 时间）可完全绕开。

> **下面这段骨架里的 `target_link_libraries(... PRIVATE ... soem)` 是不能照抄的**
> —— SOEM 的 `target_compile_options(soem PUBLIC $<$<C_COMPILER_ID:GNU>:-std=c11>)`
> 没按语言设限，会经 PUBLIC 漏进 C++ 编译。实际写法是只引 `$<TARGET_FILE:soem>`
> 并自己抄一份 include 目录，理由见 [hmi_click_position.md](hmi_click_position.md) §6.2。

```cmake
# hmi/CMakeLists.txt 骨架 (源文件清单就是 §2 那棵树的叶子, 不在这里重复)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
set(CMAKE_AUTOUIC OFF)          # 无 .ui 文件 (AUTOMOC 必须留着)
qt_add_executable(hmi main.cpp <§2 各目录下的 .cpp>)
target_link_libraries(hmi PRIVATE Qt6::Widgets ecat_core $<TARGET_FILE:soem>)
target_compile_options(hmi PRIVATE
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra>
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3>)
```

---

## 12. 代码不变量与自动检查

把 CLI 里那条"只有 `sm_guard.c` 写"推广成三条，并加一个脚本在每次提交前跑：

```bash
# scripts/check_invariants.sh
# 1. 唯一写入口
grep -rn "ecx_SDOwrite" ecat_core/src | grep -v "guard.cpp" && exit 1
# 2. Qt 侧唯一持有 SOEM 的地方
grep -rln "soem/soem.h" hmi/ | grep -v "bus/bus_worker.cpp" && exit 1
# 3. 不许绕过 cia402.h 的位判据
grep -rn "0x000F\|0x000f" hmi/ panels/ 2>/dev/null && exit 1
```

> 第 3 条**会误伤一类合法写法**:`ecx_statecheck()` 自己就按 `0x000F` 掩状态,
> 所以"检查 AL 错误位"的正确写法必然要提到 `0x000F`
> (见 [ec_motor.c:1097](../motor_api/ec_motor.c#L1097) 那条注释)。
> 落地时要么把这条改成"至少有一个 `0x000C` 判据", 要么只对"拿 `0x000F` 去判
> 未使能"这一种用法报错, 不要按字面 grep。

---

## 13. 阶段划分与验收

| 阶段 | 内容 | 验收（可判定） |
|---|---|---|
| **P0** 抽 core（无 Qt，1~2 天） | 建 `ecat_core/` + `od/`；现有 5 个工具改链 core | **在真机上把 `slide_verify` / `sm_state` / `sm_pdo` 各跑一遍，输出与改造前逐字一致** |
| **P1** 只读骨架 | `hmi/` + 单线程模型 + `ReplayBus` + 连接面板 + 从站树 + **过程数据镜像** + 6041h 位灯 | 对录像跑通；对真机只读展示与 `sm_pdo` 无授权跑法输出一致 |
| **P2** 只读扩展 | 对象字典浏览器（读）+ PDO 映射面板 + 基线 diff | 对象字典显示值与 `baseline_ykd2205pe.ini` 实测列一致 |
| **P3** 动作 | 使能状态机 / 微动 / 回零，全部 arm + 上限 + 自动撤防 | 退出码 10 能触发模态告警；拔网线/按急停能自动撤防并失能 |
| **P4** 收尾 | 曲线、记录/回放、抖动面板、windeployqt 打包，（可选）工具移入 `tools/` | 打包后在另一台机器上能启动并连上设备 |

**P0 的纪律**：这一步**只许改结构，不许改行为**。
具体说：可以把 6041h 的正确判据收进 `cia402.h`，但
**不要在同一次提交里改任何断言的期望值** —— 那是一次独立的、需要单独验收的行为变更。
否则"回归输出逐字一致"这个唯一的验收手段就失效了。

---

## 14. 明确不做的事

1. **不自动进 OP**，不自动使能，不自动恢复上次的 arm 状态（保险每次启动都在未解除位）。
2. **不持久化 PDO 映射**：写 RAM 后还原；要保留必须显式勾选并提示掉电回出厂值。
3. **不给 2102h 一键按钮**（EEPROM，掉电不可逆）。
4. **不试探厂商私有控制字** —— 有真的动滑台的风险。
5. **不把 `sm_guard` 的单写入口拆开**：GUI 不是豁免者。
6. **不为了方便而假设出厂映射** —— 实读 `1C12h`/`1C13h`/`1600h`/`1A00h` 后才定偏移。
