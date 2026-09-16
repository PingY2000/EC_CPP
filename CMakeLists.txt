# 顶层工程:本仓库自己的工具引用下载的 SOEM 库 (add_subdirectory), 不改动 SOEM 源码。
# SOEM 作为子工程时不会构建它自带的 samples, 只产出 soem 静态库。
cmake_minimum_required(VERSION 3.28)
project(EC_CPP LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_subdirectory(SOEM)
add_subdirectory(aliasinfo)
add_subdirectory(slide_verify)
add_subdirectory(slide_motion)
add_subdirectory(motor_api)

# Qt 上位机。默认 OFF —— 没装 Qt 的人照旧能编 CLI (而且那两条构建不需要 C++ 编译器)。
# 打开需要 Qt6: cmake --preset hmi-qt-mingw
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


