# vendor_sifli 独立提交清单

状态：待向公共仓库发起独立 PR。本文件不把 vendor 改动伪装成专属仓代码，也不代替公共仓库合入。

## 基线

- 上游仓库：<https://github.com/open-vela/vendor_sifli>
- 目标分支：`dev`
- VM 当前 checkout：detached HEAD `97992dc` (`fix(sf32lb52): increase LVGL demo stack`)
- 当前没有 `pjjuihj/vendor_sifli` fork；创建 fork 和推送独立公共仓 PR 需要单独授权。

## 候选功能文件

以下 13 个文件包含实际功能差异，提交前仍需由维护者逐项 review：

- `boards/sf32lb52/drivers/input/ft6146.c`：INT 双边沿，补齐触摸释放事件。
- `boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig`：VelaWear、BLE H4、网络和 ai_agent 配置。
- `boards/sf32lb52/lckfb_huangshan_pi/src/bsp_init.c`：启动早期关闭外部扬声器功放。
- `boards/sf32lb52/lckfb_huangshan_pi/src/bsp_pinmux.c`：保持 PA42 功放默认关闭。
- `boards/sf32lb52/lckfb_huangshan_pi/src/etc/init.d/rcS`：启动 VelaWear 后台任务。
- `chips/drivers/cmsis/sf32lb52x/Templates/system_bf0_ap.c`：启动/内存相关调整。
- `chips/drivers/cmsis/sf32lb52x/lcpu_patch.c`：LCPU patch 数据更新。
- `chips/drivers/cmsis/sf32lb52x/lcpu_patch_rev_b.c`：LCPU rev-B patch 数据更新。
- `chips/sf32lb52/sf32lb52_bt_adapter.c`：H4 完整帧发送。
- `chips/sf32lb52/sf32lb52_bth4.c`：HCI 广播、连接、ACL 和 reset 处理。
- `chips/sf32lb52/sifli_allocateheap.c`：避开 HCPU 配置和 mailbox 保留区。
- `chips/sf32lb52/sifli_start.c`：最早期关闭功放。
- `middleware/bluetooth/patch/sf32lb52/sf32lb52_lcpu_patch.h`：对应 LCPU patch 数据。

`boards/sf32lb52/drivers/lcd/sf32lb_lcd.c` 当前只有空白/末尾换行差异，建议排除。

## 必须排除

VM 工作树中存在 `.before-*`、`.orig`、`.rej`、`.bak`、`defconfig.minimal` 和实验日志等未跟踪文件；它们不是候选提交内容，不能进入公共仓 PR。

## 提交前门槛

1. 在 `vendor_sifli` fork 中从 `dev` 建立独立分支。
2. 对以上候选文件执行编译、冷启动和 BLE/GATT 回归，确认 LCPU patch 与 BSP 变更成套有效。
3. 由参赛者自审后向 `open-vela/vendor_sifli:dev` 发起 PR，并把公共仓 PR 链接回专属仓说明。
4. 在公共仓维护者合入前，专属仓只能声明“候选补丁已整理”，不能声明 vendor 适配已提交完成。
