#include <stdint.h>
#include "bconfig.h"

uint8_t getNodes(node_t **_nodes);
void rs485_init();
void rs485sendEvent();
void sendNodeAction(action_cfg_t *act);
void sendNodeStatus(bool online);
void sendNodeEvent(node_io_event_t event, uint16_t inputStates, uint16_t outputStates);
void sendFullConfig(uint8_t *dst, uint8_t *cfg, uint32_t size);

typedef enum {
    BEVT_ACTION = 1,
    BEVT_NEWNODE,
    BEVT_NODESTATUS,    
    BEVT_IOEVENT
} bus_event_type_t;

typedef struct __attribute__((packed)) {
    bus_event_type_t event;
    uint8_t target_node[6];
    bool online;
    model_t model;
    io_event_t io_event;
    uint16_t inputStates;
    uint16_t outputStates;
    io_action_t io_action;
} bus_event_t;
typedef void (*TBusEvent) (bus_event_t data);
void registerBUSHandler(TBusEvent event);