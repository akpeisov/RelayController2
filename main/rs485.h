#include <stdint.h>
#include "bconfig.h"

uint8_t getNodes(node_t **_nodes);
void rs485_init();
void rs485sendEvent();
void sendNodeAction(action_cfg_t *act);
void sendNodeEvent(input_event_t event);
void sendFullConfig(uint8_t *dst, uint8_t *cfg, uint32_t size);