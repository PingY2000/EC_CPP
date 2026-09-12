# EC_CPP

面向研控 **YKD2205PE** EtherCAT 滑台驱动器 (CiA 402) 的原生 C / SOEM 工具集。

## 目录结构

| 路径 | 说明 |
|---|---|
| [SOEM/](SOEM/) | 下载的第三方 EtherCAT 主站库 (**Simple Open EtherCAT Master**),保持原样不改动 |
| [aliasinfo/](aliasinfo/) | 自有工具:读取各从站 ESC 0012h-0013h 站点别名 (拨码站号) |
| [slide_verify/](slide_verify/) | 自有工具:滑台设备导入验证 (扫描总线 → 读 CiA402 对象 → PASS/WARN/FAIL → 退出码) |
| [slide_motion/](slide_motion/) | 自有工具:**带动作**验收验证 (参数基线 → 使能状态机 → 微动与反馈闭环),**默认不动** |
| ↳ `sm_state` / `sm_pdo` | 同目录下的两个**独立**专项小程序: 只验证 CiA402 状态机切换。`sm_state` 用 **SDO + PRE_OP**, `sm_pdo` 用 **PDO + OP** (会写 PDO 映射, 见下) |
| [baseline_ykd2205pe.ini](baseline_ykd2205pe.ini) | 示例参数基线 (由 `slide_motion --dump-baseline` 现场导出后人工审定) |
| [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md) | YKD2205PE 对象速查与各工具的用法/退出码文档 |
| [docs/slide_motion_verify.md](docs/slide_motion_verify.md) | `slide_motion` 的完整设计/安全须知/微动判据/实测记录 |
| [YKD2205PE.pdf](YKD2205PE.pdf) | 厂商手册 (参考) |

顶层 `CMakeLists.txt` 通过 `add_subdirectory(SOEM)` 引用 SOEM 库,
再 `add_subdirectory` 各自有工具 —— SOEM 只以子工程身份产出 `soem` 静态库,
不构建其自带 samples,源码不被改动。

## 构建

本机 (MSYS2 UCRT64 MinGW) 示例;VS/CMake 环境把 `build-mingw` 换成 `build` 即可:

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
cmake --build build-mingw --target slide_verify aliasinfo slide_motion sm_state sm_pdo
```

产物 (统一输出到仓库根的 `bin/`):
- `bin/slide_verify.exe`
- `bin/aliasinfo.exe`
- `bin/slide_motion.exe`
- `bin/sm_state.exe`
- `bin/sm_pdo.exe`

## 运行注意

- `aliasinfo` 与 `slide_verify` 是**只读**的 (不进 OP、不写 6040h 控制字、不动电机)。
- `slide_motion` **默认也是只读的**: 不给 `--allow-motion` 时不写一个字节。
  只有显式给 `--allow-motion --allow-jog` 并再过一次交互确认, 它才会让电机通电
  并移动滑台 —— 真动时**必须有人在设备旁、手放在物理急停上**。
  它永远不写 `2102h` (EEPROM)、不改 PDO 映射、不写软限位。
  退出码 **10** = 收尾写完 `6040h=0` 但回读 `6041h` 仍报 Operation enabled,
  即**电机可能仍带电** —— 它覆盖其它所有退出码, 见到请立即断电确认。
- `sm_state` / `sm_pdo` 是 CiA402 状态机切换的**专项验证**小程序, 同样**默认只读**:
  - `sm_state` 只写 `6040h` (SDO, 在 PRE_OP 下)。
  - `sm_pdo` 用 **PDO + OP** 走同样 8 步 —— 为此它**会写 PDO 映射对象**
    `1A00h`/`1C13h` (出厂 TxPDO 为空, 不补写就进不了 SAFE_OP)。**只写 RAM,
    从不写 `2102h` (EEPROM)**; 默认跑完**还原**成运行前快照 (`--keep-mapping` 才保留,
    `--no-map` 整个跳过)。需要 `--allow-pdo` 才写; 默认只打印 PDO 快照。
  - 两者都发**零运动指令**、不写 `607Ah`/`6060h`/软限位。但走到 Enable Operation
    之后电机会通电 (有保持力矩) —— **真跑时人在设备旁, 手放在物理急停上**。
  - 退出码 **10** 的含义与 `slide_motion` 相同: 收尾未能确认失能, **电机可能仍带电**。
- 五个工具都依赖 **Npcap** 独占网卡 —— 与仓库的 python 链 (pysoem) 一样,
  **勿同时运行**。
- 参数为网卡名 (Windows Npcap 形如 `\Device\NPF_{GUID}`),不带参数时列出可用网卡。
- 详细用法、输出示例与退出码见 [docs/ykd2205pe_ci402.md](docs/ykd2205pe_ci402.md)
  (含 `sm_state`/`sm_pdo` 的选项与退出码表);
  `slide_motion` 另见 [docs/slide_motion_verify.md](docs/slide_motion_verify.md)。
