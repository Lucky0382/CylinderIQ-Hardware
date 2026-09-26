// CylinderIQ Hub V2 — zigbee_coord.h (ESP32-C6)
// Zigbee 3.0 coordinator. Controls two MOES 20A smart switches
// directly — no Home Assistant, no middleware.

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SWITCH_TOP    = 0,
    SWITCH_BOTTOM = 1,
    SWITCH_SHOWER = 2,
    SWITCH_BATH   = 3,
    SWITCH_COUNT  = 4,
} switch_id_t;

typedef struct {
    bool     paired;
    bool     on;
    uint8_t  ieee[8];
    uint16_t short_addr;
    uint8_t  endpoint;
} zb_switch_t;

typedef struct {
    bool        open;
    switch_id_t target;
    uint8_t     remaining_s;
} zb_pair_state_t;

void            zigbee_coord_init(void);
void            zigbee_coord_permit_join(switch_id_t slot, uint8_t duration_s);
bool            zigbee_coord_switch_set(switch_id_t sw, bool on);
zb_switch_t     zigbee_coord_switch_get(switch_id_t sw);
zb_pair_state_t zigbee_coord_pair_state(void);
void            zigbee_coord_clear(switch_id_t sw);
void            zigbee_coord_get_network_info(uint16_t *pan_id, uint8_t *channel, bool *online);
void            zigbee_coord_reset_network(void);

#ifdef __cplusplus
}
#endif
