/* VelaWear Agent - LCD/LVGL display manager */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "velawear.h"
#include "display_manager.h"

static uint32_t display_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void display_gesture_cb(lv_event_t *event)
{
  velawear_display_t *display;
  lv_indev_t *indev;
  lv_dir_t dir = LV_DIR_NONE;
  lv_point_t point;
  velawear_event_t touch_event;
  lv_event_code_t code;

  display = (velawear_display_t *)lv_event_get_user_data(event);
  indev = lv_indev_active();
  if (display == NULL || indev == NULL)
    {
      return;
    }

  code = lv_event_get_code(event);
  lv_indev_get_point(indev, &point);
  if (code == LV_EVENT_GESTURE)
    {
      dir = lv_indev_get_gesture_dir(indev);
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
      syslog(LOG_WARNING, "[Display] Failed to queue touch gesture\n");
    }

  if (code == LV_EVENT_CLICKED)
    {
      /* A short press returns to the always-available watchface. */
      display_manager_set_page(display, VELAWEAR_PAGE_WATCHFACE);
    }
  else if (code == LV_EVENT_LONG_PRESSED)
    {
      /* Long press opens settings until a dedicated settings menu exists. */
      display_manager_set_page(display, VELAWEAR_PAGE_SETTINGS);
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

static void display_create_ui(velawear_display_t *display)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *tile;
  lv_obj_t *label;
  static const char *page_names[VELAWEAR_PAGE_COUNT] =
    {
      "Watchface", "Data Panel", "Task List", "Settings"
    };

  lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111f), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  display->status_label = lv_label_create(screen);
  lv_label_set_text(display->status_label, "Agent starting");
  lv_obj_set_style_text_color(display->status_label,
                              lv_color_hex(0x94a3b8), 0);
  lv_obj_align(display->status_label, LV_ALIGN_TOP_MID, 0, 18);

  display->page_title = lv_label_create(screen);
  lv_label_set_text(display->page_title, page_names[0]);
  lv_obj_set_style_text_color(display->page_title,
                              lv_color_hex(0x67e8f9), 0);
  lv_obj_align(display->page_title, LV_ALIGN_TOP_MID, 0, 50);

  display->tileview = lv_tileview_create(screen);
  lv_obj_set_size(display->tileview, lv_pct(100), lv_pct(66));
  lv_obj_align(display->tileview, LV_ALIGN_CENTER, 0, 8);
  for (uint32_t i = 0; i < VELAWEAR_PAGE_COUNT; i++)
    {
      tile = lv_tileview_add_tile(display->tileview, (uint8_t)i, 0,
                                  LV_DIR_HOR);
      lv_obj_set_style_bg_color(tile, lv_color_hex(0x07111f), 0);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
      label = lv_label_create(tile);
      lv_label_set_text(label, page_names[i]);
      lv_obj_set_width(label, lv_pct(90));
      lv_obj_set_style_text_color(label, lv_color_hex(0xf8fafc), 0);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
      display->page_content[i] = label;
    }
  lv_obj_add_event_cb(display->tileview, display_gesture_cb,
                      LV_EVENT_GESTURE, display);
  lv_obj_add_event_cb(display->tileview, display_gesture_cb,
                      LV_EVENT_CLICKED, display);
  lv_obj_add_event_cb(display->tileview, display_gesture_cb,
                      LV_EVENT_LONG_PRESSED, display);
  display->metrics_label = display->page_content[0];

  display->page_hint = lv_label_create(screen);
  lv_label_set_text(display->page_hint, "Swipe to change | 1/4");
  lv_obj_set_style_text_color(display->page_hint, lv_color_hex(0x64748b), 0);
  lv_obj_align(display->page_hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

static void display_render_page(velawear_display_t *display,
                                const velawear_state_t *state)
{
  char title[32];
  char content[256];
  char hint[48];
  time_t now;
  struct tm tm_now;
  const char *ble;
  const char *sedentary;

  if (display == NULL || state == NULL)
    {
      return;
    }

  ble = state->ble_connected ? "connected" : "disconnected";
  sedentary = state->is_moving ? "moving" : "monitoring";
  memset(&tm_now, 0, sizeof(tm_now));
  now = time(NULL);
  localtime_r(&now, &tm_now);

  switch ((velawear_display_page_t)display->page_index)
    {
      case VELAWEAR_PAGE_WATCHFACE:
        snprintf(title, sizeof(title), "Watchface");
        snprintf(content, sizeof(content),
                 "%02d:%02d:%02d\n%04d-%02d-%02d\nBattery: %lu%%\nBLE: %s",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 (unsigned long)state->battery_level, ble);
        break;

      case VELAWEAR_PAGE_DATA:
        snprintf(title, sizeof(title), "Data Panel");
        snprintf(content, sizeof(content),
                 "Moves today: %lu\nUptime: %lu s\nMotion: %s\nHeart rate: %s",
                 (unsigned long)state->move_count_today,
                 (unsigned long)state->uptime_seconds, sedentary,
                 state->heart_rate > 0.0f ? "available" : "--");
        break;

      case VELAWEAR_PAGE_TASKS:
        snprintf(title, sizeof(title), "Task List");
        snprintf(content, sizeof(content),
                 "- Move every 45 min\n- Stay hydrated\n- Check in with caregiver\n\nStatus: %s",
                 state->is_moving ? "active" : "ready");
        break;

      case VELAWEAR_PAGE_SETTINGS:
        snprintf(title, sizeof(title), "Settings");
        snprintf(content, sizeof(content),
                 "Theme: dark\nVibration: SOS\nBLE: %s\nDevice: SF32LB52",
                 ble);
        break;

      default:
        display->page_index = VELAWEAR_PAGE_WATCHFACE;
        display_render_page(display, state);
        return;
    }

  snprintf(hint, sizeof(hint), "Swipe to change | %u/4",
           (unsigned int)display->page_index + 1);
  display->metrics_label = display->page_content[display->page_index];
  lv_label_set_text(display->page_title, title);
  lv_label_set_text(display->metrics_label, content);
  lv_label_set_text(display->page_hint, hint);
}

int display_manager_init(velawear_display_t *display,
                         velawear_state_mgr_t *state_mgr,
                         velawear_events_t *events)
{
  lv_nuttx_dsc_t info;

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

  display_create_ui(display);
  pthread_mutex_init(&display->lock, NULL);
  display->initialized = true;
  display->last_update_ms = 0;
  syslog(LOG_INFO, "[Display] LCD/LVGL initialized\n");
  return VELAWEAR_OK;
}

void display_manager_tick(velawear_display_t *display)
{
  uint32_t now;
  velawear_state_t state;
  char metrics[128];
  const char *motion;
  const char *heart_rate;

  if (display == NULL || !display->initialized)
    {
      return;
    }

  now = display_now_ms();
  pthread_mutex_lock(&display->lock);
  if (display->alert_pending)
    {
      lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x5b1111), 0);
      lv_label_set_text(display->status_label, "FALL DETECTED!");
      lv_obj_set_style_text_color(display->status_label,
                                  lv_color_hex(0xfca5a5), 0);
      lv_label_set_text(display->metrics_label, display->pending_alert);
      lv_obj_set_style_text_color(display->metrics_label,
                                  lv_color_hex(0xffffff), 0);
      display->alert_until_ms = now + 5000;
      display->alert_active = true;
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
    }

  if (display->last_update_ms == 0 || now - display->last_update_ms >= 250)
    {
      state = state_manager_get_state(display->state_mgr);
      motion = state.is_running ? "running" :
               (state.is_moving ? "moving" : "idle");
      heart_rate = state.heart_rate > 0.0f ? "available" : "--";
      snprintf(metrics, sizeof(metrics),
               "Motion: %s\nBattery: %lu%%%s\nHeart rate: %s",
               motion, (unsigned long)state.battery_level,
               state.is_charging ? " (charging)" : "", heart_rate);
      display_render_page(display, &state);
      lv_label_set_text(display->status_label, "Agent running");
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

  display->page_index = (uint8_t)page;
  display->metrics_label = display->page_content[display->page_index];
  if (display->tileview != NULL)
    {
      lv_tileview_set_tile_by_index(display->tileview, display->page_index, 0,
                                    LV_ANIM_ON);
    }
  display->last_update_ms = 0;
}

void display_manager_next_page(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  display_manager_set_page(display,
                           (velawear_display_page_t)
                           ((display->page_index + 1) % VELAWEAR_PAGE_COUNT));
}

void display_manager_previous_page(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  display_manager_set_page(display,
                           (velawear_display_page_t)
                           ((display->page_index + VELAWEAR_PAGE_COUNT - 1) %
                            VELAWEAR_PAGE_COUNT));
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

void display_manager_cleanup(velawear_display_t *display)
{
  if (display == NULL || !display->initialized)
    {
      return;
    }

  /* The SF32 framebuffer driver owns its driver_data allocation.  Clear
   * the callback-owned pointer before LVGL deletes the display to avoid a
   * second free during shutdown. */
  if (display->lv_result.disp != NULL)
    {
      lv_display_set_driver_data(display->lv_result.disp, NULL);
    }

  lv_nuttx_deinit(&display->lv_result);
  lv_deinit();
  pthread_mutex_destroy(&display->lock);
  display->initialized = false;
  syslog(LOG_INFO, "[Display] LCD/LVGL cleaned up\n");
}
