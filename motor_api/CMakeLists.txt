# motor_api: 多轴 CiA402 电机调用接口 (CSP 位置同步 / PV 速度 / HM 回零)。
#
# 参照 slide_motion/test2.c 的底座写法 (PRE_OP 补映射 -> config_map_group -> 逐轴
# 证明偏移 -> SAFE_OP -> OP), 但它是**接口 + 验收程序**的分层, 不是一个单文件工具:
#
#   ec_motor.c         底座: 上下文 / 时钟 / SDO 读写 / 映射实读与追加 / 偏移证明 /
#                            AL 状态阶梯 / 过程数据 / 6041h 判读 / 收尾
#   ec_motor_motion.c  运动层: 使能状态机 / CSP 多轴轨迹 / PV / 回零
#   motor_test.c       调用这个接口的验收程序 (S0~S9)
#
# 安全边界提醒: 全部 ecx_SDOwrite 调用点集中在 ec_motor.c。
#      grep -n ecx_SDOwrite motor_api/*.c   ->  只应有 ec_motor.c 一处
#
# 与 slide_motion/ 里"只有 sm_guard.c 写"是同一条不变量。头文件 ec_motor.h 也不导出
# 任何裸写函数 —— 对外只有语义化的 em_set_mode / em_home / em_csp_move_* 等, 它们
# 最终都走 ec_motor.c 的 em__wr_*()。审计"这接口会写什么"只需要读那一个文件。
#
# 会写的对象只有: 6060h 运行模式; 6098h/6099h/609Ah/607Ch 回零参数;
# 1C12h/1C13h/1600h/1A00h PDO 映射 (仅 RAM, 且默认收尾还原)。
# 6040h 只经过程数据镜像写, 从不经 SDO 写。
# 从不写: 2102h (EEPROM) / 607Dh (软限位) / 2400h / 2408h / 2409h / 2201h。

add_executable(motor_test
  motor_test.c
  ec_motor.c
  ec_motor_motion.c
)
target_link_libraries(motor_test PRIVATE soem)
target_compile_options(motor_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)
