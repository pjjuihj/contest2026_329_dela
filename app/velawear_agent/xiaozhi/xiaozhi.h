/*
 * VelaWear XiaoZhi client.
 *
 * The transport follows the public protocol used by 78/xiaozhi-sf32, while
 * the lifecycle and board integration are native to NuttX/CMake.
 */

#ifndef VELAWEAR_XIAOZHI_H
#define VELAWEAR_XIAOZHI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the XiaoZhi client in its worker thread. */
int velawear_xiaozhi_start(void);

/* Stop the client and wait for its worker thread to exit. */
int velawear_xiaozhi_stop(void);

/* Query the current server/session state. */
bool velawear_xiaozhi_is_connected(void);
bool velawear_xiaozhi_is_listening(void);

/* Push-to-talk controls used by the VelaWear button path. */
int velawear_xiaozhi_button_down(void);
int velawear_xiaozhi_button_up(void);

/* Explicit protocol controls for shell/tests and future UI actions. */
int velawear_xiaozhi_listen_start(void);
int velawear_xiaozhi_listen_stop(void);
int velawear_xiaozhi_abort(void);

/*
 * Run the foreground shell mode.  Supported options are:
 *   --no-ota     skip the OTA/activation exchange
 *   --listen     start microphone streaming after the session is ready
 */
int velawear_xiaozhi_run_cli(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* VELAWEAR_XIAOZHI_H */
