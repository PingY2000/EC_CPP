# 顶层工程:本仓库自己的工具引用下载的 SOEM 库 (add_subdirectory), 不改动 SOEM 源码。
# SOEM 作为子工程时不会构建它自带的 samples, 只产出 soem 静态库。
cmake_minimum_required(VERSION 3.28)
project(EC_CPP LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_subdirectory(SOEM)
add_subdirectory(aliasinfo)
add_subdirectory(slide_verify)
add_subdirectory(slide_motion)


