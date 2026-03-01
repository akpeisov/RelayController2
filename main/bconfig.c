#include "bconfig.h"
#include <stdint.h>
#include "esp_log.h"
#include "storage.h"
#include "utils.h"
#include "rs485.h"

static const char *TAG = "BCONFIG";
io_cfg_t *gCfg;
node_uid_t self_node;

const controller_data_t controllerTypesData[] = {
    {"UNKNOWN", 0, 0, 0},    
    {"RCV1S", 4, 4, 4},    
    {"RCV1B", 10, 16, 10},  
    {"RCV2S", 4, 6, 4},
    {"RCV2M", 6, 8, 6},
    {"RCV2B", 12, 16, 12}
};

// io_cfg_t* makeDefaultConfig() {
//     if (controllerType == UNKNOWN)
//         return NULL;
//     const controller_data_t *ctl = &controllerTypesData[controllerType];

//     uint8_t outputs_cnt = ctl->outputs;
//     uint8_t btn_cnt     = ctl->buttons;
//     uint8_t inputs_cnt  = ctl->inputs;

//     uint16_t events_cnt =
//         btn_cnt +          // BTN: toggle
//         inputs_cnt * 2;        // SWITCH: on + off
    
//     uint16_t actions_cnt =
//         btn_cnt * 1 +          // toggle
//         //btn_cnt * 1 +          // longpress (WAIT)
//         inputs_cnt * 2;        // on + off
//     actions_cnt++; //for test    

//     io_cfg_t *cfg = calloc(1, sizeof(io_cfg_t));
//     if (!cfg) return NULL;
//     cfg->version = 1;

//     cfg->outputs = calloc(outputs_cnt, sizeof(output_cfg_t));
//     cfg->inputs  = calloc(btn_cnt + inputs_cnt, sizeof(input_cfg_t));
//     cfg->events  = calloc(events_cnt, sizeof(input_event_cfg_t));
//     cfg->actions = calloc(actions_cnt, sizeof(action_cfg_t));

//     cfg->outputs_count = outputs_cnt;
//     cfg->inputs_count  = btn_cnt + inputs_cnt;
//     cfg->events_count  = events_cnt;
//     cfg->actions_count = actions_cnt;

//     uint16_t ev_idx = 0;
//     uint16_t act_idx = 0;
//     uint8_t input_idx = 0;

//     // OUTPUTS
//     for (uint8_t i = 0; i < outputs_cnt; i++) {
//         cfg->outputs[i] = (output_cfg_t) {
//             .id = i,
//             .type = OUTPUT_SIMPLE,
//             .is_on = false,
//             .simple.limit_sec = 0
//         };
//     }

//     // SERVICE BUTTONS
//     for (uint8_t i = 0; i < btn_cnt; i++, input_idx++) {
//         input_cfg_t *in = &cfg->inputs[input_idx];
//         in->id = i + 16;
//         in->type = INPUT_BTN;
//         in->events_count = 1;
//         in->events_offset = ev_idx;

//         if (i!=3) {
//             // EVT_TOGGLE
//             cfg->events[ev_idx++] = (input_event_cfg_t){
//                 .event = EVT_TOGGLE,
//                 .actions_count = 1,
//                 .actions_offset = act_idx
//             };
//             cfg->actions[act_idx++] = (action_cfg_t){
//                 .target_node = self_node,
//                 .output_id = i,
//                 .action = ACT_TOGGLE
//             };  
//         } else {
//             // test action 
//             cfg->events[ev_idx++] = (input_event_cfg_t){
//                 .event = EVT_TOGGLE,
//                 .actions_count = 2,
//                 .actions_offset = act_idx
//             };
//             cfg->actions[act_idx++] = (action_cfg_t){
//                 .target_node = self_node,
//                 .output_id = 3,
//                 .action = ACT_TOGGLE
//             };  
//             cfg->actions[act_idx++] = (action_cfg_t){
//                 .target_node = ((node_uid_t){ .mac = {0xc8, 0xc9, 0xa3, 0xf9, 0xb6, 0xc8} }),
//                 .output_id = 0,
//                 .action = ACT_TOGGLE
//             };  
//         }       
        
//     }

//     // EXTERNAL INPUTS
//     for (uint8_t i = 0; i < inputs_cnt; i++, input_idx++) {
//         uint8_t out = i < outputs_cnt ? i : outputs_cnt - 1;

//         input_cfg_t *in = &cfg->inputs[input_idx];
//         in->id = i;
//         in->type = INPUT_SWITCH;
//         in->events_count = 2;
//         in->events_offset = ev_idx;

//         // EVT_ON
//         cfg->events[ev_idx++] = (input_event_cfg_t){
//             .event = EVT_ON,
//             .actions_count = 1,
//             .actions_offset = act_idx
//         };
//         cfg->actions[act_idx++] = (action_cfg_t){
//             .target_node = self_node,
//             .output_id = out,
//             .action = ACT_ON
//         };

//         // EVT_OFF
//         cfg->events[ev_idx++] = (input_event_cfg_t){
//             .event = EVT_OFF,
//             .actions_count = 1,
//             .actions_offset = act_idx
//         };
//         cfg->actions[act_idx++] = (action_cfg_t){
//             .target_node = self_node,
//             .output_id = out,
//             .action = ACT_OFF
//         };
//     }

//     return cfg;
// }

io_cfg_t* makeDefaultConfig() {
    if (controllerType == UNKNOWN)
        return NULL;
    const controller_data_t *ctl = &controllerTypesData[controllerType];

    uint8_t outputs_cnt = ctl->outputs;
    uint8_t btn_cnt     = ctl->buttons;
    uint8_t inputs_cnt  = ctl->inputs;

    uint16_t events_cnt =
        btn_cnt +          // BTN: toggle
        inputs_cnt * 2;        // SWITCH: on + off
    
    uint16_t actions_cnt =
        btn_cnt * 1 +          // toggle
        inputs_cnt * 2;        // on + off

    io_cfg_t *cfg = calloc(1, sizeof(io_cfg_t));
    if (!cfg) return NULL;
    cfg->version = 1;

    cfg->outputs = calloc(outputs_cnt, sizeof(output_cfg_t));
    cfg->inputs  = calloc(btn_cnt + inputs_cnt, sizeof(input_cfg_t));
    cfg->events  = calloc(events_cnt, sizeof(input_event_cfg_t));
    cfg->actions = calloc(actions_cnt, sizeof(action_cfg_t));

    cfg->outputs_count = outputs_cnt;
    cfg->inputs_count  = btn_cnt + inputs_cnt;
    cfg->events_count  = events_cnt;
    cfg->actions_count = actions_cnt;

    uint16_t ev_idx = 0;
    uint16_t act_idx = 0;
    uint8_t input_idx = 0;

    // OUTPUTS
    for (uint8_t i = 0; i < outputs_cnt; i++) {
        cfg->outputs[i] = (output_cfg_t) {
            .id = i,
            .type = OUTPUT_SIMPLE,
            .is_on = false,
            .simple.limit_sec = 0
        };
    }

    // SERVICE BUTTONS
    for (uint8_t i = 0; i < btn_cnt; i++, input_idx++) {
        input_cfg_t *in = &cfg->inputs[input_idx];
        in->id = i + 16;
        in->type = INPUT_BTN;
        in->events_count = 1;
        in->events_offset = ev_idx;

        // EVT_TOGGLE
        cfg->events[ev_idx++] = (input_event_cfg_t){
            .event = EVT_TOGGLE,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        cfg->actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = i,
            .action = ACT_TOGGLE
        };          
    }

    // EXTERNAL INPUTS
    for (uint8_t i = 0; i < inputs_cnt; i++, input_idx++) {
        uint8_t out = i < outputs_cnt ? i : outputs_cnt - 1;

        input_cfg_t *in = &cfg->inputs[input_idx];
        in->id = i;
        in->type = INPUT_SWITCH;
        in->events_count = 2;
        in->events_offset = ev_idx;

        // EVT_ON
        cfg->events[ev_idx++] = (input_event_cfg_t){
            .event = EVT_ON,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        cfg->actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = out,
            .action = ACT_ON
        };

        // EVT_OFF
        cfg->events[ev_idx++] = (input_event_cfg_t){
            .event = EVT_OFF,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        cfg->actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = out,
            .action = ACT_OFF
        };
    }

    return cfg;
}

static const char* eventToStr(event_type_t e) {
    switch (e) {
        case EVT_ON:        return "ON";
        case EVT_OFF:       return "OFF";
        case EVT_TOGGLE:    return "TOGGLE";
        case EVT_LONGPRESS: return "LONGPRESS";
        default:            return "UNKNOWN";
    }
}

static const char* actionToStr(action_type_t a) {
    switch (a) {
        case ACT_ON:      return "ON";
        case ACT_OFF:     return "OFF";
        case ACT_TOGGLE:  return "TOGGLE";
        case ACT_WAIT:    return "WAIT";
        default:          return "UNKNOWN";
    }
}

static void printNode(const node_uid_t *n) {
    if (memcmp(n->mac, (uint8_t[6]){0}, 6) == 0) {
        printf("LOCAL");
    } else {
        printf("%02X:%02X:%02X:%02X:%02X:%02X",
            n->mac[0], n->mac[1], n->mac[2],
            n->mac[3], n->mac[4], n->mac[5]);
    }
}

void dumpIoConfig(const io_cfg_t *cfg) {
    if (!cfg) {
        ESP_LOGE(TAG, "IO config is NULL");
        return;
    }

    printf("\n========== IO CONFIG ==========\n");
    
    printf("Version (%d):\n", cfg->version);
    printf("Outputs (%d):\n", cfg->outputs_count);
    for (uint8_t i = 0; i < cfg->outputs_count; i++) {
        const output_cfg_t *o = &cfg->outputs[i];
        printf("  OUT[%d]: type=%d state=%d",
            o->id, o->type, o->is_on);

        if (o->type == OUTPUT_SIMPLE) {
            printf(" limit=%ds", o->simple.limit_sec);
        }
        printf("\n");
    }

    printf("\nInputs (%d):\n", cfg->inputs_count);
    for (uint8_t i = 0; i < cfg->inputs_count; i++) {
        const input_cfg_t *in = &cfg->inputs[i];

        printf("  IN[%d]: id=%d type=%d events=%d\n",
            i, in->id, in->type, in->events_count);

        for (uint8_t e = 0; e < in->events_count; e++) {
            const input_event_cfg_t *evt =
                &cfg->events[in->events_offset + e];

            printf("    EVT[%d]: %s actions=%d\n",
                e, eventToStr(evt->event), evt->actions_count);

            for (uint16_t a = 0; a < evt->actions_count; a++) {
                const action_cfg_t *act =
                    &cfg->actions[evt->actions_offset + a];

                printf("      ACT[%d]: %s ",
                    a, actionToStr(act->action));

                if (act->action != ACT_WAIT) {
                    printf("out=%d node=", act->output_id);
                    printNode(&act->target_node);
                } else {
                    printf("duration=%ds", act->duration_sec);
                }

                printf("\n");
            }
        }
    }

    printf("================================\n");
}

size_t getConfigSize(io_cfg_t* cfg) {
    return sizeof(io_cfg_t)
                     + cfg->outputs_count * sizeof(output_cfg_t)
                     + cfg->inputs_count * sizeof(input_cfg_t)
                     + cfg->events_count * sizeof(input_event_cfg_t)
                     + cfg->actions_count * sizeof(action_cfg_t);
}

void loadBConfig1() {
    getMac(self_node.mac);
    
    // TODO : load config
    gCfg = makeDefaultConfig();   
    dumpIoConfig(gCfg);     
}

void loadBConfig() {    
    const char* fileName = "io_config.bin";
    ESP_LOGI(TAG, "Loading config back from file '%s'...", fileName);
    uint8_t *loadedBuf = NULL;
    size_t loadedSize = 0;    
    if (loadFile(fileName, &loadedBuf, &loadedSize) == ESP_OK) {
        gCfg = (io_cfg_t*)loadedBuf;        
    } else {
        ESP_LOGE(TAG, "Failed to load config. Making default config");
        gCfg = makeDefaultConfig();        
        if (saveFile(fileName, (const uint8_t*)gCfg, getConfigSize(gCfg)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save config");     
        }
    }
    free(loadedBuf);
    
    dumpIoConfig(gCfg);

    // return;
    // gCfg = makeDefaultConfig();        
    // if (saveFile(fileName, (const uint8_t*)gCfg, getConfigSize(gCfg)) != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to save config");     
    // }
    // temp
}

input_cfg_t *findInput(uint8_t input_id) {
    input_cfg_t *input = NULL;
    if (!gCfg) {
        ESP_LOGE(TAG, "IO config not initialized");
        return input;
    }

    const io_cfg_t *cfg = gCfg;    
    for (uint8_t i = 0; i < cfg->inputs_count; i++) {
        if (cfg->inputs[i].id == input_id) {
            return &cfg->inputs[i];            
        }
    }
    return NULL;
}

output_cfg_t *findOutput(uint8_t output_id) {
    output_cfg_t *output = NULL;
    if (!gCfg) {
        ESP_LOGE(TAG, "IO config not initialized");
        return output;
    }

    const io_cfg_t *cfg = gCfg;    
    for (uint8_t i = 0; i < cfg->outputs_count; i++) {
        if (cfg->outputs[i].id == output_id) {
            return &cfg->outputs[i];            
        }
    }
    return NULL;
}

const char *eventStr(event_type_t event) {
    switch (event) {
        case EVT_ON:        return "turned on";
        case EVT_OFF:       return "truned off";
        case EVT_TOGGLE:    return "toggled";
        case EVT_LONGPRESS: return "longpressed";
        default:            return "unknown event";
    } 
}

bool node_uid_equal(const node_uid_t *a, const node_uid_t *b) {
    return memcmp(a->mac, b->mac, sizeof(a->mac)) == 0;
}

char* strNode(node_uid_t *n) {    
    static char str[20];
    snprintf(str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
            n->mac[0], n->mac[1], n->mac[2],
            n->mac[3], n->mac[4], n->mac[5]);    
    return str;
}

uint16_t getConfigVersion() {
    return gCfg ? gCfg->version : 0;
}

void updateLocalConfig(const io_cfg_t *newCfg) {
    uint16_t currentVersion = getConfigVersion();
    uint16_t newVersion = newCfg ? newCfg->version : 0;
    ESP_LOGI(TAG, "Current config version %d, new config version %d", currentVersion, newVersion);
    if (currentVersion == newVersion) {
        ESP_LOGI(TAG, "Config version is the same. No update needed.");
        return;
    }
    if (gCfg) {
        free(gCfg->outputs);
        free(gCfg->inputs);
        free(gCfg->events);
        free(gCfg->actions);
        free(gCfg);
    }
    gCfg = malloc(sizeof(io_cfg_t));
    if (!gCfg) {
        ESP_LOGE(TAG, "Failed to allocate memory for new config");
        return;
    }
    memcpy(gCfg, newCfg, sizeof(io_cfg_t));
    ESP_LOGI(TAG, "Local config updated successfully");
}

void updateBConfig(nodes_cfg_t *cfg) {    
    for (uint8_t i = 0; i < cfg->nodes_count; i++) {
        const node_cfg_t *nodeCfg = &cfg->nodes[i];
        ESP_LOGI(TAG, "Node %d: UID %s, inputs %d, outputs %d",
            i, strNode(&nodeCfg->uid), nodeCfg->io_cfg.inputs_count, nodeCfg->io_cfg.outputs_count);
        if (node_uid_equal(&nodeCfg->uid, &self_node)) {    
            ESP_LOGI(TAG, "This config for local node. Updating IO config...");
            updateLocalConfig(&nodeCfg->io_cfg);            
        } else {
            // send to remote node
            sendFullConfig(nodeCfg->uid.mac, (uint8_t*)&nodeCfg->io_cfg, sizeof(node_cfg_t));
        }
    }
}

bool isOldControllerType() {
    return controllerType == RCV1S || controllerType == RCV1B;
}