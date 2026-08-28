/* VelaWear Agent - watchface and LVGL application center */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <stdbool.h>

#include <lvgl/lvgl.h>

#include "velawear.h"
#include "../audio_hw_test.h"
#include "display_manager.h"

#define UI_BG_COLOR          0x000000
#define UI_SURFACE_COLOR     0x111827
#define UI_SURFACE_ALT_COLOR 0x1f2937
#define UI_PRIMARY_COLOR     0x007aff
#define UI_SUCCESS_COLOR     0x34c759
#define UI_WARNING_COLOR     0xff9500
#define UI_DANGER_COLOR      0xff3b30
#define UI_TEXT_COLOR        0xffffff
#define UI_MUTED_COLOR       0x8e8e93
#define UI_CARD_RADIUS       16
#define VELAWEAR_MVP_COMPANION_LINES 7

enum display_action
{
  DISPLAY_ACTION_AUDIO_SPEAKER = 1,
  DISPLAY_ACTION_AUDIO_MIC,
  DISPLAY_ACTION_MUSIC_PLAY,
  DISPLAY_ACTION_STOP_START,
  DISPLAY_ACTION_STOP_RESET,
  DISPLAY_ACTION_COMPANION_CHAT,
  DISPLAY_ACTION_COMPANION_ENCOURAGE,
  DISPLAY_ACTION_COMPANION_REST
};

enum display_audio_job
{
  DISPLAY_AUDIO_JOB_NONE = 0,
  DISPLAY_AUDIO_JOB_MELODY,
  DISPLAY_AUDIO_JOB_MIC
};

static const char *g_app_names[VELAWEAR_APP_COUNT] =
{
  "音频中心", "音乐", "运动", "秒表",
  "任务", "诊断", "设置", "关于", "AI女友"
};

static void display_detail_action_cb(lv_event_t *event);
static void display_queue_companion_prompt(velawear_display_t *display,
                                           const char *prompt);
static lv_font_t *g_font_regular;
static bool g_freetype_ready;
static bool g_font_is_freetype;
extern lv_font_t velawear_mvp_font;
static lv_obj_t *g_alert_overlay;
static lv_obj_t *g_alert_title;
static lv_obj_t *g_alert_message;

#define VELAWEAR_COMPANION_FACE_FRAMES 4U

static const char *g_companion_idle_faces[VELAWEAR_COMPANION_FACE_FRAMES] =
{
  "[ ^_^ ]", "{ ^o^ }", "[ ^_^ ]", "( -_- )"
};

static const char *g_companion_thinking_faces[VELAWEAR_COMPANION_FACE_FRAMES] =
{
  "[ -_- ]", "{ o_o }", "[ ... ]", "{ o_o }"
};

static const char *g_companion_speaking_faces[VELAWEAR_COMPANION_FACE_FRAMES] =
{
  "{ ^o^ }", "{ ^O^ }", "{ ^o^ }", "{ ^O^ }"
};

static const char *g_companion_error_faces[VELAWEAR_COMPANION_FACE_FRAMES] =
{
  "[ T_T ]", "( -_- )", "[ T_T ]", "( -_- )"
};

static const char *g_companion_disconnected_faces[VELAWEAR_COMPANION_FACE_FRAMES] =
{
  "[ ._. ]", "{ o_o }", "[ ._. ]", "{ o_o }"
};

static void *display_audio_worker(void *arg)
{
  velawear_display_t *display = (velawear_display_t *)arg;
  uint8_t job;
  int ret;

  pthread_mutex_lock(&display->lock);
  job = display->audio_job;
  pthread_mutex_unlock(&display->lock);

  if (job == DISPLAY_AUDIO_JOB_MIC)
    {
      ret = velawear_mic_hw_test();
    }
  else
    {
      ret = velawear_music_hw_test();
    }

  pthread_mutex_lock(&display->lock);
  display->audio_test_result = ret;
  display->audio_test_running = false;
  pthread_mutex_unlock(&display->lock);
  return NULL;
}

static int display_audio_start(velawear_display_t *display, uint8_t job)
{
  pthread_t finished_thread;
  bool reap_finished = false;
  int ret;

  pthread_mutex_lock(&display->lock);
  if (display->audio_test_running)
    {
      pthread_mutex_unlock(&display->lock);
      return 1;
    }

  if (display->audio_thread_started)
    {
      finished_thread = display->audio_thread;
      display->audio_thread_started = false;
      reap_finished = true;
    }

  display->audio_job = job;
  display->audio_test_result = -1;
  display->audio_test_running = true;
  pthread_mutex_unlock(&display->lock);

  if (reap_finished)
    {
      pthread_join(finished_thread, NULL);
    }

  ret = pthread_create(&display->audio_thread, NULL,
                       display_audio_worker, display);
  if (ret != 0)
    {
      pthread_mutex_lock(&display->lock);
      display->audio_test_running = false;
      pthread_mutex_unlock(&display->lock);
      syslog(LOG_ERR, "[Display] audio worker create failed: %d\n", ret);
      return -1;
    }

  pthread_mutex_lock(&display->lock);
  display->audio_thread_started = true;
  pthread_mutex_unlock(&display->lock);
  return 0;
}

static void display_audio_poll(velawear_display_t *display)
{
  pthread_t finished_thread;
  uint8_t job = DISPLAY_AUDIO_JOB_NONE;
  int ret = -1;
  bool reap_finished = false;

  pthread_mutex_lock(&display->lock);
  if (display->audio_thread_started && !display->audio_test_running)
    {
      finished_thread = display->audio_thread;
      job = display->audio_job;
      ret = display->audio_test_result;
      display->audio_thread_started = false;
      reap_finished = true;
    }
  pthread_mutex_unlock(&display->lock);

  if (!reap_finished)
    {
      return;
    }

  pthread_join(finished_thread, NULL);
  if (display->detail_hint != NULL && display->detail_active)
    {
      if (job == DISPLAY_AUDIO_JOB_MIC)
        {
          lv_label_set_text(display->detail_hint,
                            ret == 0 ? "麦克风采样完成" :
                            "麦克风不可用");
        }
      else
        {
          lv_label_set_text(display->detail_hint,
                            ret == 0 ? "播放完成" : "播放失败");
        }
    }
  syslog(LOG_INFO, "[Display] audio test job=%u result=%d\n", job, ret);
  display->last_update_ms = 0;
}

static void display_audio_join(velawear_display_t *display)
{
  pthread_t thread;
  bool join_thread = false;

  pthread_mutex_lock(&display->lock);
  if (display->audio_thread_started)
    {
      thread = display->audio_thread;
      display->audio_thread_started = false;
      join_thread = true;
    }
  pthread_mutex_unlock(&display->lock);

  if (join_thread)
    {
      pthread_join(thread, NULL);
    }
}

static uint32_t display_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void display_apply_font(lv_obj_t *obj)
{
  if (obj != NULL && g_font_regular != NULL)
    {
      lv_obj_set_style_text_font(obj, g_font_regular, 0);
    }
}

static lv_obj_t *display_surface(lv_obj_t *parent, lv_coord_t width,
                                 lv_coord_t height)
{
  lv_obj_t *obj = lv_obj_create(parent);

  lv_obj_set_size(obj, width, height);
  lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE_COLOR), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, lv_color_hex(UI_SURFACE_ALT_COLOR), 0);
  lv_obj_set_style_radius(obj, UI_CARD_RADIUS, 0);
  lv_obj_set_style_pad_all(obj, 10, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

static lv_obj_t *display_label(lv_obj_t *parent, const char *text,
                               uint32_t color)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_width(label, lv_pct(100));
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  display_apply_font(label);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
  return label;
}

static lv_obj_t *display_action_button(velawear_display_t *display,
                                       const char *text, int action)
{
  lv_obj_t *button;
  lv_obj_t *label;

  button = display_surface(display->detail_content, lv_pct(100), 48);
  lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(button, (void *)(uintptr_t)action);
  lv_obj_add_event_cb(button, display_detail_action_cb, LV_EVENT_CLICKED,
                      display);
  label = display_label(button, text, UI_TEXT_COLOR);
  lv_obj_center(label);
  return button;
}

static void display_queue_companion_prompt(velawear_display_t *display,
                                           const char *prompt)
{
  velawear_event_t event;
  size_t length;
  int ret;

  if (display == NULL || display->events == NULL || prompt == NULL)
    {
      return;
    }

  length = strlen(prompt);
  if (length == 0 || length >= sizeof(event.data.chat.text))
    {
      syslog(LOG_WARNING, "[HCI] Companion prompt length invalid: %lu\n",
             (unsigned long)length);
      return;
    }

  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_CHAT_INPUT;
  event.priority = VELAWEAR_PRIORITY_HIGH;
  event.timestamp = display_now_ms();
  event.data.chat.length = (int)length;
  memcpy(event.data.chat.text, prompt, length);
  event.data.chat.text[length] = '\0';
  ret = event_manager_push(display->events, &event);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[HCI] Companion prompt queue failed: %d\n", ret);
      return;
    }

  display_manager_set_companion_phase(display,
                                        VELAWEAR_COMPANION_THINKING,
                                        "我正在想怎么陪你...");
  syslog(LOG_INFO, "[HCI] Companion quick prompt queued bytes=%lu\n",
         (unsigned long)length);
}

static void display_update_nav_dots(velawear_display_t *display)
{
  uint32_t i;

  if (display == NULL)
    {
      return;
    }

  for (i = 0; i < VELAWEAR_PAGE_COUNT; i++)
    {
      if (display->nav_dots[i] == NULL)
        {
          continue;
        }

      lv_obj_set_width(display->nav_dots[i],
                       i == display->page_index ? 24 : 8);
      lv_obj_set_style_bg_color(display->nav_dots[i],
                                lv_color_hex(i == display->page_index ?
                                             UI_PRIMARY_COLOR : UI_MUTED_COLOR),
                                0);
    }
}

static void display_restore_theme(velawear_display_t *display)
{
  lv_obj_t *screen;

  if (display == NULL)
    {
      return;
    }

  screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG_COLOR), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  if (display->status_label != NULL)
    {
      lv_obj_set_style_text_color(display->status_label,
                                  lv_color_hex(UI_MUTED_COLOR), 0);
    }
  if (display->page_title != NULL)
    {
      lv_obj_set_style_text_color(display->page_title,
                                  lv_color_hex(UI_PRIMARY_COLOR), 0);
    }
}

static void display_back_cb(lv_event_t *event)
{
  velawear_display_t *display =
    (velawear_display_t *)lv_event_get_user_data(event);

  if (display != NULL)
    {
      display_manager_close_app(display);
    }
}

static void display_app_button_cb(lv_event_t *event)
{
  velawear_display_t *display =
    (velawear_display_t *)lv_event_get_user_data(event);
  lv_obj_t *target = lv_event_get_target(event);
  uintptr_t value;

  if (display == NULL || target == NULL)
    {
      return;
    }

  value = (uintptr_t)lv_obj_get_user_data(target);
  lv_event_stop_bubbling(event);
  display_manager_open_app(display, (velawear_display_app_t)value);
}

static void display_detail_action_cb(lv_event_t *event)
{
  velawear_display_t *display =
    (velawear_display_t *)lv_event_get_user_data(event);
  lv_obj_t *target = lv_event_get_target(event);
  int action;
  int ret;

  if (display == NULL || target == NULL)
    {
      return;
    }

  action = (int)(uintptr_t)lv_obj_get_user_data(target);
  lv_event_stop_bubbling(event);
  ret = 0;

  switch (action)
    {
      case DISPLAY_ACTION_AUDIO_SPEAKER:
      case DISPLAY_ACTION_MUSIC_PLAY:
        if (display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "正在播放测试旋律...");
          }
        ret = display_audio_start(display, DISPLAY_AUDIO_JOB_MELODY);
        if (ret > 0 && display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "测试进行中...");
          }
        else if (ret < 0 && display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "测试启动失败");
          }
        break;

      case DISPLAY_ACTION_AUDIO_MIC:
        if (display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "正在采集麦克风...");
          }
        ret = display_audio_start(display, DISPLAY_AUDIO_JOB_MIC);
        if (ret > 0 && display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "测试进行中...");
          }
        else if (ret < 0 && display->detail_hint != NULL)
          {
            lv_label_set_text(display->detail_hint, "测试启动失败");
          }
        break;

      case DISPLAY_ACTION_STOP_START:
        if (display->stopwatch_running)
          {
            display->stopwatch_elapsed_ms +=
              display_now_ms() - display->stopwatch_started_ms;
            display->stopwatch_running = false;
          }
        else
          {
            display->stopwatch_started_ms = display_now_ms();
            display->stopwatch_running = true;
          }
        display->last_update_ms = 0;
        break;

      case DISPLAY_ACTION_STOP_RESET:
        display->stopwatch_running = false;
        display->stopwatch_started_ms = 0;
        display->stopwatch_elapsed_ms = 0;
        display->last_update_ms = 0;
        break;

      case DISPLAY_ACTION_COMPANION_CHAT:
        display_queue_companion_prompt(display, "今天有点累，陪我聊两句");
        break;

      case DISPLAY_ACTION_COMPANION_ENCOURAGE:
        display_queue_companion_prompt(display, "夸夸我，给我一点鼓励");
        break;

      case DISPLAY_ACTION_COMPANION_REST:
        display_queue_companion_prompt(display, "提醒我休息一下");
        break;

      default:
        break;
    }
}

static int display_mvp_text_line(const velawear_display_t *display,
                                     int32_t line_count, int32_t y)
{
  lv_obj_t *label;
  lv_coord_t top;
  lv_coord_t height;
  lv_coord_t line_height;
  int32_t line;

  if (display == NULL || line_count <= 0)
    {
      return -1;
    }

  label = display->page_content[0];
  if (label == NULL)
    {
      return -1;
    }

  /*
   * Page text changes invalidate LVGL layout.  A touch can arrive before the
   * next timer pass recalculates the label geometry, which made a valid
   * release coordinate fall outside the stale hit box and return -1.
   */
  lv_obj_update_layout(lv_screen_active());
  top = lv_obj_get_y(label);
  height = lv_obj_get_height(label);
  line_height = height / (lv_coord_t)line_count;
  if (height <= 0 || line_height <= 0 || y < top || y >= top + height)
    {
      return -1;
    }

  line = (y - top) / line_height;
  return line < line_count ? (int)line : -1;
}

static void display_gesture_cb(lv_event_t *event)
{
  velawear_display_t *display;
  lv_indev_t *indev;
  lv_dir_t dir = LV_DIR_NONE;
  lv_point_t point = {0, 0};
  velawear_event_t touch_event;
  lv_event_code_t code;
  int app_index;
  int companion_line;
  int ret;
  int32_t dx;
  int32_t dy;

  display = (velawear_display_t *)lv_event_get_user_data(event);
  indev = lv_indev_active();
  if (display == NULL || indev == NULL || display->detail_active)
    {
      return;
    }

  code = lv_event_get_code(event);
  lv_indev_get_point(indev, &point);
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED ||
      code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED ||
      code == LV_EVENT_GESTURE)
    {
      syslog(LOG_INFO,
             "[Display] touch code=%d x=%ld y=%ld page=%u detail=%d mvp=%d app=%u\\n",
             (int)code, (long)point.x, (long)point.y,
             (unsigned int)display->page_index,
             display->detail_active ? 1 : 0,
             display->mvp_app_detail_active ? 1 : 0,
             (unsigned int)display->mvp_selected_app);
    }
  if (code == LV_EVENT_PRESSED)
    {
      display->touch_start = point;
      display->touch_tracking = true;
      display->touch_swipe_handled = false;
    }
  else if (code == LV_EVENT_GESTURE)
    {
      dir = lv_indev_get_gesture_dir(indev);
      display->touch_swipe_handled = true;
    }

  memset(&touch_event, 0, sizeof(touch_event));
  touch_event.type = VELAWEAR_EVENT_TOUCH;
  touch_event.priority = VELAWEAR_PRIORITY_NORMAL;
  touch_event.timestamp = display_now_ms();
  touch_event.data.touch.x = point.x;
  touch_event.data.touch.y = point.y;
  touch_event.data.touch.gesture = code == LV_EVENT_GESTURE ?
                                    (int)dir : (int)code;
  if (display->events != NULL &&
      event_manager_push(display->events, &touch_event) < 0)
    {
      syslog(LOG_WARNING, "[Display] Failed to queue touch event\n");
    }

  if (code == LV_EVENT_RELEASED)
    {
      dx = (int32_t)point.x - (int32_t)display->touch_start.x;
      dy = (int32_t)point.y - (int32_t)display->touch_start.y;
      if (display->touch_tracking && !display->touch_swipe_handled &&
          (dx <= -40 || dx >= 40) &&
          (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy))
        {
          display->touch_swipe_handled = true;
          if (dx < 0)
            {
              display_manager_next_page(display);
            }
          else
            {
              display_manager_previous_page(display);
            }
        }
      display->touch_tracking = false;
    }
  else if (code == LV_EVENT_CLICKED)
    {
      if (display->touch_swipe_handled)
        {
          display->touch_swipe_handled = false;
          return;
        }

      if (display->page_index == VELAWEAR_PAGE_APPS)
        {
          syslog(LOG_INFO,
                 "[Display] touch click state mvp=%d app=%u y=%ld\\n",
                 display->mvp_app_detail_active ? 1 : 0,
                 (unsigned int)display->mvp_selected_app, (long)point.y);
          if (display->mvp_app_detail_active)
            {
              if (display->mvp_selected_app == VELAWEAR_APP_AUDIO)
                {
                  ret = display_audio_start(
                    display,
                    point.y < 220 ? DISPLAY_AUDIO_JOB_MELODY :
                                    DISPLAY_AUDIO_JOB_MIC);
                  syslog(LOG_INFO,
                         "[Display] audio app action y=%ld result=%d\n",
                         (long)point.y, ret);
                }
              else if (display->mvp_selected_app == VELAWEAR_APP_MUSIC)
                {
                  ret = display_audio_start(display,
                                             DISPLAY_AUDIO_JOB_MELODY);
                  syslog(LOG_INFO,
                         "[Display] music app action result=%d\n", ret);
                }
              else if (display->mvp_selected_app == VELAWEAR_APP_STOPWATCH)
                {
                  if (point.y < 270)
                    {
                      if (display->stopwatch_running)
                        {
                          display->stopwatch_elapsed_ms +=
                            display_now_ms() -
                            display->stopwatch_started_ms;
                          display->stopwatch_running = false;
                        }
                      else
                        {
                          display->stopwatch_started_ms = display_now_ms();
                          display->stopwatch_running = true;
                        }
                    }
                  else
                    {
                      display->stopwatch_running = false;
                      display->stopwatch_started_ms = 0;
                      display->stopwatch_elapsed_ms = 0;
                    }
                }
              else if (display->mvp_selected_app == VELAWEAR_APP_COMPANION)
                {
                  companion_line = display_mvp_text_line(
                    display, VELAWEAR_MVP_COMPANION_LINES, (int32_t)point.y);
                  syslog(LOG_INFO,
                         "[Display] companion hit line=%d y=%ld\\n",
                         companion_line, (long)point.y);
                  if (companion_line == 4)
                    {
                      display_queue_companion_prompt(
                        display, "今天有点累，陪我聊两句");
                    }
                  else if (companion_line == 5)
                    {
                      display_queue_companion_prompt(
                        display, "夸夸我，给我一点鼓励");
                    }
                  else if (companion_line == 6)
                    {
                      display_queue_companion_prompt(
                        display, "提醒我休息一下");
                    }
                }
              else
                {
                  display->mvp_app_detail_active = false;
                }

              display->last_update_ms = 0;
              return;
            }

          app_index = display_mvp_text_line(display, VELAWEAR_APP_COUNT,
                                               (int32_t)point.y);
          syslog(LOG_INFO,
                 "[Display] app center hit line=%d y=%ld\\n",
                 app_index, (long)point.y);
          if (app_index >= 0 && app_index < VELAWEAR_APP_COUNT)
            {
              display->mvp_selected_app = (uint8_t)app_index;
              display->mvp_app_detail_active = true;
              display->last_update_ms = 0;
            }
          return;
        }

      display_manager_set_page(display,
                               display->page_index == VELAWEAR_PAGE_WATCHFACE ?
                               VELAWEAR_PAGE_APPS : VELAWEAR_PAGE_WATCHFACE);
    }
  else if (code == LV_EVENT_LONG_PRESSED)
    {
      display->touch_swipe_handled = true;
      display_manager_set_page(display, VELAWEAR_PAGE_WATCHFACE);
    }
  else if ((dir & LV_DIR_LEFT) != 0 || (dir & LV_DIR_TOP) != 0)
    {
      display_manager_next_page(display);
    }
  else if ((dir & LV_DIR_RIGHT) != 0 || (dir & LV_DIR_BOTTOM) != 0)
    {
      display_manager_previous_page(display);
    }
}

static void display_create_apps_page(velawear_display_t *display,
                                     lv_obj_t *tile)
{
  static const char *app_labels[VELAWEAR_APP_COUNT] =
    {
      "音频\n音频中心", "音乐\n播放器", "运动\n传感器", "时间\n秒表",
      "任务\n清单", "诊断\n系统诊断", "设置\n系统设置", "信息\n关于",
      "AI女友\n陪伴聊天"
    };
  uint32_t i;

  display->app_grid = lv_obj_create(tile);
  lv_obj_set_size(display->app_grid, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(display->app_grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(display->app_grid, 0, 0);
  lv_obj_set_style_pad_all(display->app_grid, 6, 0);
  lv_obj_set_style_pad_row(display->app_grid, 8, 0);
  lv_obj_set_style_pad_column(display->app_grid, 8, 0);
  lv_obj_set_flex_flow(display->app_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(display->app_grid, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(display->app_grid, LV_OBJ_FLAG_SCROLLABLE);

  for (i = 0; i < VELAWEAR_APP_COUNT; i++)
    {
      lv_obj_t *button = display_surface(display->app_grid, 154, 58);
      lv_obj_t *label = display_label(button, app_labels[i], UI_TEXT_COLOR);

      lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_user_data(button, (void *)(uintptr_t)i);
      lv_obj_add_event_cb(button, display_app_button_cb, LV_EVENT_CLICKED,
                          display);
      lv_obj_center(label);
      display->app_buttons[i] = button;
    }
}

static void display_create_detail(velawear_display_t *display,
                                  lv_obj_t *screen)
{
  lv_obj_t *back;
  lv_obj_t *label;

  display->detail_root = display_surface(screen, lv_pct(92), lv_pct(72));
  lv_obj_align(display->detail_root, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_pad_all(display->detail_root, 10, 0);

  back = lv_obj_create(display->detail_root);
  lv_obj_set_size(back, 70, 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(back, lv_color_hex(UI_SURFACE_ALT_COLOR), 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_set_style_radius(back, 10, 0);
  lv_obj_add_event_cb(back, display_back_cb, LV_EVENT_CLICKED, display);
  label = display_label(back, "< 返回", UI_TEXT_COLOR);
  lv_obj_center(label);

  display->detail_title = lv_label_create(display->detail_root);
  display_apply_font(display->detail_title);
  lv_obj_set_width(display->detail_title, 180);
  lv_obj_set_style_text_color(display->detail_title,
                              lv_color_hex(UI_PRIMARY_COLOR), 0);
  lv_obj_set_style_text_align(display->detail_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(display->detail_title, LV_ALIGN_TOP_MID, 0, 8);

  display->detail_content = lv_obj_create(display->detail_root);
  lv_obj_set_size(display->detail_content, lv_pct(100), lv_pct(78));
  lv_obj_align(display->detail_content, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_bg_opa(display->detail_content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(display->detail_content, 0, 0);
  lv_obj_set_style_pad_all(display->detail_content, 2, 0);
  lv_obj_set_style_pad_row(display->detail_content, 7, 0);
  lv_obj_set_flex_flow(display->detail_content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(display->detail_content, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(display->detail_content, LV_OBJ_FLAG_SCROLLABLE);

  display->detail_hint = lv_label_create(display->detail_root);
  display_apply_font(display->detail_hint);
  lv_obj_set_width(display->detail_hint, lv_pct(100));
  lv_obj_set_style_text_color(display->detail_hint,
                              lv_color_hex(UI_MUTED_COLOR), 0);
  lv_obj_set_style_text_align(display->detail_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(display->detail_hint, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_label_set_text(display->detail_hint, "点击操作");
  lv_obj_add_flag(display->detail_root, LV_OBJ_FLAG_HIDDEN);
}

static void display_render_app(velawear_display_t *display,
                               const velawear_state_t *state)
{
  char content[384];
  uint32_t elapsed;
  uint32_t minutes;
  uint32_t seconds;
  uint32_t tenths;

  if (display == NULL || state == NULL || !display->detail_active ||
      display->metrics_label == NULL)
    {
      return;
    }

  content[0] = '\0';
  switch ((velawear_display_app_t)display->active_app)
    {
      case VELAWEAR_APP_AUDIO:
        snprintf(content, sizeof(content),
                 "喇叭\n等待测试\n\n麦克风\n点击检查设备");
        break;

      case VELAWEAR_APP_MUSIC:
        snprintf(content, sizeof(content),
                 "内置旋律\n\n准备播放\n音量 70%%");
        break;

      case VELAWEAR_APP_MOTION:
        snprintf(content, sizeof(content),
                 "运动  %s\n跑步  %s\n今日活动 %lu\n运行时间 %lu 秒",
                 state->is_moving ? "活动" : "静止",
                 state->is_running ? "是" : "否",
                 (unsigned long)state->move_count_today,
                 (unsigned long)state->uptime_seconds);
        break;

      case VELAWEAR_APP_STOPWATCH:
        elapsed = display->stopwatch_elapsed_ms;
        if (display->stopwatch_running)
          {
            elapsed += display_now_ms() - display->stopwatch_started_ms;
          }
        minutes = elapsed / 60000;
        seconds = (elapsed / 1000) % 60;
        tenths = (elapsed / 100) % 10;
        snprintf(content, sizeof(content), "%02lu:%02lu.%lu\n\n%s",
                 (unsigned long)minutes, (unsigned long)seconds,
                 (unsigned long)tenths,
                 display->stopwatch_running ? "运行中" : "已暂停");
        break;

      case VELAWEAR_APP_TASKS:
        snprintf(content, sizeof(content),
                 "[完成] 音频初始化\n[完成] 喇叭测试\n[ ] 麦克风测试\n[ ] 音乐播放\n[ ] 功耗优化");
        break;

      case VELAWEAR_APP_DIAGNOSTICS:
        snprintf(content, sizeof(content),
                 "运行时间  %lu 秒\n剩余内存  %lu B\n电源模式  %d\n触摸      正常\n音频      点击测试",
                 (unsigned long)state->uptime_seconds,
                 (unsigned long)state->free_memory, state->power_mode);
        break;

      case VELAWEAR_APP_SETTINGS:
        snprintf(content, sizeof(content),
                 "深色主题\n屏幕      正常\n音量      70%%\n休眠      10/30/60 秒\n按键      短按返回，长按主页");
        break;

      case VELAWEAR_APP_ABOUT:
        snprintf(content, sizeof(content),
                 "VelaWear\nSF32LB52 黄山派\n固件 %d.%d.%d\nLVGL 显示\n构建目标：黄山派",
                 VELAWEAR_VERSION_MAJOR, VELAWEAR_VERSION_MINOR,
                 VELAWEAR_VERSION_PATCH);
        break;

      case VELAWEAR_APP_COMPANION:
        snprintf(content, sizeof(content),
                 "AI 女友\n\n我在这里，想聊什么？\n\n"
                 "1 陪我聊聊  2 给我鼓励\n3 提醒休息");
        break;

      default:
        snprintf(content, sizeof(content), "暂不可用");
        break;
    }

  lv_label_set_text(display->metrics_label, content);
}

static void display_build_app(velawear_display_t *display,
                              velawear_display_app_t app)
{
  lv_obj_t *summary;

  lv_obj_clean(display->detail_content);
  display->metrics_label = display_label(display->detail_content, "", UI_TEXT_COLOR);
  lv_obj_set_style_text_align(display->metrics_label, LV_TEXT_ALIGN_CENTER, 0);
  summary = display->metrics_label;
  (void)summary;

  switch (app)
    {
      case VELAWEAR_APP_AUDIO:
        display_action_button(display, "测试喇叭", DISPLAY_ACTION_AUDIO_SPEAKER);
        display_action_button(display, "测试麦克风", DISPLAY_ACTION_AUDIO_MIC);
        break;

      case VELAWEAR_APP_MUSIC:
        display_action_button(display, "播放 / 暂停", DISPLAY_ACTION_MUSIC_PLAY);
        break;

      case VELAWEAR_APP_STOPWATCH:
        display_action_button(display, "开始 / 暂停", DISPLAY_ACTION_STOP_START);
        display_action_button(display, "复位", DISPLAY_ACTION_STOP_RESET);
        break;

      case VELAWEAR_APP_COMPANION:
        display_action_button(display, "陪我聊聊", DISPLAY_ACTION_COMPANION_CHAT);
        display_action_button(display, "给我鼓励", DISPLAY_ACTION_COMPANION_ENCOURAGE);
        display_action_button(display, "提醒休息", DISPLAY_ACTION_COMPANION_REST);
        break;

      default:
        break;
    }
}

static void display_set_label_text(lv_obj_t *label, const char *text)
{
  const char *current;

  if (label == NULL || text == NULL)
    {
      return;
    }

  current = lv_label_get_text(label);
  if (current != NULL && strcmp(current, text) == 0)
    {
      return;
    }

  lv_label_set_text(label, text);
}

static void display_copy_utf8_prefix(char *dst, size_t dst_size,
                                      const char *src, size_t max_bytes)
{
  size_t used = 0;
  size_t char_size;
  unsigned char first;

  if (dst == NULL || dst_size == 0)
    {
      return;
    }

  dst[0] = '\0';
  if (src == NULL || max_bytes == 0)
    {
      return;
    }

  while (src[used] != '\0' && used < max_bytes)
    {
      first = (unsigned char)src[used];
      if (first < 0x80)
        {
          char_size = 1;
        }
      else if ((first & 0xe0) == 0xc0)
        {
          char_size = 2;
        }
      else if ((first & 0xf0) == 0xe0)
        {
          char_size = 3;
        }
      else if ((first & 0xf8) == 0xf0)
        {
          char_size = 4;
        }
      else
        {
          break;
        }

      if (used + char_size > max_bytes || used + char_size >= dst_size)
        {
          break;
        }

      memcpy(dst + used, src + used, char_size);
      used += char_size;
    }

  dst[used] = '\0';
}

static void display_render_top(velawear_display_t *display,
                               const velawear_state_t *state)
{
  char content[256];
  char hint[48];
  char companion_message[256];
  char companion_line[96];
  time_t now;
  struct tm tm_now;
  const char *ble;
  const char *companion_face;
  const char *companion_text;
  uint8_t companion_phase = VELAWEAR_COMPANION_IDLE;
  uint8_t companion_frame;

  if (display == NULL || state == NULL || display->detail_active)
    {
      return;
    }

  ble = state->ble_connected ? "已连接" : "未连接";
  companion_frame = (uint8_t)((display_now_ms() / 250U) %
                             VELAWEAR_COMPANION_FACE_FRAMES);
  memset(&tm_now, 0, sizeof(tm_now));
  now = time(NULL);
  localtime_r(&now, &tm_now);

  if (display->page_index == VELAWEAR_PAGE_WATCHFACE)
    {
      snprintf(content, sizeof(content),
               "%02d:%02d\n\n%04d-%02d-%02d\n\n电量 %lu%%   蓝牙 %s\n\n音频中心  >",
               tm_now.tm_hour, tm_now.tm_min,
               tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
               (unsigned long)state->battery_level, ble);
      display_set_label_text(display->page_title, "黄山派");
      display_set_label_text(display->page_content[0], content);
      display->metrics_label = display->page_content[0];
      snprintf(hint, sizeof(hint), "左滑: 应用 | 1/2");
      display_set_label_text(display->page_hint, hint);
    }
  else if (display->mvp_app_detail_active)
    {
      switch (display->mvp_selected_app)
        {
          case VELAWEAR_APP_AUDIO:
            snprintf(content, sizeof(content),
                     "音频测试\n上半屏：测试喇叭\n下半屏：测试麦克风\n点击执行");
            break;
          case VELAWEAR_APP_MUSIC:
            snprintf(content, sizeof(content),
                     "音乐\n点击屏幕播放内置旋律\n长按返回");
            break;
          case VELAWEAR_APP_MOTION:
            snprintf(content, sizeof(content),
                     "运动\n模式: %s\n蓝牙: %s\nIMU 运行中",
                     state->is_moving ? "活动" : "静止",
                     state->ble_connected ? "已连接" : "就绪");
            break;
          case VELAWEAR_APP_STOPWATCH:
            snprintf(content, sizeof(content),
                     "秒表\n上半屏：开始/暂停\n下半屏：复位\n长按返回");
            break;
          case VELAWEAR_APP_TASKS:
            snprintf(content, sizeof(content),
                     "任务\nIMU -> 运行\n运行 -> 蓝牙\n系统: 就绪");
            break;
          case VELAWEAR_APP_DIAGNOSTICS:
            snprintf(content, sizeof(content),
                     "诊断\n运行时间: %lu 秒\n剩余内存: %lu B\n电源模式: %d",
                     (unsigned long)state->uptime_seconds,
                     (unsigned long)state->free_memory, state->power_mode);
            break;
          case VELAWEAR_APP_SETTINGS:
            snprintf(content, sizeof(content),
                     "设置\n主题: 深色\n蓝牙: 就绪\n音频: 停止");
            break;
          case VELAWEAR_APP_COMPANION:
            companion_phase = VELAWEAR_COMPANION_IDLE;
            companion_message[0] = '\0';
            pthread_mutex_lock(&display->lock);
            companion_phase = display->companion_phase;
            strncpy(companion_message, display->companion_message,
                    sizeof(companion_message) - 1);
            companion_message[sizeof(companion_message) - 1] = '\0';
            pthread_mutex_unlock(&display->lock);

            companion_line[0] = '\0';
            if (!state->ble_connected)
              {
                companion_face = g_companion_disconnected_faces[companion_frame];
                companion_text = "等待手表连接";
              }
            else
              {
                switch (companion_phase)
                  {
                    case VELAWEAR_COMPANION_THINKING:
                      companion_face = g_companion_thinking_faces[companion_frame];
                      companion_text = "我正在认真想怎么回复";
                      break;
                    case VELAWEAR_COMPANION_SPEAKING:
                      companion_face = g_companion_speaking_faces[companion_frame];
                      display_copy_utf8_prefix(companion_line,
                                               sizeof(companion_line),
                                               companion_message,
                                               sizeof(companion_line) - 1);
                      companion_text = companion_line[0] != '\0' ?
                                       companion_line : "我在回应你";
                      break;
                    case VELAWEAR_COMPANION_ERROR:
                      companion_face = g_companion_error_faces[companion_frame];
                      display_copy_utf8_prefix(companion_line,
                                               sizeof(companion_line),
                                               companion_message,
                                               sizeof(companion_line) - 1);
                      companion_text = companion_line[0] != '\0' ?
                                       companion_line : "连接有点问题";
                      break;
                    case VELAWEAR_COMPANION_IDLE:
                    default:
                      companion_face = g_companion_idle_faces[companion_frame];
                      display_copy_utf8_prefix(companion_line,
                                               sizeof(companion_line),
                                               companion_message,
                                               sizeof(companion_line) - 1);
                      companion_text = companion_line[0] != '\0' ?
                                       companion_line : "我在这里陪你";
                      break;
                  }
              }

            snprintf(content, sizeof(content),
                     "%s\n\n%s\n\n"
                     "1  陪我聊聊\n"
                     "2  给我鼓励\n"
                     "3  提醒休息",
                     companion_face, companion_text);
            break;
          default:
            snprintf(content, sizeof(content),
                     "关于\nSF32LB52 黄山派\nOpenVela MVP\nVelaWear 应用");
            break;
        }
      if (display->mvp_selected_app == VELAWEAR_APP_COMPANION)
        {
          display_set_label_text(display->page_title, "AI 女友");
          snprintf(hint, sizeof(hint), "轻触选话题 | 长按返回");
        }
      else
        {
          display_set_label_text(display->page_title, "应用页");
          snprintf(hint, sizeof(hint), "点击返回: 应用 | 2/2");
        }
      display_set_label_text(display->page_content[0], content);
      display->metrics_label = display->page_content[0];
      display_set_label_text(display->page_hint, hint);
    }
  else
    {
      snprintf(content, sizeof(content),
               "1  音频测试\n"
               "2  音乐\n"
               "3  运动\n"
               "4  秒表\n"
               "5  任务\n"
               "6  诊断\n"
               "7  设置\n"
               "8  关于\n"
               "9  AI女友");
      display_set_label_text(display->page_title, "应用中心");
      display_set_label_text(display->page_content[0], content);
      display->metrics_label = display->page_content[0];
      snprintf(hint, sizeof(hint), "点击应用: 返回 | 2/2");
      display_set_label_text(display->page_hint, hint);
    }

  if (display->mvp_app_detail_active &&
      display->mvp_selected_app == VELAWEAR_APP_COMPANION)
    {
      if (!state->ble_connected)
        {
          display_set_label_text(display->status_label, "陪伴 | 等待手表");
        }
      else
        {
          switch (companion_phase)
            {
              case VELAWEAR_COMPANION_THINKING:
                display_set_label_text(display->status_label, "陪伴 | 思考中");
                break;
              case VELAWEAR_COMPANION_SPEAKING:
                display_set_label_text(display->status_label, "陪伴 | 说话中");
                break;
              case VELAWEAR_COMPANION_ERROR:
                display_set_label_text(display->status_label, "陪伴 | 连接异常");
                break;
              case VELAWEAR_COMPANION_IDLE:
              default:
                display_set_label_text(display->status_label, "陪伴 | 待命");
                break;
            }
        }
    }
  else
    {
      display_set_label_text(display->status_label, "系统  |  就绪");
    }
  display_update_nav_dots(display);
}

static void display_create_ui(velawear_display_t *display)
{
  lv_obj_t *screen = lv_screen_active();

  /* Keep the BLE + LCD MVP to four labels.  The full TileView/app center
   * allocates too many LVGL draw objects for the SF32LB52 heap once a GATT
   * connection is active. */
  lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG_COLOR), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(screen, display_gesture_cb, LV_EVENT_PRESSED, display);
  lv_obj_add_event_cb(screen, display_gesture_cb, LV_EVENT_RELEASED, display);
  lv_obj_add_event_cb(screen, display_gesture_cb, LV_EVENT_CLICKED, display);
  lv_obj_add_event_cb(screen, display_gesture_cb, LV_EVENT_LONG_PRESSED, display);
  lv_obj_add_event_cb(screen, display_gesture_cb, LV_EVENT_GESTURE, display);

  display->status_label = lv_label_create(screen);
  display_apply_font(display->status_label);
  lv_label_set_text(display->status_label, "模式 | 就绪");
  lv_obj_set_style_text_color(display->status_label,
                              lv_color_hex(UI_MUTED_COLOR), 0);
  lv_obj_align(display->status_label, LV_ALIGN_TOP_LEFT, 8, 6);

  display->page_title = lv_label_create(screen);
  display_apply_font(display->page_title);
  lv_label_set_text(display->page_title, "黄山派");
  lv_obj_set_style_text_color(display->page_title,
                              lv_color_hex(UI_PRIMARY_COLOR), 0);
  lv_obj_align(display->page_title, LV_ALIGN_TOP_MID, 0, 6);

  display->page_content[0] = lv_label_create(screen);
  display_apply_font(display->page_content[0]);
  lv_obj_set_width(display->page_content[0], lv_pct(100));
  lv_obj_set_style_text_color(display->page_content[0],
                              lv_color_hex(UI_TEXT_COLOR), 0);
  lv_obj_set_style_text_align(display->page_content[0],
                              LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(display->page_content[0], LV_ALIGN_CENTER, 0, 0);
  display->metrics_label = display->page_content[0];

  display->page_hint = lv_label_create(screen);
  display_apply_font(display->page_hint);
  lv_label_set_text(display->page_hint, "IMU -> 蓝牙");
  lv_obj_set_style_text_color(display->page_hint,
                              lv_color_hex(UI_MUTED_COLOR), 0);
  lv_obj_align(display->page_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

  display->tileview = NULL;
  display->app_grid = NULL;
  display->detail_root = NULL;
  display->detail_title = NULL;
  display->detail_content = NULL;
  display->detail_hint = NULL;
  g_alert_overlay = NULL;
  g_alert_title = NULL;
  g_alert_message = NULL;
}

int display_manager_init(velawear_display_t *display,
                         velawear_state_mgr_t *state_mgr,
                         velawear_events_t *events)
{
  lv_nuttx_dsc_t info;
  lv_result_t freetype_result;
  lv_font_t *runtime_font;

  if (display == NULL || state_mgr == NULL || events == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(display, 0, sizeof(*display));
  display->state_mgr = state_mgr;
  display->events = events;

  if (lv_is_initialized())
    {
      syslog(LOG_WARNING, "[Display] LVGL already initialized\n");
      return VELAWEAR_ERR_BUSY;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/lcd0";
  info.input_path = "/dev/input0";
  lv_nuttx_init(&info, &display->lv_result);

  if (display->lv_result.disp == NULL)
    {
      syslog(LOG_ERR, "[Display] Failed to initialize /dev/lcd0\n");
      lv_deinit();
      return VELAWEAR_ERR_IO;
    }

  /*
   * Use one shared MiSans font for every label.  It is packaged in the board
   * ROMFS at /etc/data/font, so cloud replies are not limited to a generated
   * glyph list. Keep the compiled font as an emergency fallback.
   */
  g_font_regular = &velawear_mvp_font;
  g_font_is_freetype = false;
  freetype_result = lv_freetype_init(256);
  if (freetype_result == LV_RESULT_OK)
    {
      g_freetype_ready = true;
    }
  else if (freetype_result != LV_RESULT_INVALID)
    {
      syslog(LOG_WARNING, "[Display] FreeType engine unavailable\n");
    }

  /*
   * UIKIT may already own the global FreeType engine; INVALID from init is
   * expected in that case and does not prevent creating our font.
   */
  runtime_font = lv_freetype_font_create(
    "/etc/data/font/MiSans-Regular.ttf",
    LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20,
    LV_FREETYPE_FONT_STYLE_NORMAL);
  if (runtime_font != NULL)
    {
      /* Use the common-character bitmap set for any glyph MiSans lacks. */
      runtime_font->fallback = &velawear_mvp_font;
      g_font_regular = runtime_font;
      g_font_is_freetype = true;
      syslog(LOG_INFO, "[Display] Unified MiSans runtime font active\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "[Display] Unified MiSans unavailable; using embedded fallback\n");
    }

  display_create_ui(display);
  pthread_mutex_init(&display->lock, NULL);
  display->initialized = true;
  display->last_update_ms = 0;
  display->page_index = VELAWEAR_PAGE_WATCHFACE;
  syslog(LOG_INFO, "[Display] Watchface and APP center initialized\n");
  return VELAWEAR_OK;
}

void display_manager_tick(velawear_display_t *display)
{
  uint32_t now;
  velawear_state_t state;
  lv_obj_t *alert_label;

  if (display == NULL || !display->initialized)
    {
      return;
    }

  now = display_now_ms();
  display_audio_poll(display);
  pthread_mutex_lock(&display->lock);
  if (display->alert_pending)
    {
      lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x5b1111), 0);
      lv_label_set_text(display->status_label, "主动提醒");
      lv_obj_set_style_text_color(display->status_label,
                                  lv_color_hex(0xfca5a5), 0);
      alert_label = display->detail_active ? display->metrics_label :
                    display->page_content[0];
      if (alert_label != NULL)
        {
          lv_label_set_text(alert_label, display->pending_alert);
          lv_obj_set_style_text_color(alert_label,
                                      lv_color_hex(UI_TEXT_COLOR), 0);
        }
      if (g_alert_message != NULL && g_alert_overlay != NULL)
        {
          lv_label_set_text(g_alert_message, display->pending_alert);
          lv_obj_clear_flag(g_alert_overlay, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(g_alert_overlay);
        }
      display->alert_until_ms = now + 60000;
      display->alert_active = true;
      syslog(LOG_INFO,
             "[Display] Alert consumed page=%u detail=%d label=%p hold=60000ms text=%s\n",
             (unsigned int)display->page_index, display->detail_active ? 1 : 0,
             alert_label, display->pending_alert);
      display->alert_pending = false;
    }
  pthread_mutex_unlock(&display->lock);

  if (display->alert_active)
    {
      if (now < display->alert_until_ms)
        {
          lv_timer_handler();
          return;
        }

      display->alert_active = false;
      syslog(LOG_INFO, "[Display] Alert expired\n");
      if (g_alert_overlay != NULL)
        {
          lv_obj_add_flag(g_alert_overlay, LV_OBJ_FLAG_HIDDEN);
        }
      display_restore_theme(display);
      display->last_update_ms = 0;
    }

  if (display->last_update_ms == 0 || now - display->last_update_ms >=
      (display->active_app == VELAWEAR_APP_STOPWATCH &&
       display->stopwatch_running ? 100 : 250))
    {
      state = state_manager_get_state(display->state_mgr);
      if (display->detail_active)
        {
          display_render_app(display, &state);
        }
      else
        {
          display_render_top(display, &state);
        }
      display->last_update_ms = now;
    }

  lv_timer_handler();
}

void display_manager_set_page(velawear_display_t *display,
                              velawear_display_page_t page)
{
  if (display == NULL || !display->initialized || page >= VELAWEAR_PAGE_COUNT)
    {
      return;
    }

  if (display->detail_active)
    {
      display_manager_close_app(display);
    }

  display->page_index = (uint8_t)page;
  display->mvp_app_detail_active = false;
  display->metrics_label = display->page_index == VELAWEAR_PAGE_WATCHFACE ?
                           display->page_content[0] : NULL;
  display_update_nav_dots(display);
  if (display->tileview != NULL)
    {
      lv_tileview_set_tile_by_index(display->tileview, display->page_index, 0,
                                    LV_ANIM_ON);
    }
  display->last_update_ms = 0;
}

void display_manager_next_page(velawear_display_t *display)
{
  if (display == NULL || !display->initialized || display->detail_active)
    {
      return;
    }

  display_manager_set_page(display,
                           (velawear_display_page_t)
                           ((display->page_index + 1) % VELAWEAR_PAGE_COUNT));
}

void display_manager_previous_page(velawear_display_t *display)
{
  if (display == NULL || !display->initialized || display->detail_active)
    {
      return;
    }

  display_manager_set_page(display,
                           (velawear_display_page_t)
                           ((display->page_index + VELAWEAR_PAGE_COUNT - 1) %
                            VELAWEAR_PAGE_COUNT));
}

void display_manager_open_app(velawear_display_t *display,
                              velawear_display_app_t app)
{
  if (display == NULL || !display->initialized || app >= VELAWEAR_APP_COUNT)
    {
      return;
    }

  /* The production image uses a low-memory label-only app center.  Keep the
   * public API safe if a caller selects an app while the optional full LVGL
   * detail objects are absent. */
  if (display->detail_root == NULL)
    {
      display->page_index = VELAWEAR_PAGE_APPS;
      display->mvp_selected_app = (uint8_t)app;
      display->mvp_app_detail_active = true;
      display->last_update_ms = 0;
      return;
    }

  display->active_app = (uint8_t)app;
  display->detail_active = true;
  if (display->tileview != NULL)
    {
      lv_obj_add_flag(display->tileview, LV_OBJ_FLAG_HIDDEN);
    }
  if (display->page_hint != NULL)
    {
      lv_obj_add_flag(display->page_hint, LV_OBJ_FLAG_HIDDEN);
    }
  for (uint32_t i = 0; i < VELAWEAR_PAGE_COUNT; i++)
    {
      if (display->nav_dots[i] != NULL)
        {
          lv_obj_add_flag(display->nav_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
  lv_obj_clear_flag(display->detail_root, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(display->detail_title, g_app_names[app]);
  lv_label_set_text(display->detail_hint, "点击操作");
  display_build_app(display, app);
  display->last_update_ms = 0;
}

void display_manager_close_app(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  display->detail_active = false;
  display->mvp_app_detail_active = false;
  display->active_app = 0;
  if (display->detail_root != NULL)
    {
      lv_obj_add_flag(display->detail_root, LV_OBJ_FLAG_HIDDEN);
    }
  if (display->tileview != NULL)
    {
      lv_obj_clear_flag(display->tileview, LV_OBJ_FLAG_HIDDEN);
    }
  if (display->page_hint != NULL)
    {
      lv_obj_clear_flag(display->page_hint, LV_OBJ_FLAG_HIDDEN);
    }
  for (uint32_t i = 0; i < VELAWEAR_PAGE_COUNT; i++)
    {
      if (display->nav_dots[i] != NULL)
        {
          lv_obj_clear_flag(display->nav_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
  display->metrics_label = display->page_index == VELAWEAR_PAGE_WATCHFACE ?
                           display->page_content[0] : NULL;
  display->last_update_ms = 0;
}

bool display_manager_is_busy(const velawear_display_t *display)
{
  return display != NULL &&
         ((display->detail_active &&
           (display->active_app == VELAWEAR_APP_AUDIO ||
            display->active_app == VELAWEAR_APP_MUSIC ||
            display->active_app == VELAWEAR_APP_STOPWATCH)) ||
          (display->mvp_app_detail_active &&
           (display->mvp_selected_app == VELAWEAR_APP_AUDIO ||
            display->mvp_selected_app == VELAWEAR_APP_MUSIC ||
            display->mvp_selected_app == VELAWEAR_APP_STOPWATCH)));
}

void display_manager_show_alert(velawear_display_t *display,
                                const char *message)
{
  if (display == NULL || !display->initialized || message == NULL)
    {
      return;
    }

  pthread_mutex_lock(&display->lock);
  strncpy(display->pending_alert, message,
          sizeof(display->pending_alert) - 1);
  display->pending_alert[sizeof(display->pending_alert) - 1] = '\0';
  display->alert_pending = true;
  pthread_mutex_unlock(&display->lock);
}

void display_manager_set_companion_phase(velawear_display_t *display,
                                          velawear_companion_phase_t phase,
                                          const char *message)
{
  bool changed;

  if (display == NULL || !display->initialized ||
      phase > VELAWEAR_COMPANION_ERROR)
    {
      return;
    }

  pthread_mutex_lock(&display->lock);
  changed = display->companion_phase != (uint8_t)phase;
  if (message != NULL && strcmp(display->companion_message, message) != 0)
    {
      changed = true;
    }
  display->companion_phase = (uint8_t)phase;
  if (message != NULL)
    {
      strncpy(display->companion_message, message,
              sizeof(display->companion_message) - 1);
      display->companion_message[sizeof(display->companion_message) - 1] = '\0';
    }
  pthread_mutex_unlock(&display->lock);

  if (changed)
    {
      display->last_update_ms = 0;
      syslog(LOG_INFO,
             "[Display] Companion phase=%u text_bytes=%lu\n",
             (unsigned int)phase,
             message == NULL ? 0UL : (unsigned long)strlen(message));
    }
}

void display_manager_clear_alert(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  pthread_mutex_lock(&display->lock);
  display->alert_pending = false;
  display->alert_active = false;
  display->alert_until_ms = 0;
  pthread_mutex_unlock(&display->lock);

  if (g_alert_overlay != NULL)
    {
      lv_obj_add_flag(g_alert_overlay, LV_OBJ_FLAG_HIDDEN);
    }
  display_restore_theme(display);
  display->last_update_ms = 0;
  syslog(LOG_INFO, "[Display] Alert acknowledged\n");
}

void display_manager_cleanup(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  if (display->lv_result.disp != NULL)
    {
      lv_display_set_driver_data(display->lv_result.disp, NULL);
    }

  display_audio_join(display);
  lv_nuttx_deinit(&display->lv_result);
  if (g_font_is_freetype && g_font_regular != NULL)
    {
      lv_freetype_font_delete(g_font_regular);
    }
  g_font_regular = NULL;
  g_font_is_freetype = false;
  if (g_freetype_ready)
    {
      lv_freetype_uninit();
      g_freetype_ready = false;
    }
  lv_deinit();
  g_alert_overlay = NULL;
  g_alert_title = NULL;
  g_alert_message = NULL;
  pthread_mutex_destroy(&display->lock);
  display->initialized = false;
  syslog(LOG_INFO, "[Display] LCD/LVGL cleaned up\n");
}
