/* VelaWear Agent - LCD/LVGL display manager */
#ifndef __VELAWEAR_DISPLAY_MANAGER_H
#define __VELAWEAR_DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <lvgl/lvgl.h>
#include <core/state_manager.h>

typedef struct velawear_display
{
  bool initialized;
  velawear_state_mgr_t *state_mgr;
  lv_nuttx_result_t lv_result;
  lv_obj_t *status_label;
  lv_obj_t *metrics_label;
  uint32_t last_update_ms;
  uint32_t alert_until_ms;
  bool alert_active;
  bool alert_pending;
  char pending_alert[256];
  pthread_mutex_t lock;
} velawear_display_t;

int display_manager_init(velawear_display_t *display,
                         velawear_state_mgr_t *state_mgr);
void display_manager_tick(velawear_display_t *display);
void display_manager_show_alert(velawear_display_t *display,
                                const char *message);
void display_manager_cleanup(velawear_display_t *display);

#endif /* __VELAWEAR_DISPLAY_MANAGER_H */
