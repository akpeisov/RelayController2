#include "bconfig.h"
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "storage.h"
#include "utils.h"
#include "rs485.h"

static const char *TAG = "BCONFIG";
io_cfg_t *gCfg;
node_uid_t self_node;
SemaphoreHandle_t cfgMutex;
static TBCEvent bcevent = NULL;

const controller_data_t controllerTypesData[] = {
    {"UNKNOWN", 0, 0, 0},    
    {"RCV1S", 4, 4, 4},    
    {"RCV1B", 10, 16, 10},  
    {"RCV2S", 4, 6, 4},
    {"RCV2M", 6, 8, 6},
    {"RCV2B", 12, 16, 12}
};

const char* fileName = "io_config.bin";

io_cfg_t* makeDefaultConfig(void) {
    if (controllerType == UNKNOWN)
        return NULL;

    const controller_data_t *ctl = &controllerTypesData[controllerType];

    uint8_t  outputs_cnt = ctl->outputs;
    uint8_t  btn_cnt     = ctl->buttons;
    uint8_t  inputs_cnt  = ctl->inputs;

    uint16_t total_inputs = btn_cnt + inputs_cnt;

    uint16_t events_cnt =
        btn_cnt +              // BTN: toggle
        inputs_cnt * 2;        // SWITCH: on + off

    uint16_t actions_cnt =
        btn_cnt * 1 +          // toggle
        inputs_cnt * 2;        // on + off


    size_t total_size = 
        sizeof(io_cfg_t)
        + outputs_cnt * sizeof(output_cfg_t)
        + total_inputs * sizeof(input_cfg_t)
        + events_cnt * sizeof(input_event_cfg_t)
        + actions_cnt * sizeof(action_cfg_t);

    io_cfg_t *cfg = calloc(1, total_size);
    if (!cfg)
        return NULL;

    cfg->version       = 1;
    cfg->outputs_count = outputs_cnt;
    cfg->inputs_count  = total_inputs;
    cfg->events_count  = events_cnt;
    cfg->actions_count = actions_cnt;

    output_cfg_t       *outputs = cfg_outputs(cfg);
    input_cfg_t        *inputs  = cfg_inputs(cfg);
    input_event_cfg_t  *events  = cfg_events(cfg);
    action_cfg_t       *actions = cfg_actions(cfg);

    uint16_t ev_idx  = 0;
    uint16_t act_idx = 0;
    uint8_t  input_idx = 0;

    // OUTPUTS
    for (uint8_t i = 0; i < outputs_cnt; i++) {
        outputs[i] = (output_cfg_t){
            .id = i,
            .type = OUTPUT_SIMPLE,
            .is_on = false,
            .simple.limit_sec = 0
        };
    }

    // EXTERNAL INPUTS
    for (uint8_t i = 0; i < inputs_cnt; i++, input_idx++) {

        uint8_t out = (i < outputs_cnt) ? i : (outputs_cnt - 1);

        input_cfg_t *in = &inputs[input_idx];
        in->id = i;
        in->type = INPUT_SWITCH;
        in->events_count = 2;
        in->events_offset = ev_idx;

        // EVT_ON
        events[ev_idx] = (input_event_cfg_t){
            .event = EVT_ON,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        ev_idx++;

        actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = out,
            .action = ACT_ON
        };

        // EVT_OFF
        events[ev_idx] = (input_event_cfg_t){
            .event = EVT_OFF,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        ev_idx++;

        actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = out,
            .action = ACT_OFF
        };
    }

    // SERVICE BUTTONS
    for (uint8_t i = 0; i < btn_cnt; i++, input_idx++) {

        input_cfg_t *in = &inputs[input_idx];
        in->id = i + 16;
        in->type = INPUT_BTN;
        in->events_count = 1;
        in->events_offset = ev_idx;

        // EVT_TOGGLE
        events[ev_idx] = (input_event_cfg_t){
            .event = EVT_TOGGLE,
            .actions_count = 1,
            .actions_offset = act_idx
        };
        ev_idx++;

        actions[act_idx++] = (action_cfg_t){
            .target_node = self_node,
            .output_id = i,
            .action = ACT_TOGGLE
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
        case ACT_ALLOFF:  return "ALLOFF";
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

bool validateConfig(io_cfg_t* cfg) {
    if (!cfg) {
        ESP_LOGE(TAG, "Config is NULL");
        return false;
    }
    if (cfg->outputs_count > 16 || cfg->inputs_count > 32 || cfg->events_count > 64 || cfg->actions_count > 256) {
        ESP_LOGE(TAG, "Too many elements in config");
        return false;
    }
    return true;
}

void dumpIoConfig(const io_cfg_t *cfg) {
    if (!cfg) {
        ESP_LOGE(TAG, "IO config is NULL");
        return;
    }

    printf("\n========== IO CONFIG ==========\n");

    printf("Version (%u)\n", cfg->version);
    printf("Outputs (%u)\n", cfg->outputs_count);
    printf("Inputs  (%u)\n", cfg->inputs_count);
    printf("Events  (%u)\n", cfg->events_count);
    printf("Actions (%u)\n\n", cfg->actions_count);

if (!validateConfig(cfg)) {
    ESP_LOGE(TAG, "Invalid config.");
    return;
}

    const output_cfg_t      *outputs = cfg_outputs(cfg);
    const input_cfg_t       *inputs  = cfg_inputs(cfg);
    const input_event_cfg_t *events  = cfg_events(cfg);
    const action_cfg_t      *actions = cfg_actions(cfg);

    // OUTPUTS
    for (uint8_t i = 0; i < cfg->outputs_count; i++) {

        const output_cfg_t *o = &outputs[i];

        printf("  OUT[%u]: type=%d state=%d",
               o->id, o->type, o->is_on);

        if (o->type == OUTPUT_SIMPLE) {
            printf(" limit=%us", o->simple.limit_sec);
        }

        printf("\n");
    }

    printf("\n");

    // INPUTS
    for (uint8_t i = 0; i < cfg->inputs_count; i++) {

        const input_cfg_t *in = &inputs[i];

        printf("  IN[%u]: id=%u type=%d events=%u\n",
               i, in->id, in->type, in->events_count);

        // защита от битого offset
        if ((uint32_t)in->events_offset + in->events_count > cfg->events_count) {
            printf("    !!! INVALID EVENT OFFSET !!!\n");
            continue;
        }

        for (uint8_t e = 0; e < in->events_count; e++) {

            const input_event_cfg_t *evt =
                &events[in->events_offset + e];

            printf("    EVT[%u]: %s actions=%u\n",
                   e, eventToStr(evt->event), evt->actions_count);

            // защита от битого offset
            if ((uint32_t)evt->actions_offset + evt->actions_count > cfg->actions_count) {
                printf("      !!! INVALID ACTION OFFSET !!!\n");
                continue;
            }

            for (uint16_t a = 0; a < evt->actions_count; a++) {

                const action_cfg_t *act =
                    &actions[evt->actions_offset + a];

                printf("      ACT[%u]: %s ",
                       a, actionToStr(act->action));

                if (act->action != ACT_WAIT) {
                    printf("out=%u node=", act->output_id);
                    printNode(&act->target_node);
                } else {
                    printf("duration=%us", act->duration_sec);
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

esp_err_t saveBConfig(io_cfg_t* cfg) {
    return saveFile(fileName, (uint8_t*)cfg, getConfigSize(cfg));
}

void loadBConfig() {    
    if (cfgMutex == NULL) {
        cfgMutex = xSemaphoreCreateMutex();
    }
    getMac(self_node.mac);    
    ESP_LOGI(TAG, "Loading config back from file '%s'...", fileName);
    uint8_t *loadedBuf = NULL;
    size_t loadedSize = 0;    
    if (loadFile(fileName, &loadedBuf, &loadedSize) == ESP_OK) {
        gCfg = malloc(loadedSize);
        bool ok = false;
        if (gCfg) {
            memcpy(gCfg, loadedBuf, loadedSize);                        
            ok = true;
        }         
        if (!ok || !validateConfig(gCfg)) {
            ESP_LOGE(TAG, "Invalid config in file. Making default config");
            free(gCfg);
            gCfg = makeDefaultConfig();  
            saveBConfig(gCfg);        
        }
        // validate config
        //gCfg = makeDefaultConfig();        
    } else {
        ESP_LOGE(TAG, "Failed to load config. Making default config");
        gCfg = makeDefaultConfig();        
        // if (saveFile(fileName, (const uint8_t*)gCfg, getConfigSize(gCfg)) != ESP_OK) {
        //     ESP_LOGE(TAG, "Failed to save config");     
        // }
        saveBConfig(gCfg);
    }
    free(loadedBuf);
    ESP_LOGI(TAG, "Config version %d", getConfigVersion());

    dumpIoConfig(gCfg);

    //ESP_LOG_BUFFER_HEXDUMP("cfg", (const uint8_t*)gCfg, getConfigSize(gCfg), CONFIG_LOG_DEFAULT_LEVEL);  
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

    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        const io_cfg_t *cfg = gCfg;    
        input_cfg_t *inputs = cfg_inputs(cfg);
        for (uint8_t i = 0; i < cfg->inputs_count; i++) {
            if (inputs[i].id == input_id) {
                input = &inputs[i];
                break;
            }
        }
        xSemaphoreGive(cfgMutex);
    } 
    return input;
}

output_cfg_t *findOutput(uint8_t output_id) {
    output_cfg_t *output = NULL;
    if (!gCfg) {
        ESP_LOGE(TAG, "IO config not initialized");
        return output;
    }

    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        const io_cfg_t *cfg = gCfg;  
        output_cfg_t *outputs = cfg_outputs(cfg);  
        for (uint8_t i = 0; i < cfg->outputs_count; i++) {
            if (outputs[i].id == output_id) {
                output = &outputs[i];
                break;
            }
        }        
        xSemaphoreGive(cfgMutex);
    }
    return output;
}

uint8_t getConfigOutputsCount() {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        uint8_t count = gCfg ? gCfg->outputs_count : 0;
        xSemaphoreGive(cfgMutex);
        return count;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return 0;
    }
    //return gCfg ? gCfg->outputs_count : 0;
}

output_cfg_t *getConfigOutput(uint8_t i) {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        output_cfg_t *output = gCfg ? &cfg_outputs(gCfg)[i] : NULL;
        xSemaphoreGive(cfgMutex);
        return output;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return NULL;
    }
    //return &cfg_outputs(gCfg)[i];
}

uint8_t getConfigInputsCount() {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        uint8_t count = gCfg ? gCfg->inputs_count : 0;
        xSemaphoreGive(cfgMutex);
        return count;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return 0;
    }
    //return gCfg ? gCfg->inputs_count : 0;
}

input_cfg_t *getConfigInput(uint8_t i) {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        input_cfg_t *input = gCfg ? &cfg_inputs(gCfg)[i] : NULL;
        xSemaphoreGive(cfgMutex);
        return input;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return NULL;
    }
    //return &cfg_inputs(gCfg)[i];
}

action_cfg_t *getConfigAction(uint16_t offset) {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        action_cfg_t *action = gCfg ? &cfg_actions(gCfg)[offset] : NULL;
        xSemaphoreGive(cfgMutex);
        return action;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return NULL;
    }
    //return &cfg_actions(gCfg)[offset];
}

input_event_cfg_t *getConfigEvent(uint16_t offset) {
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        input_event_cfg_t *event = gCfg ? &cfg_events(gCfg)[offset] : NULL;
        xSemaphoreGive(cfgMutex);
        return event;
    } else {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return NULL;
    }
    //return &cfg_events(gCfg)[offset];
}

const char *eventStr(event_type_t event) {
    switch (event) {
        case EVT_ON:        return "turned on";
        case EVT_OFF:       return "turned off";
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

uint16_t getConfigSizeDbg() {
    return getConfigSize(gCfg);
}

esp_err_t updateLocalConfig(const io_cfg_t *newCfg) {
    if (!validateConfig(newCfg)) {
        ESP_LOGE(TAG, "updateLocalConfig. Invalid config");
        return ESP_FAIL;
    }
    uint16_t currentVersion = getConfigVersion();
    uint16_t newVersion = newCfg ? newCfg->version : 0;
    ESP_LOGI(TAG, "Current config version %d, new config version %d", currentVersion, newVersion);
    if (currentVersion == newVersion) {
        ESP_LOGI(TAG, "Config version is the same. No update needed.");
        return ESP_ERR_INVALID_VERSION;
    }
    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        // backup old config
        size_t size = getConfigSize(gCfg);
        ESP_LOGI(TAG, "Backing up old config. Size %d bytes", size);
        io_cfg_t *oldCfg = malloc(size);
        if (!oldCfg) {
            ESP_LOGE(TAG, "Failed to allocate memory for config backup. Update aborted.");
            xSemaphoreGive(cfgMutex);
            return ESP_FAIL;
        }
        memcpy(oldCfg, gCfg, size);
        free(gCfg);
        
        size = getConfigSize(newCfg);
        gCfg = malloc(size);
        if (gCfg) {
            memcpy(gCfg, newCfg, size);
            if (saveBConfig(gCfg) == ESP_OK) { // TODO :  STORAGE: Saving file path /storage/io_config.bin И виснет
                ESP_LOGI(TAG, "Local config updated successfully");
            } else {
                ESP_LOGE(TAG, "Error while saving local config");
            }
            free(oldCfg);            
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for new config");
            // restore old config
            gCfg = oldCfg;                
        }
        xSemaphoreGive(cfgMutex);
    }
    return ESP_OK;
}

void updateBConfig(uint8_t *ptr /*nodes_cfg_t *cfg*/) {    
    // esp_err_t res = ESP_OK;
    uint8_t nodes_count = *ptr;
    ptr += sizeof(uint8_t);  // nodes_count shift
    if (nodes_count > 10) {
        ESP_LOGE(TAG, "Updating binary config. Too many nodes %d", nodes_count);
        return;
    }

    ESP_LOGI(TAG, "Updating binary config. Nodes %d", nodes_count);    
    for (int i = 0; i < nodes_count; i++) {
        node_cfg_t *node = (node_cfg_t*)ptr;
                        
        size_t io_data_size = getConfigSize(&node->io_cfg);
        size_t node_size = sizeof(node_uid_t) + io_data_size;

        ESP_LOGI(TAG, "Node %d. Size %d. UID %s, inputs %d, outputs %d",
                 i, node_size, strNode(&node->uid), node->io_cfg.inputs_count, node->io_cfg.outputs_count);
        //dumpIoConfig(&node->io_cfg);
        if (node_uid_equal(&node->uid, &self_node)) {    
            ESP_LOGI(TAG, "This config for local node. Updating IO config...");
            esp_err_t res = updateLocalConfig(&node->io_cfg); 
            bconfig_event_t evt;
            evt.node = self_node;
            evt.version = getConfigVersion();
            if (res == ESP_ERR_INVALID_VERSION)
                evt.result = 2;
            else if (res == ESP_OK)
                evt.result = 1;
            else
                evt.result = 0;
            bcevent(evt);           
        } else {
            ESP_LOGI(TAG, "This config for another node. Sending it via bus... Version %d", node->io_cfg.version);            
            sendNodeConfig(node->uid.mac, (uint8_t*)&node->io_cfg, io_data_size);
        }
        ptr += node_size;    
    }
    ESP_LOGI(TAG, "End of update bconfig");
}

bool isOldControllerType() {
    return controllerType == RCV1S || controllerType == RCV1B;
}

void testConfig() {
    //dumpIoConfig(gCfg);
    ESP_LOG_BUFFER_HEXDUMP("cfg", (const uint8_t*)gCfg, getConfigSize(gCfg), CONFIG_LOG_DEFAULT_LEVEL);  
}

void registerBHandler(TBCEvent event) {
    bcevent = event;
}

void setAllOff() {
    ESP_LOGI(TAG, "Setting all outputs to off");
    if (!gCfg) {
        ESP_LOGE(TAG, "IO config not initialized");
        return;
    }

    if (xSemaphoreTake(cfgMutex, portMAX_DELAY) == pdTRUE) {
        const io_cfg_t *cfg = gCfg;  
        output_cfg_t *outputs = cfg_outputs(cfg);  
        for (uint8_t i = 0; i < cfg->outputs_count; i++) {
            outputs[i].is_on = false;
        }        
        xSemaphoreGive(cfgMutex);
    }
}