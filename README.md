# EC_CPP

面向研控 **YKD2205PE** EtherCAT 滑台驱动器 (CiA 402) 的原生 C / SOEM 工具集。

## 目录结构

| 路径 | 说明 |
|---|---|
| [SOEM/](SOEM/) | 下载的第三方 EtherCAT 主站库 (**Simple Open EtherCAT Master**),保持原样不改动 |
| [aliasinfo/](aliasinfo/) | 自有工具:读取各从站 ESC 0012h-0013h 站点别名 (拨码站号) |
| [slide_verify/](slide_verify/) | 自有工具:滑台设备导入验证 (扫描总线 → 读 CiA402 对象 → PASS/WARN/FAIL → 退出码) |
| [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md) | YKD2205PE 对象速查与两工具的用法/退出码文档 |
| [YKD2205PE.pdf](YKD2205PE.pdf) | 厂商手册 (参考) |

顶层 `CMakeLists.txt` 通过 `add_subdirectory(SOEM)` 引用 SOEM 库,
再 `add_subdirectory` 各自有工具 —— SOEM 只以子工程身份产出 `soem` 静态库,
不构建其自带 samples,源码不被改动。

## 构建

本机 (MSYS2 UCRT64 MinGW) 示例;VS/CMake 环境把 `build-mingw` 换成 `build` 即可:

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
cmake --build build-mingw --target slide_verify aliasinfo
```

产物:
- `build-mingw/slide_verify/slide_verify.exe`
- `build-mingw/aliasinfo/aliasinfo.exe`

## 运行注意

- 两个工具都是**只读** (不进 OP、不写 6040h 控制字、不动电机),并依赖 **Npcap**
  独占网卡 —— 与仓库的 python 链 (pysoem) 一样,**勿同时运行**。
- 参数为网卡名 (Windows Npcap 形如 `\Device\NPF_{GUID}`),不带参数时列出可用网卡。
- 详细用法、输出示例与退出码见 [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md)。
