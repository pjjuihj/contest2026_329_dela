# VelaWear 硬件资源矩阵

更新时间：2026-08-25  
适用对象：黄山派（SF32LB52）上的 VelaWear Agent 开发与演示。  
本文是硬件参考，不把“驱动初始化日志”当作“物理效果已确认”。

## 1. 当前演示条件

- 开发板由 USB 直接供电。
- **当前没有接电池**。因此电池容量、剩余电量、充放电曲线、低电量告警均不能在本演示环境中实现或验证。
- CH340N 的 UART1 以 1,000,000 bps 用于烧录和调试；其 RTS 与 SoC 复位相连，打开串口可能复位开发板。

## 2. 证据等级

| 标记 | 含义 |
|---|---|
| BSP | 黄山派板级 README、引脚复用或 bring-up 源码声明 |
| CFG | 当前 `lckfb_huangshan_pi_ble_repro/.config` 已启用 |
| RUN | 本次 VelaWear 启动或 HIL 串口日志已出现 |
| HIL | 仍需人在设备旁做物理确认 |

## 3. 板载与已接入资源

| 资源 | 板级证据与接口 | 当前 VelaWear 使用情况 | 能支持的功能 | 状态与限制 |
|---|---|---|---|---|
| SF32LB52 + 8 MB OPI-PSRAM | BSP；板级文档说明；应用音频 DMA 缓冲明确放 HCPU SRAM | C/C++ Agent、LVGL、DMA、BLE 均在此 SoC 上运行 | 本地状态机、事件决策、轻量算法 | RUN：应用已启动；不等价于可运行大型离线 ASR 模型 |
| LSM6DSL 六轴 IMU | CFG：`CONFIG_SENSORS_LSM6DSL=y`；应用节点 `/dev/lsm6dsl0` | 50 Hz 运动、跑步/静止、跌倒状态机 | 久坐提醒、运动分类、跌倒检测、可进一步做基于 IMU 的步数/睡眠启发式 | RUN：驱动初始化、运动日志已出现；步数/睡眠算法尚未实现 |
| 1.85" CO5300 AMOLED | BSP：390×450 QSPI；`/dev/lcd0`、`/dev/fb0`；PA00/PA01、PA02–PA08、PA26、PA38 | LVGL 表盘、应用中心、告警覆盖层 | 本地告警、状态显示、设置页 | RUN：`/dev/lcd0` 和 LVGL 已初始化；HIL：需目视确认画面 |
| FT6146 电容触控 | BSP：I2C1，SDA PA33、SCL PA37、INT PA41、RST PA09；`/dev/input0` | UI 触控输入 | 菜单、阈值设置、确认/取消 | RUN：节点已打开；HIL：需触摸确认 |
| KEY1 / KEY2 | BSP：PA34 / PA43；`/dev/buttons`；当前板级按钮驱动实际注册 KEY2 | VelaWear 已启用 KEY2 输入 | 本地菜单、手动确认、演示模式切换 | RUN：KEY2 启用日志已出现；HIL：需按键验证 |
| RGB LED | BSP：PA32 / GPTIM2_CH1 PWM | 当前 `CONFIG_RGBLED` 未启用，应用未使用 | 状态灯、告警指示 | 板上资源存在；需启用驱动并验证 PWM 极性 |
| MEMS 麦克风 + AUDCODEC ADC | 应用直接使用 AUDCODEC ADC CH0、DMA request 39；16 kHz；无 `/dev/audio/pcm_in0` | 640-word 循环 DMA、声级平均值/峰值、`sound_start/end` 事件 | 环境声/人声活动检测、拍手或敲击触发 | RUN：ADC DMA 采样已验证；HIL：近讲连续语音触发待确认；不是 ASR/关键词识别 |
| AUDCODEC DAC + NS4150 外部功放 | BSP：PA42 为 `AUDIO_PA_CTRL`，上电默认关闭；应用 DMA request 41 | 提醒音 DMA 已完成；外部 PA 宏当前为 0 | 本地提示音、旋律 | HIL：PA42 在 DAC 解静音后会触发复位，故当前安全禁用；不能宣称可听 |
| BLE | CFG：Zblue + `BT_UART_ON_DEV_NAME=/dev/ttyHCI0`；应用自定义 GATT 状态/阈值特征 | 广播、状态读取/通知、久坐阈值写入 | 手机查看状态、调阈值、告警状态通知 | RUN：GATT 注册与 advertising 已出现；HIL：手机/宿主机连接和通知待确认 |
| AW32001 充电 IC | BSP：I2C2，SCL PA10、SDA PA11、地址 0x49 | 板级 bring-up 配置充电 IC | 未来接电池后的充电控制/状态读取基础 | **当前未接电池**；且 `BATTERY_CHARGER/GAUGE/MONITOR` 均未配置，不能提供电量百分比 |
| ADC0 | BSP：8 通道、12 位，`/dev/adc0` | 系统 ADC 节点可用；VelaWear 麦克风走的是 AUDCODEC ADC，不是 ADC0 | 外接模拟传感器、经确认分压后的电压测量 | 未确认电池或其他板载信号连接到 ADC0；不得直接用它报告电池电量 |
| RTC、硬件定时器、看门狗 | BSP：`/dev/rtc0`、`/dev/timer0`、`/dev/watchdog0` | VelaWear 已启用硬件看门狗；使用单调时钟做久坐计时 | 定时提醒、离线日程、可靠性监测 | RUN：看门狗启动日志已出现；RTC/硬件定时器尚未被 VelaWear 接入 |
| 16 MB NOR | BSP：MPI2，`/dev/config0` | 固件 XIP、配置存储 | 持久化设置、离线日志 | RUN：NOR MTD 注册已出现 |
| UART1 / USB | BSP：UART1 PA18/PA19、CH340N；另有 USB CDC ACM 设备 | 烧录、NSH、串口日志 | 开发调试、自动化测试 | RUN：已用于本项目；RTS-to-RST 是已知约束 |

## 4. 仅用现有硬件可以继续实现的功能

这些工作不需要新增外设，但仍需要代码和 HIL 验证：

1. 基于 IMU 的步数估计、佩戴/静止时段统计、简化睡眠候选判定。
2. 基于 RTC/定时器的本地定时提醒与 UI 设置页。
3. 使用 KEY2、触控和 LED 的本地交互与状态指示。
4. BLE 状态、阈值配置和告警通知的手机客户端流程。
5. 麦克风声级触发（不是语音识别）与提醒音仲裁。
6. 未来接入电池后，先读取 AW32001 状态；电量百分比仍需额外的测量链路和标定。

## 5. 当前不能声称具备的能力

| 能力 | 原因 |
|---|---|
| 电池百分比、低电量告警、续航评估 | 当前没有接电池；固件也未启用 battery charger/gauge/monitor；未确认 ADC0 电压分压与标定 |
| 心率、血氧、体温 | 本板当前项目没有相应传感器驱动或数据生产者 |
| 真实振动提醒 | BSP 和引脚表中没有确认的振动马达资源；当前“vibrate”动作不能当作硬件振动证明 |
| 外放可听提示音 | 外部功放 PA42 路径当前为复位风险，已安全禁用，需修复后再验证 |
| 语音命令、关键词、离线 ASR | 现有链路只提供声级；还需要 ASR 算法/模型、内存评估和输入质量验证 |
| 云端 LLM | 当前没有配置网络传输、服务端接口或凭据；BLE 不能替代网络链路 |
| 手机消息接收 | 自定义 BLE 服务当前只提供状态和阈值，不包含手机通知协议 |
| GPS、摄像头、SD 卡 | 当前 BSP/引脚表没有对应的已接入资源；板级文档明确 SD 卡未接线 |

## 6. 实施优先级

1. 先完成不依赖新增硬件的 IMU、触控/按键、RTC 定时、BLE 状态与声级事件功能。
2. USB 供电演示期间，不实现电池百分比或低电量功能；README 和 UI 不得宣称已支持。
3. 音频外放必须在 PA42/NS4150 路径稳定后再重新启用。
4. 引入新硬件前，先在本文补充型号、供电、电气接口、占用引脚、驱动、数据率和验证命令。

## 7. 证据来源

- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md`
- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/src/bsp_init.c`
- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/src/bsp_pinmux.c`
- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/src/bsp_power.c`
- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/src/sf32lb52_buttons.c`
- `cmake_out/lckfb_huangshan_pi_ble_repro/.config`
- `contest2026_329_dela/app/velawear_agent/drivers/imu_sensor.c`
- `contest2026_329_dela/app/velawear_agent/drivers/audio_hw_test.c`
