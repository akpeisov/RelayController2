#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "rs485.h"
#include "bconfig.h"
#include "utils.h"
#include "hardware.h"

#define TAG "RS485"

#define UART_PORT UART_NUM_1
#define UART_TX   4 
#define UART_RX   35
#define UART_RTS  12 // на старых 5

#define UART_BUF 1024
#define MAX_NODES 19

#define MAGIC 0xA5
#define MAXTASKNAME 22
#define BROADCAST_MAC "\xFF\xFF\xFF\xFF\xFF\xFF"

static uint8_t self_mac[6];
static uint8_t self_model;
static uint16_t msg_counter = 0;

typedef enum {
    MSG_HELLO     = 1,
    MSG_HELLO_ACK = 2,
    MSG_EVENT     = 3,
    MSG_ACTION    = 4,
    MSG_ACTION_ACK = 5,
    MSG_CFG_CHUNK = 6,
    MSG_CFG_ACK   = 7,
    MSG_PING      = 8
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  type;
    uint16_t len;
    uint8_t  src[6];
    uint8_t  dst[6];
    uint16_t msg_id;
    uint8_t  payload[];
} frame_t;

static node_t nodes[MAX_NODES];
static int node_count = 0;

typedef struct {
    uint16_t msg_id;
    TaskHandle_t task;
} pending_ack_t;

#define MAX_PENDING 8
static pending_ack_t pending[MAX_PENDING];
static SemaphoreHandle_t pending_mux;

typedef struct {
    action_cfg_t act;
    uint16_t msg_id;
} action_task_ctx_t;

uint16_t next_msg_id(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    uint16_t id;

    portENTER_CRITICAL(&mux);
    id = msg_counter++;
    portEXIT_CRITICAL(&mux);
    return id;
}

static bool mac_equal(uint8_t *a, uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

static void add_or_update_node(uint8_t *mac, uint8_t model) {
    for (int i = 0; i < node_count; i++) {
        if (mac_equal(nodes[i].mac, mac)) {
            nodes[i].last_seen = esp_timer_get_time();
            return;
        }
    }
    if (node_count < MAX_NODES) {
        memcpy(nodes[node_count].mac, mac, 6);
        nodes[node_count].last_seen = esp_timer_get_time();
        nodes[node_count].model = model;
        node_count++;
        //ESP_LOGI(TAG, "New node discovered");
        ESP_LOGI(TAG, "New node discovered %02X%02X%02X%02X%02X%02X model %d", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], model);
    }
}

uint8_t getNodes(node_t **_nodes) {
    *_nodes = nodes;
    return node_count;
}

static void rs485_send(uint8_t *dst, msg_type_t type, void *payload, uint16_t len, uint16_t msg_id) {
    // TODO : check for node exists in node list
    uint8_t buf[UART_BUF];

    frame_t *f = (frame_t *)buf;
    f->magic = MAGIC;
    f->type = type;
    f->len = len;
    memcpy(f->src, self_mac, 6);
    memcpy(f->dst, dst, 6);
    f->msg_id = msg_id;

    if (payload && len)
        memcpy(f->payload, payload, len);
    
    //ESP_LOG_BUFFER_HEXDUMP(TAG, buf, sizeof(frame_t) + len, CONFIG_LOG_DEFAULT_LEVEL);

    uart_write_bytes(UART_PORT, buf, sizeof(frame_t) + len);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(20));    
}

static void handle_frame(frame_t *f) {
    ESP_LOGI(TAG, "Received frame type %d, len %d", f->type, f->len);

    if (f->magic != MAGIC) return;

    // если пакет не нам, то игнорим
    if (!mac_equal(f->dst, self_mac) && memcmp(f->dst, BROADCAST_MAC, 6) != 0)
        return;

    switch (f->type) {

        case MSG_HELLO:            
            add_or_update_node(f->src, f->payload[0]); 
            rs485_send(f->src, MSG_HELLO_ACK, NULL, 0, next_msg_id());
            break;

        case MSG_HELLO_ACK:
            ESP_LOGI(TAG, "HELLO_ACK received");
            break;

        case MSG_EVENT:
            ESP_LOGI(TAG, "EVENT received");
            // execute_event(...)
            break;

        case MSG_ACTION:
            ESP_LOGI(TAG, "ACTION received");
            if (f->len == 2) {
                ESP_LOGI(TAG, "output %d action %d", f->payload[0], f->payload[1]);
                action_cfg_t act;
                act.output_id = f->payload[0];
                act.action = f->payload[1];
                act.target_node = LOCAL_NODE;
                // do action
                setOutput(&act);
                // ack отправляет тот же msgId, но не для бродкастовых пакетов
                if (memcmp(f->dst, BROADCAST_MAC, 6) != 0)
                    rs485_send(f->src, MSG_ACTION_ACK, NULL, 0, f->msg_id);                
            }            
            break;
         
        case MSG_ACTION_ACK:
            xSemaphoreTake(pending_mux, portMAX_DELAY);
            for (int i = 0; i < MAX_PENDING; i++) {
                if (pending[i].msg_id == f->msg_id 
                    && pending[i].task != NULL) {
                    xTaskNotify(pending[i].task, 1, eSetValueWithOverwrite);
                    break;
                }
            }
            xSemaphoreGive(pending_mux);
            break;    

        case MSG_CFG_CHUNK:
            ESP_LOGI(TAG, "CFG chunk received");
            // cfg_append_chunk(...)
            break;

        case MSG_PING:
            // nothing
            break;
    }
}

static void uart_rx_task(void *arg) {
    uint8_t buf[UART_BUF];
    int buf_len = 0;
    ESP_LOGI(TAG, "Starting RS485 receive task...");
    
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf + buf_len,
                                  UART_BUF - buf_len,
                                  pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        buf_len += len;

        while (buf_len >= sizeof(frame_t)) {
            frame_t *f = (frame_t *)buf;

            if (f->magic != MAGIC) {
                memmove(buf, buf + 1, --buf_len);
                continue;
            }

            int frame_len = sizeof(frame_t) + f->len;
            if (buf_len < frame_len) break;

            handle_frame(f);

            memmove(buf, buf + frame_len, buf_len - frame_len);
            buf_len -= frame_len;
        }
    }
}

static void discovery_task(void *arg) {
    ESP_LOGI(TAG, "Starting RS485 discovery task...");
    while (1) {
        rs485_send((uint8_t *)BROADCAST_MAC, MSG_HELLO, &self_model, 1, next_msg_id());
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void sendNodeActionTask(void *arg) {    
    action_task_ctx_t *ctx = arg;
    uint8_t payload[2] = {
        ctx->act.output_id,
        ctx->act.action
    };

    // регистрируем ожидание ACK
    xSemaphoreTake(pending_mux, portMAX_DELAY);
    int pendingIdx = 255;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].task == NULL) {
            pending[i].msg_id = ctx->msg_id;
            pending[i].task = xTaskGetCurrentTaskHandle();
            pendingIdx = i;
            break;
        }
    }
    xSemaphoreGive(pending_mux);

    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) {
            ESP_LOGI(TAG, "Retrying #%d. Msgid %d", retry, ctx->msg_id);
        }
        rs485_send(ctx->act.target_node.mac,
                   MSG_ACTION,
                   payload,
                   sizeof(payload),
                   ctx->msg_id);

        uint32_t notified;
        if (xTaskNotifyWait(0, UINT32_MAX, &notified,
                            pdMS_TO_TICKS(500)) == pdTRUE) {
            // ACK получен
            break;
        }
    }

    // удаляем из pending
    xSemaphoreTake(pending_mux, portMAX_DELAY);
    if (pendingIdx < 255) {
        pending[pendingIdx].task = NULL;
    }    
    xSemaphoreGive(pending_mux);
    vTaskDelete(NULL);
}

void sendNodeAction(action_cfg_t *act) {        
    action_task_ctx_t *ctx = malloc(sizeof(action_task_ctx_t));
    if (!ctx) return;

    ctx->act = *act;
    ctx->msg_id = next_msg_id();

    char taskName[MAXTASKNAME];
    snprintf(taskName, MAXTASKNAME,
             "sendNode_%02X%02X%02X%02X%02X%02X",
             act->target_node.mac[0], act->target_node.mac[1], act->target_node.mac[2],
             act->target_node.mac[3], act->target_node.mac[4], act->target_node.mac[5]);

    TaskHandle_t xHandle = xTaskGetHandle(taskName);
    if (xHandle) {
        vTaskDelete(xHandle);
    }

    xTaskCreate(sendNodeActionTask,
                taskName,
                4096,
                ctx,
                5,
                NULL);
}

void sendNodeEvent(uint8_t input, event_type_t evt) {
    // TODO : send each event to all nodes
}

void rs485_init() {
    self_model = controllerType;   
    getMac(self_mac);    
    ESP_LOGI(TAG, "Initing RS485. Mac is %02X%02X%02X%02X%02X%02X", self_mac[0], self_mac[1], self_mac[2], self_mac[3], self_mac[4], self_mac[5]);

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_param_config(UART_PORT, &cfg);
    uint8_t rts_pin = UART_RTS;
    if (self_mac[5] != 0xc4)
        rts_pin = 5;

    ESP_LOGI(TAG, "RTS pin %d", rts_pin);    
    uart_set_pin(UART_PORT, UART_TX, UART_RX, rts_pin, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT, UART_BUF, UART_BUF, 0, NULL, 0);
    uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX);  

    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);
    xTaskCreate(discovery_task, "discovery", 2048, NULL, 5, NULL);
    pending_mux = xSemaphoreCreateMutex();
}

/*
static void sendNodeActionTask(void *arg) {    
    action_task_ctx_t *ctx = arg;
    uint8_t payload[2] = {
        ctx->act.output_id,
        ctx->act.action
    };

    // регистрируем ожидание ACK
    xSemaphoreTake(pending_mux, portMAX_DELAY);
    int pendingIdx = 255;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].task == NULL) {
            pending[i].msg_id = ctx->msg_id;
            pending[i].task = xTaskGetCurrentTaskHandle();
            pendingIdx = i;
            break;
        }
    }
    xSemaphoreGive(pending_mux);

    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) {
            ESP_LOGI(TAG, "Retrying #%d. Msgid %d", retry, ctx->msg_id);
        }
        rs485_send(ctx->act.target_node.mac,
                   MSG_ACTION,
                   payload,
                   sizeof(payload),
                   ctx->msg_id);

        uint32_t notified;
        if (xTaskNotifyWait(0, UINT32_MAX, &notified,
                            pdMS_TO_TICKS(500)) == pdTRUE) {
            // ACK получен
            break;
        }
    }

    // удаляем из pending
    xSemaphoreTake(pending_mux, portMAX_DELAY);
    if (pendingIdx < 255) {
        pending[pendingIdx].task = NULL;
    }    
    xSemaphoreGive(pending_mux);
    vTaskDelete(NULL);
}

void sendNodeAction(action_cfg_t *act) {        
    action_task_ctx_t *ctx = malloc(sizeof(action_task_ctx_t));
    if (!ctx) return;

    ctx->act = *act;
    ctx->msg_id = next_msg_id();

    char taskName[MAXTASKNAME];
    snprintf(taskName, MAXTASKNAME,
             "sendNode_%02X%02X%02X%02X%02X%02X",
             act->target_node.mac[0], act->target_node.mac[1], act->target_node.mac[2],
             act->target_node.mac[3], act->target_node.mac[4], act->target_node.mac[5]);

    TaskHandle_t xHandle = xTaskGetHandle(taskName);
    if (xHandle) {
        vTaskDelete(xHandle);
    }

    xTaskCreate(sendNodeActionTask,
                taskName,
                4096,
                ctx,
                5,
                NULL);
}

void sendNodeEvent(uint8_t input, event_type_t evt) {
    // TODO : send each event to all nodes
}
*/

void sendConfigChunk(uint8_t *dst, uint8_t *chunk_data, uint16_t chunk_len) {
    // TODO : implement it
    // пример Отправка конфигурации 
    // uint8_t config_data[64];
    // ... заполнение данных ...
    // sendConfigChunk(target_mac, config_data, sizeof(config_data));

    queued_msg_t *msg = malloc(sizeof(queued_msg_t));
    if (!msg) return;
    
    uint8_t *payload = malloc(chunk_len);
    if (!payload) {
        free(msg);
        return;
    }
    
    memcpy(payload, chunk_data, chunk_len);
    memcpy(msg->dst, dst, 6);
    msg->type = MSG_CFG_CHUNK;
    msg->msg_id = next_msg_id();
    msg->payload = payload;
    msg->payload_len = chunk_len;    
    msg->require_ack = true;
    msg->waiting_task = xTaskGetCurrentTaskHandle();    
    addQueue(msg);
}