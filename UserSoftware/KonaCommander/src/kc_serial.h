#ifndef KC_SERIAL_H
#define KC_SERIAL_H

#include "kc_protocol.h"

#define KC_LINK_EDEVICE   -100
#define KC_LINK_ETYPE     -101
#define KC_LINK_ETIMER    -102
#define KC_LINK_EWRITE    -103
#define KC_LINK_ETIMEOUT  -104
#define KC_LINK_EFRAME    -105
#define KC_LINK_ECOBS     -106

int kc_link_open(void);
void kc_link_close(void);
int kc_link_request(struct kc_frame *request, struct kc_frame *response);

#endif

