# 0007 - 禁用 QuickApp 框架以解决链接错误

## 状态

已接受

## 背景

在编译 VelaWear Agent 固件时，链接阶段出现大量未定义符号错误，主要来自：
- `libquickapp.a`：缺少 GUI 函数（`gui_get_root_widget`、`gui_set_global` 等）
- `libfeature_impls.a`：缺少 prompt 函数（`prompt_cancel`、`prompt_ok`）

这些错误源于预编译的 quickapp 库与当前 GUI 框架版本不兼容。

## 决策

在 defconfig 中禁用 QuickApp 框架：
```
# CONFIG_QUICKAPP is not set
```

## 考虑的选项

1. **禁用 QuickApp**：最简单，VelaWear Agent 不依赖 quickapp 功能
2. **更新预编译库**：需要从源码重新编译 quickapp 库，工作量大
3. **提供 stub 实现**：为缺失的 GUI 函数提供空实现，可能引入运行时问题

## 结论

选择选项 1。VelaWear Agent 是独立的 NuttX 应用，不依赖 quickapp 框架。禁用 quickapp 后：
- 固件大小从 5.4MB 降至 4.6MB
- 链接错误完全消除
- 编译成功，生成可用的 nuttx.bin

## 后续工作

如果未来需要 quickapp 功能（如运行快应用），需要：
1. 更新预编译库以匹配当前 GUI 框架版本
2. 或从源码编译 quickapp 库