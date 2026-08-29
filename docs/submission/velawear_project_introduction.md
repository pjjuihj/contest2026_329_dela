# VelaWear：基于 openvela 的健康腕带智能体

版本：2026-08-29

## 1. 项目概述

VelaWear 是运行在 SF32LB52 黄山派上的健康腕带智能体。项目基于 openvela 和 ai_agent，结合 IMU、MEMS 麦克风、LCD、触摸屏、BLE/GATT 与低功耗策略，为久坐提醒、跌倒响应、健康状态提示和腕上交互提供统一的软件路径。

项目目标不是只显示传感器数据，而是让设备能够感知事件、进行规则决策、调度执行动作，并通过显示、声音、震动或 BLE 与用户和伴侣设备反馈。

## 2. 用户场景

- 久坐提醒：检测持续静止状态，生成 Wellness reminder 动作。
- 跌倒响应：通过 IMU 规则进入分阶段的应急处理路径。
- 健康状态：整合传感器事件、设备电量和 BLE 连接状态。
- 腕上交互：提供 watchface、APP center、触摸导航和 BLE/GATT 数据通道。
- 降级运行：云端不可用时保持本地规则、传感器采集和 BLE 能力，不把断网误报为对话成功。

## 3. 系统架构

```text
IMU / MEMS microphone / touch
              |
        Event Manager
              |
        Decision Engine
              |
         Action Manager
       /       |       \
    LCD     audio     vibration/BLE
              |
       XiaoZhi / PAN bridge
```

代码位于专属仓的 `app/velawear_agent`。核心模块包括事件管理、规则决策、状态管理、动作调度、IMU/音频采集、LCD/触摸、BLE/GATT、PAN/XiaoZhi 桥接和 Wellness Skill。

## 4. 已实现并验证的内容

### 4.1 固件与运行时

- VelaWear 应用已纳入 openvela CMake/Ninja 演示链。
- 代码提交：`cf62211`；验收文档提交：`0f13ccb`。
- 目标板：SF32LB52 黄山派，配置 `lckfb_huangshan_pi`。
- Agent 冷启动进入 `nsh>`，输出 `Agent started successfully`。
- 自定义 Skill 安装到 `/data/agent/skills/velawear-wellness.md`。

### 4.2 构建与烧录

- Ninja 构建：38/38 成功。
- `nuttx.bin`：5,994,568 bytes。
- SHA-256：`71be9a043790408bdf5121dcd0ad09492d2483cfb5d55894f2c2b03350251ad9`。
- 使用 `sftool` 以 1,000,000 baud 写入 `0x12010000`。
- 烧录结果：100% 写入，`--verify` 通过，软复位成功。

### 4.3 BLE 与 HIL

- HCI 配置命令 `0x2006`、`0x2008`、`0x2009`、`0x200a` 均返回 `status=0x00`。
- Windows 独立 WinRT 扫描发现 `VelaWear`，地址 `CD:AB:78:56:34:12`，RSSI 约 `-20 dBm`。
- GATT 服务 UUID：`12345678-9abc-def0-1234-56789abcdef0`。
- HIL 回归：服务、5 个特征、CCCD、通知和 UART 关联均成功，脚本输出 `[3/3] completed`。
- 证据日志：`docs/evidence/velawear_ble_hil_2026-08-29.log`。

### 4.4 音频与主机测试

- MEMS 麦克风 DMA 采样 16,000 Hz，2 秒采集 32,000 samples 成功。
- Windows companion UI/LLM bridge 测试：30 tests，全部通过。
- Agent protocol tests：通过。

## 5. AI Agent 与 Skill 设计

Wellness Skill 将健康事件解释为可执行动作，规划 `get_current_time`、`cron_add`、`cron_list` 和设备反馈链路。设备还提供 BLE/GATT 通道，便于伴侣端读取状态、设置阈值和接收通知。

当前已验证 Skill 文件安装和设备运行时路径；由于测试环境没有有效云端网络和后端凭据，真实对话、Skill 回复和主动 cron 触发尚未宣称完成。

## 6. 当前限制与最终提交前工作

以下项目仍需在最终演示前补齐：

1. 接入团队自建并维护的服务端、可用网络和有效凭据，完成一轮真实 LLM 对话。
2. 执行 `/skill` 和 `/remind`，保存 Skill 回复、cron 确认、主动触发及设备反馈的连续证据。
3. 用相机验证 LCD/触摸，用录音设备验证扬声器；当前串口日志不等同于物理验收。
4. 如宣称语音唤醒，必须使用“你好，openvela / Hello, openvela”并录制证据。
5. 按比赛要求录制不超过 5 分钟的演示视频。
6. 按公共仓库流程提交 `vendor_sifli` 板级修改，并完成 GitHub PR 自审合入。

## 7. 可复现入口

```text
构建：ninja -C cmake_out/lckfb_huangshan_pi -j4
烧录：sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 ... write_flash --verify ...
扫描：python tools/sf32lb52_ble_winrt_probe.py
HIL：python tools/sf32lb52_finish_ble_hil.py --skip-flash
```

代码仓库：<https://github.com/open-vela/contest2026_329_dela>

提交分支：`codex/velawear-hil-2026-08-29`

## 8. 开源与证据声明

项目根目录采用 Apache 2.0 许可证。本文档只引用已经保存的构建、烧录、冷启动、BLE/GATT、HIL 和主机测试证据；未把日志、初始化或软件路径当作 LCD 可见、扬声器可听、真实 LLM 或语音唤醒的证明。
