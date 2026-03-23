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
static bool reboot = false;

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

void sendConfig() {
    cJSON *config = getConfigJSON();
    if (config == NULL) {
        ESP_LOGE(TAG, "No config");
        return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "CONFIG");
    cJSON_AddItemReferenceToObject(msg, "payload", config);
    //cJSON_AddItemToObject(msg, "payload", config);
    char *message = cJSON_PrintUnformatted(msg);
    if (message != NULL) {      
        WSSendTextPacket(message, strlen(message));
        free(message);
    }

    cJSON_Delete(msg);    
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

esp_err_t setLed(char **response, char *content) {
    cJSON *parent = cJSON_Parse(content);
    if (!cJSON_IsObject(parent)) {
        setErrorTextJson(response, "Is not a JSON object");
        cJSON_Delete(parent);
        return ESP_FAIL;
    }
    uint16_t r = cJSON_GetObjectItem(parent, "r")->valueint;
    uint16_t g = cJSON_GetObjectItem(parent, "g")->valueint;
    uint16_t b = cJSON_GetObjectItem(parent, "b")->valueint;    
    setRGBFaceValue(r, g, b);
    setTextJson(response, "OK");
    cJSON_Delete(parent);
    return ESP_OK;
}

esp_err_t doOta(char **response, char *content) {
    if (content == NULL || (strncmp(content, "ws://", 5) == 0 && strncmp(content, "wss://", 6) == 0)) {
        setErrorTextJson(response, "Is not a valid ws address");
        return ESP_FAIL;
    }
    char *res = startOTA(content);
    if (!strcmp(res, "OK")) {
        setTextJson(response, "OK");
        return ESP_OK;
    }
    setErrorTextJson(response, res);
    return ESP_FAIL;    
}

esp_err_t editConfig(httpd_req_t *req) {    
    extern const char config_start[] asm("_binary_config_html_start");
    extern const char config_end[] asm("_binary_config_html_end");

    const uint32_t config_len = config_end - config_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, config_start, config_len);
    
    return ESP_OK;
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
    } else if (!strcmp(uri, "/service/ota")) {
        if (req->method == HTTP_POST) {
            err = getContent(&content, req);
            if (err == ESP_OK) {
                err = doOta(&response, content);
            }
        }                      
    } else if (!strcmp(uri, "/service/testled")) {
        if (req->method == HTTP_POST) {
            err = getContent(&content, req);
            if (err == ESP_OK) {
                if (xSemaphoreTake(gMutex, portMAX_DELAY) == pdTRUE) {
                    err = setLed(&response, content); 
                    xSemaphoreGive(gMutex);
                }
            }
        }            
    } else if (!strcmp(uri, "/service/editconfig")) {
        if (req->method == HTTP_GET) {
            err = editConfig(req);
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

void sendEvent(io_event_t event) {
    // AA 0E NODE TT ID SS TMR ISlow IShi OSlow OShi
    // TT type SS state TMR of output
    uint16_t total_len = sizeof(ws_event_t) + sizeof(io_event_t);
    ws_event_t *packet = malloc(total_len);
    if (packet) {
        packet->msg_type = 'E';
        memcpy(packet->payload, &event, sizeof(io_event_t));
        ESP_LOG_BUFFER_HEXDUMP(TAG, (uint8_t*)packet, total_len, CONFIG_LOG_DEFAULT_LEVEL);
        WSSendPacket((uint8_t*)packet, total_len);        
        free(packet);
    } else {
        ESP_LOGE(TAG, "Can't allocate memory for packet");
    }
}

void serviceTask(void *pvParameter) {
    ESP_LOGI(TAG, "Creating service task");
    while(1) {   
        if (reboot) {
            static uint8_t cntReboot = 0;
            if (cntReboot++ >= 3) {
                ESP_LOGI(TAG, "Reboot now!");
                esp_restart();
            }
        }        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void IOhandler(io_event_t event) {
    // сюда прилетают сообщения об изменении соостояния IO от HW. Нужно отправить на сервере и по 485
    // и надо еще от шины. и тут решить отправлять за других или нет
    ESP_LOGI(TAG, "IOhandler typeIO %d io %d state %s, node %s", 
             event.io_type, event.io_id, event.state ? "on" : "off", strNode(&event.node));
    // input or output event happened. Publish it to cloud and local network
    //sendEvent(event.node.mac, event.io_type, event.io_id, event.state, event.timer, getOutputs(), getInputs());    
    sendEvent(event);    
}

void setConfigResult(node_uid_t node, uint16_t version, config_result_t res) {
    size_t total_len = 10;
    uint8_t *packet = malloc(total_len);
    if (packet) {
        packet[0] = 'C';        
        memcpy(packet+1, node.mac, sizeof(node_uid_t));
        packet[7] = version & 0xFF;
        packet[8] = (version >> 8) & 0xFF;
        packet[9] = res;
        WSSendPacket((uint8_t*)packet, total_len);        
        free(packet);
    }
}

void BusHandler(bus_event_t event) {
    // может прилететь событие онлайн/оффлан, новой ноды, а может изменения состояния IO других нод
    // BEVT_NODESTATUS - изменение статуса подключения к серверу ноды
    // BEVT_NEWNODE - добавление новой ноды вместе со статусами IO
    // BEVT_ACTION - изменение состояния выхода текущей ноды. 
    // Принять пакет, сделать действие, получить актуальное состояние выхода и отправить его на сервер. Или лучше отдельно отслеживать изменение состояний изменения IO и отправлять на сервер...
    switch (event.event) {
        case BEVT_NEWNODE:
            sendNewNode(event.node.mac, event.model, event.outputStates, event.inputStates);
            break;        
        case BEVT_NODESTATUS:            
            break;
        case BEVT_ACTION:            
            action_cfg_t act;
            // act.output_id = event.io_event.io_id;
            // act.action = event.io_event.action;
            // act.target_node = event.io_event.node;
            act.action = event.io_action.action;
            act.output_id = event.io_action.io_id;
            act.target_node = event.io_action.node;
            setOutput(&act);
            break;
        case BEVT_IOEVENT:
            if (!event.online) {
                // only for offline node                
                sendEvent(event.io_event);
                ESP_LOGI(TAG, "bus event. id %d, outputs %d, inputs %d",
                event.io_event.io_id, event.io_event.outputsStates, event.io_event.inputStates);
                // sendEvent(event.io_event.node.mac, event.io_event.io_type, event.io_event.io_id,
                //     event.io_event.state, event.io_event.timer, event.outputStates, event.inputStates);                
            }
            break;
        case BEVT_CFG_VER:
            if (!event.online) {
                setConfigResult(event.configResult.node, event.configResult.version, event.configResult.result);
            }
        default:
            ESP_LOGI(TAG, "Unknown bus event");
            break;
    }

    // TODO : отправка за оффлайн ноду
}

void bHandler(bconfig_event_t event) {
    // callback from bconfig
    setConfigResult(event.node, event.version, event.result);
}

static void wsHandler(ws_event_id_t event, const uint8_t *data, uint32_t len) {
    switch (event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            // waiting for hello message from server with timestamp, then we will set time and send HELLO with JWT token
            setRGBFace("green");
            break;
        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            setRGBFace("yellow");
            setOnline(false);
            break;
        case WS_EVENT_DATA:
            ESP_LOGI(TAG, "WebSocket data received. Len %d", len);
            if (getConfigValueBool("debug")) ESP_LOG_BUFFER_HEXDUMP("WS DATA", data, len, CONFIG_LOG_DEFAULT_LEVEL);
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
                        //nodes_cfg_t *cfg = (nodes_cfg_t*)data+1;
                        //ESP_LOG_BUFFER_HEXDUMP("new cfg", data+1, len-1, CONFIG_LOG_DEFAULT_LEVEL);
                        //updateConfig(data+1);    
                        updateBConfig(data+1);                                     
                        break;    
                    case 'E': // Common error   
                        ESP_LOGI(TAG, "Common error %d", data[1]);
                        setOnline(false);
                        WSRestart();
                        break;
                    case 'A': // Action from cloud
                        // 0   1  2-7   8       9     10    11-12
                        // AA  A  NODE  OUT/IN  ID  ACTION  CRC16                        
                        node_uid_t node;
                        memcpy(node.mac, data+1, 6);                 
                        if (data[7] == 0) { // output action
                            // output
                            action_cfg_t act;
                            act.output_id = data[8];
                            act.action = data[9];
                            memcpy(act.target_node.mac, &data[1], 6);
                            setOutput(&act);
                        } else if (isLocalNode(&node)) {
                            // обработка кнопок с фронта
                            processInputEvent(data[9], data[8]);                            
                        } else {
                            ESP_LOGI(TAG, "other action not implemened");
                            // action already worked correctly, but button action to remote node not yet
                        }
                        break;
                    case 0xC0: // command
                        switch (data[1]) {
                            case 'R':
                                ESP_LOGI(TAG, "Reboot command");
                                reboot = true;
                                break;
                            case 'O':
                                ESP_LOGI(TAG, "OTA command");
                                startOTA(getConfigValueString("network/otaURL") );
                                break;
                            case 'I':
                                ESP_LOGI(TAG, "Info command");
                                sendInfo();
                                break;
                            case 'C':
                                ESP_LOGI(TAG, "Config requested");
                                sendConfig();
                                break;
                            default:
                                ESP_LOGE(TAG, "Unknown command %s", data[1]);
                                break;
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
        //WSinit("ws://192.168.4.120:8888/ws", &wsHandler, getConfigValueBool("network/cloud/log"));            
        WSinit(getConfigValueString("network/cloud/address"), &wsHandler, getConfigValueBool("network/cloud/log"));            
    }
}

void initCore() {   
    // register handler 
    registerIOHandler(&IOhandler);
    registerBUSHandler(&BusHandler);
    registerBHandler(&bHandler);
    //runWebServer();
    //xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint32_t usStackDepth, void *const pvParameters, UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask)
    xTaskCreate(&serviceTask, "serviceTask", 4096, NULL, 5, NULL);    
    xTaskCreate(&infoTask, "infoTask", 4096, NULL, 10, NULL);        
}