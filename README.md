# EC_CPP

面向研控 **YKD2205PE** EtherCAT 滑台驱动器 (CiA 402) 的原生 C / SOEM 工具集。

## 目录结构

| 路径 | 说明 |
|---|---|
| [SOEM/](SOEM/) | 下载的第三方 EtherCAT 主站库 (**Simple Open EtherCAT Master**),保持原样不改动 |
| [aliasinfo/](aliasinfo/) | 自有工具:读取各从站 ESC 0012h-0013h 站点别名 (拨码站号) |
| [slide_verify/](slide_verify/) | 自有工具:滑台设备导入验证 (扫描总线 → 读 CiA402 对象 → PASS/WARN/FAIL → 退出码) |
| [slide_motion/](slide_motion/) | 自有工具:**带动作**验收验证 (参数基线 → 使能状态机 → 微动与反馈闭环),**默认不动** |
| ↳ `sm_state` / `sm_pdo` | 同目录下的两个**独立**专项小程序: 只验证 CiA402 状态机切换。`sm_state` 用 **SDO + PRE_OP**, `sm_pdo` 用 **PDO + OP** (会写 PDO 映射, 见下) |
| [motor_api/](motor_api/) | 自有工具:**多轴 CiA402 运动接口** (位置同步 CSP / 速度 PV / 回零 HM) + 验收程序 `motor_test`。**至少支持同时驱动两台**, 目标值每周期经过程数据下发 |
| [hmi/](hmi/) | 自有工具:**Qt Widgets 上位机** —— 两台滑台 ±500000 脉冲的 **CSP 点击定位界面** (点哪里去哪里 / 开机位置即零点 / 速度可调)。**默认不编**, 见下 |
| [baseline_ykd2205pe.ini](baseline_ykd2205pe.ini) | 示例参数基线 (由 `slide_motion --dump-baseline` 现场导出后人工审定) |
| [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md) | YKD2205PE 对象速查与各工具的用法/退出码文档 |
| [docs/slide_motion_verify.md](docs/slide_motion_verify.md) | `slide_motion` 的完整设计/安全须知/微动判据/实测记录 |
| [docs/hmi_click_position.md](docs/hmi_click_position.md) | `hmi` (Qt 上位机) 的完整设计: 线程模型 / 坐标与零点 / CSP 插补 / 护栏 / 构建部署的坑 / 验收状态 |
| [YKD2205PE.pdf](YKD2205PE.pdf) | 厂商手册 (参考) |

顶层 `CMakeLists.txt` 通过 `add_subdirectory(SOEM)` 引用 SOEM 库,
再 `add_subdirectory` 各自有工具 —— SOEM 只以子工程身份产出 `soem` 静态库,
不构建其自带 samples,源码不被改动。

`hmi/` 由一个开关控制: `-DEC_BUILD_HMI=ON` 才会被 `add_subdirectory`。**默认 OFF**,
所以没装 Qt 的机器照旧能编上面那些 CLI (那几条构建连 C++ 编译器都不需要有)。
界面侧**不 include SOEM**、不调 `ecx_*`: 所有总线操作都在工作线程里, 见
[hmi/ecatworker.h](hmi/ecatworker.h) 顶部。

## 构建

本机 (MSYS2 UCRT64 MinGW) 示例;VS/CMake 环境把 `build-mingw` 换成 `build` 即可:

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
cmake --build build-mingw --target slide_verify aliasinfo slide_motion sm_state sm_pdo motor_test
```

产物 (统一输出到仓库根的 `bin/`):
- `bin/slide_verify.exe`
- `bin/aliasinfo.exe`
- `bin/slide_motion.exe`
- `bin/sm_state.exe`
- `bin/sm_pdo.exe`
- `bin/motor_test.exe`
- `bin/hmi.exe` (仅当 `EC_BUILD_HMI=ON`)

### 上位机 `hmi` (Qt)

完整设计、部署细节与**哪些验收做了 / 哪些还没做**见
[docs/hmi_click_position.md](docs/hmi_click_position.md)。

只走仓库自带的 [CMakePresets.json](CMakePresets.json) 里的 `hmi-qt-ucrt64`
(**生成器是 Ninja, 与上面那条 MinGW Makefiles 的构建目录不能混用**):

```bash
cmake --preset hmi-qt-ucrt64
cmake --build out/build/hmi-qt-ucrt64 --target hmi
```

Qt 必须取自 **MSYS2 的 UCRT64 仓库**, 与上面那些 CLI 用**同一个** `C:/msys64/ucrt64` 工具链:

```bash
MSYSTEM=UCRT64 pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base
```

> **不要用 Qt 官方安装器那个 MinGW 版 Qt。** 它自带的是 **msvcrt** 版 MinGW (没有
> `_UCRT`), 而 SOEM 的 `SOEM/cmake/Windows.cmake` 给 GNU 编译器加了 `-D_UCRT -lucrt`,
> 且 `SOEM/osal/win32/osal.c` 确实要用它 (`timespec_get`/`TIME_UTC` 在该工具链的
> `time.h` 里只有 `_UCRT` 打开时才有声明, 没有 msvcrt 退路)。于是进程里会同时存在
> 两套 CRT 的 stdio 表: 一个 CRT 建出来的 `FILE*` 被另一个 CRT 去 `_lock_file` ——
> **工作线程第一次 `printf` 就 SIGSEGV 在 `msvcrt!_lock`**。SOEM 是下载库不能改,
> 所以只能换 Qt。详见 [CMakePresets.json](CMakePresets.json) 里那条 preset 的说明。

生成器要在**带 `C:/msys64/ucrt64/bin` 的 PATH** 下跑 (moc 等 Qt 工具自身要加载那儿的
DLL, 否则 configure 就报 `AUTOUIC ... Exit code 0xc0000139`; MSYS2 的 UCRT64 shell
本来就是这个 PATH)。项目没有 `.ui` 文件, 已显式关掉 AUTOUIC。

跑之前用 MSYS2 的 `windeployqt6` 把 Qt 运行时搬到 `bin/`:

```bash
C:/msys64/ucrt64/bin/windeployqt6.exe --release --compiler-runtime \
    --no-translations bin/hmi.exe
```

> MSYS2 的 `windeployqt` **只搬 Qt 自己的 DLL**, 会把 MSYS2 运行时 (`libstdc++-6.dll`/
> `libgcc_s_seh-1.dll`/`libicu*.dll`/`zlib1.dll` …) 当成"PATH 里已有"而跳过 ——
> 双击 `bin\hmi.exe` 会报 `0xc0000139` (缺入口点)。这些得从 `C:/msys64/ucrt64/bin`
> 补拷进 `bin/`。`--no-translations` 是必须的: 否则会多出几十个
> `bin/translations/*.qm`, 而 `.gitignore` 只忽略 `*.dll`/`*.exe`。

## 运行注意

- `aliasinfo` 与 `slide_verify` 是**只读**的 (不进 OP、不写 6040h 控制字、不动电机)。
- `slide_motion` **默认也是只读的**: 不给 `--allow-motion` 时不写一个字节。
  只有显式给 `--allow-motion --allow-jog` 并再过一次交互确认, 它才会让电机通电
  并移动滑台 —— 真动时**必须有人在设备旁、手放在物理急停上**。
  它永远不写 `2102h` (EEPROM)、不改 PDO 映射、不写软限位。
  退出码 **10** = 收尾写完 `6040h=0` 但回读 `6041h` 仍报 Operation enabled,
  即**电机可能仍带电** —— 它覆盖其它所有退出码, 见到请立即断电确认。
- `sm_state` / `sm_pdo` 是 CiA402 状态机切换的**专项验证**小程序, 同样**默认只读**:
  - `sm_state` 只写 `6040h` (SDO, 在 PRE_OP 下)。
  - `sm_pdo` 用 **PDO + OP** 走同样 8 步 —— 为此它**会写 PDO 映射对象**
    `1A00h`/`1C13h` (出厂 TxPDO 为空, 不补写就进不了 SAFE_OP)。**只写 RAM,
    从不写 `2102h` (EEPROM)**; 默认跑完**还原**成运行前快照 (`--keep-mapping` 才保留,
    `--no-map` 整个跳过)。需要 `--allow-pdo` 才写; 默认只打印 PDO 快照。
  - 两者都发**零运动指令**、不写 `607Ah`/`6060h`/软限位。但走到 Enable Operation
    之后电机会通电 (有保持力矩) —— **真跑时人在设备旁, 手放在物理急停上**。
  - 退出码 **10** 的含义与 `slide_motion` 相同: 收尾未能确认失能, **电机可能仍带电**。
- `motor_api` / `motor_test` 是**多轴运动接口** (CSP 位置同步 / PV 速度 / HM 回零),
  与 `slide_motion` 的 SDO + PP 并列, 区别在于**目标值每周期经过程数据下发** ——
  CSP/PV 要求目标值每周期刷新, SDO 的 700 ms 往返追不上。同样**默认一个字节都不写**:
  写 PDO 映射要 `--allow-pdo`, 使能/运动要 `--allow-motion`, 回零要 `--home`
  (它会撞限位、会找原点开关)。用 `--mode csp|pv` 选 S6 跑位置同步还是速度模式。
  **先不带任何 `--allow` 参数跑一次**: 它读完总线、打印实读的 PDO 映射与只读参数后
  就退出 0, 一个字节都不写。退出码 **10** 同理。
  > **`--allow-motion` 本身就是那句确认, 运行时不再问一次 y/N** —— 别在脚本里顺手
  > 加上它。(`slide_motion` 有交互确认, 这个工具没有。)
  > 它**不写死** `1600h`/`1A00h`, 而是先读 `1C12h`/`1C13h` 问"哪个 PDO 生效" ——
  > 本机 `1C12h` 指的是 **`1601h`**, `1600h` 是一张**没生效**的表。
  > 同理 `6060h`(运行模式) 就在 `1601h` 里, 所以它由过程数据驱动而不是 SDO ——
  > 对它做 SDO 写会被下一帧撤销。
  > **这条规矩适用于表里每一项**: `6081h`/`6083h`/`6084h` 也在 `1601h` 里, 主站每
  > 周期都在下发它们 —— 不驱动就是下发 0。`6083h` 被下发成 0 会让 PV 的斜坡永远
  > 起不来(现象: 报速度指令已被接受、却一步不走), 所以加减速度现在由接口下发,
  > 可用 `--ramp-acc`/`--ramp-dec` 指定。详见
  > [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md) 的「PDO 映射」。
- `hmi` (Qt 上位机) 与 CLI **同一套护栏**, 只是把 `--allow-*` 换成了按钮:
  - **启动后一个字节都不写**。按钮形态完全由总线遥测推出来, 不是点一下就先变的样子。
  - 顶栏「连接」= `em_open` + `em_setup` + 进 OP, **开始每 2ms 发帧**, 并会覆盖生效 RxPDO
    里主站拥有的那些项 —— **但这一步不发使能, 电机不带电**。点它会先弹一个说清这些的确认框。
  - **「使能」是唯一让电机带电的按钮**, 每次点击都弹模态确认 (要求"人在设备旁、手放在
    物理急停上") —— 这就是 CLI 的 `--allow-motion`。**没有持久勾选、不自动使能**。
  - 「停止」= 目标冻在当前位置并**保持保持力矩** (不卸力); 「失能」才是回失能态。
    `6041h` bit3 报故障 → 自动冻结目标 + 红色横幅。
  - 收尾 (断开 / 关窗) = 失能 → **还原 PDO 映射** → 降 `PRE_OP` → 关网卡。收尾时若
    **未能确认所有轴失能**, 会弹模态告警 (= CLI 退出码 **10** 的语义) —— 见到请立即
    断掉驱动器动力电源, 不要只依赖软件。
  - 界面坐标 ≠ `6064h`: 界面显示的是 `6064h − 软件零点`, 而**零点就是「连接」那一刻
    读到的位置**。所以开机 (连接后) 滑台显示在正中、且**一步不走**; 另有「设为 0」按钮
    就地重设零点 (它只挪软件零点, 不动驱动器的目标值)。
  - 行程 ±500000 脉冲 (50000 pul/圈 ⇒ ±10 圈)。点画布即走 —— **点远处就是一次长距离
    移动**, 点越界会被夹在 ±500000 上并在画布上标出来。
  - 速度每轴一个滑块, 1000~100000 pul/s, 默认 20000。界面侧自己按实测 `dt` 做梯形插补
    (进近段自动减速), 不写 `6081h`, 也不走 `em_csp_move_multi`。
  - 它**不写** `2102h` (EEPROM)、**不动** `607Dh` 软限位、**不改** `2400h/2408h/2409h/2201h`。
- 全部工具都依赖 **Npcap** 独占网卡 —— 与仓库的 python 链 (pysoem) 一样,
  **勿同时运行**。界面开着时同样独占: 别在它连着的时候再跑 CLI。
- 参数为网卡名 (Windows Npcap 形如 `\Device\NPF_{GUID}`),不带参数时列出可用网卡。
- 详细用法、输出示例与退出码见 [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md)
  (含 `sm_state`/`sm_pdo` 的选项与退出码表);
  `slide_motion` 另见 [docs/slide_motion_verify.md](docs/slide_motion_verify.md)。
