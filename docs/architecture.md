# VelaWear Agent 架构设计文档

## 一、系统架构概述

VelaWear Agent 采用分层架构设计，自下而上分为：

1. **硬件层**：SF32LB52 黄山派开发板
2. **系统层**：OpenVela RTOS + 已有驱动
3. **框架层**：ai_agent 框架
4. **应用层**：VelaWear Agent 核心逻辑
5. **交互层**：LVGL UI + 语音交互

```
┌─────────────────────────────────────────────────────────────┐
│                      用户交互层                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   LVGL UI    │  │   语音交互    │  │   触摸交互    │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
├─────────────────────────────────────────────────────────────┤
│                      VelaWear Agent 应用层                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Event Manager│  │ State Manager│  │ Task Manager │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐                        │
│  │Decision Engine│  │Action Manager│                        │
│  └──────────────┘  └──────────────┘                        │
├─────────────────────────────────────────────────────────────┤
│                      ai_agent 框架层                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │    Router     │  │     Tool     │  │    Skill     │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐                        │
│  │ cron_service │  │ message_bus  │                        │
│  └──────────────┘  └──────────────┘                        │
├─────────────────────────────────────────────────────────────┤
│                      OpenVela 系统层                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │    Kernel     │  │   Drivers    │  │  Filesystem  │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
├─────────────────────────────────────────────────────────────┤
│                      硬件层 (SF32LB52)                       │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐   │
│  │  IMU   │ │  LCD   │ │  BLE   │ │  RTC   │ │  ADC   │   │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、核心模块设计

### 2.1 Event Manager（事件管理器）

**职责**：统一管理来自各传感器和系统的事件

**设计要点**：
- 事件队列：线程安全的 FIFO 队列
- 事件类型：IMU、Audio、BLE、Timer、System
- 事件优先级：高/中/低三级
- 事件处理：异步处理，不阻塞事件源

```c
// 事件类型定义
typedef enum {
    EVENT_TYPE_IMU,          // IMU 运动事件
    EVENT_TYPE_AUDIO,        // 音频/语音事件
    EVENT_TYPE_BLE,          // BLE 通信事件
    EVENT_TYPE_TIMER,        // 定时器事件
    EVENT_TYPE_SYSTEM,       // 系统事件
} event_type_t;

// 事件优先级
typedef enum {
    EVENT_PRIORITY_LOW,      // 低优先级
    EVENT_PRIORITY_MEDIUM,   // 中优先级
    EVENT_PRIORITY_HIGH,     // 高优先级
} event_priority_t;

// 事件结构体
typedef struct {
    event_type_t type;       // 事件类型
    event_priority_t priority; // 优先级
    uint32_t timestamp;      // 时间戳
    void *data;              // 事件数据
    size_t data_len;         // 数据长度
} velawear_event_t;
```

**工作流程**：
1. 传感器产生事件
2. 事件推送到队列
3. 事件处理线程从队列取出事件
4. 根据事件类型分发到对应处理器
5. 处理器更新状态或触发决策

### 2.2 State Manager（状态管理器）

**职责**：跟踪设备状态和用户状态

**状态分类**：

| 状态类别 | 包含信息 | 更新频率 |
|----------|----------|----------|
| 设备状态 | 电量、BLE连接、WiFi、充电状态 | 事件驱动 |
| 用户状态 | 活动状态、心率、步数、睡眠状态 | 传感器驱动 |
| 环境状态 | 时间、噪音级别 | 定时更新 |

```c
// 设备状态
typedef struct {
    bool is_wearing;           // 是否佩戴
    bool is_charging;          // 是否充电中
    int battery_level;         // 电量百分比
    bool is_connected_ble;     // BLE 连接状态
    bool is_wifi_connected;    // WiFi 连接状态
} device_state_t;

// 用户状态
typedef struct {
    int activity_level;        // 活动强度 (0-100)
    int heart_rate;            // 心率
    int steps;                 // 步数
    bool is_sleeping;          // 是否在睡眠
    time_t last_interaction;   // 最后交互时间
    motion_type_t motion_type; // 运动类型
} user_state_t;
```

### 2.3 Task Manager（任务管理器）

**职责**：管理定时任务和事件驱动任务

**任务类型**：

| 类型 | 说明 | 示例 |
|------|------|------|
| TIMED | 定时执行 | 每5分钟更新运动数据 |
| EVENT_DRIVEN | 事件触发 | 收到BLE消息时通知 |
| CONDITIONAL | 条件触发 | 心率超过阈值时告警 |

```c
typedef struct {
    char task_id[32];          // 任务ID
    task_type_t type;          // 任务类型
    time_t scheduled_time;     // 计划时间
    bool is_recurring;         // 是否重复
    int recurrence_interval;   // 重复间隔（秒）
    void (*callback)(void *);  // 回调函数
    void *user_data;           // 用户数据
    bool enabled;              // 是否启用
} velawear_task_t;
```

**集成 cron_service**：
- 使用 ai_agent 框架的 cron_service 实现定时任务
- 支持 EVERY（周期）和 AT（一次性）两种模式
- 持久化到文件，重启后恢复

### 2.4 Decision Engine（决策引擎）

**职责**：基于状态和事件做出决策

**决策规则**：

```c
typedef struct {
    char rule_id[32];          // 规则ID
    bool (*condition)(...);    // 条件函数
    void (*action)(...);       // 动作函数
    int priority;              // 优先级
    bool enabled;              // 是否启用
} decision_rule_t;
```

**内置规则**：

| 规则 | 条件 | 动作 |
|------|------|------|
| 久坐提醒 | 连续60分钟无活动 | LCD显示+震动 |
| 心率异常 | 心率>120或<50 | LCD显示+声音告警 |
| 运动完成 | 达到运动目标 | 推送运动报告 |
| 低电量 | 电量<20% | 显示低电量提醒 |
| 睡眠检测 | 检测到入睡 | 切换到睡眠模式 |

### 2.5 Action Manager（动作管理器）

**职责**：执行决策结果，调用硬件能力

**动作类型**：

| 类型 | 说明 | 硬件接口 |
|------|------|----------|
| DISPLAY | LCD显示 | `/dev/lcd0`、`/dev/fb0` |
| AUDIO | 音频播放 | media 框架 |
| VIBRATE | 震动 | PWM |
| BLE_NOTIFY | BLE通知 | BLE GATT |
| AI_QUERY | AI查询 | velaclaw_client |

---

## 三、数据流设计

### 3.1 传感器数据流

```
传感器 → Event Manager → State Manager → Decision Engine → Action Manager
  │                          │                  │                │
  │     IMU数据              │   更新用户状态    │   评估规则     │   执行动作
  │     心率数据             │   更新设备状态    │   生成决策     │   LCD显示
  │     BLE消息              │                  │                │   声音播放
  └─────────────────────────────────────────────────────────────────────┘
```

### 3.2 主动任务流程

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  cron_service│────>│ Task Manager│────>│Decision Engine│
│  (定时触发)   │     │  (任务调度)   │     │  (条件评估)   │
└─────────────┘     └─────────────┘     └──────┬──────┘
                                               │
                                               v
                                        ┌─────────────┐
                                        │Action Manager│
                                        │  (执行动作)   │
                                        └──────┬──────┘
                                               │
                    ┌──────────────────────────┼──────────────────────────┐
                    │                          │                          │
                    v                          v                          v
             ┌─────────────┐           ┌─────────────┐           ┌─────────────┐
             │  LCD 显示    │           │  声音播放    │           │  BLE 通知    │
             └─────────────┘           └─────────────┘           └─────────────┘
```

---

## 四、线程模型

### 4.1 线程划分

| 线程 | 职责 | 优先级 | 栈大小 |
|------|------|--------|--------|
| main | LVGL 事件循环 | 高 | 16KB |
| event_handler | 事件处理 | 中 | 8KB |
| sensor_poll | 传感器轮询 | 中 | 4KB |
| task_scheduler | 任务调度 | 低 | 4KB |
| ble_handler | BLE 处理 | 中 | 4KB |

### 4.2 线程同步

- **事件队列**：pthread_mutex + pthread_cond
- **状态更新**：读写锁（pthread_rwlock）
- **任务调度**：信号量（sem_t）

---

## 五、功耗管理设计

### 5.1 功耗模式

| 模式 | CPU频率 | IMU采样率 | LCD状态 | BLE状态 | 功耗 |
|------|---------|-----------|---------|---------|------|
| ACTIVE | 最高 | 50Hz | 亮屏 | 活跃 | 高 |
| IDLE | 正常 | 10Hz | 息屏 | 活跃 | 中 |
| LIGHT_SLEEP | 低 | 1Hz | 息屏 | 低功耗 | 低 |
| DEEP_SLEEP | 最低 | 0.1Hz | 息屏 | 关闭 | 极低 |

### 5.2 模式切换策略

```c
void power_manager_auto_adjust(void) {
    user_state_t user = state_manager_get_user_state();
    device_state_t device = state_manager_get_device_state();

    if (user.is_sleeping) {
        power_manager_set_mode(POWER_MODE_DEEP_SLEEP);
    } else if (user.activity_level > 50) {
        power_manager_set_mode(POWER_MODE_ACTIVE);
    } else if (user.activity_level < 10) {
        power_manager_set_mode(POWER_MODE_IDLE);
    } else {
        power_manager_set_mode(POWER_MODE_LIGHT_SLEEP);
    }
}
```

---

## 六、错误处理设计

### 6.1 错误分类

| 错误类型 | 处理策略 | 示例 |
|----------|----------|------|
| 传感器错误 | 重试 + 降级 | IMU读取失败，使用上次数据 |
| LLM 连接失败 | 降级到本地逻辑 | velaclaw_client_open 失败 |
| 内存不足 | 清理缓存 + 告警 | 事件队列满 |
| BLE 断开 | 自动重连 | 手机蓝牙断开 |

### 6.2 降级策略

```c
// LLM 不可用时的降级
if (!g_agent_connected) {
    // 使用本地关键词分类
    result.type = classify_local(input_text);
} else {
    // 使用 LLM 分类
    result.type = classify_with_llm(input_text);
}
```

---

## 七、扩展性设计

### 7.1 新增传感器

1. 在 `sensors/` 目录添加新传感器接口
2. 在 Event Manager 注册新事件类型
3. 在 State Manager 添加新状态字段
4. 在 Decision Engine 添加新规则

### 7.2 新增动作

1. 在 `actions/` 目录添加新动作接口
2. 在 Action Manager 注册新动作类型
3. 在 Decision Engine 添加触发新动作的规则

### 7.3 新增决策规则

```c
// 注册新规则
decision_engine_add_rule(&(decision_rule_t){
    .rule_id = "new_rule",
    .condition = check_new_condition,
    .action = execute_new_action,
    .priority = 50,
});
```

---

## 八、测试策略

### 8.1 单元测试

- Event Manager：事件队列操作
- State Manager：状态更新和读取
- Task Manager：任务添加和执行
- Decision Engine：规则评估

### 8.2 集成测试

- 完整场景流程测试
- 多模块协作测试
- 异常情况处理测试

### 8.3 性能测试

- 内存使用监控
- CPU 占用监控
- 功耗测试

---

## 九、部署架构

```
┌─────────────────────────────────────────────────────────────┐
│                    SF32LB52 黄山派                            │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    OpenVela RTOS                     │    │
│  │  ┌─────────────────────────────────────────────┐    │    │
│  │  │              VelaWear Agent                  │    │    │
│  │  │  ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐   │    │    │
│  │  │  │ Event │ │ State │ │ Task  │ │Decision│   │    │    │
│  │  │  │Manager│ │Manager│ │Manager│ │Engine │   │    │    │
│  │  │  └───────┘ └───────┘ └───────┘ └───────┘   │    │    │
│  │  │  ┌───────┐ ┌───────┐ ┌───────┐             │    │    │
│  │  │  │ Action│ │Sensors│ │  UI   │             │    │    │
│  │  │  │Manager│ │       │ │(LVGL) │             │    │    │
│  │  │  └───────┘ └───────┘ └───────┘             │    │    │
│  │  └─────────────────────────────────────────────┘    │    │
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              硬件外设 (IMU/LCD/BLE/RTC/ADC)         │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ BLE
                              v
                    ┌─────────────────┐
                    │   手机 App      │
                    │  (消息同步)     │
                    └─────────────────┘
```
