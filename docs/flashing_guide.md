# SF32LB52 黄山派 烧录调试指南

> 本文档详细介绍如何编译、烧录和调试 SF32LB52 黄山派开发板。

**来源**: [黄山派 README](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md)

---

## 一、前置准备

### 1.1 工具链版本要求

| 工具 | 最低版本 | 安装命令 |
|------|----------|----------|
| arm-none-eabi-gcc | 10.3 | `sudo apt install gcc-arm-none-eabi` |
| cmake | 3.22 | `sudo apt install cmake` |
| ninja | 1.10 | `sudo apt install ninja-build` |
| python3 | 3.10 | `sudo apt install python3` |
| sftool | 0.2.5 | 见下方安装说明 |
| picocom | - | `sudo apt install picocom` |

### 1.2 安装 sftool（烧录工具）

**方式一：下载预编译包（推荐）**

```bash
curl -L -o /tmp/sftool.tar.xz \
  https://github.com/OpenSiFli/sftool/releases/download/0.2.5/sftool-0.2.5-x86_64-unknown-linux-gnu.tar.xz
tar xf /tmp/sftool.tar.xz -C /tmp && sudo mv /tmp/sftool /usr/local/bin/
sftool --version    # 验证安装
```

**方式二：Cargo 编译安装**

```bash
cargo install sftool
```

### 1.3 串口权限

```bash
# 将用户加入 dialout 组
sudo usermod -aG dialout $USER

# 重新登录或重启生效
# 或者临时生效：
sudo chmod 666 /dev/ttyUSB0
```

---

## 二、硬件连接

### 2.1 接口说明

| 接口 | 用途 | 说明 |
|------|------|------|
| USB | 供电 + 主机串口 | CH340N USB-UART 桥接，主机对应 `/dev/ttyUSB0` |
| DBG_UART1 | 固件烧录 + 日志输出 | PA18（RX）/ PA19（TX），默认 1000000 波特率 |
| KEY1 | 用户按键 | PA34 |
| KEY2 | 用户按键 | PA43 |

> `DBG_UART1` 同时承载固件烧录和运行日志。BOOT 阶段输出 `SFBL` 可用于确认该调试通道正常；串口终端和烧录工具都应使用 1000000 波特率。

### 2.2 重要：RTS-to-RST 走线

黄山派的 CH340N USB-UART 芯片的 **RTS 引脚直连到 SF32LB52 的复位信号**（低电平有效）。

**这意味着**：
- PC 端工具可以软件触发硬件复位
- 自动烧录和自测依赖这条走线
- **不要切断或上拉这条走线**

**串口工具选择**：

| 工具 | 是否可用 | 说明 |
|------|----------|------|
| picocom | ✅ 推荐 | 支持 `--noreset --lower-rts` |
| minicom | ⚠️ 可用 | open() 时会复位一次，之后正常 |
| screen | ❌ 不可用 | 无法控制 RTS，会持续复位 |
| cu | ❌ 不可用 | 无法控制 RTS，会持续复位 |

---

## 三、编译固件

### 3.1 基本编译

```bash
# 进入 openvela 工程根目录
cd /path/to/openvela

# 编译黄山派固件
cmake -B cmake_out/lckfb_huangshan_pi -S "$PWD/nuttx" -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"

# 执行编译
cmake --build cmake_out/lckfb_huangshan_pi
```

**编译产物**：`cmake_out/lckfb_huangshan_pi/nuttx.bin`（约 1.5MB）

### 3.2 增量编译

```bash
# 修改代码后，直接重新编译（增量编译，速度快）
cmake --build cmake_out/lckfb_huangshan_pi
```

### 3.3 清理后重新编译

```bash
# 修改 defconfig 后需要清理重建
rm -rf cmake_out/lckfb_huangshan_pi
cmake -B cmake_out/lckfb_huangshan_pi -S "$PWD/nuttx" -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/lckfb_huangshan_pi
```

### 3.4 修改 defconfig

```bash
cd cmake_out/lckfb_huangshan_pi
ninja menuconfig                 # 交互式调整配置
ninja savedefconfig              # 保存到 defconfig
cp defconfig \
   ../../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig
```

---

## 四、烧录固件

### 4.1 基本烧录命令

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

> 在 `sftool 0.2.5` 中，`-b` 指定烧录/读回时实际使用的串口波特率，默认也是 `1000000`。不要为黄山派显式设为 `115200`；这会强制降低传输速度。保留 `-b 1000000` 可避免脚本或环境误配置。

**参数说明**：

| 参数 | 说明 |
|------|------|
| `-c SF32LB52` | 芯片型号 |
| `-p /dev/ttyUSB0` | 串口设备（根据实际修改） |
| `-b 1000000` | 波特率（1M） |
| `--before default_reset` | 烧录前复位芯片 |
| `--after soft_reset` | 烧录后软重启 |
| `@0x12010000` | NOR Flash 偏移地址 |

### 4.2 指定串口

```bash
# 查看可用串口
ls /dev/ttyUSB*

# 使用指定串口
sftool -c SF32LB52 -p /dev/ttyUSB1 ...
```

### 4.3 兼容模式烧录

如果遇到超时或校验失败：

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --compat true \
       write_flash cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

### 4.4 常见问题排查

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `Failed to connect to the chip` | ROM bootloader 监听窗口错过 | 重新插拔 USB 后重试 |
| 板子反复打印 `SFBL` | AMOLED 屏拉低 USB 电源 | 改用 5V/2A 充电头或电池供电 |
| 超时或校验失败 | 通信不稳定 | 加 `--compat` 参数重试 |
| 烧录成功但无法启动 | 固件损坏 | 重新编译并烧录 |

---

## 五、串口调试

### 5.1 连接串口

```bash
# picocom 连接（推荐）
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0
```

**重要参数**：

| 参数 | 说明 |
|------|------|
| `-b 1000000` | 波特率 1M |
| `--noreset` | 不在连接时复位 |
| `--lower-rts` | RTS 保持低（防止复位） |
| `--lower-dtr` | DTR 保持低 |

### 5.2 退出 picocom

```
Ctrl+A, Ctrl+X
```

### 5.3 预期启动输出

```
SFBL
ABCD
ADC calibration data missing, use defaults
INFO: NOR MTD registered at /dev/config0 (offset=2464 blocks=1024)

NuttShell (NSH)
nsh>
```

---

## 六、基础验证

在 `nsh>` 提示符下执行以下命令验证硬件：

### 6.1 系统信息

```bash
nsh> free                    # 查看内存（8MB PSRAM）
nsh> ps                      # 查看任务列表
nsh> uname -a                # 系统版本
```

### 6.2 设备列表

```bash
nsh> ls /dev
# 预期输出：
# adc0      buttons   config0   console   fb0       gpio0     gpio1
# gpio2     i2c0      i2c1      input0    lcd0      pwm0      ram0
# rtc0      spi1      timer0    ttyACM0   ttyS0     urandom   watchdog0
```

### 6.3 外设测试

```bash
# ADC 采样
nsh> adc -n 1

# RTC 时间
nsh> date

# 按键测试（按 KEY1/KEY2）
nsh> buttons 5

# LCD 显示测试（画矩形）
nsh> fb

# LVGL widgets demo + 触摸测试
nsh> lvgldemo widgets

# I2C 设备扫描
nsh> i2c dev 0x10 0x77       # FT6146 触控应在 0x38 处 ACK
nsh> i2c dev 0x40 0x4f       # AW32001 充电 IC 应在 0x49 处 ACK

# NuttX 内核测试
nsh> ostest
```

---

## 七、快速开始脚本

创建 `flash.sh` 脚本：

```bash
#!/bin/bash
# flash.sh - 编译并烧录到黄山派

set -e

PORT=${1:-/dev/ttyUSB0}
BUILD_DIR="cmake_out/lckfb_huangshan_pi"

echo "=== 编译 ==="
cmake --build $BUILD_DIR

echo "=== 烧录 ==="
sftool -c SF32LB52 -p $PORT -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash $BUILD_DIR/nuttx.bin@0x12010000

echo "=== 完成 ==="
echo "连接串口: picocom -b 1000000 --noreset --lower-rts --lower-dtr $PORT"
```

使用：

```bash
chmod +x flash.sh
./flash.sh                    # 默认 /dev/ttyUSB0
./flash.sh /dev/ttyUSB1       # 指定串口
```

---

## 八、自动化测试（CI/CD）

RTS-to-RST 走线支持全自动测试，无需物理交互：

```python
import serial
import time

# 连接串口
ser = serial.Serial('/dev/ttyUSB0', 1_000_000, timeout=0.5)

# 触发硬件复位
ser.rts = True
time.sleep(0.05)

# 释放复位，板子开始 boot
ser.rts = False
time.sleep(0.5)

# 等待 NSH 提示符
ser.read_until(b"nsh> ")

# 执行测试命令
ser.write(b"ostest\r\n")
out = ser.read_until(b"PASSED\n")

# 验证结果
assert b"PASSED" in out
print("Test passed!")
```

---

## 九、开发工作流

### 9.1 日常开发流程

```
修改代码 → 编译 → 烧录 → 测试 → 循环
```

### 9.2 完整命令序列

```bash
# 1. 修改代码
vim app/velawear_agent/src/velawear_main.c

# 2. 编译
cmake --build cmake_out/lckfb_huangshan_pi

# 3. 烧录
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000

# 4. 连接串口测试
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0

# 5. 在 nsh> 下运行
nsh> velawear
```

### 9.3 调试技巧

| 技巧 | 说明 |
|------|------|
| `printf` 调试 | 使用 `syslog()` 或 `printf()` 输出调试信息 |
| `ps` 命令 | 查看任务状态和栈使用 |
| `free` 命令 | 监控内存使用 |
| `dmesg` 命令 | 查看内核日志 |
| 按键中断 | 使用 KEY1/KEY2 触发特定调试功能 |

---

## 十、参考链接

- [黄山派 Wiki](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/)
- [sftool GitHub](https://github.com/OpenSiFli/sftool)
- [picocom 手册](https://github.com/npat-efault/picocom)
- [NuttX 调试指南](https://nuttx.apache.org/docs/latest/guides/debugging.html)
