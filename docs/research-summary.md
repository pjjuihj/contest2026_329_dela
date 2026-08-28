# SF32LB52 黄山派 VelaWear Agent 项目研究总结

> 研究日期：2026-08-15
> 研究员：嵌入式系统研究员
> 项目：2026 AI 硬件开发者大赛 #329 dela 团队

---

## 一、项目当前状态：CRITICAL - 启动崩溃

### 1.1 已验证能工作的部分

| 功能 | 状态 | 证据 |
|------|------|------|
| openvela NuttX 固件编译 | ✅ 成功 | nuttx.bin 约 4.6MB |
| sftool 烧录连接 | ✅ 成功 | 能写入 Flash 2 (MPI2 @ 0x12010000) |
| 固件写入验证 | ✅ 成功 | 读回验证向量表完整：SP=0x2002ACA0, PC=0x120130CD |
| ROM bootloader 启动 | ✅ 部分成功 | 能看到 SFBL 输出 |
| HAL_Init() 执行 | ✅ 部分成功 | 能看到 C 输出（ABCD 启动序列的第三阶段） |

### 1.2 已知失败的部分

| 问题 | 详细描述 | 影响 |
|------|----------|------|
| SFBL 循环 | 板子只输出 SFBL 循环，无法进入 NSH | **致命** |
| 应用崩溃 | 启动序列：SFBL -> SFBL -> C -> SFBL -> C -> SFBL（约 11 秒周期） | **致命** |
| NuttX 内核未启动 | 从未看到 D 输出（nx_start()） | **致命** |
| 蓝牙未启用 | 用户明确要求我需要蓝牙，但 BLE 栈未工作 | **功能缺失** |

### 1.3 关键硬件事实（至关重要）

1. **黄山派不暴露 BOOT 引脚** - 没有物理启动模式开关/跳线
2. **MPI1 (0x10000000) = PSRAM (OPSRAM 模式 3)**，不是 NOR Flash
3. **MPI2 (0x12000000) = NOR Flash**（外部 QSPI）
4. sftool 无法写入 MPI1 (0x10000000) - stub 不支持
5. openvela 启动只需要一个文件：nuttx.bin@0x12010000（不需要 ftab、ram_patch、bootloader）
6. CH340N USB-UART RTS 引脚直连芯片复位信号线

---

## 二、SF32LB52 启动流程深度解析

### 2.1 启动架构概览

SF32LB52 采用**两级启动架构**（对于 SDK bootloader 路径）或**直接到应用路径**（对于 openvela 路径）：

上电/复位
    |
    v
+------------------+
| ROM Bootloader   |  <- 掩膜 ROM（不可修改，在芯片内）
| (SFBL)           |
| 输出 SFBL        |
+--------+---------+
         |
         v
+------------------------+
| 检查 BMR 寄存器        |  <- __HAL_SYSCFG_GET_BOOT_MODE()
| (bootmode 引脚状态)    |
+--------+---------------+
         |
    +----+----+
    |         |
    v         v
 BMR=0      BMR=1
 正常启动    下载模式
    |         |
    v         v
+--------+  +------------------+
| 读取   |  | 等待 UART        |
| Flash  |  | DFU 命令         |
| 表     |  | (1 秒窗口)       |
+--------+  +------------------+
    |
    v
+------------------------+
| 找到有效镜像？          |
+----+-------------------+
     |         |
   是          否
     |         |
     v         v
  跳转到      进入下载
  应用        模式（自动回退）

### 2.2 两种启动模式

| 模式 | BMR 值 | 行为 | 触发方式 |
|------|--------|------|----------|
| **正常启动** | SYSCFG_BOOT_NORMAL (0) | 验证 ftab，定位活动镜像，跳转执行 | BOOT 引脚接地（但黄山派无此引脚） |
| **下载模式** | SYSCFG_BOOT_UART (1) | 等待 UART DFU 命令 | BOOT 引脚接高电平 / sftool 复位触发 |

### 2.3 SFBL 启动序列（ROM Bootloader）

**关键时序**：

1. **输出 SFBL**：在 UART1 上以 1,000,000 波特率输出
2. **1 秒监听窗口**：BOOT_MODE_DELAY = 1000000 微秒
3. **决定启动模式**：根据 BMR 寄存器值
4. **验证镜像**：检查 ftab 和镜像头部
5. **跳转执行**：加载 SP 和 PC，跳转到应用

**源码位置**：
- sifli-sdk-bt/example/boot_loader/project/butterflmicro/board/bf0_ap_hal_msp.c：第 43-57 行

### 2.4 启动决策树

ROM Bootloader 启动
    |
    v
检查 BMR 寄存器 (hwp_hpsys_cfg->BMR)
    |
    |-- BMR == 1 (SYSCFG_BOOT_UART) -> 直接进入下载模式
    |
    +-- BMR == 0 (SYSCFG_BOOT_NORMAL) -> 继续检查
        |
        v
    检查 RAM Hook (hwp_hpsys_aon->RESERVE0)
        |
        |-- hook != NULL -> 跳转到 hook 地址（软件覆盖）
        |
        +-- hook == NULL -> 继续检查 Flash
            |
            v
        验证 Flash 配置表 (ftab)
            |
            |-- 魔数有效 (0x53454346)？
            |-- running_imgs[CORE_HCPU] 有效？
            |-- 镜像长度有效 (不是 0xFFFFFFFF)？
            |
            |-- 全部通过 -> 跳转到应用
            |
            +-- 任一失败 -> 自动进入下载模式

### 2.5 关键寄存器和地址

| 项目 | 地址/寄存器 | 说明 |
|------|------------|------|
| 启动模式寄存器 (BMR) | hwp_hpsys_cfg->BMR 位 0 | 0=正常启动，1=下载模式 |
| 启动模式常量 | SYSCFG_BOOT_NORMAL=0, SYSCFG_BOOT_UART=1 | 定义在 bf0_hal.h:100-101 |
| 启动模式读取宏 | __HAL_SYSCFG_GET_BOOT_MODE() | 定义在 bf0_hal.h:132 |
| Flash 配置表 | 0x10000000 | 片内 flash，存放 ftab |
| Ftab 魔数 | 0x53454346 (SECF) | 定义在 dfu.h:390 |
| 未初始化 Flash | 0xFFFFFFFF | 定义在 dfu.h:392 |
| RAM Hook 寄存器 | hwp_hpsys_aon->RESERVE0 | 软件启动覆盖机制 |
| UART 下载端口 | UART1 (黄山派) | 1,000,000 波特率 |
| 下载窗口 | ~1 秒 (BOOT_MODE_DELAY=1000000us) | ROM bootloader 监听时间 |
| 应用入口 (黄山派) | 0x12010000 (外部 QSPI) | XIP 执行地址 |
| 应用入口 (SDK 默认) | 0x10020000 (片内 flash) | 默认启动地址 |

---

## 三、已知问题根因分析

### 3.1 核心问题：应用在 HAL_Init() 和 nx_start() 之间崩溃

**症状**：
- 启动序列：SFBL -> SFBL -> C -> SFBL -> C -> SFBL（约 11 秒周期）
- C 表示 HAL_Init() 完成（ABCD 启动序列的第三阶段）
- 从未看到 D 表示 nx_start() 未执行

**可能原因（按可能性排序）**：

#### 1. 电源问题（最常见）

**原因**：CO5300 AMOLED 显示屏在初始化期间消耗大量电流，导致 USB 电源轨电压下降。

**证据**：
- 官方 README 明确指出：如果板子只输出 SFBL 循环，说明 AMOLED 屏幕拉低了 USB 电源轨
- 11 秒延迟可能表示电源不足导致的重复尝试

**验证方法**：
1. 断开 AMOLED FPC 连接器
2. 使用 5V/2A 充电器或电池供电
3. 重新测试启动

#### 2. MPI1 PSRAM 初始化冲突

**原因**：openvela 配置 MPI1 为 PSRAM 模式，但 ROM bootloader 可能期望 NOR Flash。

**证据**：
- bsp_init.c 中的 board_init_psram() 可能失败或冲突
- 交接文档建议：尝试在 defconfig 中禁用 PSRAM

**验证方法**：
在 defconfig 中添加：
# CONFIG_BSP_USING_PSRAM is not set

#### 3. 板级初始化崩溃

**原因**：bsp_init.c 或 board_app_initialize 中的某些初始化失败。

**证据**：
- 应用启动但崩溃在 NuttX 内核启动前
- 可能是外设驱动初始化失败（LCD、触摸、充电 IC 等）

**验证方法**：
启用调试输出：
CONFIG_DEBUG_FEATURES=y
CONFIG_DEBUG_ERROR=y
CONFIG_DEBUG_WARN=y
CONFIG_ARCH_STACKTRACE=y

#### 4. 11 秒延迟的含义

**分析**：
- SFBL 出现 -> 11 秒延迟 -> C 出现 -> SFBL 再次出现
- 这表明 ROM bootloader 在下载模式等待了 ~10 秒，然后尝试启动，崩溃后复位
- 黄山派不暴露 BOOT 引脚，但 ROM bootloader 仍可能进入下载模式

**可能解释**：
- 电源不足导致 ftab 验证失败，自动回退到下载模式
- 或者 ftab 本身有问题（但 ADR-0008 指出 openvela 不需要 ftab）

### 3.2 SFBL 循环问题

**症状**：板子只输出 SFBL 循环，没有 ABCD。

**可能原因**：

1. **电源不足**：AMOLED 拉低 USB 电源轨
2. **镜像损坏**：Flash 中没有有效的应用镜像
3. **错误的 Flash 偏移**：应用必须在 0x12010000
4. **RTS-to-RST 干扰**：串口工具在连接时断言 RTS，保持芯片复位
5. **Flash 芯片检测失败**：ROM bootloader 无法与 NOR Flash 通信

**解决方案**：
- 使用 5V/2A 充电器
- 重新烧录固件
- 使用正确的串口命令：picocom -b 1000000 --noreset --lower-rts --lower-dtr


---

## 四、sftool 烧录工作原理

### 4.1 硬件基础：RTS-to-RST 路径

黄山派的 CH340N USB-UART 桥接芯片的 **RTS 引脚直连到 SF32LB52 的复位信号**（低电平有效）。

**作用**：
- PC 端工具可以软件触发硬件复位
- 自动烧录和自测都依赖这个走线
- 与 ESP-IDF / esptool.py 在 ESP32 板上的 auto-reset 机制完全相同

### 4.2 sftool 连接序列

sftool 使用 --before default_reset 标志触发 bootloader 入口序列：

1. **断言 RTS（低电平）**：拉低 SF32LB52 的 RESET 引脚，强制硬件复位
2. **释放 RTS（高电平）**：释放复位，芯片开始从 ROM 启动
3. **ROM bootloader 启动**：在 UART1 上以 1M 波特率输出 SFBL
4. **1 秒监听窗口**：ROM bootloader 等待下载命令
5. **sftool 发送连接握手**：在 1 秒窗口内，sftool 发送 ATSF32 连接命令
6. **进入下载模式**：如果握手在窗口内收到，bootloader 保持在下载模式并接受 DFU 命令

### 4.3 关键命令

**烧录命令**：
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000        --before default_reset --after soft_reset        write_flash nuttx.bin@0x12010000

**参数说明**：
- -c SF32LB52：指定芯片型号
- -p /dev/ttyUSB0：指定串口
- -b 1000000：波特率 1M
- --before default_reset：烧录前复位芯片
- --after soft_reset：烧录后软重启
- write_flash nuttx.bin@0x12010000：写入固件到指定地址

**兼容模式**（通信不稳定时使用）：
sftool -c SF32LB52 -p /dev/ttyUSB0 --compat        write_flash nuttx.bin@0x12010000

### 4.4 常见失败原因

| 失败信息 | 原因 | 解决方案 |
|----------|------|----------|
| Failed to connect to the chip | 错过了 ROM bootloader 的 ~1 秒监听窗口 | 拔掉 USB，等待 2 秒，重新插入，立即重试 |
| 超时或校验失败 | 通信不稳定 | 添加 --compat 标志 |
| 写入验证失败 | Flash 写入错误 | 重新烧录，检查电源 |

### 4.5 自动化测试

RTS-to-RST 路径使全自动 CI / pytest / expect 测试成为可能：

import serial, time
ser = serial.Serial('/dev/ttyUSB0', 1_000_000, timeout=0.5)
ser.rts = True;  time.sleep(0.05)        # 触发硬件复位
ser.rts = False; time.sleep(0.5)         # 释放，板子开始 boot
ser.read_until(b"nsh> ")                 # 等待 NSH 提示符
ser.write(b"ostest\r\n")
out = ser.read_until(b"PASSED\n")
assert b"PASSED" in out


---

## 五、VelaWear Agent 项目架构

### 5.1 核心概念

| 概念 | 定义 | 说明 |
|------|------|------|
| **Agent（智能体）** | 持续运行的核心守护进程 | 主动感知用户状态、做出决策、执行动作 |
| **Event（事件）** | 系统中发生的事情 | 轮询事件（传感器读取）和推送事件（BLE 消息） |
| **State（状态）** | 设备和用户的当前情况 | 设备状态、用户状态、环境状态 |
| **Decision（决策）** | Agent 根据状态和事件做出的判断 | 硬编码规则、可配置规则、LLM 推理 |
| **Action（动作）** | Agent 执行的操作 | 显示、音频、通信、系统动作 |
| **Skill（技能）** | Agent 的特定能力模块 | fitness、health、notification |
| **Task（任务）** | Agent 需要执行的工作单元 | 定时任务、事件驱动任务、条件触发任务 |

### 5.2 三层架构

Agent
+-- 感知层（Sensors）
|   +-- IMU -> 运动状态
|   +-- 心率 -> 健康状态
|   +-- BLE -> 外部消息
+-- 决策层（Decision Engine）
|   +-- 硬编码规则 -> 紧急响应
|   +-- 可配置规则 -> 日常决策
|   +-- LLM 推理 -> 复杂分析
+-- 执行层（Action Manager）
    +-- LCD -> 显示反馈
    +-- Audio -> 声音反馈
    +-- BLE -> 通信反馈

### 5.3 与普通嵌入式应用的区别

| 维度 | 普通嵌入式应用 | Agent 应用 |
|------|---------------|------------|
| 触发方式 | 用户点按钮 -> 代码执行 | 用户说话 -> AI 理解 -> 选择工具 -> 执行 |
| 主动性 | 被动响应 | 主动感知、主动提醒 |
| 决策能力 | 硬编码逻辑 | 规则 + LLM 推理 |
| 执行能力 | 有限预定义动作 | Tool/Shell 调用，可扩展 |

### 5.4 关键技术选型

| 技术 | 选型 | 说明 |
|------|------|------|
| **LLM 集成** | openvelaClaw Client | 连接远程 LLM |
| **主动任务** | cron_service | 独立 pthread，持久化，精确到秒 |
| **数据持久化** | cJSON + 文件系统 | 轻量级，适合嵌入式 |
| **语音交互** | voice_channel PTT + ASR | 按键触发语音输入 |
| **UI 框架** | LVGL + lv_tileview | 多页面水平滑动 |

### 5.5 评分标准对应

| 评分指标 | 分数 | VelaWear 对应 |
|----------|------|---------------|
| 技术难度 | 30 | 多源事件融合、决策引擎、低功耗优化 |
| 产品创新性 | 20 | 主动感知、主动提醒、多模态交互 |
| 项目完整度 | 20 | 完整的 Agent 架构、可运行 Demo |
| AI 开发 | 10 | ai_agent 集成、自定义 Skill/Tool |
| 商业潜力 | 10 | 穿戴设备市场、健康应用场景 |
| 展示效果 | 10 | 路演和 PPT 呈现 |

---

## 六、硬件能力总结

### 6.1 SF32LB52 芯片特点

- **CPU**：双核 big.LITTLE Arm Cortex-M33 STAR-MC1
  - HCPU：240 MHz（图形、音频、神经网络加速）
  - LCPU：48 MHz（BLE 协议栈、实时任务、待机管理）
- **蓝牙**：双模蓝牙 5.3
- **图形**：高性能 2D/2.5D 图形引擎
- **AI**：神经网络加速器 (NNACC)
- **音频**：内置音频 CODEC
- **电源**：内置 PMU 和充电管理（锂电版本）

### 6.2 黄山派开发板外设

| 外设 | 驱动 | 设备节点 | 说明 |
|------|------|----------|------|
| 1.85" 390x450 AMOLED | co5300 + sf32lb_lcd | /dev/lcd0、/dev/fb0 | QSPI 接口 |
| FT6146 电容触控 | ft6146 | /dev/input0 | I2C1 接口 |
| AW32001 充电 IC | bsp_power.c | /dev/i2c1 | I2C2 @0x49 |
| KEY1 / KEY2 按键 | sf32lb52_buttons | /dev/buttons | GPIO |
| ADC0（8 通道，12 位） | sf32lb_adc | /dev/adc0 | 模拟输入 |
| RTC | sf32lb_rtc | /dev/rtc0 | 实时时钟 |
| 看门狗 | sf32lb_iwdg | /dev/watchdog0 | 系统监控 |
| 硬件定时器 | sf32lb_tim | /dev/timer0 | 精确定时 |
| 内置 NOR（16 MB） | sf32lb_flash MTD | /dev/config0 | MPI2 总线 |
| USB CDC ACM | cdcacm | /dev/ttyACM0 | USB 设备 |
| UART1 控制台 | sf32lb_uart | /dev/console、/dev/ttyS0 | CH340N，1 Mbps |

### 6.3 GPIO 引脚映射

| 功能 | GPIO |
|------|------|
| UART1 RX / TX（控制台） | PA18 / PA19 |
| 触摸 I2C1 SDA / SCL | PA33 / PA37 |
| 触摸 INT / RST | PA41 / PA09 |
| 充电 I2C2 SDA / SCL | PA11 / PA10 |
| QSPI LCD CS / CLK / TE | PA03 / PA04 / PA02 |
| QSPI LCD D0 / D1 / D2 / D3 | PA05 / PA06 / PA07 / PA08 |
| LCD reset / VADD_EN | PA00 / PA01 |
| VSYS / VCC_3V3 电源开关 | PA38 / PA26 |
| KEY1 / KEY2 | PA34 / PA43 |
| RGB LED（PWM） | PA32 |


---

## 七、下一步行动计划

### 优先级 1：修复启动崩溃（紧急）

#### 步骤 1：隔离电源问题
1. 断开 AMOLED FPC 连接器
2. 使用 5V/2A 充电器或电池供电
3. 重新烧录并测试启动

#### 步骤 2：启用调试输出
在 vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig 中添加：
CONFIG_DEBUG_FEATURES=y
CONFIG_DEBUG_ERROR=y
CONFIG_DEBUG_WARN=y
CONFIG_ARCH_STACKTRACE=y

#### 步骤 3：重新编译并烧录
cd openvela
cmake -B cmake_out/lckfb_huangshan_pi -S "$PWD/nuttx" -GNinja   -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh   -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/lckfb_huangshan_pi

sftool -c SF32LB52 -p /dev/ttyUSB1 --before default_reset --after soft_reset   --compat true   write_flash --verify   cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000

#### 步骤 4：捕获崩溃信息
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB1
# 按 RESET 按钮，观察完整启动日志

### 优先级 2：禁用 PSRAM 测试

如果步骤 1 未解决问题，尝试禁用 PSRAM：
# CONFIG_BSP_USING_PSRAM is not set

### 优先级 3：最小化启动测试

创建一个最小 defconfig，只保留 UART 和基本外设：
- 禁用 LCD、触摸、传感器
- 禁用 LVGL
- 禁用 AI agent
- 只保留 UART 控制台

### 优先级 4：启用蓝牙

用户明确要求蓝牙功能。在 defconfig 中检查并启用：
CONFIG_BT_ENABLED=y
CONFIG_BT BLE=y

### 优先级 5：11 秒延迟调查

- 记录每次 SFBL 和 C 之间的确切时间
- 检查是否有其他输出被遗漏
- 考虑使用逻辑分析仪抓取 UART 信号

---

## 八、风险和注意事项

### 8.1 安全风险

1. **下载模式无认证**：1 秒监听窗口接受任何 UART 连接，无需认证
2. **自动回退可利用**：擦除 ftab 可强制芯片每次启动都进入下载模式
3. **RAM Hook 是持久化向量**：恶意代码可设置 hwp_hpsys_aon->RESERVE0 重定向启动

### 8.2 开发风险

1. **电源敏感性**：ROM bootloader 的 flash 验证对电源质量敏感
2. **RTS 干扰**：串口工具在连接时断言 RTS 会保持芯片复位
3. **时序敏感**：1 秒下载窗口很紧凑，错过需重新插拔 USB
4. **ftab 损坏风险**：如果 0x10000000 的 ftab 损坏，芯片每次启动都进入下载模式

### 8.3 编译风险

1. **分支依赖**：板级适配仅在 dev-ai-contest-2026 分支可编译
2. **工具链版本**：需要 arm-none-eabi-gcc >= 10.3，cmake >= 3.22，ninja >= 1.10
3. **预编译库兼容性**：quickapp 预编译库与当前 GUI 框架不兼容，需禁用

---

## 九、总结

### 9.1 项目现状

VelaWear Agent 项目处于 **CRITICAL 状态**，核心问题是 SF32LB52 开发板在启动时崩溃，无法进入 NuttX 内核。应用程序确实启动了（可以看到 HAL_Init() 完成），但在 nx_start() 之前崩溃。

### 9.2 根本原因

最可能的原因是 **电源问题**（AMOLED 显示屏拉低 USB 电源轨）或 **MPI1 PSRAM 初始化冲突**。

### 9.3 关键发现

1. **SF32LB52 启动流程**：ROM bootloader (SFBL) -> 验证镜像 -> 跳转应用
2. **openvela 启动路径**：只需要 nuttx.bin@0x12010000，不需要 ftab
3. **sftool 工作原理**：利用 RTS-to-RST 硬件路径触发复位，在 1 秒窗口内连接
4. **项目架构**：基于 ai_agent 框架，三层架构（感知、决策、执行）

### 9.4 下一步

**立即行动**：
1. 断开 AMOLED，使用 5V/2A 电源，重新测试
2. 启用调试输出，捕获崩溃信息
3. 如果仍失败，禁用 PSRAM 测试

**短期目标**：
1. 修复启动崩溃，使 NuttX 能正常启动
2. 启用蓝牙功能
3. 验证基本外设（UART、按键、ADC）

**中期目标**：
1. 集成 ai_agent 框架
2. 开发 VelaWear Agent 核心功能
3. 实现主动感知和提醒机制

**长期目标**：
1. 完成完整的 Agent 架构
2. 实现多模态交互（语音、显示、BLE）
3. 准备比赛路演和演示

---

## 十、参考资料

### 本地文档
1. /home/cmj/桌面/1/docs/sf32lb52-sfbl-recovery.md - SFBL 恢复指南
2. /home/cmj/桌面/1/docs/sf32lb52-sfbl-troubleshooting-zh.md - 故障排除（中文）
3. /home/cmj/桌面/1/docs/sf32lb52-bootloader-software-entry.md - 启动机制深入分析
4. /home/cmj/桌面/1/docs/sifli-chip-research.md - 芯片研究
5. /home/cmj/桌面/1/contest2026_329_dela/CONTEXT.md - 项目上下文和构建命令
6. /home/cmj/桌面/1/contest2026_329_dela/CLAUDE.md - Claude 指令
7. /home/cmj/桌面/1/contest2026_329_dela/docs/research.md - 研究文档
8. /home/cmj/桌面/1/contest2026_329_dela/docs/adr/0008-ftab-for-boot.md - ftab ADR
9. /tmp/handoff-329-dela-2026-08-15.md - 交接文档
10. /home/cmj/桌面/1/contest2026_329_dela/openvela/vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md - 板级文档

### 外部资源
1. SiFli Wiki (https://wiki.sifli.com) - SF32LB52 芯片文档
2. 黄山派 Wiki (https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/) - 硬件文档
3. sftool GitHub (https://github.com/OpenSiFli/sftool) - 烧录工具
4. openvela 文档 (https://doc.openvela.com) - RTOS 文档
5. NuttX 调试指南 (https://nuttx.apache.org/docs/latest/guides/debugging.html)

---

*研究总结完成：2026-08-15*
*下一步：优先修复启动崩溃问题*

