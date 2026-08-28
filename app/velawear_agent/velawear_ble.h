/*
 * VelaWear Agent - BLE service
 */

#ifndef __VELAWEAR_BLE_H
#define __VELAWEAR_BLE_H

#include "../velawear.h"

int velawear_ble_init(velawear_config_t *config);
void velawear_ble_cleanup(void);
void velawear_ble_update_motion(int motion_type, float intensity);
void velawear_ble_set_alert(bool active);

#endif /* __VELAWEAR_BLE_H */
