# LVGL 技术指南（openvela / SF32LB52 黄山派）

> 本文档整理了 LVGL 在 openvela 上的使用方法，包括触摸 API、回调 API、控件使用等。

**来源**: [LVGL 官方文档](https://docs.lvgl.io/9.2/en/html/index.html) | [mini_memo 示例](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/mini_memo_guide.md)

---

## 一、LVGL 初始化（openvela/NuttX 后端）

### 1.1 基本初始化流程

```c
#include "lvgl.h"
#include "lv_nuttx.h"

int main(int argc, char *argv[])
{
    /* 1. 初始化 LVGL 核心 */
    lv_init();

    /* 2. 初始化 NuttX 后端（LCD + 触摸 + 事件循环） */
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    lv_nuttx_dsc_init(&info);
    lv_nuttx_init(&info, &result);

    /* 3. 创建 UI */
    // ...

    /* 4. 进入事件循环 */
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
    lv_nuttx_uv_loop(&ui_loop, &result);
#else
    lv_nuttx_loop();
#endif

    return 0;
}
```

### 1.2 Kconfig 配置

```
CONFIG_GRAPHICS_LVGL=y              # 启用 LVGL
CONFIG_LV_USE_NUTTX=y               # NuttX 后端
CONFIG_LV_USE_NUTTX_LCD=y           # LCD 支持
CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y   # 触摸屏支持
CONFIG_LV_USE_NUTTX_LIBUV=y         # libuv 事件循环
CONFIG_LV_FONT_MONTSERRAT_16=y      # 字体
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_24=y
```

---

## 二、触摸 API

### 2.1 触摸事件类型

LVGL 的触摸通过输入设备（Input Device）抽象层处理，自动生成以下事件：

| 事件 | 说明 | 触发条件 |
|------|------|----------|
| `LV_EVENT_PRESSED` | 按下 | 手指触摸屏幕 |
| `LV_EVENT_PRESSING` | 按住 | 手指持续触摸 |
| `LV_EVENT_RELEASED` | 释放 | 手指离开屏幕 |
| `LV_EVENT_CLICKED` | 点击 | 按下+释放（短按） |
| `LV_EVENT_LONG_PRESSED` | 长按 | 持续触摸超过阈值 |
| `LV_EVENT_LONG_PRESSED_REPEAT` | 长按重复 | 长按后持续触发 |
| `LV_EVENT_VALUE_CHANGED` | 值改变 | 滑块等控件值变化 |
| `LV_EVENT_GESTURE` | 手势 | 滑动、捏合等 |

### 2.2 触摸坐标获取

```c
/* 在事件回调中获取触摸坐标 */
static void event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        /* 获取触摸坐标（相对于对象） */
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        printf("Touch at: x=%d, y=%d\n", point.x, point.y);
    }
}
```

### 2.3 触摸区域检测

```c
/* 检测触摸是否在指定区域内 */
bool is_touch_in_area(lv_point_t *point, lv_area_t *area)
{
    return (point->x >= area->x1 && point->x <= area->x2 &&
            point->y >= area->y1 && point->y <= area->y2);
}
```

### 2.4 滑动手势检测

```c
static void swipe_event_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    switch (dir) {
        case LV_DIR_LEFT:
            printf("Swipe Left\n");
            break;
        case LV_DIR_RIGHT:
            printf("Swipe Right\n");
            break;
        case LV_DIR_TOP:
            printf("Swipe Up\n");
            break;
        case LV_DIR_BOTTOM:
            printf("Swipe Down\n");
            break;
    }
}

/* 注册手势事件 */
lv_obj_add_event_cb(obj, swipe_event_cb, LV_EVENT_GESTURE, NULL);
```

---

## 三、事件回调 API

### 3.1 事件回调注册

```c
/* 基本语法 */
lv_obj_add_event_cb(obj, event_cb, event_code, user_data);

/* 示例：按钮点击事件 */
lv_obj_t *btn = lv_btn_create(lv_scr_act());
lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, NULL);
```

### 3.2 事件回调函数原型

```c
static void event_cb(lv_event_t *e)
{
    /* 获取事件代码 */
    lv_event_code_t code = lv_event_get_code(e);

    /* 获取触发事件的对象 */
    lv_obj_t *obj = lv_event_get_target(e);

    /* 获取用户数据 */
    void *user_data = lv_event_get_user_data(e);

    /* 处理不同事件 */
    switch (code) {
        case LV_EVENT_CLICKED:
            printf("Clicked!\n");
            break;
        case LV_EVENT_PRESSED:
            printf("Pressed!\n");
            break;
        case LV_EVENT_RELEASED:
            printf("Released!\n");
            break;
        case LV_EVENT_VALUE_CHANGED:
            printf("Value changed!\n");
            break;
        default:
            break;
    }
}
```

### 3.3 常用事件代码

```c
/* 输入事件 */
LV_EVENT_PRESSED              // 按下
LV_EVENT_PRESSING             // 按住中
LV_EVENT_RELEASED             // 释放
LV_EVENT_CLICKED              // 点击（按下+释放）
LV_EVENT_LONG_PRESSED         // 长按
LV_EVENT_LONG_PRESSED_REPEAT  // 长按重复
LV_EVENT_GESTURE              // 手势

/* 值事件 */
LV_EVENT_VALUE_CHANGED        // 值改变
LV_EVENT_INSERT               // 文本插入
LV_EVENT_READY                // 就绪
LV_EVENT_CANCEL               // 取消

/* 绘制事件 */
LV_EVENT_DRAW_MAIN            // 绘制主体
LV_EVENT_DRAW_POST            // 绘制后
LV_EVENT_DRAW_MAIN_BEGIN      // 绘制主体开始
LV_EVENT_DRAW_MAIN_END        // 绘制主体结束

/* 其他 */
LV_EVENT_DELETE               // 对象删除
LV_EVENT_SIZE_CHANGED         // 尺寸改变
LV_EVENT_STYLE_CHANGED        // 样式改变
LV_EVENT_LAYOUT_CHANGED       // 布局改变
```

### 3.4 事件冒泡

```c
/* 启用事件冒泡 */
lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);

/* 在父对象上处理子对象的事件 */
static void parent_event_cb(lv_event_t *e)
{
    lv_obj_t *child = lv_event_get_target(e);  // 实际触发的对象
    lv_obj_t *parent = lv_event_get_current_target(e);  // 注册回调的对象

    if (child != parent) {
        printf("Event from child object\n");
    }
}
```

### 3.5 多事件注册

```c
/* 同一个回调处理多个事件 */
lv_obj_add_event_cb(obj, multi_event_cb, LV_EVENT_PRESSED, NULL);
lv_obj_add_event_cb(obj, multi_event_cb, LV_EVENT_RELEASED, NULL);
lv_obj_add_event_cb(obj, multi_event_cb, LV_EVENT_CLICKED, NULL);

static void multi_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
        case LV_EVENT_PRESSED:
            /* 按下处理 */
            break;
        case LV_EVENT_RELEASED:
            /* 释放处理 */
            break;
        case LV_EVENT_CLICKED:
            /* 点击处理 */
            break;
    }
}
```

---

## 四、常用控件 API

### 4.1 Label（标签）

```c
/* 创建标签 */
lv_obj_t *label = lv_label_create(lv_scr_act());

/* 设置文本 */
lv_label_set_text(label, "Hello VelaWear");
lv_label_set_text_fmt(label, "Steps: %d", steps);

/* 设置长文本模式 */
lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);  // 循环滚动
lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);             // 自动换行

/* 设置文本对齐 */
lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

/* 设置字体 */
lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);

/* 设置文本颜色 */
lv_obj_set_style_text_color(label, lv_color_white(), 0);
```

### 4.2 Button（按钮）

```c
/* 创建按钮 */
lv_obj_t *btn = lv_btn_create(lv_scr_act());

/* 设置尺寸 */
lv_obj_set_size(btn, 120, 50);

/* 设置位置 */
lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);

/* 添加标签 */
lv_obj_t *label = lv_label_create(btn);
lv_label_set_text(label, "Click Me");

/* 添加点击事件 */
lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

/* 设置样式 */
lv_obj_set_style_bg_color(btn, lv_color_hex(0x007AFF), 0);
lv_obj_set_style_radius(btn, 10, 0);
```

### 4.3 Image（图片）

```c
/* 创建图片 */
lv_obj_t *img = lv_img_create(lv_scr_act());

/* 设置图片源 */
LV_IMG_DECLARE(my_image);
lv_img_set_src(img, &my_image);

/* 设置缩放 */
lv_img_set_zoom(img, 256);  // 256 = 1x, 512 = 2x

/* 设置角度（旋转） */
lv_img_set_angle(img, 900);  // 90度（单位：0.1度）
```

### 4.4 Arc（弧形）

```c
/* 创建弧形 */
lv_obj_t *arc = lv_arc_create(lv_scr_act());

/* 设置尺寸 */
lv_obj_set_size(arc, 150, 150);

/* 设置范围 */
lv_arc_set_range(arc, 0, 100);

/* 设置值 */
lv_arc_set_value(arc, 75);

/* 设置角度 */
lv_arc_set_bg_angles(arc, 0, 360);
lv_arc_set_angles(arc, 0, 270);

/* 设置样式 */
lv_obj_set_style_arc_color(arc, lv_color_hex(0x007AFF), LV_PART_INDICATOR);
lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
```

### 4.5 Bar（进度条）

```c
/* 创建进度条 */
lv_obj_t *bar = lv_bar_create(lv_scr_act());

/* 设置尺寸 */
lv_obj_set_size(bar, 200, 20);

/* 设置范围 */
lv_bar_set_range(bar, 0, 100);

/* 设置值 */
lv_bar_set_value(bar, 60, LV_ANIM_ON);

/* 设置动画时间 */
lv_bar_set_anim_time(bar, 500);
```

### 4.6 Slider（滑块）

```c
/* 创建滑块 */
lv_obj_t *slider = lv_slider_create(lv_scr_act());

/* 设置范围 */
lv_slider_set_range(slider, 0, 100);

/* 设置值 */
lv_slider_set_value(slider, 50, LV_ANIM_ON);

/* 值改变事件 */
lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    printf("Slider value: %d\n", value);
}
```

### 4.7 Chart（图表）

```c
/* 创建图表 */
lv_obj_t *chart = lv_chart_create(lv_scr_act());
lv_obj_set_size(chart, 200, 150);

/* 设置图表类型 */
lv_chart_set_type(chart, LV_CHART_TYPE_LINE);  // 折线图
// lv_chart_set_type(chart, LV_CHART_TYPE_BAR);  // 柱状图

/* 设置点数 */
lv_chart_set_point_count(chart, 10);

/* 添加数据系列 */
lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(0x007AFF), LV_CHART_AXIS_PRIMARY_Y);

/* 设置数据 */
lv_chart_set_next_value(chart, ser, 10);
lv_chart_set_next_value(chart, ser, 20);
lv_chart_set_next_value(chart, ser, 30);

/* 刷新图表 */
lv_chart_refresh(chart);
```

---

## 五、布局 API

### 5.1 Flex 布局

```c
/* 启用 Flex 布局 */
lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);           // 垂直列
lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);              // 水平行
lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN_WRAP);      // 垂直列+换行

/* 设置对齐方式 */
lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START,           // 主轴对齐
                              LV_FLEX_ALIGN_CENTER,         // 交叉轴对齐
                              LV_FLEX_ALIGN_SPACE_AROUND);  // 间距对齐

/* 设置间距 */
lv_obj_set_style_pad_row(cont, 10, 0);    // 行间距
lv_obj_set_style_pad_column(cont, 10, 0); // 列间距

/* 子项弹性增长 */
lv_obj_set_flex_grow(child, 1);  // 占据剩余空间
```

### 5.2 Grid 布局

```c
/* 定义列 */
static lv_coord_t col_dsc[] = {100, LV_GRID_FR(1), 100, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[] = {50, LV_GRID_FR(1), 50, LV_GRID_TEMPLATE_LAST};

/* 启用 Grid 布局 */
lv_obj_set_grid_dsc_array(cont, col_dsc, row_dsc);

/* 设置子项位置 */
lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 1,  // 列：起始0，跨1列
                            LV_GRID_ALIGN_STRETCH, 0, 1);  // 行：起始0，跨1行
```

---

## 六、样式 API

### 6.1 样式初始化和应用

```c
/* 创建样式 */
static lv_style_t style;
lv_style_init(&style);

/* 设置样式属性 */
lv_style_set_bg_color(&style, lv_color_hex(0x1a1a2e));
lv_style_set_bg_opa(&style, LV_OPA_COVER);
lv_style_set_radius(&style, 12);
lv_style_set_border_width(&style, 0);
lv_style_set_pad_all(&style, 10);

/* 应用样式到对象 */
lv_obj_add_style(obj, &style, 0);  // 0 = 主体部分

/* 应用到特定状态 */
lv_obj_add_style(obj, &style, LV_STATE_PRESSED);  // 按下状态
lv_obj_add_style(obj, &style, LV_PART_INDICATOR); // 指示器部分
```

### 6.2 常用样式属性

```c
/* 背景 */
lv_style_set_bg_color(&style, lv_color_hex(0x000000));
lv_style_set_bg_opa(&style, LV_OPA_COVER);
lv_style_set_bg_grad_color(&style, lv_color_hex(0x1a1a2e));
lv_style_set_bg_grad_dir(&style, LV_GRAD_DIR_VER);

/* 边框 */
lv_style_set_border_color(&style, lv_color_hex(0x333333));
lv_style_set_border_width(&style, 2);
lv_style_set_border_opa(&style, LV_OPA_50);
lv_style_set_radius(&style, 10);

/* 阴影 */
lv_style_set_shadow_color(&style, lv_color_hex(0x000000));
lv_style_set_shadow_width(&style, 20);
lv_style_set_shadow_ofs_x(&style, 5);
lv_style_set_shadow_ofs_y(&style, 5);

/* 文本 */
lv_style_set_text_color(&style, lv_color_white());
lv_style_set_text_font(&style, &lv_font_montserrat_20);
lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

/* 内边距 */
lv_style_set_pad_left(&style, 10);
lv_style_set_pad_right(&style, 10);
lv_style_set_pad_top(&style, 5);
lv_style_set_pad_bottom(&style, 5);
```

### 6.3 OLED 暗色主题

```c
/* 适合 OLED 的暗色主题 */
static void setup_oled_theme(void)
{
    /* 全局背景黑色（OLED 省电） */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    /* 主色调 */
    lv_color_t primary = lv_color_hex(0x007AFF);    // 蓝色
    lv_color_t secondary = lv_color_hex(0x34C759);  // 绿色
    lv_color_t warning = lv_color_hex(0xFF9500);     // 橙色
    lv_color_t danger = lv_color_hex(0xFF3B30);      // 红色
    lv_color_t text_primary = lv_color_white();
    lv_color_t text_secondary = lv_color_hex(0x8E8E93);
}
```

---

## 七、动画 API

### 7.1 基本动画

```c
/* 创建动画 */
lv_anim_t a;
lv_anim_init(&a);

/* 设置动画目标和属性 */
lv_anim_set_var(&a, obj);
lv_anim_set_exec_xcb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
lv_anim_set_values(&a, 0, 200);

/* 设置动画时间 */
lv_anim_set_time(&a, 500);

/* 设置路径（缓动函数） */
lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

/* 启动动画 */
lv_anim_start(&a);
```

### 7.2 常用动画路径

```c
lv_anim_set_path_cb(&a, lv_anim_path_linear);       // 线性
lv_anim_set_path_cb(&a, lv_anim_path_ease_in);       // 缓入
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);      // 缓出
lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);   // 缓入缓出
lv_anim_set_path_cb(&a, lv_anim_path_overshoot);     // 过冲
lv_anim_set_path_cb(&a, lv_anim_path_bounce);        // 弹跳
```

### 7.3 样式过渡动画

```c
/* 定义过渡样式 */
static lv_style_transition_dsc_t trans;
static lv_style_prop_t props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT,
    LV_STYLE_PROP_INV
};
lv_style_transition_dsc_init(&trans, props, lv_anim_path_ease_out, 300, 0);

/* 应用到样式 */
lv_style_set_transition(&style_pressed, &trans);
```

---

## 八、定时器 API

### 8.1 创建定时器

```c
/* 创建定时器（周期执行） */
lv_timer_t *timer = lv_timer_create(timer_cb, 1000, user_data);  // 1000ms

/* 定时器回调 */
static void timer_cb(lv_timer_t *timer)
{
    void *user_data = lv_timer_get_user_data(timer);
    /* 定期执行的任务 */
}
```

### 8.2 定时器控制

```c
/* 暂停定时器 */
lv_timer_pause(timer);

/* 恢复定时器 */
lv_timer_resume(timer);

/* 设置周期 */
lv_timer_set_period(timer, 2000);

/* 手动触发 */
lv_timer_ready(timer);

/* 删除定时器 */
lv_timer_del(timer);
```

### 8.3 mini_memo 中的定时器使用

```c
/* mini_memo 使用定时器刷新数据和检查提醒 */
static void flush_timer_cb(lv_timer_t *timer)
{
    /* 每5秒刷新一次数据 */
    memo_store_flush();
}

static void remind_timer_cb(lv_timer_t *timer)
{
    /* 每60秒检查一次提醒 */
    memo_item_t items[MEMO_MAX_DISPLAY];
    int64_t now = (int64_t)time(NULL);

    int count = memo_store_get_due_reminders(now, items, MEMO_MAX_DISPLAY);
    for (int i = 0; i < count; i++) {
        memo_ui_show_notification("Reminder", items[i].content);
        memo_store_mark_read(items[i].id);
    }
}

/* 初始化定时器 */
g_flush_timer = lv_timer_create(flush_timer_cb, 5000, NULL);
g_remind_timer = lv_timer_create(remind_timer_cb, 60000, NULL);
```

---

## 九、Tileview（多页面视图）

### 9.1 创建 Tileview

```c
/* 创建 tileview */
lv_obj_t *tv = lv_tileview_create(lv_scr_act());

/* 添加页面（左、中、右） */
lv_obj_t *tile_left = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
lv_obj_t *tile_mid = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
lv_obj_t *tile_right = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);

/* 在页面上添加内容 */
lv_obj_t *label = lv_label_create(tile_mid);
lv_label_set_text(label, "Middle Page");

/* 设置默认页面 */
lv_obj_set_scroll_snap_x(tv, LV_SCROLL_SNAP_CENTER);
```

### 9.2 mini_memo 的多页面布局

```c
/* mini_memo 使用 4 个水平页面 */
lv_obj_t *tv = lv_tileview_create(lv_scr_act());

/* 左1：备忘列表 */
lv_obj_t *tile_memo = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);

/* 左2：待办列表 */
lv_obj_t *tile_todo = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);

/* 中间：PTT 语音输入 */
lv_obj_t *tile_ptt = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);

/* 右1：设置 */
lv_obj_t *tile_settings = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);
```

---

## 十、消息框（Notification）

### 10.1 创建消息框

```c
/* 创建消息框 */
lv_obj_t *mbox = lv_msgbox_create(NULL);
lv_msgbox_add_title(mbox, "Notification");
lv_msgbox_add_text(mbox, "久坐提醒：请起身活动一下！");
lv_msgbox_add_close_btn(mbox);

/* 设置自动关闭 */
lv_obj_align(mbox, LV_ALIGN_CENTER, 0, 0);
```

### 10.2 自定义通知样式

```c
/* 创建自定义通知 */
lv_obj_t *create_notification(const char *title, const char *body)
{
    lv_obj_t *notif = lv_obj_create(lv_scr_act());
    lv_obj_set_size(notif, 300, 100);
    lv_obj_align(notif, LV_ALIGN_TOP_MID, 0, 10);

    /* 设置样式 */
    lv_obj_set_style_bg_color(notif, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_radius(notif, 12, 0);
    lv_obj_set_style_border_width(notif, 0, 0);
    lv_obj_set_style_shadow_width(notif, 20, 0);

    /* 标题 */
    lv_obj_t *title_label = lv_label_create(notif);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x007AFF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 10);

    /* 内容 */
    lv_obj_t *body_label = lv_label_create(notif);
    lv_label_set_text(body_label, body);
    lv_obj_set_style_text_color(body_label, lv_color_white(), 0);
    lv_obj_align(body_label, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    return notif;
}
```

---

## 十一、按键事件处理

### 11.1 物理按键事件

```c
/* 按键回调 */
static void key_event_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);

    switch (key) {
        case LV_KEY_ENTER:
            printf("Enter pressed\n");
            break;
        case LV_KEY_LEFT:
            printf("Left pressed\n");
            break;
        case LV_KEY_RIGHT:
            printf("Right pressed\n");
            break;
        case LV_KEY_UP:
            printf("Up pressed\n");
            break;
        case LV_KEY_DOWN:
            printf("Down pressed\n");
            break;
    }
}

/* 注册按键事件 */
lv_obj_add_event_cb(obj, key_event_cb, LV_EVENT_KEY, NULL);
```

### 11.2 按键组（Group）

```c
/* 创建按键组 */
lv_group_t *g = lv_group_create();
lv_group_add_obj(g, btn1);
lv_group_add_obj(g, btn2);
lv_group_add_obj(g, btn3);

/* 将输入设备关联到组 */
lv_indev_t *indev = lv_indev_get_next(NULL);
lv_indev_set_group(indev, g);
```

---

## 十二、内存管理

### 12.1 内存配置

```
CONFIG_LV_MEM_CUSTOM=y          # 使用自定义内存分配
# 或
CONFIG_LV_MEM_SIZE=32768        # LVGL 内部堆大小（32KB）
```

### 12.2 内存优化建议

| 优化项 | 方法 | 效果 |
|--------|------|------|
| 字体 | 只包含需要的字体大小 | 减少 Flash |
| 图片 | 使用压缩格式（RLE） | 减少 Flash |
| 控件 | 及时删除不用的控件 | 减少 RAM |
| 样式 | 复用样式对象 | 减少 RAM |
| 缓冲区 | 使用部分缓冲区 | 减少 RAM |

### 12.3 部分缓冲区模式

```c
/* 使用部分缓冲区（节省内存） */
#define BUF_LINES 10
static lv_color_t buf[LV_HOR_RES_MAX * BUF_LINES];

lv_disp_draw_buf_init(&draw_buf, buf, NULL, LV_HOR_RES_MAX * BUF_LINES);
```

---

## 十三、VelaWear Agent UI 设计建议

### 13.1 页面规划

| 页面 | 内容 | 交互 |
|------|------|------|
| 表盘 | 时间、步数、心率、电量 | 点击查看详情 |
| 运动 | 运动状态、实时数据 | 开始/暂停运动 |
| 通知 | 消息列表 | 点击查看详情 |
| 设置 | 各项设置选项 | 滑动、点击 |

### 13.2 设计规范

```c
/* 屏幕尺寸 */
#define SCREEN_WIDTH  390
#define SCREEN_HEIGHT 450

/* 颜色方案 */
#define COLOR_BG        lv_color_black()          // OLED 背景
#define COLOR_PRIMARY   lv_color_hex(0x007AFF)    // 主色
#define COLOR_SECONDARY lv_color_hex(0x34C759)    // 辅助色
#define COLOR_WARNING   lv_color_hex(0xFF9500)    // 警告色
#define COLOR_DANGER    lv_color_hex(0xFF3B30)    // 危险色
#define COLOR_TEXT      lv_color_white()          // 文本色
#define COLOR_TEXT_SEC  lv_color_hex(0x8E8E93)    // 次要文本

/* 字体 */
#define FONT_LARGE  &lv_font_montserrat_48
#define FONT_MEDIUM &lv_font_montserrat_24
#define FONT_SMALL  &lv_font_montserrat_16
```

---

## 参考资料

1. [LVGL 官方文档](https://docs.lvgl.io/9.2/en/html/index.html)
2. [mini_memo 应用开发指引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/mini_memo_guide.md)
3. [LVGL 控件参考](https://docs.lvgl.io/9.2/en/html/widgets/index.html)
4. [LVGL 样式参考](https://docs.lvgl.io/9.2/en/html/overview/style.html)
5. [LVGL 事件参考](https://docs.lvgl.io/9.2/en/html/overview/event.html)
