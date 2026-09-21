# 顶层工程:scan (扫描采集上位机) 与 hmi (它的前身界面) 引用下载的 SOEM 库
# (add_subdirectory), **不改动 SOEM 源码** —— 它是完整保留下来的第三方库。
# SOEM 作为子工程时不会构建它自带的 samples, 只产出 soem 静态库。
cmake_minimum_required(VERSION 3.28)
project(EC_CPP LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_subdirectory(SOEM)

# ---- 本仓库现在只有两个程序: scan (产品) 与 hmi (它的前身界面) ----
# **2026-09-21 清理**: aliasinfo / slide_verify / slide_motion (含 sm_state / sm_pdo /
# test2) 与 motor_api 的验收程序 motor_test 全部从仓库移除 —— scan 不依赖它们。
# motor_api/ 这个目录**没有 CMakeLists 也不再有构建目标**: 它现在是**源码目录**,
# ec_motor.c / ec_motor_motion.c 由 hmi 与 scan 各自直接编进去 (那两份 CMakeLists 里
# 写着为什么不起一个库目标)。所以这里没有 add_subdirectory(motor_api) 这回事。

# Qt 上位机。默认 OFF —— **但现在没有 CLI 兜底了**: 两个程序都要 Qt6 + C++ 编译器,
# OFF 等于只编 SOEM 静态库 (那是给"只验 SOEM 能不能编过"用的)。
# 打开需要 Qt6: cmake --preset hmi-qt-ucrt64
option(EC_BUILD_HMI "构建 Qt 上位机 (需要 Qt6 + C++ 编译器)" OFF)
if(EC_BUILD_HMI)
  add_subdirectory(hmi)
endif()

# 扫描采集上位机 (scan/)。**默认跟着 HMI 一起开** —— 它用的是同一个 Qt、同一个 preset,
# 不为它再来一条 configure 命令。只想编 hmi 的人 -DEC_BUILD_SCAN=OFF 就行。
option(EC_BUILD_SCAN "构建扫描采集上位机 (需要 Qt6 + C++ 编译器)" ${EC_BUILD_HMI})
if(EC_BUILD_SCAN)
  add_subdirectory(scan)
endif()


