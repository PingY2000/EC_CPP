# 本仓库自有工具 slide_motion (独立于下载的 SOEM 源码)。
# 链接 soem 库即可使用其 PUBLIC 头文件路径 (include/ + build 生成的 ec_options.h)。
#
# 安全边界提醒: 全部 ecx_SDOwrite 调用点集中在 sm_guard.c。
#      grep -n ecx_SDOwrite slide_motion/*.c   ->  只应有 sm_guard.c 一处
add_executable(slide_motion
  slide_motion.c
  sm_bus.c
  sm_guard.c
  sm_baseline.c
  sm_motion.c
)
target_link_libraries(slide_motion PRIVATE soem)
target_compile_options(slide_motion PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)

# sm_state: 独立的 CiA402 状态机切换验证小程序, 刻意不链接 sm_guard.c/sm_bus.c。
# 它的写入口只有 sm_state.c 里的 st_wr_cw(), 且只写 6040h。
add_executable(sm_state sm_state.c)
target_link_libraries(sm_state PRIVATE soem)
target_compile_options(sm_state PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)
