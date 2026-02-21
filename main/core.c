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

void sendNewNode(uint8_t *mac, model_t model, uint16_t outputStates, uint16_t inputStates) {
    uint8_t buffer[13];
    buffer[0] = 'N'; // new node
    memcpy(buffer+1, mac, 6);
    buffer[7] = model;
    buffer[8] = outputStates >> 8;
    buffer[9] = outputStates & 0xFF;
    buffer[10] = inputStates >> 8;
    buffer[11] = inputStates & 0xFF;
    WSSendPacket(buffer, 12);
}

/*
void sendInfo() {
    if (!wsConnected) {
        return;
    }

    uint8_t buffer[256];
    buffer[0] = 'I'; // info packet
    uint8_t *p = buffer + 1;
    
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
ESP_LOGI(TAG, "info. neighbor count %d", hdr.neighborCount);
    for (int i = 0; i < hdr.neighborCount; i++) {
        neighbor_t n;
        memcpy(n.mac, nodes[i].mac, 6);
        n.model = nodes[i].model;
        n.outputStates = nodes[i].outputStates;
        n.inputStates = nodes[i].inputStates;
        n.online = nodes[i].online;
        memcpy(p, &n, sizeof(n));
        p += sizeof(n);
    }
    size_t packetSize = p - buffer;
    
    WSSendPacket(buffer, packetSize);
}
    */

void sendInfo() {
    if (!wsConnected) return;

    uint8_t buffer[512];
    buffer[0] = 'I';

    uint8_t *p = buffer + 1;
    device_info_hdr_t hdr = {0};

    getMac(hdr.mac);
    hdr.freeMemory = esp_get_free_heap_size();
    hdr.uptimeRaw = getUpTimeRaw();
    hdr.version = getConfigVersion();
    hdr.curdate = time(NULL);
    hdr.wifiRSSI = getRSSI();
    hdr.ethIP = ethIp;
    hdr.wifiIP = wifiIp;

    snprintf(hdr.resetReason,
             sizeof(hdr.resetReason),
             "%s",
             espResetReason());

    hdr.outputStates = getOutputs();
    hdr.inputStates = getInputs();

    node_t *nodes;
    hdr.neighborCount = getNodes(&nodes);

    size_t hdrSize = offsetof(device_info_hdr_t, neighbors);

    size_t required =
        1 +
        hdrSize +
        hdr.neighborCount * sizeof(neighbor_t);

    if (required > sizeof(buffer)) {
        ESP_LOGE(TAG, "Info packet too big!");
        return;
    }

    memcpy(p, &hdr, hdrSize);
    p += hdrSize;

    for (int i = 0; i < hdr.neighborCount; i++) {
        neighbor_t *n = (neighbor_t *)p;
        memcpy(n->mac, nodes[i].mac, 6);
        n->model = nodes[i].model;
        n->outputStates = nodes[i].outputStates;
        n->inputStates = nodes[i].inputStates;
        n->online = nodes[i].online;
        p += sizeof(neighbor_t);
    }

    size_t packetSize = p - buffer;

    ESP_LOGI(TAG, "info. neighbor count %d", hdr.neighborCount);

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
    char *cert = loadJwtCert();
    if (cert==NULL) {
        ESP_LOGE(TAG, "Failed to load JWT certificate");
        cJSON_Delete(payload);
        free(payload_str);
        return NULL;
    }    
    size_t tokenLen = 512;
    char *token = malloc(tokenLen);
    if (generate_jwt_rs2562(payload_str, cert, token, tokenLen) != ESP_OK) {
        free(token);
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

static void setOnline(bool online) {
    wsConnected = online;
    sendNodeStatus(online);
}

void sendEvent(uint8_t* mac, uint8_t io_type, uint8_t io_id, bool state, uint16_t timer, uint16_t outputStates, uint16_t inputStates) {
    // AA 0E NODE TT ID SS TMR ISlow IShi OSlow OShi
    // TT type SS state TMR of output    
    uint8_t buflen = 16;
    uint8_t buffer[buflen];    
    buffer[0] = 'E'; // event
    memcpy(buffer+1, mac, 6);
    buffer[7] = io_type;
    buffer[8] = io_id;    
    buffer[9] = state;
    buffer[10] = outputStates & 0xFF; 
    buffer[11] = (outputStates >> 8) & 0xFF; 
    buffer[12] = inputStates & 0xFF;
    buffer[13] = (inputStates >> 8) & 0xFF; 
    // TODO: io states, timer optional if io_type is output
    if (io_type == 0) { 
        buffer[14] = timer >> 8;
        buffer[15] = timer & 0xFF;
    } else {
        buflen = 14;
    }
    WSSendPacket(buffer, buflen);
}

void IOhandler(io_event_t event) {
    ESP_LOGI(TAG, "IOhandler typeIO %d io %d state %s, node %s", 
             event.io_type, event.io_id, event.state ? "on" : "off", strNode(&event.node));
    // input or output event happened. Publish it to cloud and local network
    sendEvent(event.node.mac, event.io_type, event.io_id, event.state, event.timer, getOutputs(), getInputs());
    // AA 0E NODE TT ID SS TMR
    // uint8_t buflen = 12;
    // uint8_t buffer[buflen];    
    // buffer[0] = 'E'; // event
    // memcpy(buffer+1, &event.node.mac, 6);
    // buffer[7] = event.io_type;
    // buffer[8] = event.io_id;
    // //buffer[9] = event.event;        
    // buffer[9] = event.state;
    // if (event.io_type == 0) { 
    //     buffer[10] = event.timer >> 8;
    //     buffer[11] = event.timer & 0xFF;
    // } else {
    //     buflen = 10;
    // }
    // WSSendPacket(buffer, buflen);

    // сюда прилетают сообщения об изменении соостояния IO от HW. Нужно отправить на сервере и по 485
    // и надо еще от шины. и тут решить отправлять за других или нет
}

void BusHandler(bus_event_t event) {
    // может прилететь событие онлайн/оффлан, новой ноды, а может изменения состояния IO других нод
    // BEVT_NODESTATUS - изменение статуса подключения к серверу ноды
    // BEVT_NEWNODE - добавление новой ноды вместе со статусами IO
    // BEVT_ACTION - изменение состояния выхода текущей ноды. 
    // Принять пакет, сделать действие, получить актуальное состояние выхода и отправить его на сервер. Или лучше отдельно отслеживать изменение состояний изменения IO и отправлять на сервер...
    switch (event.event) {
        case BEVT_NEWNODE:
            sendNewNode(event.target_node, event.model, event.outputStates, event.inputStates);
            break;        
        case BEVT_NODESTATUS:            
            break;
        case BEVT_ACTION:            
            action_cfg_t act;
            act.output_id = event.io_event.io_id;
            act.action = event.io_event.action;
            act.target_node = event.io_event.node;
            setOutput(&act);
        case BEVT_IOEVENT:
            if (!event.online) {
                // only for offline
                sendEvent(event.target_node, event.io_event.io_type, event.io_event.io_id,
                    event.io_event.state, event.io_event.timer, event.outputStates, event.inputStates);                
            }
            break;
        default:
            break;
    }

    // TODO : отправка за оффлайн ноду
}

static void wsHandler(ws_event_id_t event, const uint8_t *data, uint32_t len) {
    switch (event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            // waiting for hello message from server with timestamp, then we will set time and send HELLO with JWT token
            break;
        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            setOnline(false);
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
                            setOnline(true);
                            sendInfo();
                        } else {
                            ESP_LOGE(TAG, "HELLO response error %d", data[1]);
                            setOnline(false);
                            WSRestart();
                        }                        
                        break;
                    case 'C': // config
                        nodes_cfg_t *cfg = (nodes_cfg_t*)data;                        
                        updateBConfig(cfg);                        
                        break;    
                    case 'E': // Common error   
                        ESP_LOGI(TAG, "Common error %d", data[1]);
                        setOnline(false);
                        WSRestart();
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
        WSinit("ws://192.168.4.120:8888/ws", &wsHandler, getConfigValueBool("network/cloud/log"));            
    }
}

void initCore() {   
    // register handler 
    registerIOHandler(&IOhandler);
    registerBUSHandler(&BusHandler);
    //runWebServer();
    //xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint32_t usStackDepth, void *const pvParameters, UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask)
    xTaskCreate(infoTask, "infoTask", 4096, NULL, 10, NULL);    
}