# Ophir PD300R + Juno+：Qt/C++ 上位机开发资料说明

## 1. 项目目标

本项目用于在 Windows PC 上，通过 Ophir PD300R 光电探测器 + Juno+ USB 接口，将光功率数据集成到现有的 Qt/C++ 上位机中。

目标链路：

```text
PD300R
  │ Ophir Sensor 接口
  ▼
Juno+
  │ USB Mini-B
  ▼
Windows PC
  │
  ├─ StarLab / USB Driver
  │
  └─ OphirLMMeasurement COM Object
           │
           ▼
      Qt/C++ 上位机
```

本项目不应自行逆向 Juno+ USB 协议。Ophir 官方已经提供用于 USB 设备系统集成的 OphirLMMeasurement COM Object；StarLab 安装过程会安装相应 USB 支持软件并注册 COM Object。Juno+ 明确支持 COM Object 接口。 

> 重要：本文档不包含《OphirLMMeasurement COM Object User Manual》的 API 细节。COM API 的方法、参数、返回值、事件和数据结构必须以该官方手册及官方示例源码为准。

---

## 2. 必备硬件资料

### 2.1 PD300R Datasheet

文件建议命名：

```text
PD300R_Datasheet.pdf
```

用途：让程序开发人员了解实际传感器型号、波长范围、功率范围、响应时间、噪声、允许功率、光斑位置依赖及其他测量限制。

当前官方资料中 PD300R 系列被归类为圆形 Photodiode Sensors。具体规格应以实际 PD300R 的完整 datasheet 为准，不应让程序通过型号名称自行推断规格。

尤其应让 AI 注意以下概念：

```text
Sensor model
Wavelength range
Power range / range switching
Response time
Noise
Accuracy / wavelength dependence
Beam position dependence
Damage threshold
Filter state（若实际型号支持）
```

PD300R 系列官方资料中存在不同型号/变体，因此测试程序应首先识别实际传感器型号，再依据实际硬件条件配置测试参数。

---

## 3. Juno+ 硬件资料

### 3.1 PC Interfaces / Juno+ Datasheet

推荐文件：

```text
PC_Interfaces.pdf
JunoPlus_Datasheet.pdf
```

当前已有的 `PC-interfaces-0.pdf` 可以直接作为资料输入。

与本项目直接相关的信息包括：

- Juno+ 将 Ophir 智能传感器连接到 PC；
- Juno+ 通过 USB 与 PC 通信；
- Juno+ 由 USB 供电；
- 支持标准 Ophir 智能传感器；
- 支持 Photodiode 传感器；
- 支持 COM Object 接口；
- Juno+ 支持通过 StarLab 进行功率/能量记录、平均值和统计等操作；
- Juno+ 单个模块支持一个传感器；
- 官方资料给出的能量数据最大记录速率为 10 kHz；
- Juno+ 提供 USB 和模拟输出，其中模拟输出为用户选择的满量程电压输出。

不要把上述“最大记录速率”直接理解为 PD300R 在所有测试条件下都可以得到 10 kHz 的有效功率数据。实际可用速率还受到传感器本身响应、测量模式和接口/API使用方式的影响。

---

## 4. StarLab：PC 运行环境

### 4.1 StarLab 的作用

StarLab 在本项目中主要承担两个基础职责：

1. 安装 Ophir USB 设备所需的驱动和支持软件；
2. 注册 OphirLMMeasurement COM Object。

StarLab 本身不是 Qt/C++ 上位机的数据处理层。Qt/C++ 程序可以直接通过 OphirLMMeasurement COM Object 与 Ophir USB 设备通信。

Ophir 官方说明：安装 StarLab 后，必要的 USB device drivers 和 Ophir COM Object 会安装/注册到 Windows；Automation Examples 也会安装到 StarLab 目录下。

### 4.2 推荐安装验证流程

首先安装当前版本的 StarLab。

然后按以下顺序验证：

```text
PD300R 接 Juno+
        ↓
Juno+ USB 接 PC
        ↓
Windows 识别设备
        ↓
StarLab 识别传感器
        ↓
StarLab 可以显示实时功率
        ↓
再进入 Qt/C++ 开发
```

如果 StarLab 本身无法读取功率，不应先修改 Qt/C++ 代码，而应先排查传感器、Juno+、USB 线、驱动及 StarLab 环境。

### 4.3 官方当前软件信息

Ophir 当前软件页面列出的 StarLab 为 USB 设备使用的软件，并明确列出了 Juno+ 支持。当前页面显示 StarLab v4.00，发布日期为 2026-05-12；实际开发机器应记录安装的具体版本号，不要假设始终是这个版本。

---

## 5. 官方 Automation Examples：优先级非常高

### 5.1 必须保存的目录

安装 StarLab 后，查找：

```text
C:\Program Files\Ophir Optronics\StarLab 3.xx\Automation Examples\Com object
```

实际 StarLab 版本号可能不同，以当前安装目录为准。

官方说明这里包含多种语言的 COM Object 客户端示例，包括：

```text
C++
C#
Java
LabVIEW
MATLAB
Python
VB.NET
```

本项目重点保留：

```text
C++ 示例
```

其他语言示例可以作为交叉验证材料保留，但不应让 AI 优先根据 Python/C# 自行转换 API。

### 5.2 给 AI 的源码资料

建议直接把下面整个目录复制给 AI：

```text
Automation Examples\Com object\
```

尤其是：

```text
C++ 示例源码
项目文件
头文件
源文件
配置文件
相关 DLL/类型库说明（如果示例目录带有）
```

原因：官方示例已经解决了 COM Object 的调用细节。Ophir 官方明确表示这些示例用于帮助系统集成商将 Ophir 测量能力集成到自己的软件中，并提供 C++ 等示例。

对于本项目，官方 C++ 示例应视为**第一参考实现**。

---

## 6. Qt/C++ 开发环境

### 6.1 当前上位机技术路线

本项目不是 Python 项目。

主技术栈：

```text
Windows
Qt 6
C++
CMake
OphirLMMeasurement COM Object
```

GUI 使用 Qt Widgets 或项目现有的 Qt UI 框架。

### 6.2 Qt 访问 COM 的方式

Qt for Windows 提供 ActiveQt。

对于非可视 COM 对象，推荐使用：

```cpp
QAxObject
```

Qt 官方文档说明 `QAxObject` 可以封装 COM 对象，并通过 `dynamicCall()`、属性访问等方式调用 COM API。ActiveQt 的 CMake 模块名为 `AxContainer`。

典型 CMake 配置形式：

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Widgets AxContainer)

target_link_libraries(MyApp PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::AxContainer
)
```

实际项目应以当前 Qt Kit 是否安装 ActiveQt/AxContainer 为准。

### 6.3 Qt 中调用 COM 的建议路线

第一阶段优先使用：

```text
QAxObject
  ↓
Ophir COM Object
```

不要第一阶段就去做：

```text
WinUSB
libusb
USB HID 协议猜测
USB 原始帧解析
自定义驱动
```

这些都没有必要，因为 Ophir 已经提供官方 COM 集成层。

### 6.4 关于 QAxObject 的重要 Qt 约束

如果继承 `QAxObject`，Qt 官方有特殊限制：不能在这个子类中继续使用 `Q_OBJECT`。更合适的工程做法是：

```text
OphirComClient : QObject
    └── QAxObject 作为成员
```

而不是：

```text
class OphirComClient : public QAxObject
{
    Q_OBJECT   // 不推荐
};
```

这样可以保留正常的 Qt signals/slots 架构。

---

## 7. 推荐的 Qt/C++ 软件结构

不要让 UI 界面直接散落调用 Ophir COM API。

推荐：

```text
MainWindow
    │
    │ Qt signals / slots
    ▼
OphirPowerMeter
    │
    ├── COM wrapper
    │       │
    │       └── OphirLMMeasurement
    │
    ├── Device information
    ├── Sensor information
    ├── Measurement configuration
    ├── Acquisition
    └── Error handling
```

进一步可以拆成：

```text
OphirPowerMeter
    ├── connectDevice()
    ├── disconnectDevice()
    ├── deviceInfo()
    ├── sensorInfo()
    ├── configureMeasurement()
    ├── startMeasurement()
    ├── stopMeasurement()
    └── readMeasurement()
```

上层测试程序只关心统一的数据结构，例如：

```cpp
struct PowerSample
{
    double value;
    qint64 timestamp;
    int status;
};
```

具体字段类型必须根据 Ophir COM Object 实际返回值进行调整，不要让 AI 根据这个示例结构自行假定 COM API 的返回类型。

---

## 8. 推荐的软件数据流

### 8.1 设备连接层

```text
Windows
  ↓
StarLab 安装的 USB 支持
  ↓
Ophir COM Object
  ↓
Juno+
  ↓
PD300R
```

### 8.2 程序数据流

```text
Device Discovery
      ↓
Open Device
      ↓
Read Device/Sensor Information
      ↓
Configure measurement
      ↓
Start acquisition / stream
      ↓
Receive measurement data
      ↓
Validate status
      ↓
Convert to project data structure
      ↓
Qt signal
      ↓
Test Logic / UI / Logger
```

### 8.3 测试层

后续测试逻辑可以独立于 Ophir 驱动：

```text
OphirPowerMeter
        ↓
PowerSample
        ↓
MeasurementProcessor
        ├── Average
        ├── Min / Max
        ├── Standard Deviation
        ├── Stability
        └── Limit Check
        ↓
TestResult
        ↓
UI / CSV / Database
```

这样以后更换功率计、增加第二个测量设备或做自动化测试，不需要重写 UI。

---

## 9. 第一阶段开发目标

第一版不要同时做完整测试系统。

建议只验证下面的最小闭环：

```text
1. 创建 COM Object
2. 扫描 Ophir USB 设备
3. 找到 Juno+
4. 打开设备
5. 读取设备/传感器基本信息
6. 启动测量
7. 读取功率数据
8. 在 Qt 界面实时显示功率
9. 停止测量
10. 正常关闭设备
```

只有这一步稳定后，再加入：

```text
平均值
稳定性
CSV
测试限值
PASS/FAIL
自动测试流程
```

---

## 10. 数据记录建议

建议原始数据和测试结果分开。

### 原始数据

每条数据至少尽量保留：

```text
PC time
Ophir timestamp
Power value
Unit
Status
Device identifier
Sensor model
Wavelength
Range
```

具体可获得字段以 Ophir COM Object 实际接口为准。

### 测试结果

例如：

```text
Test ID
Start time
End time
Sensor
Wavelength
Configured range
Average power
Minimum power
Maximum power
Standard deviation
Stability metric
Lower limit
Upper limit
Result
```

不要只保存最终 PASS/FAIL，否则后续很难追溯测试过程。

---

## 11. 采样与线程建议

### 11.1 不建议在 GUI 线程持续阻塞读取

不要把长时间等待/数据采集直接塞进：

```cpp
MainWindow::onStartClicked()
```

也不要让 UI 线程执行无限循环读取。

推荐：

```text
MainWindow
    │
    ├── start/stop command
    │
    ▼
Ophir acquisition worker
    │
    ▼
PowerSample
    │
    ▼
Qt signal
    │
    ├── UI
    ├── Logger
    └── Test Processor
```

具体 COM apartment/threading 实现应根据实际 Qt 工程和官方示例进行验证。初版优先保证设备调用集中在一个明确的 COM 客户端对象中，不要让多个线程同时操作同一个 Ophir COM 实例。

### 11.2 不要先追求 10 kHz

Juno+ 官方资料给出的能量记录能力可以达到 10 kHz，但这个数字不是“所有 PD300R 功率测试均应按 10 kHz 读取”的要求。

普通自动化功率测试建议先确定：

```text
需要多少真实信息？
传感器响应时间是多少？
测试对象变化速度是多少？
最终指标是什么？
```

第一阶段先验证稳定读取，再决定采样策略。

---

## 12. 故障排查顺序

### 情况 A：StarLab 找不到 PD300R

优先检查：

```text
PD300R 是否正确连接
Juno+ 是否正常供电
USB 线是否正常
Windows 是否识别设备
StarLab 是否安装正确
```

### 情况 B：StarLab 正常，但 Qt/C++ 找不到设备

优先检查：

```text
StarLab 是否已经安装
Ophir COM Object 是否注册
OphirLMMeasurement.dll 是否存在
Automation Example 是否可以运行
Qt 程序位数是否与运行环境匹配
```

不要直接转去排查 USB 原始协议。

### 情况 C：能发现设备，但打开失败

检查：

```text
StarLab 是否正在独占使用设备
是否已有其他程序占用设备
COM Object 是否正确注册
程序运行权限
32/64 位环境
```

### 情况 D：能连接但没有有效功率数据

检查：

```text
传感器型号
测量模式
波长设置
量程
测量状态
返回数据 status
```

不要仅仅检查 `value != 0`。

---

## 13. AI 编程时必须遵守的规则

把下面规则一并提供给 AI：

```text
1. 目标硬件是 Ophir PD300R + Juno+。
2. 目标平台是 Windows。
3. 上位机使用 Qt + C++，不是 Python 主程序。
4. 使用 Ophir 官方 OphirLMMeasurement COM Object 进行 USB 通信。
5. 不要自行逆向 Juno+ USB 协议。
6. StarLab 用于安装驱动、COM Object 和验证硬件通信。
7. 以 Ophir 官方 C++ Automation Example 作为代码实现的第一参考。
8. COM API 的方法、参数、返回值、事件和数据类型必须以《OphirLMMeasurement COM Object User Manual》为准。
9. 不允许根据 Python、C# 或网上博客猜测 COM API 参数。
10. Qt 中优先通过 ActiveQt/QAxObject 封装 COM Object。
11. 不要让 UI 线程执行持续阻塞的数据采集。
12. 不要把设备通信逻辑直接写进 MainWindow。
13. 设备层、数据处理层、测试逻辑层和 UI 层分离。
14. 所有设备错误和无效数据必须显式处理。
15. 开发第一阶段只实现设备发现、连接、功率读取和关闭。
```

---

## 14. 给 AI 的资料包建议

最终建议提供给 AI：

```text
Ophir_PowerMeter_Dev/
│
├── 01_Hardware/
│   ├── PD300R_Datasheet.pdf
│   ├── JunoPlus_Datasheet.pdf
│   └── PC_Interfaces.pdf
│
├── 02_Official_Examples/
│   └── Automation Examples/
│       └── Com object/
│           ├── C++/
│           ├── C#/
│           ├── Python/
│           └── ...
│
├── 03_Software/
│   ├── StarLab_User_Manual.pdf
│   └── PC_Software_Drivers.pdf
│
├── 04_Project/
│   ├── Ophir_PowerMeter_Qt_Cpp_Dev.md
│   └── existing Qt project source code
│
└── 05_Com_API/
    └── OphirLMMeasurement_COM_Object_User_Manual.pdf
```

其中 `05_Com_API` 这份手册虽然不包含在本文档正文中，但开发时仍然应该提供给 AI，因为它是 COM API 的权威定义。

---

## 15. 当前应重点收集的文件

### 必须

```text
PD300R Datasheet
Juno+ Datasheet / PC Interfaces
StarLab Automation Examples - C++
OphirLMMeasurement COM Object User Manual
```

### 建议

```text
StarLab User Manual
PC Software & Drivers
User Commands for Software Integrators
```

### 不需要优先收集

```text
其他 Ophir 型号的完整 Datasheet
Python 示例（如果 C++ 示例已经完整）
LabVIEW 示例
大量第三方博客
USB 原始协议资料
```

---

## 16. 官方资料来源

优先级按以下顺序处理：

1. Ophir 官方 COM Object 页面；
2. Ophir 官方 Juno/Juno+ 产品资料；
3. Ophir 官方 PD300R Datasheet；
4. StarLab 安装目录中的官方 Automation Examples；
5. Qt 官方 ActiveQt/QAxObject 文档；
6. 第三方文章仅作为辅助，不用于确定 COM API 的实际调用签名。

官方 COM 页面明确说明 Juno+ 支持 COM Object，StarLab 安装会完成 COM Object 注册，并提供 C++ 等语言的 Automation Examples。

Qt 官方 ActiveQt 文档则定义了 `QAxObject`、`dynamicCall()`、`querySubObject()` 以及 CMake 的 `Qt6::AxContainer` 集成方式。

---

## 17. 建议的最终架构

```text
┌─────────────────────────────────────────────┐
│                 Qt GUI                      │
│                                             │
│  Power Display / Test Control / Results     │
└──────────────────────┬──────────────────────┘
                       │ Qt signals/slots
                       ▼
┌─────────────────────────────────────────────┐
│            Measurement Controller            │
│                                             │
│  start / stop / configure / data processing │
└──────────────────────┬──────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│            OphirPowerMeter                  │
│                                             │
│  Device discovery                           │
│  Open / Close                               │
│  Sensor configuration                       │
│  Acquisition                                │
│  Error handling                             │
└──────────────────────┬──────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│          Qt ActiveQt / QAxObject             │
│                                             │
│       OphirLMMeasurement COM Object         │
└──────────────────────┬──────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│                 Juno+                      │
└──────────────────────┬──────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│                 PD300R                     │
└─────────────────────────────────────────────┘
```

这套架构的核心原则是：**Ophir 通信封装在设备层，Qt UI 不直接接触 COM 细节；测试逻辑只处理统一的功率数据。**

---

## 18. 参考资料说明

本文档基于：

- Ophir `PC Interfaces / Juno+` 官方资料；
- Ophir PD300R 系列官方资料；
- Ophir 官方 COM Object / System Integration 页面；
- Ophir StarLab Automation Examples 的官方说明；
- Qt 官方 ActiveQt / QAxObject 文档。

其中 COM API 的具体调用签名、参数和返回类型不在本文档中复制，应以官方《OphirLMMeasurement COM Object User Manual》和实际安装的官方 C++ Example 为准。
