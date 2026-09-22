# scan: 滑台蛇形扫描 + 逐点功率采集 (Qt Widgets)。
#
# 与 hmi/ 的关系: **同一套总线工作线程, 另一个主窗口**。
# hmi/ecatworker.cpp 原样编进本目标 (下面源文件列表里那两行) —— 不复制、不改写。
# 于是 2ms 的 CSP 插补循环、坐标/零点、全套安全护栏都只有一份实现。
#
# 界面侧**不 include SOEM**: 所有 ecx_* 都在 ecatworker.cpp 里。见 ecatworker.h 顶部那段。
#
# 两句啰嗦但必要的话与 hmi/CMakeLists.txt 里的一字不差 (为什么这么写在那儿有完整理由):
#   · 不写 target_link_libraries(... soem) 而只引 $<TARGET_FILE:soem>
#     —— soem 那条 PUBLIC 的 -std=c11 没按语言设限, 会漏进 C++ 编译
#   · 不链 motor_api 的库而直接编它的两个 .c —— 它目前没有库目标, 为这个去加一个
#     就超出"纯增量"的范围了, 代价只是这两个文件被编两遍

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# 没有 .ui 文件 (界面全是手写布局), 用不上 uic。AUTOMOC 必须留着。
set(CMAKE_AUTOUIC OFF)

# ---------------------------------------------------------------- 共用的那部分源文件
# 两个目标都要: 纯逻辑 + 状态机 + 模拟功率计 + CSV + 参数记忆。列成一个变量, 免得两处对不上。
#
# scanprefs **只链 Qt6::Core** (QSettings 在 Core 里) —— 这是它能进这个列表的前提:
# 自检那边能真跑一遍"存进去再读回来相等"。见 scanprefs.h 顶部那段。
#
# meterlog 同一条规矩: 它是「连续读数」的**全部时序**, 不含界面, 所以自检能用手拨的钟把
# "到点才发 / 未决期间不再发 / 超时记一笔并续采"整轮跑完。画曲线的那个控件 (metercurve)
# 不在这里 —— 它要 Qt6::Widgets。
#
# ophircom/ophirmeter 是 PD300R + Juno+ 那条真机路 (见 ophircom.h 顶部)。它们也在这里,
# 是因为自检要能回答"这台机器上 COM 对象注册了没有、能不能建出来、没插表头时能不能
# 干净地报错" —— 那正是**不需要硬件**的那部分。两个目标因此都要链 ole32/oleaut32。
set(SCAN_COMMON_SRC
  scanplan.cpp      scanplan.h
  scanarrive.cpp    scanarrive.h
  scancontroller.cpp scancontroller.h
  powermeter.cpp    powermeter.h
  ophircom.cpp      ophircom.h
  ophirmeter.cpp    ophirmeter.h
  meterlog.cpp      meterlog.h
  scanlog.cpp       scanlog.h
  scanprefs.cpp     scanprefs.h
  busview.h
  editgate.h
  axisutil.h
)

# ---------------------------------------------------------------- GUI 程序
qt_add_executable(scan
  main.cpp
  scanwindow.cpp    scanwindow.h
  mapcanvas.cpp     mapcanvas.h
  metercurve.cpp    metercurve.h
  ${SCAN_COMMON_SRC}
  ${CMAKE_SOURCE_DIR}/hmi/ecatworker.cpp
  ${CMAKE_SOURCE_DIR}/hmi/ecatworker.h
  ${CMAKE_SOURCE_DIR}/motor_api/ec_motor.c
  ${CMAKE_SOURCE_DIR}/motor_api/ec_motor_motion.c
)

# hmi/ 那个目录也要在 include 路径里 —— ecatworker.h 是被 #include "ecatworker.h" 引的
target_include_directories(scan PRIVATE
  ${CMAKE_SOURCE_DIR}/hmi
  ${CMAKE_SOURCE_DIR}/motor_api
  ${CMAKE_SOURCE_DIR}/SOEM/include
  ${CMAKE_BINARY_DIR}/SOEM/include
  ${CMAKE_SOURCE_DIR}/SOEM/osal
  ${CMAKE_SOURCE_DIR}/SOEM/osal/win32
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Include
)

target_link_libraries(scan PRIVATE
  Qt6::Widgets
  $<TARGET_FILE:soem>            # 带依赖: soem 会先编出来
  ucrt ws2_32 winmm
  ole32 oleaut32                 # ophircom.cpp 走 IDispatch: CoCreateInstance / SysAllocString ...
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Lib/x64/wpcap.lib
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Lib/x64/Packet.lib
)

# WIN32_EXECUTABLE=FALSE: **保留控制台窗口**, 与 hmi 一样 ——
# motor_api 的日志与扫描每次自动中止的原因都打在 stdout, 排查时两边都能看见。
set_target_properties(scan PROPERTIES
  WIN32_EXECUTABLE FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

target_compile_options(scan PRIVATE
  $<$<COMPILE_LANGUAGE:C>:$<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>>
  $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>>
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W3>
)

# ---------------------------------------------------------------- 自检 (不需要硬件, 也不需要界面)
#
# **这是本轮唯一能真正验扫描逻辑的东西**: 到位的时序、暂停/继续/中止、外部改目标、
# 续扫从哪接上 —— 全是时序问题, 而仓库里目前没有可用的真机回路。
# 所以状态机只认 BusView, 测试里换成 FakeBus (自己按速度推 pos, 能注入掉帧/故障/限位),
# 用一只手拨的时钟把整轮扫描跑完。
#
# 它**不链 SOEM 也不链 Widgets** —— 只用到 Qt6::Core, 于是几十秒就能跑完,
# 可以随时跑, 不必有设备、不必有网卡。
add_executable(scan_selftest
  selftest.cpp
  ${SCAN_COMMON_SRC}
)

target_include_directories(scan_selftest PRIVATE
  ${CMAKE_SOURCE_DIR}/hmi          # busview.h 里 include 了 ecatworker.h
  ${CMAKE_SOURCE_DIR}/motor_api
)

# ole32/oleaut32 是 ophircom.cpp 要的。自检里那条腿 (test_ophir) 会真去建一次 COM 对象 ——
# 没装 StarLab 的机器上它会 SKIP, 不是 FAIL。见 selftest.cpp 里的 skipCase()。
target_link_libraries(scan_selftest PRIVATE Qt6::Core ole32 oleaut32)

target_compile_options(scan_selftest PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W3>
)

# ---------------------------------------------------------------- 这条 -O1 是绕开一个工具链 bug
#
# **现象**: 本目标在 -O2 / -O3 下链接必然失败 —— `collect2: ld returned 5 exit status`
# (也有 1 / 9 / 116), ld 自己的报错一个字都没有, 输出文件被建出来但内容是**一片零**
# (459154~479044 字节不等, 每次不同)。`bin/scan.exe` 与 `bin/hmi.exe` 在同样的 -O3 下链接正常。
#
# **已排除**: 不是 selftest.cpp 里某个写法 —— 症状只跟优化级别有关:
#
#     -O0 -> 链接成功 (1031626 B, MZ)      -O2 -> 失败
#     -O1 -> 链接成功 ( 608314 B, MZ)      -O3 -> 失败
#
# 单独 `ld -r` 这一组目标文件是**成功**的, 只有完整链接才炸; `-ffunction-sections`、
# `-fno-asynchronous-unwind-tables`、`-fno-ipa-icf` 都试过, 没有一个管用。
# 所以这是 GCC 16.1.0 (MSYS2 UCRT64) 这一套 ld 自己的问题, 不是本仓库代码的问题。
#
# **为什么不干脆把整个 preset 降到 -O1**: 出厂的 bin/scan.exe 在 -O3 下编得好好的, 为测试
# 目标的毛病去动ship 出去的东西, 是拿芝麻换西瓜。而这是个**自检程序** —— 它只回答
# "逻辑对不对", 优化级别对它毫无意义。真机那边 (`bin/scan.exe`) 保持 -O3 不变。
#
# GCC/binutils 一升级就可以把下面这行删掉, 然后重跑一遍自检确认。
#
# **别把上面这个症状跟"输出文件被占用"混为一谈** —— 后者长得一模一样 (ld 一声不吭、退出码
# 非 0、输出文件没被写出来), 但原因是 `bin/scan.exe` 还在跑 (试跑完忘了关就会这样), Windows
# 不让写。**先 `tasklist | findstr scan.exe` 看一眼**, 有残留就杀掉再重链, 别急着改优化级别。
target_compile_options(scan_selftest PRIVATE $<$<CXX_COMPILER_ID:GNU>:-O1>)
