# 本仓库自有工具 slide_verify (独立于下载的 SOEM 源码)。
# 链接 soem 库即可使用其 PUBLIC 头文件路径 (include/ + build 生成的 ec_options.h)。
add_executable(slide_verify slide_verify.c)
target_link_libraries(slide_verify PRIVATE soem)
target_compile_options(slide_verify PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)
