/* VelaWear Agent - LCD/LVGL display manager */
#ifndef __VELAWEAR_DISPLAY_MANAGER_H
#define __VELAWEAR_DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <lvgl/lvgl.h>
#include <core/state_manager.h>
#include <core/event_manager.h>

typedef enum velawear_display_page
{
  VELAWEAR_PAGE_WATCHFACE = 0,
  VELAWEAR_PAGE_APPS,
  VELAWEAR_PAGE_COUNT
} velawear_display_page_t;

typedef enum velawear_display_app
{
  VELAWEAR_APP_AUDIO = 0,
  VELAWEAR_APP_MUSIC,
  VELAWEAR_APP_MOTION,
  VELAWEAR_APP_STOPWATCH,
  VELAWEAR_APP_TASKS,
  VELAWEAR_APP_DIAGNOSTICS,
  VELAWEAR_APP_SETTINGS,
  VELAWEAR_APP_ABOUT,
  VELAWEAR_APP_COMPANION,
  VELAWEAR_APP_COUNT
} velawear_display_app_t;

typedef enum velawear_companion_phase
{
  VELAWEAR_COMPANION_IDLE = 0,
  VELAWEAR_COMPANION_THINKING,
  VELAWEAR_COMPANION_SPEAKING,
  VELAWEAR_COMPANION_ERROR
} velawear_companion_phase_t;

typedef struct velawear_display
{
  bool initialized;
  velawear_state_mgr_t *state_mgr;
  velawear_events_t *events;
  lv_nuttx_result_t lv_result;
  lv_obj_t *status_label;
  lv_obj_t *page_title;
  lv_obj_t *metrics_label;
  lv_obj_t *page_hint;
  lv_obj_t *tileview;
  lv_obj_t *page_content[VELAWEAR_PAGE_COUNT];
  lv_obj_t *nav_dots[VELAWEAR_PAGE_COUNT];
  lv_obj_t *app_grid;
  lv_obj_t *detail_root;
  lv_obj_t *detail_title;
  lv_obj_t *detail_content;
  lv_obj_t *detail_hint;
  lv_obj_t *app_buttons[VELAWEAR_APP_COUNT];
  uint32_t last_update_ms;
  uint8_t page_index;
  lv_point_t touch_start;
  bool touch_tracking;
  bool touch_swipe_handled;
  bool mvp_app_detail_active;
  uint8_t mvp_selected_app;
  uint8_t active_app;
  bool detail_active;
  bool stopwatch_running;
  uint32_t stopwatch_started_ms;
  uint32_t stopwatch_elapsed_ms;
  uint32_t alert_until_ms;
  bool alert_active;
  bool alert_pending;
  char pending_alert[256];
  uint8_t companion_phase;
  char companion_message[256];
  pthread_t audio_thread;
  uint8_t audio_job;
  bool audio_thread_started;
  bool audio_test_running;
  int audio_test_result;
  pthread_mutex_t lock;
} velawear_display_t;

int display_manager_init(velawear_display_t *display,
                         velawear_state_mgr_t *state_mgr,
                         velawear_events_t *events);
void display_manager_tick(velawear_display_t *display);
void display_manager_set_page(velawear_display_t *display,
                              velawear_display_page_t page);
void display_manager_next_page(velawear_display_t *display);
void display_manager_previous_page(velawear_display_t *display);
void display_manager_open_app(velawear_display_t *display,
                              velawear_display_app_t app);
void display_manager_close_app(velawear_display_t *display);
bool display_manager_is_busy(const velawear_display_t *display);
void display_manager_show_alert(velawear_display_t *display,
                                const char *message);
void display_manager_set_companion_phase(velawear_display_t *display,
                                          velawear_companion_phase_t phase,
                                          const char *message);
void display_manager_clear_alert(velawear_display_t *display);
void display_manager_cleanup(velawear_display_t *display);

#endif /* __VELAWEAR_DISPLAY_MANAGER_H */
