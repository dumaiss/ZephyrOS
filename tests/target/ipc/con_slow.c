/* ZephyrOS IPC acceptance: the `con` service's hot-segment slow buffer
 * (test artifact).
 *
 * Its own compilation unit and data area (_HOT) put it in con's hot
 * segment, which the loader places outside bank 6's 8000h-BFFFh: that is
 * the deliberate service-side violation the path-C copy case needs
 * (correction 01 to architecture 36.8).
 */

#include "ipc_test.h"

uint8_t ipc_con_slow_buffer[IPC_CON_SLOW_BYTES];
