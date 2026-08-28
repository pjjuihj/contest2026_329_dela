# ADR-0008: Flash Table (ftab) for SF32LB52 Boot

## Status

Accepted

## Context

SF32LB52 板子在执行 `erase_chip` 后，片内 flash（0x100000）的 ftab 和 boot_patch 被擦除，导致 ROM bootloader 无法找到固件入口地址，板子只输出 "SFBL" 后无法启动。

openvela 的 NuttX 固件烧录在外部 QSPI flash 的 0x12010000 地址，但 SiFli SDK 默认的 ftab 配置指向片内 flash 的 0x10020000。

## Decision

1. 创建自定义 ftab，将 HCPU 固件地址指向 0x12010000（外部 QSPI flash）
2. 使用 SiFli SDK 的 `download.py` 脚本通过 DFU 协议写入 ftab
3. ftab 二进制文件已生成在 `/tmp/ftab_openvela.bin`

## Flash 布局

| 区域 | 地址 | 大小 | 说明 |
|------|------|------|------|
| Flash Table | 0x10000000 | 20KB | 片内 flash，存放 ftab |
| Factory Cal | 0x10005000 | 8KB | 校准数据 |
| Boot Patch | 0x10010000 | 64KB | ROM bootloader 补丁 |
| Boot Loader | 0x10020000 | 128KB | 启动加载器 |
| NuttX (openvela) | 0x12010000 | 16MB | 外部 QSPI flash |

## 自定义 ftab 配置

```json
{
    "tables": [
        {"base":0x10000000, "size":0x5000,    "xip_base":0,          "flags":0},
        {"base":0x10005000, "size":0x2000,    "xip_base":0,          "flags":0},
        {"base":0,          "size":0,         "xip_base":0,          "flags":0},
        {"base":0,          "size":0,         "xip_base":0,          "flags":0},
        {"base":0x12010000, "size":0x1000000, "xip_base":0x12010000, "flags":0},
        {"base":0x10010000, "size":0x10000,   "xip_base":0x20050000, "flags":0},
        ...
    ]
}
```

## 烧录步骤

1. 板子进入下载模式（BOOT+RESET）
2. 使用 sftool 烧录 nuttx.bin@0x12010000
3. 使用 SiFli SDK 的 download.py 写入 ftab
4. 复位板子，检查是否输出 "SFBL" + "ABCD"

## Consequences

- 需要维护自定义 ftab 配置
- 如果 openvela 的 NuttX 地址变化，需要更新 ftab
- boot_patch 可能需要单独处理（如果 ROM bootloader 有 bug）
