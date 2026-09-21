# hmi: Qt Widgets 上位机 —— 两台滑台的 ±500000 脉冲 CSP 点击定位台。
#
# 默认不编 (顶层 EC_BUILD_HMI=OFF) —— 现在 OFF 等于只编 SOEM 静态库 (2026-09-21 起
# 仓库里没有 CLI 程序了)。所以 enable_language(CXX) 放在这里而不是顶层 project() 里:
# 只验 SOEM 能不能编过的人不该被要求有 C++ 编译器。
#
# 界面侧**不 include SOEM**: 所有 ecx_* 都在 ecatworker.cpp 里。见 ecatworker.h 顶部那段。
#
# 为什么直接编 motor_api 的两个 .c 而不是链一个库: motor_api/ 现在是**纯源码目录**
# (2026-09-21 起没有 CMakeLists, 也没有构建目标)。两个 .c 由 hmi 与 scan 各自直接编进去。
# 为一个只有两个 .c 的目录起库目标, 收益比代价小。代价只是这两个文件被编两遍。

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# 没有 .ui 文件 (界面全是手写布局), 用不上 uic, 关掉。
# 顺带一个实际好处: MSYS2 的 uic.exe 在 configure 阶段就会被试跑一次, 而它依赖
# ucrt64/bin 里的 Qt6Core.dll —— 关掉就不必为了它把整个 MSYS2 bin 塞进 PATH。
# AUTOMOC 必须留着 (三个类都有 Q_OBJECT)。
set(CMAKE_AUTOUIC OFF)

qt_add_executable(hmi
  main.cpp
  mainwindow.cpp   mainwindow.h
  axispanel.cpp    axispanel.h
  ecatworker.cpp   ecatworker.h
  ${CMAKE_SOURCE_DIR}/motor_api/ec_motor.c
  ${CMAKE_SOURCE_DIR}/motor_api/ec_motor_motion.c
)

target_include_directories(hmi PRIVATE ${CMAKE_SOURCE_DIR}/motor_api)

# 为什么不写 target_link_libraries(hmi PRIVATE soem) 而只引它的静态库文件:
# SOEM/cmake/Windows.cmake 里那条 target_compile_options(soem PUBLIC $<$<C_COMPILER_ID:GNU>:-std=c11>)
# **没按语言设限**, 会跟着 PUBLIC 的 usage requirements 漏进 C++ 编译, 而且排在
# -std=gnu++17 **后面** —— 实测 C++ 文件被当 C11 编, 每条编译命令都报一条
# "option '-std=c11' is valid for C/ObjC but not for C++"。SOEM 是下载库, 一个字不改,
# 所以这份 include 目录与链接项在这里照抄一份 (少一个会在编译期就炸, 不会静默出错)。
#
# 注意 soem 那几条 usage requirements 里还有 `_UCRT` 与 `-lucrt`: 在**这条** preset 里
# 它们是对的 —— 编译器是 MSYS2 的 ucrt64 gcc, _UCRT 本来就是预定义宏, 链接 ucrt 也正是
# 唯一那套 CRT。换回 Qt 官方那个 msvcrt 版 MinGW 就会出事 (它没有 _UCRT, 而 SOEM 的
# osal.c 用 timespec_get/TIME_UTC, 那边只有 _UCRT 打开时才有声明), 详见 CMakePresets.json。
target_include_directories(hmi PRIVATE
  ${CMAKE_SOURCE_DIR}/SOEM/include
  ${CMAKE_BINARY_DIR}/SOEM/include
  ${CMAKE_SOURCE_DIR}/SOEM/osal
  ${CMAKE_SOURCE_DIR}/SOEM/osal/win32
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Include
)

target_link_libraries(hmi PRIVATE
  Qt6::Widgets
  $<TARGET_FILE:soem>            # 带依赖: soem 会先编出来
  ucrt ws2_32 winmm
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Lib/x64/wpcap.lib
  ${CMAKE_SOURCE_DIR}/SOEM/oshw/win32/wpcap/Lib/x64/Packet.lib
)


# WIN32_EXECUTABLE=FALSE: **保留控制台窗口**。motor_api 的中文日志 (选轴 / 偏移证明 /
# 使能阶梯 / WKC) 全打在 stdout, 排查时能同时看见界面和日志比看不见强得多。
set_target_properties(hmi PROPERTIES
  WIN32_EXECUTABLE FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

target_compile_options(hmi PRIVATE
  $<$<COMPILE_LANGUAGE:C>:$<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>>
  $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>>
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W3>
)
