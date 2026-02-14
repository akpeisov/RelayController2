#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "core.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdint.h>
#include "hardware.h"
#include "config.h"
#include "ota.h"
#include "utils.h"
#include "bconfig.h"
#include "network.h"
#include "rs485.h"
#include "ws.h"
#include "jwt.h"
#include "storage.h"
#include "webserver.h"
#include <time.h>
#include <sys/time.h>

#define MAXTASKNAME 15

static const char *TAG = "CORE";
static bool wsConnected = false;

extern const char jwt_start[] asm("_binary_jwt_pem_start");
extern const char jwt_end[] asm("_binary_jwt_pem_end");

void IOhandler(io_event_t event) {
    ESP_LOGI(TAG, "IOhandler typeIO %d io %d state %s, node %s", 
             event.io_type, event.io_id, event.state ? "on" : "off", strNode(&event.node));
    // input or output event happened. Publish it to cloud and local network
    // AA 0E NODE TT ID SS TMR
    uint8_t buflen = 12;
    uint8_t buffer[buflen];    
    buffer[0] = 'E'; // event
    memcpy(buffer+1, &event.node.mac, 6);
    buffer[7] = event.io_type;
    buffer[8] = event.io_id;
    //buffer[9] = event.event;        
    buffer[9] = event.state;
    if (event.io_type == 0) { 
        buffer[10] = event.timer >> 8;
        buffer[11] = event.timer & 0xFF;
    } else {
        buflen = 10;
    }
    WSSendPacket(buffer, buflen);
}

void sendInfo() {
    if (!wsConnected) {
        return;
    }

    uint8_t buffer[256];
    uint8_t *p = buffer;
    
    device_info_hdr_t hdr = {0};
    getMac(hdr.mac);    
    hdr.freeMemory = esp_get_free_heap_size();
    hdr.uptimeRaw = getUpTimeRaw();
    hdr.version = getConfigVersion();
    hdr.curdate = time(NULL); 
    hdr.wifiRSSI = getRSSI();
    hdr.ethIP = ethIp;
    hdr.wifiIP = wifiIp;
    strncpy(hdr.resetReason, espResetReason(), sizeof(hdr.resetReason));    
    hdr.outputStates = getOutputs();
    hdr.inputStates = getInputs();

    node_t *nodes;    
    hdr.neighborCount = getNodes(&nodes);

    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (int i = 0; i < hdr.neighborCount; i++) {
        neighbor_t n;
        memcpy(n.mac, nodes[i].mac, 6);
        n.model = nodes[i].model;
        n.outputStates = nodes[i].outputStates;
        n.inputStates = nodes[i].inputStates;
        memcpy(p, &n, sizeof(n));
        p += sizeof(n);
    }
    size_t packetSize = p - buffer;
    
    WSSendPacket(buffer, packetSize);
}

char *loadJwtCert() {
    char *jwt_key = NULL;
    if (loadTextFile("certs/jwt.pem", &jwt_key) != ESP_OK) {
        ESP_LOGE(TAG, "Can't load jwt certificate. Getting from fw");
        if (jwt_key != NULL)
            free(jwt_key);
        uint16_t len = jwt_end - jwt_start;
        jwt_key = (char*)malloc(len+1);
        strncpy(jwt_key, jwt_start, len);    
        jwt_key[len] = '\0';
        ESP_LOGI(TAG, "jwtLen %d", len);
    }    
    //ESP_LOG_BUFFER_HEXDUMP(TAG, jwt_key, strlen(jwt_key)+1, CONFIG_LOG_DEFAULT_LEVEL);
    return jwt_key;
}

char *getJWTToken() {
    time_t now;    
    time(&now);
    
    // Payload
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        ESP_LOGE(TAG, "getJWTToken error. Can't create payload object");
        return NULL;
    }
    cJSON_AddNumberToObject(payload, "iat", (uint32_t)now);
    cJSON_AddNumberToObject(payload, "exp", (uint32_t)now+300); // 5 minutes
    cJSON_AddStringToObject(payload, "mac", getMacStr());
    cJSON_AddStringToObject(payload, "model", getControllerTypeText(controllerType));
    char *payload_str = cJSON_PrintUnformatted(payload);
    char *token = malloc(512);
    char *cert = loadJwtCert();
    if (cert==NULL) {
        ESP_LOGE(TAG, "Failed to load JWT certificate");
        cJSON_Delete(payload);
        free(payload_str);
        return NULL;
    }
    //generate_jwtRS256(payload_str, token, cert);
    size_t tokenLen = 512;
    if (generate_jwt_rs2562(payload_str,
                             cert,
                             token,
                             tokenLen) != ESP_OK) {
                                token = NULL;
                             }
    free(cert);            
    cJSON_Delete(payload);
    free(payload_str);
    return token;
}

void sendHello() {
    ESP_LOGI(TAG, "Sending HELLO message...");    
    char *token = getJWTToken();    
    if (token == NULL) {
        ESP_LOGE(TAG, "Failed to generate JWT token for HELLO message");
        return;
    }
    ESP_LOGI(TAG, "Generated JWT token: %s", token);
    uint16_t len = strlen(token) + 1;
    uint8_t *data = malloc(len);
    if (!data) {
        free(token);
        ESP_LOGE(TAG, "Failed to allocate memory for HELLO message");
        return;
    }    
    data[0] = 'H';  // Message type 'H' for HELLO
    memcpy(data + 1, token, strlen(token)); // JWT token

    WSSendPacket(data, len);
    free(token);
    free(data);    
}

static void infoTask(void *arg) {
    while (1) {
        sendInfo();    
        vTaskDelay(pdMS_TO_TICKS(1000 * 10));
    }
}

esp_err_t uiRouter(httpd_req_t *req) {    
    //ESP_LOGI(TAG, "%d %s", req->method, req->uri);
    char *uri = getClearURI(req->uri);
    char *response = NULL;
    char *content = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    httpd_resp_set_type(req, "application/json");
	if (!strcmp(uri, "/service/config")) {
        if (req->method == HTTP_GET) {            
            err = getConfig(&response);
        } else if (req->method == HTTP_POST) {
            err = getContent(&content, req);
            if (err == ESP_OK) {
                if (xSemaphoreTake(gMutex, portMAX_DELAY) == pdTRUE) {
                    err = setConfig(&response, content); 
                    xSemaphoreGive(gMutex);
                }
            }
        }            
    }
    //free(response);
    if (err == ESP_OK) {
        httpd_resp_set_status(req, "200");        
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404");        
        setErrorTextJson(&response, "Method not found!");        
        //httpd_resp_send(req, "Not found!"); //req->uri, strlen(req->uri));
    } else {
        httpd_resp_set_status(req, "400");
    }
    if (response != NULL) {
        httpd_resp_send(req, response, -1);
        free(response);
    }   
    if (content != NULL)
        free(content);
    free(uri);

    return ESP_OK;
}

void runWebServer() {
	if (webserverRegisterRouter(&uiRouter) != ESP_OK) {
		ESP_LOGE(TAG, "Can't register router");
	}
	initWebServer(1);	
}

void processServerHello(const uint8_t *data, uint32_t len) {
    if (len < 5) {
        ESP_LOGE(TAG, "Invalid HELLO message length %d", len);
        return;
    }
    uint32_t timestamp = (data[4] & 0xFF) | ((data[5] & 0xFF) << 8) | ((data[6] & 0xFF) << 16) | ((data[7] & 0xFF) << 24);
    ESP_LOGI(TAG, "Received HELLO from server. Server time %u", timestamp);
    // set device time to server time
    struct timeval tv = {
        .tv_sec = timestamp,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);
    sendHello();
}

static void wsEvent(ws_event_id_t event, const uint8_t *data, uint32_t len) {
    switch (event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            // send hello
            //sendHello();
            break;
        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            wsConnected = false;
            break;
        case WS_EVENT_DATA:
            ESP_LOGI(TAG, "WebSocket data received. Len %d", len);
            if (data != NULL && len > 0) {
                switch (data[0]) {
                    case 0xBB: // hello from server
                        processServerHello(data, len);                    
                        break;
                    case 'H': // HELLO response
                        if (data[1] == 0) {
                            ESP_LOGI(TAG, "Received HELLO response");
                            wsConnected = true;
                            sendInfo();
                        } else {
                            ESP_LOGE(TAG, "HELLO response error %d", data[1]);
                            wsConnected = false;
                            WSRestart();
                        }                        
                        break;
                    case 'C': // config
                        nodes_cfg_t *cfg = (nodes_cfg_t*)data;                        
                        updateBConfig(cfg);                        
                        break;    
                    case 'E': // Common error   
                        ESP_LOGI(TAG, "Common error %d", data[1]);
                        wsConnected = false;
                        break;
                    case 'A': // Action from cloud
                        // 0   1  2-7   8       9     10    11-12
                        // AA  A  NODE  OUT/IN  ID  ACTION  CRC16
                        if (data[7] == 0) {
                            // output
                            action_cfg_t act;
                            act.output_id = data[8];
                            act.action = data[9];
                            memcpy(act.target_node.mac, &data[1], 6);
                            setOutput(&act);
                        } else {
                            // input - not supported for now
                            ESP_LOGW(TAG, "Input action from cloud not supported");
                        }
                        break;
                    default:
                        break;
                }
            }
            break;
        case WS_EVENT_ERROR:
            break;
    }
}

void initWS() {
    if (getConfigValueBool("network/cloud/enabled")) {
        //WSinit(getConfigValueString("network/cloud/address"), &wsEvent, getConfigValueBool("network/cloud/log"));            
        WSinit("ws://192.168.4.120:8888/ws", &wsEvent, getConfigValueBool("network/cloud/log"));            
    }
}

void initCore() {   
    // register handler 
    registerIOHandler(&IOhandler);
    //runWebServer();
    //xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint32_t usStackDepth, void *const pvParameters, UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask)
    xTaskCreate(infoTask, "infoTask", 4096, NULL, 10, NULL);    
}