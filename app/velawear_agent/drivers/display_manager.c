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
  lv_obj_t *title;
  lv_obj_t *hint;

  lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111f), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  title = lv_label_create(screen);
  lv_label_set_text(title, "VelaWear");
  lv_obj_set_style_text_color(title, lv_color_hex(0x67e8f9), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

  display->status_label = lv_label_create(screen);
  lv_label_set_text(display->status_label, "Agent starting");
  lv_obj_set_style_text_color(display->status_label,
                              lv_color_hex(0x94a3b8), 0);
  lv_obj_align(display->status_label, LV_ALIGN_TOP_MID, 0, 72);

  display->metrics_label = lv_label_create(screen);
  lv_label_set_text(display->metrics_label,
                    "Motion: idle\nBattery: 100%\nHeart rate: --");
  lv_obj_set_style_text_color(display->metrics_label,
                              lv_color_hex(0xf8fafc), 0);
  lv_obj_align(display->metrics_label, LV_ALIGN_CENTER, 0, 0);

  hint = lv_label_create(screen);
  lv_label_set_text(hint, "USB power  |  IMU online");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x64748b), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
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
      lv_label_set_text(display->metrics_label, metrics);
      lv_label_set_text(display->status_label, "Agent running");
      display->last_update_ms = now;
    }

  lv_timer_handler();
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
