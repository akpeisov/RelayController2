#ifndef BCONFIG_H
#define BCONFIG_H

#include "esp_system.h"
#include <stdint.h>
#include <sys/types.h>

#define CFG_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
} node_uid_t;

//typedef enum {
typedef enum __attribute__((packed)) {
    OUTPUT_SIMPLE = 1,
    OUTPUT_TIMED,
    OUTPUT_ONESHOT
} output_type_t;

typedef struct __attribute__((packed)) {
    uint8_t id;
    output_type_t type;
    bool is_on;
    bool def_value;
    uint16_t timer;
    union {
        struct {
            uint16_t limit_sec;
        } simple;
        struct {
            uint16_t on_sec;
            uint16_t off_sec;
        } timed;
    };
} output_cfg_t;

typedef enum __attribute__((packed)) {
    INPUT_BTN = 1,
    INPUT_SWITCH,
    INPUT_SWITCH_INV
} input_type_t;

typedef enum __attribute__((packed)) {
        ACT_ON = 1,
        ACT_OFF,
        ACT_TOGGLE,
        ACT_WAIT,
        ACT_ALLOFF
} action_type_t;

typedef struct __attribute__((packed)) {
    node_uid_t target_node;
    uint8_t output_id;
    action_type_t action;
    uint16_t duration_sec;   
} action_cfg_t;

typedef enum __attribute__((packed)) {
    EVT_ON = 1,
    EVT_OFF,
    EVT_TOGGLE,
    EVT_LONGPRESS,
    EVT_UNKNOWN
} event_type_t;

typedef struct __attribute__((packed)) {
    uint8_t input_id;
    event_type_t event;
} input_event_t;

typedef struct __attribute__((packed)) {
    uint8_t io_id;
    uint8_t io_type; // input or output   
    uint8_t state;
} node_io_event_t;

typedef struct __attribute__((packed)) {
    event_type_t event;
    uint8_t actions_count;
    uint16_t actions_offset;
} input_event_cfg_t;

typedef struct __attribute__((packed)) {
    uint8_t id;
    input_type_t type;
    bool is_on;

    uint8_t events_count;
    uint16_t events_offset;
} input_cfg_t;

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint8_t outputs_count;
    uint8_t inputs_count;
    uint16_t events_count;
    uint16_t actions_count;

    uint8_t data[];
    // output_cfg_t outputs[];
    // input_cfg_t inputs[];
    // input_event_cfg_t events[];
    // action_cfg_t actions[];
} io_cfg_t;

typedef struct __attribute__((packed)) {
    node_uid_t uid;
    io_cfg_t io_cfg;    
} node_cfg_t;

typedef struct __attribute__((packed)) {
    uint8_t nodes_count;
    node_cfg_t nodes[];
} nodes_cfg_t;

typedef enum {
    UNKNOWN = 0,
    RCV1S,
    RCV1B,
    RCV2S,
    RCV2M,
    RCV2B,
} model_t;

extern model_t controllerType;

typedef struct {
    uint8_t mac[6];
    uint8_t model;
    int64_t last_seen;
    uint16_t outputStates;
    uint16_t inputStates;
    bool online;
    bool sent;
} node_t;

typedef struct {
	char name[10];
	uint8_t outputs;
	uint8_t inputs;
	uint8_t buttons;
} controller_data_t;

extern const controller_data_t controllerTypesData[];

typedef struct __attribute__((packed)) {
    uint8_t io_id;
    uint8_t io_type; //input or output
    //event_type_t event;
    bool state;
    node_uid_t node;
    uint16_t timer; // for timed output
    action_type_t action;
} io_event_t;

//extern io_cfg_t *gCfg;
// config accessors
input_cfg_t *findInput(uint8_t input_id);
output_cfg_t *findOutput(uint8_t output_id);
output_cfg_t *getConfigOutput(uint8_t i);
uint8_t getConfigOutputsCount();
uint8_t getConfigInputsCount();
input_cfg_t *getConfigInput(uint8_t i);
action_cfg_t *getConfigAction(uint16_t offset);
input_event_cfg_t *getConfigEvent(uint16_t offset);
uint16_t getConfigVersion();

void loadBConfig();
void updateBConfig(uint8_t *ptr);
void updateLocalConfig(const io_cfg_t *newCfg);

uint16_t getConfigSizeDbg();

const char *eventStr(event_type_t event);
bool node_uid_equal(const node_uid_t *a, const node_uid_t *b);
char* strNode(node_uid_t *n);
bool isOldControllerType();

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t model;
    uint16_t outputStates;
    uint16_t inputStates;
    bool online;
} neighbor_t;

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];          // AABBCCDDEEFF
    uint32_t freeMemory;
    uint32_t uptimeRaw;
    uint16_t version;
    uint32_t curdate;
    int8_t   wifiRSSI;
    uint32_t ethIP;
    uint32_t wifiIP;    
    char     resetReason[36]; 
    uint16_t outputStates;
    uint16_t inputStates;
    uint8_t  neighborCount;
    neighbor_t neighbors[];
} device_info_hdr_t;

// static inline output_cfg_t* cfg_outputs(io_cfg_t *cfg);
// static inline input_cfg_t* cfg_inputs(io_cfg_t *cfg);
// static inline input_event_cfg_t* cfg_events(io_cfg_t *cfg);
// static inline action_cfg_t* cfg_actions(io_cfg_t *cfg);

#pragma once

static inline output_cfg_t* cfg_outputs(io_cfg_t *cfg) {
    return (output_cfg_t*) cfg->data;
}

static inline input_cfg_t* cfg_inputs(io_cfg_t *cfg) {
    return (input_cfg_t*)
        (cfg->data + cfg->outputs_count * sizeof(output_cfg_t));
}

static inline input_event_cfg_t* cfg_events(io_cfg_t *cfg) {
    return (input_event_cfg_t*)
        (cfg->data
         + cfg->outputs_count * sizeof(output_cfg_t)
         + cfg->inputs_count * sizeof(input_cfg_t));
}

static inline action_cfg_t* cfg_actions(io_cfg_t *cfg) {
    return (action_cfg_t*)
        (cfg->data
         + cfg->outputs_count * sizeof(output_cfg_t)
         + cfg->inputs_count * sizeof(input_cfg_t)
         + cfg->events_count * sizeof(input_event_cfg_t));
}

void testConfig();

#endif // BCONFIG_H