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

# sm_pdo: 独立的"控制字走 PDO + 进 OP"验证小程序 —— 回答"PRE_OP+SDO 切不动的
# CiA402 状态机, PDO+OP 能不能切动"。
#
# 链接只读底座 sm_bus.c (时钟 / SDO 读 / 寄存器读 / 错误栈排空 / 身份判定;
# 它不含任何 ecx_SDOwrite), 同样刻意不链接 sm_guard.c。
# 写入口是 sm_pdo.c 自己的 pd_wr_raw(): 只写 PDO 映射对象 1A00h/1C13h 与
# --reset-fault 的故障复位。6040h **不经 SDO 写**, 只经过程数据镜像写。
add_executable(sm_pdo sm_pdo.c sm_bus.c)
target_link_libraries(sm_pdo PRIVATE soem)
target_compile_options(sm_pdo PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)

# test2: sm_pdo 的骨架版 —— 单文件, 无护栏/无快照/无退出码契约, 只留着"要跑起来
# 必须有的那一部分"。风格照 test1.c (线性、printf 直读), API 用真实的 ecx_*。
#
# 它自称最小的代价是三条明确放弃的保障, 看代码前先知道:
#   - 不发/不还原 PDO 映射快照: 写完 1A00h/1C13h 就留在 RAM 里 (掉电回出厂值)。
#   - 不复核 6041h 的 PDO/SDO 双通路一致性 (只在失败时用 SDO 回读 6040h 分主站侧/驱动器侧)。
#   - 只接受单台从站 (多台直接拒绝), 不做 YKD 身份判定。
# 写入口是它自己的 sdo_write() (写后回读, 因为本版 SOEM 把加急写的 abort 当成功);
# 推送过的 6040h 只有 0x0000/0x0006/0x0007/0x000F。真跑用 sm_pdo。
add_executable(test2 test2.c)
target_link_libraries(test2 PRIVATE soem)
target_compile_options(test2 PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)
