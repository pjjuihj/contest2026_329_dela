# VelaWear Agent 开发研究

> 本文档整理了 VelaWear Agent 开发所需的技术资料，基于 openvela 官方文档和源码。

---

## 一、ai_agent 框架架构

### 1.1 核心组件

ai_agent 是运行在 openvela 嵌入式系统上的 AI Agent 框架，专为手表、眼镜、音箱等小型设备设计。

**核心差异化能力**：

| 能力 | 说明 |
|------|------|
| **主动任务机制** | Agent 不等用户开口，根据时间和条件主动推送 |
| **Router 意图路由** | 区分「记一下」vs「提醒我」vs「总结一下」，智能分流 |
| **Tool/Shell 调用** | 读传感器、写文件、发通知、执行命令——Agent 能动手 |
| **自然语言→结构化输出** | 语音输入自动解析为待办/日程/备忘 |
| **多渠道 + 多设备协作** | 飞书、微信、语音、MQTT、Node 协议 |

**来源**: [AI 硬件产品创新赛道详细指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_hardware_track_guide.md)

### 1.2 三层架构

开发应用时，主要和这三层打交道：

1. **openvela + LVGL**：提供显示、输入、事件循环——应用跑在这上面
2. **ai_agent 框架**：提供 Router、Tool、主动任务、Skills——应用调用这些能力
3. **接入通道**：用户通过微信/飞书/语音和应用交互

**来源**: [ai_agent 应用开发上手指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_agent_quickstart.md)

### 1.3 与普通嵌入式应用的区别

- 普通应用：「用户点按钮 → 代码执行」
- Agent 应用：「用户说话 → AI 理解 → 选择工具 → 执行 → 可能还主动推送」

---

## 二、SF32LB52 黄山派硬件能力

### 2.1 开发板概述

黄山派是立创开发板（LCKFB）发售的基于 SF32LB52 的学习/可穿戴套件，源自思澈 **SF32LB52-LCHSPI-ULP** 参考设计。

**芯片特点**：低功耗双模蓝牙 + 自研 GPU + 多媒体 + 集成屏幕和传感器

**来源**: [黄山派 README](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md)

### 2.2 支持的外设

| 外设 | 驱动 | 设备节点 |
|------|------|----------|
| 1.85" 390×450 AMOLED（CO5300，QSPI） | `co5300` + `sf32lb_lcd` | `/dev/lcd0`、`/dev/fb0` |
| FT6146 电容触控（I2C1） | `ft6146` | `/dev/input0` |
| AW32001 充电 IC（I2C2 @0x49） | `bsp_power.c`（内核态） | `/dev/i2c1`（裸 I2C） |
| KEY1 / KEY2 按键 | `sf32lb52_buttons` | `/dev/buttons` |
| ADC0（8 通道，12 位） | `sf32lb_adc` | `/dev/adc0` |
| RTC | `sf32lb_rtc` | `/dev/rtc0` |
| 看门狗 | `sf32lb_iwdg` | `/dev/watchdog0` |
| 硬件定时器 | `sf32lb_tim` | `/dev/timer0` |
| 内置 NOR（16 MB，MPI2 总线） | `sf32lb_flash` MTD | `/dev/config0` |
| USB CDC ACM 设备 | `cdcacm` | `/dev/ttyACM0` |
| UART1 控制台（CH340N，1 Mbps） | `sf32lb_uart` | `/dev/console`、`/dev/ttyS0` |

**来源**: [黄山派 README - 支持的外设](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md)

### 2.3 GPIO 引脚映射

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

### 2.4 编译和烧录

**编译**：
```bash
cmake -B cmake_out/lckfb_huangshan_pi -S "$PWD/nuttx" -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/lckfb_huangshan_pi
```

**烧录**：
```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

**串口连接**：
```bash
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0
```

### 2.5 烧录调试通道核验（2026-08-16）

- 官方板级 README 将该通道称为 **UART1 控制台**：经 CH340N 连接，速率为 1 Mbps，系统节点为 `/dev/console` 和 `/dev/ttyS0`。其 GPIO 表按 RX/TX 顺序给出 `PA18 / PA19`，即 RX=PA18、TX=PA19。官方 README 未使用 `DBG_UART1` 这一名称。
- 官方烧录示例对同一主机串口 `/dev/ttyUSB0` 显式指定 `-b 1000000`，并将平面 XIP 镜像写到 NOR `0x12010000`。ROM bootloader 为 `SFBL`，没有独立 bootloader 或分区表；CH340N 的 RTS 直连低有效复位，自动烧录依赖此走线。
- 控制台配置为 1,000,000 8N1 且关闭流控。RTS 复位后，预期先看到 `SFBL`，再进入 NuttX/NSH。

**证据边界与版本差异**：

- 板级 README 证明的是**推荐烧录命令显式使用** 1,000,000，而不是 `sftool` 的默认值。
- 工具官方 `sftool` 0.2.5 README 将 `-b` 定义为烧录/读回的波特率，默认值为 1,000,000，并将 `--compat` 记录为裸布尔开关。
- 当前 VM 中 `sftool 0.2.5 --help` 显示 `--compat <COMPAT>`，可选值为 `true`/`false`。因此此 VM 上应使用 `--compat true`；在其他环境先运行 `sftool --help`，并按本机显示的语法构造命令。`-b 115200` 会在该 VM 上显式覆盖 1,000,000 的默认值并降低传输速度。

**来源**：

- [黄山派 README：UART1 控制台与 GPIO 映射](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md#L60-L67)
- [黄山派 README：烧录、SFBL 与 RTS 复位](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md#L103-L110)
- [黄山派 README：标准烧录命令](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md#L147-L150)
- [黄山派 README：控制台配置与启动输出](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md#L167-L177)
- [sftool 0.2.5 README：全局选项](https://github.com/OpenSiFli/sftool/blob/0.2.5/README.md#L249-L257)

---

## 三、自定义 Skill 和 Tool

### 3.1 Skill 系统

Skills 告诉 ai_agent 在特定场景下「该怎么做」。和工具的区别：工具是「能做事」，技能是「知道怎么做」。

**内置 Skills**：weather（天气）、daily-briefing（每日简报）、reminder（提醒）、translate（翻译）、news-digest（新闻摘要）

**来源**: [ai_agent 应用开发上手指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_agent_quickstart.md)

### 3.2 Tool 系统

**内置工具一览**：

| 类别 | 工具 | 能做什么 |
|------|------|----------|
| 搜索 | web_search | 搜索网页 |
| 文件 | read_file, write_file | 读写文件 |
| 定时 | cron_add, cron_list | 添加/查看定时任务 |
| 视觉 | analyze_image | 分析图片 |
| 设备 | get_battery, get_heartrate | 获取设备状态 |
| 飞书 | feishu_doc_create | 创建飞书文档 |
| 音乐 | music_play, music_search | 播放/搜索音乐 |

### 3.3 开发自定义应用的两种模式

**模式 A：基于设备的通信协议和云端大模型，独立开发 AI 场景应用**
- 适合：熟悉 LLM 云端通信以及设备端通信协议的使用
- 参考实现：ai_chat demo

**模式 B：基于 ai_agent 框架开发 LVGL 应用或快应用**
- 创建独立的 LVGL 应用或快应用，通过 ai_agent 的消息总线交互
- 适合：了解 agent 开发、需要更灵活的 LLM 调用和丰富的工具扩展能力
- 参考实现：mini_memo 应用

**来源**: [AI 硬件产品创新赛道详细指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_hardware_track_guide.md)

---

## 四、主动任务机制

### 4.1 cron_service API

ai_agent 框架提供了 `cron_service` 机制实现主动任务。

**核心 API**：
```c
// infra/cron_service.h - cron 服务核心 API
int  cron_service_init(void);
int  cron_service_start(void);
int  cron_service_stop(void);

// 添加/删除/列出 cron job
int  cron_add_job(const cron_job_t* job);
int  cron_remove_job(const char* name);
int  cron_list_jobs(cron_job_t* out, int max);
```

**cron_job_t 关键字段**：
```c
typedef struct {
    char id[64];           // 唯一 ID
    char name[128];        // 任务名（用于删除/查询）
    bool enabled;          // 是否启用
    int  kind;             // CRON_KIND_EVERY / CRON_KIND_AT
    int  interval_s;       // EVERY 模式的间隔秒数
    int64_t at_epoch;      // AT 模式的触发时间戳
    char message[256];     // 触发时推送的消息内容
    char channel[64];      // 推送渠道（如 "mini_memo"）
    char chat_id[128];     // 推送目标
    bool delete_after_run; // 一次性任务执行后自动删除
    char action[128];      // 触发时执行的 tool 名（可选）
    char action_args[256]; // tool 参数（可选）
} cron_job_t;
```

**来源**: [mini_memo 应用开发指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/mini_memo_guide.md)

### 4.2 主动任务类型

| 主动类型 | 说明 | 示例 |
|----------|------|------|
| 定时主动 | 按时间周期主动推送 | 每早 8 点推送天气和日程 |
| 阈值主动 | 满足条件时主动告警 | 心率超过 120 自动提醒 |
| 事件主动 | 检测到变化时主动通知 | 检测到新消息主动播报 |
| 上下文主动 | 基于历史记忆主动建议 | 连续 3 天没运动，主动建议出门走走 |

### 4.3 LVGL Timer vs cron_service 对比

| 维度 | LVGL Timer | cron_service |
|------|------------|--------------|
| 独立于 UI | ❌ 依赖 LVGL 事件循环 | ✅ 独立 pthread，UI 退出仍运行 |
| 持久化 | ❌ 重启后重新轮询 | ✅ cJSON 文件持久化，重启恢复 |
| 定时精度 | 60s 轮询，最坏延迟 60s | 精确到秒，cond_timedwait 唤醒 |
| 支持 recurring | ❌ 仅隐含 AT | ✅ EVERY（周期）+ AT（一次性） |
| 通知渠道 | 仅 lv_msgbox | system / voice / feishu 等 |
| LLM 可调用 | ❌ | ✅ tool_cron_add/list/remove |

---

## 五、LVGL UI 开发模式

### 5.1 mini_memo 架构参考

```
mini_memo
├── main 入口（mini_memo_main.c）
│   ├── libuv/poll 事件循环
│   └── 命令行参数解析（--ptt-selftest）
├── mini_memo_core（数据层，mini_memo_core.c/h）
│   ├── memo_store：cJSON + 文件系统持久化
│   ├── memo_classify_local：本地关键词分类
│   └── memo_agent：openvelaClaw LLM 集成
├── mini_memo_ui（表现层，mini_memo_ui.c/h）
│   ├── lv_tileview：4页面水平滑动
│   ├── LVGL Timer：flush(5s) + remind(60s)
│   └── PTT 按钮：voice_channel 集成
└── openvelaClaw Client（远程服务）
    ├── velaclaw_ask：LLM 分类
    └── voice_channel：PTT + ASR
```

### 5.2 启动流程

```c
int main(int argc, FAR char* argv[])
{
    // 1. 解析命令行参数
    // 2. 初始化 LVGL
    lv_init();
    lv_nuttx_dsc_init(&info);
    lv_nuttx_init(&info, &result);

    // 3. 初始化数据存储
    memo_store_init(CONFIG_MINI_MEMO_DATA_DIR);

    // 4. 初始化 openvelaClaw Agent（LLM + voice）
    memo_agent_init();

    // 5. 初始化 UI（含 timer）
    memo_ui_init();

    // 6. 进入事件循环
    lv_nuttx_uv_loop(&ui_loop, &result);
    return 0;
}
```

### 5.3 openvelaClaw Client 集成

```c
// 连接 openvelaClaw Daemon
g_client = velaclaw_client_open("mini_memo");
if (!g_client) {
    // LLM 不可用，降级到本地逻辑
    g_agent_connected = false;
    return voice_ret;
}
g_agent_connected = true;
```

### 5.4 LLM 调用示例

```c
// LLM 分类 prompt
static const char* g_classify_prompt_fmt =
    "You are a memo classifier. Given the user's voice input, "
    "classify it and extract structured data.\n\n"
    "Input: \"%s\"\n\n"
    "Respond ONLY with JSON:\n"
    "{\"type\":\"memo|todo|schedule\","
    "\"content\":\"<cleaned content>\","
    "\"remind_at\":<unix_timestamp_or_0>}\n";
```

### 5.5 语音集成

```c
// PTT 按钮事件处理
// LV_EVENT_PRESSED: voice_channel_start()
// LV_EVENT_RELEASED: voice_channel_stop_with_text(text_out, text_cap)
```

### 5.6 数据持久化

```c
// cJSON + 文件系统持久化
// 持久化文件格式（memos.json）：
{
    "version": 1,
    "next_id": 5,
    "items": [
        {
            "id": 1,
            "type": 1,
            "content": "提醒我明早8点开会",
            "timestamp": 1709913600,
            "remind_at": 1709942400,
            "is_read": false
        }
    ]
}
```

---

## 六、Kconfig 配置参考

### 6.1 ai_agent 配置

```
CONFIG_EXAMPLES_AI_AGENT_VELA=y      # 启用 ai_agent
CONFIG_FEATURE_SYSTEM_VELACLAW=y     # 启用 velaclaw feature
CONFIG_MQ_MAXMSGSIZE=4096            # 消息队列最大消息大小
```

### 6.2 可选模块（按需关闭以节省内存）

| 模块 | Kconfig | 默认 | 占用 | 关闭建议 |
|------|---------|------|------|----------|
| 飞书 Bot | `AI_AGENT_FEISHU` | 开启 | ~108KB | 不用飞书就关 |
| 微信 Bot | `AI_AGENT_WEIXIN` | 开启 | ~45KB | 不用微信就关 |
| MQTT | `AI_AGENT_MQTT` | 开启 | ~15KB | 不做 IoT 就关 |
| 多设备协作 | `AI_AGENT_NODE` | 开启 | ~20KB | 不用 Hub/Node 就关 |
| MCP 协议 | `AI_AGENT_MCP` | 开启 | ~10KB | 不用远程工具就关 |
| LVGL UI | `AI_AGENT_LVGL_UI` | 关闭 | ~50KB | 需要屏幕聊天才开 |
| BLE GATT | `AI_AGENT_BLE_GATT` | 关闭 | — | 需要 BLE 数据通道时开 |
| BLE 网络通道 | `AI_AGENT_BLE_NET` | 关闭 | — | 无 WiFi、经手机 App 蓝牙 SPP 代理上网时开 |
| 相机 | `AI_AGENT_CAMERA` | 关闭 | 依赖 VIDEO | 视觉识别场景开 |

### 6.3 mini_memo Kconfig 参考

```kconfig
config LVX_USE_DEMO_MINI_MEMO
    bool "Mini Memo - AI Memory Assistant"
    default n
    depends on GRAPHICS_LVGL
    depends on LV_USE_NUTTX
    depends on EXAMPLES_AI_AGENT_VELA || VELACLAW_DAEMON
    select NETUTILS_CJSON
    ---help---
        AI-powered memory assistant with voice input,
        intent classification, and proactive reminders.
        Requires openvelaClaw framework for LLM and tools.

if LVX_USE_DEMO_MINI_MEMO

config MINI_MEMO_DATA_DIR
    string "Mini Memo data directory"
    default "/data/mini_memo"

config MINI_MEMO_REVIEW_INTERVAL
    int "Default periodic review interval (seconds)"
    default 14400

endif
```

---

## 七、VelaWear Agent 开发要点

### 7.1 核心开发原则

已有能力负责"基础能力"，我们写的代码负责"把这些能力组织成 Agent"：

| 已有能力 | 我们开发 |
|----------|----------|
| IMU 能力 | 运动事件和主动提醒逻辑 |
| Audio 能力 | 语音指令到任务的逻辑 |
| BLE 能力 | 手机与 Agent 的任务同步 |
| Timer 能力 | 任务触发机制 |
| LVGL 能力 | Agent 状态和任务 UI |
| Power 能力 | Agent 的睡眠/唤醒策略 |

### 7.2 关键技术选型

1. **LLM 集成**：使用 openvelaClaw Client 连接远程 LLM
2. **主动任务**：使用 cron_service（非 LVGL Timer）
3. **数据持久化**：cJSON + 文件系统
4. **语音交互**：voice_channel PTT + ASR
5. **UI 框架**：LVGL + lv_tileview

### 7.3 评分标准对应

| 评分指标 | 分数 | VelaWear 对应 |
|----------|------|---------------|
| 技术难度 | 30 | 多源事件融合、决策引擎、低功耗优化 |
| 产品创新性 | 20 | 主动感知、主动提醒、多模态交互 |
| 项目完整度 | 20 | 完整的 Agent 架构、可运行 Demo |
| AI 开发 | 10 | ai_agent 集成、自定义 Skill/Tool |
| 商业潜力 | 10 | 穿戴设备市场、健康应用场景 |
| 展示效果 | 10 | 路演和 PPT 呈现 |

---

## 八、参考资料

1. [AI 硬件产品创新赛道详细指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_hardware_track_guide.md)
2. [ai_agent 应用开发上手指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_agent_quickstart.md)
3. [mini_memo 应用开发指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/mini_memo_guide.md)
4. [黄山派 README](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md)
5. [openvela 快应用调用端侧 AI Agent](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/quickapp/quickapp_velaclaw.md)
6. [2026 首届 openvela AI 硬件开发者大赛](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/contest_overview.md)
