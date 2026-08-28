# VelaWear BLE Agent 协议（v1）

## 目标与边界

手表负责传感器采集、离线告警和固定本地动作；手机作为 BLE 网关，负责联网调用 AI
Agent，并把模型结论转换为本协议允许的命令。蓝牙本身不提供互联网能力。

固件不接受模型生成的任意文本、脚本或函数名，只接受固定命令码。这样手机或云端异常
时，手表的 IMU、声音和本地提醒仍可独立工作。

## GATT 服务

服务 UUID：`12345678-9abc-def0-1234-56789abcdef0`

| 特征 | UUID 尾段 | 属性 | 用途 |
| --- | --- | --- | --- |
| status（既有） | `...5679` | Read / Notify | `[motion, intensity%, alert]` |
| threshold（既有） | `...567a` | Read / Write | 久坐秒数，uint16 little-endian |
| agent_event | `...567b` | Notify | 手表上报事件 |
| agent_command | `...567c` | Write | 手机下发受限命令 |
| agent_result | `...567d` | Notify | 手表返回命令执行结果 |

`agent_event` 与 `agent_result` 的 CCC 可直接订阅。当前 USB 直供 MVP 使用无需配对的
GATT 写入，以兼容 SF32LB52 当前控制器与 Windows 主机；协议仍执行命令白名单、参数校验
和序号防重放。量产前必须恢复加密写入，并加入可完成的认证交互。

## agent_event：11 字节 Notify

| 字节 | 字段 | 说明 |
| --- | --- | --- |
| 0 | version | 固定 `1` |
| 1 | type | `1` motion、`3` fall、`4` audio、`9` sedentary |
| 2-3 | sequence | uint16 little-endian |
| 4 | priority | 正常 `1`、紧急 `3` |
| 5 | intensity | 0–100 |
| 6-7 | value | uint16 little-endian；声音事件为平均绝对值 |
| 8-9 | peak | uint16 little-endian；声音事件为峰值 |
| 10 | flags | bit0=告警激活，bit1=声音活跃 |

## agent_command：6 字节 Write

字节布局：`version, command, sequence_lo, sequence_hi, argument_lo, argument_hi`。

| command | 名称 | argument 允许值 | 手表行为 |
| --- | --- | --- | --- |
| `1` | ACK_ALERT | `0` | 清除 BLE 告警状态 |
| `2` | SET_SEDENTARY_THRESHOLD | 10–3600 秒 | 更新本地久坐阈值 |
| `3` | SHOW_REMINDER | `0` stretch、`1` hydrate、`2` rest | 显示预置中文提醒并置告警状态 |

手表拒绝长度、协议版本、命令码或参数不合法的写入；还会拒绝相同或倒退的 uint16
序号。序号按模 65536 单调前进，`0xffff → 0x0000` 合法。

## agent_result：5 字节 Notify

字节布局：`version, command, sequence_lo, sequence_hi, result`。

`result`：`0` 成功、`1` 拒绝、`2` 本地执行失败。

## 手机网关接入顺序

1. 扫描并连接 `VelaWear`。
2. 订阅 `agent_event` 和 `agent_result`。
3. 将事件归一化后再发送给云端 AI Agent；跌倒等本地安全告警不能等待云端返回。
4. 将 Agent 输出映射为上表中的一个命令，分配新序号，通过 `agent_command` 写入。
5. 等待同序号 `agent_result`；超时或拒绝时在手机侧处理，不重复发送旧序号。

电脑侧无依赖协议工具：

```sh
python3 tools/velawear_agent_gateway.py command threshold --sequence 0x1234 --seconds 300
# 010234122c01

python3 tools/velawear_agent_gateway.py decode-event 0103341203640700090001
```

测试：`python3 tools/test_velawear_agent_gateway.py`。

## 当前未实现

- Android/iOS 正式 App、账户体系、云端 Agent API 和密钥管理；
- 语音识别或自由文本上屏；
- 实体手机配对、GATT 订阅、加密写入、断连重连的硬件在环验证；
- 固件烧录。当前设备不在身边，且本阶段未执行烧录。
