# hmi:两台滑台的 ±500000 脉冲 CSP 点击定位界面

`hmi/` 是一个 **Qt Widgets 上位机**:两根滑台,在 **±500000 脉冲**内,**CSP 模式**,
**点画布哪里就去哪里**,**开机(连接后)显示在正中且一步不走**,**速度可调**。

它是 [motor_api/](../motor_api/) 的调用方,不是它的替代:不重写底层、不碰 SOEM、
不加第三套总线初始化。定位与取舍见 [qt_hmi_layout.md](qt_hmi_layout.md) §0。

---

## 1. 目录与文件

| 文件 | 内容 |
|---|---|
| [hmi/ecatworker.h](../hmi/ecatworker.h) / [.cpp](../hmi/ecatworker.cpp) | **全程序唯一持有 `em_bus_t`、唯一调总线相关 `motor_api` 的地方**;跑在一个 `QThread` 里 |
| [hmi/axispanel.h](../hmi/axispanel.h) / [.cpp](../hmi/axispanel.cpp) | 一根轴的面板:自绘画布(`paintEvent`)+ 速度滑块 + 读数 |
| [hmi/mainwindow.h](../hmi/mainwindow.h) / [.cpp](../hmi/mainwindow.cpp) | 顶栏(网卡/连接/使能/停止/失能/回中)+ 若干 `AxisPanel` + 状态栏 |
| [hmi/main.cpp](../hmi/main.cpp) | `em_console_init()` + `QApplication` + 字体/夜间配色 |
| [hmi/CMakeLists.txt](../hmi/CMakeLists.txt) | 独立构建;默认不编 |

画布**不用 Qt Charts / QCustomPlot** —— 两者都是 GPL,自绘反而更短。

## 2. 线程模型

一个 `QThread` 跑一个自由循环,**它是唯一碰总线的地方**。界面线程一个 `ecx_*` 都不调,
也不 include SOEM 的任何头文件 —— 界面侧与总线侧之间只有 `post*` / `set*` 命令
与 `telemetry()` 快照。这样"`ecx_*` 必须串行"与"网卡单进程独占"两条约束是**结构上**成立的,
不靠调用纪律。

```
while (!quit):
  drainCommands()                     // 串行执行排队的低频命令 (SDO / 状态机迁移)
  if (已进 OP):
      if (零点已定) interpolate(dt)   // 逐轴算这一步该下发到哪
      wkc = em_service(bus)           // ★ 一帧, 绝不能断: 断了驱动器的 SM 看门狗会踢它出 OP
      if (零点未定) tryInitOrigin()
  publish(wkc)                        // 写遥测快照 (没进 OP 也要写, 界面才不会僵在旧值)
  em_sleep_ms(2)
teardown()
```

与最初设想的一处偏离:原计划用 `QCoreApplication::processEvents()` 把命令"插"进循环,
实际改成了**自己排一个命令队列,在 `drainCommands()` 里串行执行** —— 不引入嵌套事件循环,
命令之间的顺序也因此是确定的。

遥测是**快照 + `QMutex`**:界面 33 ms 的 `QTimer` 读它刷控件,**不逐周期 emit 信号**
(1kHz × N 根轴会淹掉事件循环)。按钮形态完全由遥测的 `in_op || busy` 推出来,
**不是点一下就先变的样子** —— 连接失败时按钮不会错写成「断开」。

## 3. 坐标与零点

两个坐标系,别混:

- **驱动器坐标** = `6064h` 原始值。
- **界面坐标** = `6064h − 软件零点`。**对外只讲界面坐标。**

**零点就是「连接」那一刻读到的位置**:等到所有轴的镜像(过程数据)都落地后,取
`origin[i] = em_pos(ax[i])`、`tgt = want = 0`。所以连接完滑台显示在正中,
**启动一步不走**(这是"开机默认在中间"的实现方式 —— 不是走到中间,而是把当前点定义成中间)。

「设为 0」按钮就地重设零点:`origin += 当前显示值`,同时把 `tgt`/`want` 减去同一个量。
**物理目标值一个脉冲都不变**,所以它不会让电机动。

范围 ±500000 脉冲(50000 pul/圈 ⇒ ±10 圈)。点越界会被**夹住**(不拒绝),
并在画布上标出"已到边界"。

## 4. CSP 插补

CSP 下**规划责任在主站**(手册:「循环同步位置模式下，控制器完成位置指令规划并输出
规划好的目标位置 `607Ah`…驱动器内部完成位置、速度控制」), 所以这里的形状**就是**
电机的形状。界面侧按**实测** `dt` 推进目标(Windows 不是实时系统,不假设 2 ms):

```
d    = want - tgt
a    = max(vel, 1000) / 0.3          // 300ms 刹停的减速度 —— **只用于减速**
v_ok = min(vel, sqrt(2*a*|d|))       // 进近段自动减速, 不冲过头
step = v_ok * dt / 1000              // 至少 1 个脉冲, 免得无限逼近
tgt += clamp(d, -step, +step)
em_csp_set_target(ax, origin + tgt)  // 每周期一次 —— 这就是 CSP
```

**`a` 只出现在减速包络里, 加速段没有任何包络** —— 点击那一刻 `|d|` 很大,
`allow >> vel`, 于是 `v` 直接取滑块满速, 第一个周期就全速。所以实际形状是
**「速度阶跃加速 → 匀速 → 恒定减速度减速」**, **不是梯形**(真梯形要求加速段也受限;
`HMI_STOP_MS` 顾名思义是"刹停时间", 只约束减速)。电机会不会因为这记阶跃而丢步/过流,
取决于驱动器的电流限幅 —— 这是**没有规划**的结果, 不是留了余量。

`dt > 100ms` 时按 100ms 推进(与 `em_csp_move_multi` 同一口径:卡一下就跳一大步
等于一次高速冲刺)。**不做** S 曲线、不做前瞻 —— 这是调试台,不是运动控制器。
也**不做**加速段限制(见上)。不写 `6081h`,不走 `em_csp_move_multi`。

> 想真梯形: 在 [ecatworker.cpp:516](../hmi/ecatworker.cpp#L516) 那段给加速段加一条对称的
> `sqrt(2*a_acc*|已走距离|)` 包络即可。要 S 曲线得再有加加速度限制 —— 那是另一个量级的改动,
> 已明确不做。改加速段会改变"点远处多久到"的手感, 改完要按 §8 重跑一遍手动验收。

## 5. 安全护栏(与 CLI 一一对应)

| 界面动作 | 等价 CLI | 干什么 |
|---|---|---|
| **连接** | `--allow-pdo` 起的那一步 | `em_open` → `em_setup(...,allow_remap=1)` → `em_enter_op`。**进 OP 并开始每 2ms 发帧**,会覆盖生效 RxPDO 里主站拥有的项 —— **但不发使能,电机不带电**。点它先弹一个说清这些的确认框 |
| **使能** | `--allow-motion` | 唯一让电机带电的按钮。逐轴 `em_set_mode(CSP)` → `em_enable_all`,随后**把目标钉回当前位置**,所以使能那一帧不会动。**每次点击都弹模态确认**(要求"人在设备旁、手放在物理急停上"),**没有持久勾选、不自动使能** |
| **停止** | — | 目标冻在当前位置并**保持保持力矩**(不卸力)。故意不写 `6040h=0x0000` |
| **失能** | — | 回失能态,电机释放(滑台可能因自重下滑) |
| **全部回中** | — | 两轴都去界面坐标 0 |
| 收尾(断开/关窗) | 退出码 10 的语义 | 失能 → **还原 PDO 映射** → 降 `PRE_OP` → 关网卡 |

另有一条:**启动后一个字节都不写**;`6041h` bit3 报故障时在上升沿冻结目标 + 红色横幅。

收尾时若**未能确认所有轴失能**(`em_shutdown(bus, 1, &live)` 的 `live`),
弹**模态**告警要求立即断驱动器动力电源 —— 软件已经没有通道去撤力矩了。

同样**不写** `2102h` (EEPROM)、**不动** `607Dh` 软限位、**不改** `2400h/2408h/2409h/2201h`。

## 6. 构建与部署

### 6.0 前置条件(每台新机器做一次)

`cmake --preset hmi-qt-ucrt64` **不会**帮你装任何东西。缺一样就停在哪一样上,
报错信息往往指不到真正的原因(实测: UCRT64 里没装 Qt 时, 报的是
`Could not find a package configuration file provided by "Qt6"` 加上第二条命令的
`ninja: error: loading 'build.ninja'`)。所以**先按这张表逐项自检**:

| # | 要有的东西 | 自检(在 MSYS2 UCRT64 shell 里跑) | 没有怎么装 |
|---|---|---|---|
| 1 | MSYS2 的 UCRT64 工具链 | `C:/msys64/ucrt64/bin/gcc.exe --version` | 装 MSYS2, 别用别的 MinGW |
| 2 | **ucrt64 版 Qt6** | `ls C:/msys64/ucrt64/lib/cmake/Qt6/Qt6Config.cmake` | `MSYSTEM=UCRT64 pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base` |
| 3 | `moc`(Qt 元对象编译器) | `ls C:/msys64/ucrt64/share/qt6/bin/moc.exe` | 同上, 由 2 带出来 |
| 4 | **`cmake` 本身** | `cmake --version` | `MSYSTEM=UCRT64 pacman -S --needed mingw-w64-ucrt-x86_64-cmake` |
| 5 | **Ninja**(preset 指到 `C:/Qt/Tools/Ninja/ninja.exe`) | `ls C:/Qt/Tools/Ninja/ninja.exe` | `MSYSTEM=UCRT64 pacman -S --needed mingw-w64-ucrt-x86_64-ninja`, 再把 preset 里那条 `CMAKE_MAKE_PROGRAM` 改指过去 |
| 6 | PATH 里有 `C:/msys64/ucrt64/bin` | `which gcc` | 用 UCRT64 shell(它的 PATH 本来就是这个), 见 6.2 |

**第 4 项容易漏**: MSYS2 **默认不带 `cmake`**。没有它, 本文件下面那条
`cmake --preset hmi-qt-ucrt64` 在 UCRT64 shell 里直接是 `command not found` ——
而报错完全指不到"缺前置条件"。装完 4、5 两项之后, 本节所有命令都能在 UCRT64 shell 里
原样照抄, 不必再去借 VS 自带的那个 `cmake`
(`.../Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`,
能用来生成/驱动 Ninja, 但没装 4、5 两项时它是唯一能敲的 `cmake`, 容易让人以为
"MSYS2 这条路走不通")。

**一条命令自检 1-5**:

```bash
for f in C:/msys64/ucrt64/bin/gcc.exe C:/msys64/ucrt64/lib/cmake/Qt6/Qt6Config.cmake \
         C:/msys64/ucrt64/share/qt6/bin/moc.exe C:/Qt/Tools/Ninja/ninja.exe; do
  [ -e "$f" ] && echo "OK   $f" || echo "MISS $f"
done
cmake --version >/dev/null 2>&1 && echo "OK   cmake" \
  || echo "MISS cmake (pacman -S mingw-w64-ucrt-x86_64-cmake)"
```

> **`moc.exe` 不在 `ucrt64/bin` 下, 在 `ucrt64/share/qt6/bin/` 下** —— MSYS2 把 Qt 的
> 工具装在那儿, 而 `ucrt64/bin` 里放的是 `Qt6Core.dll` 这些。两者的关系是:
> CMake 用**绝对路径**找到 `moc.exe`(所以第 3 项不要求它在 PATH 里), 但 `moc.exe`
> 启动时要从磁盘加载 `ucrt64/bin/Qt6Core.dll` —— 这就是第 6 项存在的原因。
> 所以**别拿 `ucrt64/bin/moc.exe` 当自检条件**: 它永远不存在, 那是一条假 MISS。

五个 `OK` 才继续。装完 2 之后再
`pacman -Qq | grep -i qt` 确认能列出 `mingw-w64-ucrt-x86_64-qt6-base`。

> **别拿 `C:/Qt` 那份 Qt 顶上第 2 项。** 那是 Qt 官方安装器的 msvcrt 版 MinGW Qt,
> 用它编译能过、窗口也能起, 但工作线程第一次 `printf` 就崩 —— 原因见 6.1。
> 换句话说, 这一项"缺了乱补"比"缺着不补"更危险。

前置条件齐了才跑:

```bash
cmake --preset hmi-qt-ucrt64
cmake --build out/build/hmi-qt-ucrt64 --target hmi
```

只走 [CMakePresets.json](../CMakePresets.json) 里的 `hmi-qt-ucrt64`(Ninja,
编译器与其它 CLI 同一个 `C:/msys64/ucrt64`)。顶层 `EC_BUILD_HMI` **默认 OFF**,
所以没装 Qt 的机器照旧能编那些 CLI。
configure 失败时 `out/build/hmi-qt-ucrt64/` 里不会有 `build.ninja`,
**第二条命令报 `ninja: error: loading 'build.ninja'` 永远是连带的** —— 去修第一条的错。

### 6.1 Qt 必须与 SOEM 同一套 CRT —— 这是本次最大的坑

Qt **只能**取 MSYS2 UCRT64 仓库那份:

```bash
MSYSTEM=UCRT64 pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base
```

**不能用 Qt 官方安装器那个 MinGW 版 Qt。** 它自带的是 **msvcrt** 版 MinGW 13.1.0
(`gcc -dM` 里没有 `_UCRT`),而:

- `SOEM/cmake/Windows.cmake` 给 GNU 编译器加了 `-D_UCRT -lucrt`;
- `SOEM/osal/win32/osal.c` 确实需要它 —— 该工具链的 `time.h` 里
  `timespec_get`/`TIME_UTC` **只有 `_UCRT` 打开时才有声明**,且内联调用的是 UCRT 的
  `_timespec64_get`,没有 msvcrt 退路。

把 `libucrt.a` 链进一个 msvcrt 工具链的程序,就等于让**两套 CRT 的 stdio 表**并存:
一个 CRT 建出来的 `FILE*` 被另一个 CRT 去 `_lock_file` —— 现象是
**工作线程第一次 `printf` 就 SIGSEGV 在 `msvcrt!_lock`**。
主线程往往能侥幸跑过,所以**别用"窗口起得来"判它通过**。

SOEM 是下载的第三方库,一个字不改,所以只能换 Qt。

### 6.2 两个构建期的坑

- **生成器要在带 `C:/msys64/ucrt64/bin` 的 PATH 下跑。** `moc` 这类 Qt 工具自身要加载
  那儿的 DLL,否则 configure 阶段就报 `AUTOUIC ... Exit code 0xc0000139`
  (= `STATUS_ENTRYPOINT_NOT_FOUND`)。MSYS2 的 UCRT64 shell 本来就是这个 PATH。
  本项目没有 `.ui` 文件,已显式 `set(CMAKE_AUTOUIC OFF)`(AUTOMOC 必须留着)。
- **不能写 `target_link_libraries(hmi PRIVATE soem)`。** SOEM 的
  `target_compile_options(soem PUBLIC $<$<C_COMPILER_ID:GNU>:-std=c11>)` **没按语言设限**,
  会经 PUBLIC 的 usage requirements 漏进 C++ 编译且**排在 `-std=gnu++17` 后面** ——
  实测 C++ 文件被当 C11 编,每条命令都报一条告警。所以 hmi 的 CMakeLists 里把 SOEM 的
  include 目录与链接项**照抄一份**、只引 `$<TARGET_FILE:soem>`。

### 6.3 部署

```bash
C:/msys64/ucrt64/bin/windeployqt6.exe --release --compiler-runtime \
    --no-translations bin/hmi.exe
```

- MSYS2 的 `windeployqt` **只搬 Qt 自己的 DLL**,会把 MSYS2 运行时
  (`libstdc++-6.dll`/`libgcc_s_seh-1.dll`/`libicu*.dll`/`zlib1.dll`/`libfreetype-6.dll`
  … 共 **23 个**)当成"PATH 里已有"而跳过 —— 双击会报 `0xc0000139`。这些得从
  `C:/msys64/ucrt64/bin` 补拷进 `bin/`。
- `--no-translations` 是必须的,否则会多出几十个 `bin/translations/*.qm`,
  而 `.gitignore` 只忽略 `*.dll`/`*.exe`。

`bin/` 下的 Qt DLL/插件目录全部命中 `.gitignore`,不入库。

## 7. 本次改到的东西

- **`motor_api` 只加不改**(两个纯增量函数):
  - `em_csp_set_target(ax, target)` —— CSP 的本来面目就是"每周期下发一次 `607Ah`"。
    **纯镜像写: 不发帧、不做 SDO**,调用方负责确认"已使能且 `6061h == CSP"`。
  - `em_list_adapters(out, max)` —— 界面要有网卡下拉框。`em_print_adapters()`
    改成调它再打印,**输出逐字不变**。
- 顶层 `CMakeLists.txt` 加 `option(EC_BUILD_HMI ... OFF)` + `add_subdirectory(hmi)`。
- `CMakePresets.json` 加 `hmi-qt-ucrt64`。
- 文档: README 的 `hmi/` 行与「上位机 `hmi` (Qt)」/「运行注意」两节、
  `qt_hmi_layout.md` §0 现状。

## 8. 验收状态

**已验证(可复现):**

- 构建 **0 warning 0 error**;`grep -rn "soem.h\|ecx_" hmi/` 只命中注释,
  **没有一处 include 或调用**;`grep -rn ecx_SDOwrite motor_api/` 仍只有 `ec_motor.c`。
- `bin/hmi.exe` 起来后**窗口正常、有响应**,中文标题不乱码;
  工作线程打印出 9 块网卡(含 `Realtek PCIe GbE`)—— **这行 `printf` 正是以前崩在
  `msvcrt!_lock` 的那一处**,现在过了。
- **把 PATH 砍到只有 `C:\Windows\System32;C:\Windows` 再启动也正常** ——
  即双击可用,不依赖 MSYS2 环境。
- 样式表解析无告警(`main.cpp` 里那段 raw string 是 CRLF)。
- CLI 未被碰坏:`motor_test.exe` 不带任何 `--allow*` 跑通,**退出码 0**,
  实读 `1C12h→1600h`(5 项)/ `1C13h→1A00h`(5 项)、`6064h = 0`、`6502h = 0xA5`。
- 排查期间被 Qt 工具链覆盖的 `bin/aliasinfo.exe` 与 `bin/motor_test.exe`
  **已用原 MSYS2 preset 重编回去**(现全部只 import `api-ms-win-crt-*`,不再有 `msvcrt.dll`)。
- 全部改动文件的 `git diff --stat` == `git diff --ignore-cr-at-eol --stat`(无 EOL 噪声);
  `hmi/` 全部 CRLF。

**尚未验证(需要人在设备旁、手放在物理急停上):**

- **界面的「连接」这条路一次都没跑过**(我没法点按钮)。也就是说
  `em_setup(allow_remap=1)` + `em_enter_op` + 零点初始化这段只有静态检查,
  **没有实机走过**。第一次跑请盯着控制台:选轴 / 补映射 / 进 OP 的记录会打在那儿。
- 「使能」与任何动作、速度滑块、回中、停止、失能、两轴独立性、±500000 边界 —— 全部待实测。
  建议第一次点一个离当前位置 **±20000 以内**的坐标看方向对不对。
- 界面开着时 Npcap 被它独占,**同时不要再跑 CLI**。

## 9. 本次不做

- 不抽 `ecat_core`、不做对象字典浏览器 / 基线 diff / 抖动面板 / 报文回放
  —— 那些是 [qt_hmi_layout.md](qt_hmi_layout.md) 的 P0~P4。
- 不做回零界面:零点靠软件零点,不写驱动器。
- 不碰排查到一半的那个 S5 `WKC=1` 路径(用户已明确停止)。
- 不做 PDO 映射修改/还原的界面,固定走 `em_setup(...,1)` + `em_shutdown(...,1)`。
