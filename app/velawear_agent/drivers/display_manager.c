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

static void display_create_ui(velawear_display_t *display)
{
  lv_obj_t *screen = lv_screen_active();

  lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111f), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  display->status_label = lv_label_create(screen);
  lv_label_set_text(display->status_label, "Agent starting");
  lv_obj_set_style_text_color(display->status_label,
                              lv_color_hex(0x94a3b8), 0);
  lv_obj_align(display->status_label, LV_ALIGN_TOP_MID, 0, 18);

  display->page_title = lv_label_create(screen);
  lv_label_set_text(display->page_title, "Watchface");
  lv_obj_set_style_text_color(display->page_title,
                              lv_color_hex(0x67e8f9), 0);
  lv_obj_align(display->page_title, LV_ALIGN_TOP_MID, 0, 50);

  display->metrics_label = lv_label_create(screen);
  lv_label_set_text(display->metrics_label,
                    "VelaWear\nStarting...");
  lv_obj_set_style_text_color(display->metrics_label,
                              lv_color_hex(0xf8fafc), 0);
  lv_obj_align(display->metrics_label, LV_ALIGN_CENTER, 0, 0);

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
  lv_label_set_text(display->page_title, title);
  lv_label_set_text(display->metrics_label, content);
  lv_label_set_text(display->page_hint, hint);
}

int display_manager_init(velawear_display_t *display,
                         velawear_state_mgr_t *state_mgr)
{
  lv_nuttx_dsc_t info;

  if (display == NULL || state_mgr == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(display, 0, sizeof(*display));
  display->state_mgr = state_mgr;

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
