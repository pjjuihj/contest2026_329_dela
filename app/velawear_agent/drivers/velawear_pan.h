#ifndef __VELAWEAR_PAN_H
#define __VELAWEAR_PAN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int velawear_pan_init(void);
void velawear_pan_cleanup(void);
bool velawear_pan_is_configured(void);
bool velawear_pan_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAWEAR_PAN_H */
