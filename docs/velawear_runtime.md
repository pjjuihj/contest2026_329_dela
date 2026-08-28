# VelaWear Agent 实机验证记录

更新时间：2026-08-28；硬件：SF32LB52 黄山派；供电：USB；串口：/dev/ttyUSB0。

## 已验证

- CONFIG_LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT=y 已加入黄山派 nsh 配置，manifest 通过 linkfile 纳入应用。
- 固件按 SiFli ROM SFBL 流程启动，镜像使用平坦 XIP 地址 0x12010000，不额外烧录 bootloader/FTAB。
- 2026-08-28 修复版编译成功：Ninja 40/40，status=0；当前产物大小和 SHA-256 以 `docs/contest2026_submission_checklist.md` 的最新记录为准。
- 写入和 verify 成功：
  sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --before default_reset --after soft_reset --connect-attempts 10 --compat true write_flash --no-compress --verify cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
- nsh> velawear 已验证：配置、事件管理器、状态管理器、决策引擎、动作管理器、IMU、LLM 初始化成功，事件线程和动作线程均启动。
- 当前未接音频输入设备，/dev/audio/pcm_in0 打开失败会记录 warning，但按设计降级为 audio disabled，不阻止 Agent 启动。

## 修复记录

### 1. 消息队列断言

黄山派配置 CONFIG_MQ_MAXMSGSIZE=32，原实现把完整 velawear_event_t（约 600 B）直接作为 mqueue 消息，触发 mq_msgqalloc.c 断言。现在 mqueue 只传 uintptr_t 指针，完整事件复制到堆上，由接收线程释放。初始化时先 unlink 旧队列，避免崩溃后残留旧指针。

### 2. IMU 设备访问

实际设备节点是 /dev/lsm6dsl0。初始化使用 SNIOC_START，读取使用官方驱动的 SNIOC_LSM6DSLSENSORREAD ioctl，不再使用不匹配的 read() 路径。

### 3. 无心率传感器时的误告警

状态管理器以 0.0 表示 无有效心率数据。决策规则现在先判断心率大于 0，再判断高于 180 或低于 40，避免在未接心率传感器时重复输出Heart rate critical: 0.0。

## 串口注意事项

RTS-to-RST 已连接，打开串口可能触发一次复位。脚本读取时应保持 RTS/DTR 为低，并等待 nsh> 后再发送命令：

  s = serial.Serial('/dev/ttyUSB0', 1000000, timeout=0.2)
  s.rts = False
  s.dtr = False

2026-08-28 复验中，sftool 使用 `-b 1000000` 对完整 `nuttx.bin` 写入并 verify 成功（status=0）；该目标后续烧录保持 1,000,000 波特率。串口回归也使用 1,000,000 波特率。

## 尚未纳入本次验收

- 音频输入：硬件未接入，保留降级路径；扬声器/麦克风的实际声音证据仍未采集。
- AW32001/I2C：按当前硬件连接情况暂不扫描。
- 按键、RTC、PWM：沿用已完成的黄山派基线验收；LCD/触摸已接入 VelaWear 启动回归，但可见 UI、触摸动作和振动仍需人工视频/现场验收。
- BLE 独立验收：虚拟机虽识别 VMware Bluetooth Adapter，但 `hci0` 当前 DOWN，启用返回 `Function not implemented (38)`，未形成独立扫描结果；仍需手机或实体 BLE 扫描器验证广播、连接、CCCD 和特征值往返。


### 4. LCD/LVGL 状态页

VelaWear 已按官方 lvgldemo 的同线程模型接入 LVGL：主线程执行 lv_init、lv_nuttx_init 和 lv_timer_handler，显示设备为 /dev/lcd0，触摸输入为 /dev/input0。启动后创建 VelaWear 状态页，显示运动状态、电池状态和心率有效性。

应用栈配置为 40960 B；实机检测到可用栈约 40856 B，满足 LVGL 要求的 32768 B 最小值。实机日志确认 LCD 和触摸设备均 open success，且无 LVGL 栈警告。
