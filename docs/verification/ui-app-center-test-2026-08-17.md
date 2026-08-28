# VelaWear 应用中心验证记录

日期：2026-08-18

## 构建

- 工程：`/home/cmj/桌面/1/contest2026_329_dela/openvela`
- 构建目录：`cmake_out/lckfb_huangshan_pi`
- 命令：`ninja -C /home/cmj/桌面/1/contest2026_329_dela/openvela/cmake_out/lckfb_huangshan_pi -j8`
- 结果：通过
- Flash：4,633,916 B / 16 MB = 27.62%
- SRAM：179,956 B / 512 KB = 34.32%
- 备注：保留链接器 RWX segment 警告；无编译/链接错误。

## 烧录

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --compat true \
  --connect-attempts 0 --before default_reset --after soft_reset \
  write_flash /home/cmj/桌面/1/contest2026_329_dela/openvela/cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

结果：100% 完成，板子软复位进入 `nsh>`。

## 复位黑屏修复

复现时复位日志只有 `SFBL`、`NuttShell` 和 `nsh>`，没有 `[Display]`；根因是板级 `src/etc/init.d/rcS` 原来只有 `sleep 3`，没有启动应用。已增加：

```sh
sleep 3
velawear &
```

修复后复位日志出现 `velawear [8:100]`，随后出现 `[Display] Watchface and APP center initialized` 和 `[Buttons] KEY2 input enabled`，无需手动输入 `velawear`。

## 串口启动

串口：`/dev/ttyUSB0`，1,000,000 bps，8N1，RTS/DTR 拉低。

启动日志确认：

- `Display] Watchface and APP center initialized`
- `[Buttons] KEY2 input enabled`
- `lv_freetype_init` 报告系统字体引擎已由 UIKIT 预初始化；应用成功创建 `/etc/data/font/MiSans-Regular.ttf`，未出现 `MiSans Chinese font unavailable`。
- LCD `/dev/lcd0` 和触摸 `/dev/input0` 打开成功
- 启动阶段没有调用音频播放函数，没有自动蜂鸣日志
- 现有音频传感器节点 `/dev/audio/pcm_in0` 仍不可用，显示层需保留“不可用”状态

## 音频回归

### 喇叭测试复位修复

问题表现：点击 UI 喇叭测试后出现 `[AudioMusic] embedded melody begin`，随后约数秒复位，未出现 `melody feed complete`。根因是 FIFO 轮询在单核上持续忙等，既阻塞了 UI 主线程，也会饿死看门狗；原先把调用移到线程后仍可能占满同优先级 CPU。

已完成两处修复：

1. UI 音频测试改为工作线程，LVGL 主线程只负责启动、回收和更新状态。
2. 音乐 FIFO 每 256 个采样调用一次 `sched_yield()`，让主循环和看门狗获得调度机会。

隔离回归已通过：同一 `velawear_music_hw_test()` 输出 `melody feed complete`、`melody played (0)` 并回到 `nsh>`，没有新的 `SFBL`。

`velawear music`：

```text
[AudioMusic] embedded melody begin
[AudioMusic] melody feed complete
[AudioMusic] melody played (0)
```

`velawear mic`：

```text
[AudioTest] MICROPHONE: samples=32000 min=-3731 max=32715 avg_abs=129
[AudioTest] MICROPHONE: peak_abs=32715 above
[AudioTest] complete
```

## UI 验收清单

- [x] 表盘和 APP 中心已创建
- [x] APP 网格包含音频中心、音乐、运动、秒表、任务、诊断、设置、关于
- [x] 表盘、APP 网格、详情页、操作按钮、状态提示已改为中文
- [x] 使用板载 `/etc/data/font/MiSans-Regular.ttf` 渲染中文，避免默认英文字体缺字
- [x] 音频和音乐页面使用现有测试入口
- [x] 音频测试改为后台线程，完成后由 LVGL 主线程更新中文结果
- [x] 运动、秒表、任务、诊断、设置、关于页面有独立内容
- [x] 启动无自动蜂鸣（串口日志确认）
- [x] KEY2 已打开 `/dev/buttons`，短按/长按逻辑已接入
- [ ] 需要在实物屏上确认八个图标的触摸命中区域和页面视觉效果
- [ ] 需要在实物上确认 KEY2 的短按/长按动作
- [x] 中文字体版本连续运行约 70 秒，日志跨过 `mode=2`（30 s）和 `mode=3`（60 s），期间没有重启签名
- [ ] 需要在实物上确认 120 秒后滑动/按键唤醒动作

## 已知限制

1. 当前工程没有截图回传链路，最终图标布局和触摸命中仍需实物确认。
2. `/dev/audio/pcm_in0` 不存在，因此麦克风 APP 必须显示不可用；独立 `velawear mic` 硬件采样命令仍可工作。
3. 音乐/喇叭按钮调用的是现有内置旋律测试，真实文件播放列表属于后续扩展。
