/*
 * VelaWear Agent - BLE service
 */

#ifndef __VELAWEAR_BLE_H
#define __VELAWEAR_BLE_H

#include "../velawear.h"

int velawear_ble_init(velawear_config_t *config, velawear_events_t *events);
void velawear_ble_cleanup(void);
void velawear_ble_update_motion(int motion_type, float intensity);
void velawear_ble_set_alert(bool active);
int velawear_ble_set_sedentary_threshold(uint16_t seconds);
int velawear_ble_publish_agent_event(const velawear_event_t *event);
void velawear_ble_report_agent_command_result(uint16_t sequence,
                                              uint8_t command_id,
                                              uint8_t result);
int velawear_ble_send_message(const char *text, uint32_t priority);
int velawear_ble_request_llm(const char *prompt, char *response,
                              size_t response_size, uint32_t timeout_ms);

#endif /* __VELAWEAR_BLE_H */
