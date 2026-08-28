# VelaWear Agent API 文档

## 一、Event Manager API

### 1.1 初始化

```c
/**
 * @brief 初始化事件管理器
 * @return 0 成功，负数失败
 */
int event_manager_init(void);
```

### 1.2 事件推送

```c
/**
 * @brief 推送事件到队列
 * @param event 事件指针
 * @return 0 成功，负数失败
 */
int event_manager_push(const velawear_event_t *event);
```

### 1.3 事件取出

```c
/**
 * @brief 从队列取出事件（阻塞）
 * @param event 输出事件指针
 * @param timeout_ms 超时时间（毫秒），-1 无限等待
 * @return 0 成功，负数失败
 */
int event_manager_pop(velawear_event_t *event, int timeout_ms);
```

### 1.4 注册处理器

```c
/**
 * @brief 注册事件处理器
 * @param type 事件类型
 * @param handler 处理函数
 * @return 0 成功，负数失败
 */
typedef void (*event_handler_t)(const velawear_event_t *event);
int event_manager_register_handler(event_type_t type, event_handler_t handler);
```

### 1.5 事件结构体

```c
typedef enum {
    EVENT_TYPE_IMU,          // IMU 运动事件
    EVENT_TYPE_AUDIO,        // 音频/语音事件
    EVENT_TYPE_BLE,          // BLE 通信事件
    EVENT_TYPE_TIMER,        // 定时器事件
    EVENT_TYPE_SYSTEM,       // 系统事件
} event_type_t;

typedef enum {
    EVENT_PRIORITY_LOW,      // 低优先级
    EVENT_PRIORITY_MEDIUM,   // 中优先级
    EVENT_PRIORITY_HIGH,     // 高优先级
} event_priority_t;

typedef struct {
    event_type_t type;       // 事件类型
    event_priority_t priority; // 优先级
    uint32_t timestamp;      // 时间戳
    void *data;              // 事件数据
    size_t data_len;         // 数据长度
} velawear_event_t;
```

---

## 二、State Manager API

### 2.1 初始化

```c
/**
 * @brief 初始化状态管理器
 * @return 0 成功，负数失败
 */
int state_manager_init(void);
```

### 2.2 获取设备状态

```c
/**
 * @brief 获取设备状态
 * @return 设备状态结构体
 */
device_state_t state_manager_get_device_state(void);
```

### 2.3 获取用户状态

```c
/**
 * @brief 获取用户状态
 * @return 用户状态结构体
 */
user_state_t state_manager_get_user_state(void);
```

### 2.4 更新状态

```c
/**
 * @brief 更新设备状态
 * @param state 新状态
 * @return 0 成功，负数失败
 */
int state_manager_update_device_state(const device_state_t *state);

/**
 * @brief 更新用户状态
 * @param state 新状态
 * @return 0 成功，负数失败
 */
int state_manager_update_user_state(const user_state_t *state);
```

### 2.5 状态结构体

```c
typedef struct {
    bool is_wearing;           // 是否佩戴
    bool is_charging;          // 是否充电中
    int battery_level;         // 电量百分比 (0-100)
    bool is_connected_ble;     // BLE 连接状态
    bool is_wifi_connected;    // WiFi 连接状态
} device_state_t;

typedef enum {
    MOTION_TYPE_STILL,         // 静止
    MOTION_TYPE_WALKING,       // 步行
    MOTION_TYPE_RUNNING,       // 跑步
    MOTION_TYPE_CYCLING,       // 骑行
    MOTION_TYPE_SLEEPING,      // 睡眠
} motion_type_t;

typedef struct {
    int activity_level;        // 活动强度 (0-100)
    int heart_rate;            // 心率 (bpm)
    int steps;                 // 步数
    bool is_sleeping;          // 是否在睡眠
    time_t last_interaction;   // 最后交互时间
    motion_type_t motion_type; // 运动类型
} user_state_t;
```

---

## 三、Task Manager API

### 3.1 初始化

```c
/**
 * @brief 初始化任务管理器
 * @return 0 成功，负数失败
 */
int task_manager_init(void);
```

### 3.2 添加任务

```c
/**
 * @brief 添加任务
 * @param task 任务指针
 * @return 0 成功，负数失败
 */
int task_manager_add_task(const velawear_task_t *task);
```

### 3.3 移除任务

```c
/**
 * @brief 移除任务
 * @param task_id 任务ID
 * @return 0 成功，负数失败
 */
int task_manager_remove_task(const char *task_id);
```

### 3.4 获取待执行任务

```c
/**
 * @brief 获取待执行任务列表
 * @param tasks 输出任务数组
 * @param max_tasks 最大任务数
 * @return 实际任务数
 */
int task_manager_get_pending_tasks(velawear_task_t *tasks, int max_tasks);
```

### 3.5 任务结构体

```c
typedef enum {
    TASK_TYPE_TIMED,           // 定时任务
    TASK_TYPE_EVENT_DRIVEN,    // 事件驱动任务
    TASK_TYPE_CONDITIONAL,     // 条件触发任务
} task_type_t;

typedef struct {
    char task_id[32];          // 任务ID
    task_type_t type;          // 任务类型
    time_t scheduled_time;     // 计划执行时间
    bool is_recurring;         // 是否重复
    int recurrence_interval;   // 重复间隔（秒）
    void (*callback)(void *);  // 任务回调函数
    void *user_data;           // 用户数据
    bool enabled;              // 是否启用
} velawear_task_t;
```

### 3.6 集成 cron_service

```c
/**
 * @brief 添加 cron 任务
 * @param job cron 任务指针
 * @return 0 成功，负数失败
 */
int task_manager_add_cron_job(const cron_job_t *job);

/**
 * @brief 移除 cron 任务
 * @param name 任务名
 * @return 0 成功，负数失败
 */
int task_manager_remove_cron_job(const char *name);
```

---

## 四、Decision Engine API

### 4.1 初始化

```c
/**
 * @brief 初始化决策引擎
 * @return 0 成功，负数失败
 */
int decision_engine_init(void);
```

### 4.2 添加规则

```c
/**
 * @brief 添加决策规则
 * @param rule 规则指针
 * @return 0 成功，负数失败
 */
int decision_engine_add_rule(const decision_rule_t *rule);
```

### 4.3 移除规则

```c
/**
 * @brief 移除决策规则
 * @param rule_id 规则ID
 * @return 0 成功，负数失败
 */
int decision_engine_remove_rule(const char *rule_id);
```

### 4.4 评估规则

```c
/**
 * @brief 评估所有规则
 * @param context 决策上下文
 * @return 触发的动作数量
 */
int decision_engine_evaluate(const decision_context_t *context);
```

### 4.5 规则结构体

```c
typedef struct {
    char rule_id[32];          // 规则ID
    bool (*condition)(const decision_context_t *ctx);  // 条件函数
    void (*action)(const decision_context_t *ctx);     // 动作函数
    int priority;              // 优先级 (0-100)
    bool enabled;              // 是否启用
} decision_rule_t;

typedef struct {
    device_state_t device_state;
    user_state_t user_state;
    environment_state_t env_state;
    velawear_event_t current_event;
} decision_context_t;
```

### 4.6 内置规则示例

```c
// 久坐提醒规则
bool check_sedentary(const decision_context_t *ctx) {
    return ctx->user_state.activity_level < 10 &&
           (time(NULL) - ctx->user_state.last_interaction) > 3600;
}

void action_sedentary_reminder(const decision_context_t *ctx) {
    action_manager_execute(&(velawear_action_t){
        .type = ACTION_TYPE_DISPLAY,
        .params = "久坐提醒：请起身活动一下！",
    });
}

// 注册规则
decision_engine_add_rule(&(decision_rule_t){
    .rule_id = "sedentary_reminder",
    .condition = check_sedentary,
    .action = action_sedentary_reminder,
    .priority = 80,
});
```

---

## 五、Action Manager API

### 5.1 初始化

```c
/**
 * @brief 初始化动作管理器
 * @return 0 成功，负数失败
 */
int action_manager_init(void);
```

### 5.2 执行动作

```c
/**
 * @brief 执行动作
 * @param action 动作指针
 * @return 0 成功，负数失败
 */
int action_manager_execute(const velawear_action_t *action);
```

### 5.3 注册动作处理器

```c
/**
 * @brief 注册动作处理器
 * @param type 动作类型
 * @param handler 处理函数
 * @return 0 成功，负数失败
 */
typedef int (*action_handler_t)(const velawear_action_t *action);
int action_manager_register_handler(action_type_t type, action_handler_t handler);
```

### 5.4 动作结构体

```c
typedef enum {
    ACTION_TYPE_DISPLAY,       // LCD 显示
    ACTION_TYPE_AUDIO,         // 音频播放
    ACTION_TYPE_VIBRATE,       // 震动
    ACTION_TYPE_BLE_NOTIFY,    // BLE 通知
    ACTION_TYPE_AI_QUERY,      // AI 查询
} action_type_t;

typedef struct {
    action_type_t type;        // 动作类型
    void *params;              // 参数
    size_t params_len;         // 参数长度
} velawear_action_t;
```

### 5.5 LCD 显示动作

```c
/**
 * @brief 显示文本
 * @param text 文本内容
 * @param duration_ms 显示时长（毫秒），0 表示持续显示
 * @return 0 成功，负数失败
 */
int lcd_action_show_text(const char *text, int duration_ms);

/**
 * @brief 显示通知
 * @param title 标题
 * @param body 内容
 * @return 0 成功，负数失败
 */
int lcd_action_show_notification(const char *title, const char *body);

/**
 * @brief 更新表盘
 * @param data 表盘数据
 * @return 0 成功，负数失败
 */
int lcd_action_update_watchface(const void *data);
```

---

## 六、Sensor API

### 6.1 IMU 传感器

```c
/**
 * @brief 初始化 IMU 传感器
 * @return 0 成功，负数失败
 */
int imu_sensor_init(void);

/**
 * @brief 读取 IMU 数据
 * @param data 输出数据指针
 * @return 0 成功，负数失败
 */
int imu_sensor_read(imu_data_t *data);

/**
 * @brief 识别运动类型
 * @param data IMU 数据
 * @return 运动类型
 */
motion_type_t imu_classify_motion(const imu_data_t *data);

typedef struct {
    float accel_x, accel_y, accel_z;    // 加速度 (m/s²)
    float gyro_x, gyro_y, gyro_z;       // 角速度 (rad/s)
    uint32_t timestamp;                  // 时间戳
} imu_data_t;
```

### 6.2 音频传感器

```c
/**
 * @brief 初始化音频传感器
 * @return 0 成功，负数失败
 */
int audio_sensor_init(void);

/**
 * @brief 开始录音
 * @return 0 成功，负数失败
 */
int audio_sensor_start_listening(void);

/**
 * @brief 停止录音并获取识别结果
 * @param text_out 输出文本缓冲区
 * @param text_cap 缓冲区大小
 * @return 0 成功，负数失败
 */
int audio_sensor_stop_listening(char *text_out, size_t text_cap);
```

### 6.3 BLE 传感器

```c
/**
 * @brief 初始化 BLE
 * @return 0 成功，负数失败
 */
int ble_sensor_init(void);

/**
 * @brief 发送 BLE 通知
 * @param msg 消息指针
 * @return 0 成功，负数失败
 */
int ble_sensor_send_notification(const ble_message_t *msg);

/**
 * @brief 注册 BLE 消息回调
 * @param callback 回调函数
 * @return 0 成功，负数失败
 */
int ble_sensor_register_callback(void (*callback)(const ble_message_t *));

typedef struct {
    uint16_t service_uuid;     // 服务 UUID
    uint16_t char_uuid;        // 特征 UUID
    uint8_t *data;             // 数据
    size_t data_len;           // 数据长度
} ble_message_t;
```

---

## 七、Power Manager API

### 7.1 功耗模式

```c
typedef enum {
    POWER_MODE_ACTIVE,         // 活跃模式
    POWER_MODE_IDLE,           // 空闲模式
    POWER_MODE_LIGHT_SLEEP,    // 轻度睡眠
    POWER_MODE_DEEP_SLEEP,     // 深度睡眠
} power_mode_t;
```

### 7.2 设置功耗模式

```c
/**
 * @brief 设置功耗模式
 * @param mode 功耗模式
 * @return 0 成功，负数失败
 */
int power_manager_set_mode(power_mode_t mode);

/**
 * @brief 获取当前功耗模式
 * @return 当前功耗模式
 */
power_mode_t power_manager_get_mode(void);

/**
 * @brief 自动调整功耗模式
 * @return 0 成功，负数失败
 */
int power_manager_auto_adjust(void);
```

---

## 八、UI API

### 8.1 初始化

```c
/**
 * @brief 初始化 UI
 * @return 0 成功，负数失败
 */
int velawear_ui_init(void);
```

### 8.2 页面管理

```c
/**
 * @brief 显示主页面（表盘）
 * @return 0 成功，负数失败
 */
int ui_show_watchface(void);

/**
 * @brief 显示通知页面
 * @param title 标题
 * @param body 内容
 * @return 0 成功，负数失败
 */
int ui_show_notification(const char *title, const char *body);

/**
 * @brief 显示运动数据页面
 * @param data 运动数据
 * @return 0 成功，负数失败
 */
int ui_show_fitness(const fitness_data_t *data);

/**
 * @brief 显示设置页面
 * @return 0 成功，负数失败
 */
int ui_show_settings(void);
```

### 8.3 数据更新

```c
/**
 * @brief 更新表盘数据
 * @param time 时间
 * @param steps 步数
 * @param heart_rate 心率
 * @param battery 电量
 * @return 0 成功，负数失败
 */
int ui_update_watchface(time_t time, int steps, int heart_rate, int battery);
```

---

## 九、主入口 API

### 9.1 初始化

```c
/**
 * @brief 初始化 VelaWear Agent
 * @return 0 成功，负数失败
 */
int velawear_init(void);
```

### 9.2 启动

```c
/**
 * @brief 启动 VelaWear Agent
 * @return 0 成功，负数失败
 */
int velawear_start(void);
```

### 9.3 停止

```c
/**
 * @brief 停止 VelaWear Agent
 * @return 0 成功，负数失败
 */
int velawear_stop(void);
```

### 9.4 主函数

```c
/**
 * @brief VelaWear Agent 主函数
 * @param argc 参数数量
 * @param argv 参数数组
 * @return 0 成功，非0失败
 */
int main(int argc, char *argv[]);
```

---

## 十、错误码定义

```c
#define VELAWEAR_OK              0    // 成功
#define VELAWEAR_ERR_NOMEM      -1    // 内存不足
#define VELAWEAR_ERR_INVAL      -2    // 无效参数
#define VELAWEAR_ERR_TIMEOUT    -3    // 超时
#define VELAWEAR_ERR_IO         -4    // IO 错误
#define VELAWEAR_ERR_NOTINIT    -5    // 未初始化
#define VELAWEAR_ERR_BUSY       -6    // 忙碌
#define VELAWEAR_ERR_NOTFOUND   -7    // 未找到
#define VELAWEAR_ERR_FULL       -8    // 队列已满
#define VELAWEAR_ERR_EMPTY      -9    // 队列为空
```

---

## 十一、配置选项

### 11.1 Kconfig 配置

```kconfig
config LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT
    bool "VelaWear Agent - Smart Wearable Agent"
    default n
    depends on GRAPHICS_LVGL
    depends on LV_USE_NUTTX
    depends on EXAMPLES_AI_AGENT_VELA || VELACLAW_DAEMON
    select NETUTILS_CJSON
    select SENSORS_LSM6DSL
    ---help---
        VelaWear Agent for smart wearable applications.

if LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT

config VELAWEAR_DATA_DIR
    string "VelaWear data directory"
    default "/data/velawear"

config VELAWEAR_SEDENTARY_TIMEOUT
    int "Sedentary reminder timeout (seconds)"
    default 3600

config VELAWEAR_HEART_RATE_HIGH
    int "Heart rate high threshold (bpm)"
    default 120

config VELAWEAR_HEART_RATE_LOW
    int "Heart rate low threshold (bpm)"
    default 50

endif
```

### 11.2 编译选项

```makefile
# Makefile
PROGNAME  = velawear
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 40960
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT)

MAINSRC = velawear_main.c
CSRCS   = src/event_manager.c \
          src/state_manager.c \
          src/task_manager.c \
          src/decision_engine.c \
          src/action_manager.c \
          sensors/imu_sensor.c \
          sensors/audio_sensor.c \
          sensors/ble_sensor.c \
          actions/lcd_action.c \
          actions/audio_action.c \
          actions/ble_action.c \
          ui/velawear_ui.c
```
