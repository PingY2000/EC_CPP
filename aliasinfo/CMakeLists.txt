# 本仓库自有工具 aliasinfo (独立于下载的 SOEM 源码)。
add_executable(aliasinfo aliasinfo.c)
target_link_libraries(aliasinfo PRIVATE soem)
target_compile_options(aliasinfo PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)
