#include <stddef.h>
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

//#include "driver/gpio.h"

#define TAG "RS485"

#define UART_PORT UART_NUM_1
#define UART_TX   4 
#define UART_RX   35
#define UART_RTS  12 
#define UART_RTS_OLD 5

#define UART_BUF 1024
#define MAX_NODES 19

#define MAGIC 0xA5
#define MAXTASKNAME 22
#define MAX_WAITING_TIME 1000//300
#define BROADCAST_MAC "\xFF\xFF\xFF\xFF\xFF\xFF"

static node_uid_t self_node;
static uint16_t msg_counter = 0;
static TBusEvent busevent = NULL;

typedef enum {
    MSG_HELLO     = 1,
    MSG_HELLO_ACK,
    MSG_EVENT,
    MSG_ACTION,
    MSG_ACK,
    MSG_CFG_CHUNK,
    //MSG_CFG_ACK   = 7,
    MSG_CFG_START,
    MSG_CFG_END,
    MSG_CFG_NACK,  
    MSG_PING,
    MSG_STATUS,
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

typedef struct {
    action_cfg_t act;
    uint16_t msg_id;
} action_task_ctx_t;

typedef struct {
    uint8_t dst[6];
    msg_type_t type;
    uint16_t msg_id;
    void *payload;
    uint16_t payload_len;
    bool require_ack;
    TaskHandle_t waiting_task;  // NULL если не ждем подтверждения
    uint32_t timestamp;  
} queued_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t  io_type;
    uint8_t  io_id;
    uint8_t  state;
    uint16_t output_states;
    uint16_t input_states;
    uint8_t  online;
    uint8_t  model;
} device_payload_t;

// config specific
typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint16_t crc16;
    uint16_t chunk_size;    
} cfg_start_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;     
    uint16_t len;
    uint8_t  data[];
} cfg_chunk_t;

typedef struct __attribute__((packed)) {
    uint16_t last_seq;
} cfg_end_t;

typedef struct {
    uint8_t *buffer;
    uint32_t total_size;
    uint16_t expected_crc;
    uint16_t chunk_size;
    uint16_t received_chunks;
    uint16_t last_seq;
} cfg_rx_ctx_t;

static cfg_rx_ctx_t rx;

#define QUEUE_SIZE 32
static QueueHandle_t msg_queue = NULL;
static SemaphoreHandle_t bus_mutex = NULL;  // для контроля доступа к шине
#define MAX_PENDING 8
static pending_ack_t pending[MAX_PENDING];
static SemaphoreHandle_t pending_mux;
#define BUS_IDLE_TIMEOUT_MS 20     // Время простоя шины перед началом передачи
#define RANDOM_BACKOFF_MAX_MS 50  // Максимальная случайная задержка
static uint32_t last_rx_time = 0;  // Время последнего приема
static bool bus_busy = false;      // Флаг занятости шины
static SemaphoreHandle_t bus_access_sem = NULL;
static bool node_online = false;

uint16_t next_msg_id(void) {
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

static void add_or_update_node(uint8_t *mac, model_t model, 
                               uint16_t outputStates, uint16_t inputStates, bool online) {
    for (int i = 0; i < node_count; i++) {
        if (mac_equal(nodes[i].mac, mac)) {
            nodes[i].last_seen = esp_timer_get_time();
            nodes[i].online = online;
            nodes[i].inputStates = inputStates;
            nodes[i].outputStates = outputStates;
            return;
        }
    }
    if (node_count < MAX_NODES) {
        memcpy(nodes[node_count].mac, mac, 6);
        nodes[node_count].last_seen = esp_timer_get_time();
        nodes[node_count].model = model;
        nodes[node_count].online = online;
        nodes[node_count].inputStates = inputStates;
        nodes[node_count].outputStates = outputStates;
        node_count++;        
        ESP_LOGI(TAG, "New node discovered %02X%02X%02X%02X%02X%02X model %d", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], model);

        bus_event_t evt; 
        evt.event = BEVT_NEWNODE;       
        // TODO : states, model?
        memcpy(evt.target_node, mac, 6);
        evt.online = online;
        evt.model = model;
        evt.inputStates = inputStates;
        evt.outputStates = outputStates;
        if (busevent) busevent(evt);
    }
}

static void update_node_status(uint8_t *mac, bool online) {
    for (int i = 0; i < node_count; i++) {
        if (mac_equal(nodes[i].mac, mac)) {
            nodes[i].last_seen = esp_timer_get_time();
            nodes[i].online = online;
            // send event to core
            bus_event_t evt; 
            evt.event = BEVT_NODESTATUS;                   
            memcpy(evt.target_node, mac, 6);
            evt.online = online;
            if (busevent) busevent(evt);
            return;
        }
    }
}

uint8_t getNodes(node_t **_nodes) {
    *_nodes = nodes;
    return node_count;
}

static bool wait_for_bus_idle(uint32_t timeout_ms) {
    uint32_t start_time = xTaskGetTickCount();
    
    while (1) {
        // Проверяем, не было ли приема в последнее время
        uint32_t current_time = xTaskGetTickCount();
        uint32_t idle_time = current_time - last_rx_time;
        
        if (idle_time >= pdMS_TO_TICKS(BUS_IDLE_TIMEOUT_MS) && !bus_busy) {
            // Шина свободна достаточное время
            bus_busy = true;  // Помечаем как занятую нами
            return true;
        }
        
        // Если таймаут истек
        if ((current_time - start_time) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        
        // Случайная задержка перед следующей проверкой
        uint32_t backoff = rand() % RANDOM_BACKOFF_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(10 + backoff));
    }
}

const char* msg_type_to_str(msg_type_t msg_type) {
    switch (msg_type) {
        case MSG_HELLO:      return "MSG_HELLO";
        case MSG_HELLO_ACK:  return "MSG_HELLO_ACK";
        case MSG_EVENT:      return "MSG_EVENT";
        case MSG_ACTION:     return "MSG_ACTION";
        case MSG_ACK:        return "MSG_ACK";
        case MSG_CFG_CHUNK:  return "MSG_CFG_CHUNK";        
        case MSG_CFG_START:  return "MSG_CFG_START";
        case MSG_CFG_END:    return "MSG_CFG_END";
        case MSG_CFG_NACK:   return "MSG_CFG_NACK";
        case MSG_PING:       return "MSG_PING";        
        case MSG_STATUS:     return "MSG_STATUS";
        default:             return "MSG_UNKNOWN";
    }
}

char *printFrame(frame_t *f) {
    static char buf[256];
    int offset = snprintf(buf, sizeof(buf), "%02X SRC %02X%02X%02X%02X%02X%02X DST %02X%02X%02X%02X%02X%02X %s ID %d LEN %d PL ",
             f->magic,             
             f->src[0], f->src[1], f->src[2], f->src[3], f->src[4], f->src[5],
             f->dst[0], f->dst[1], f->dst[2], f->dst[3], f->dst[4], f->dst[5],
             msg_type_to_str(f->type),             
             f->msg_id,
             f->len);
    // Добавляем payload в hex
    if (f->len > 0) {
        for (int i = 0; i < f->len && offset < sizeof(buf) - 3; i++) {
            offset += snprintf(buf + offset, sizeof(buf) - offset, "%02X", f->payload[i]);
            if (i < f->len - 1 && offset < sizeof(buf) - 1) {
                buf[offset++] = ' ';
                buf[offset] = '\0';
            }
        }
    }

    return buf;
}

static void rs485_send(uint8_t *dst, msg_type_t type, void *payload, uint16_t len, uint16_t msg_id) {
    // check for node exists in nodes list
    if (!mac_equal(dst, (uint8_t*)BROADCAST_MAC)) {
        bool node_found = false;
        for (int i = 0; i < node_count; i++) {
            if (mac_equal(nodes[i].mac, dst)) {
                node_found = true;
                break;
            }
        }
        // if (!node_found) {
        //     ESP_LOGW(TAG, "Attempt to send to unknown node %02X%02X%02X%02X%02X%02X", 
        //              dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
        //     return;
        // }
    }

    uint8_t buf[UART_BUF];

    frame_t *f = (frame_t *)buf;
    f->magic = MAGIC;
    f->type = type;
    f->len = len;
    memcpy(f->src, self_node.mac, 6);
    memcpy(f->dst, dst, 6);
    f->msg_id = msg_id;

    if (payload && len)
        memcpy(f->payload, payload, len);
    
    //ESP_LOG_BUFFER_HEXDUMP(TAG, buf, sizeof(frame_t) + len, CONFIG_LOG_DEFAULT_LEVEL);
    ESP_LOGI(TAG, "TX %s", printFrame(f));
    uart_write_bytes(UART_PORT, buf, sizeof(frame_t) + len);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(20));    
}

static bool rs485_send_with_arbitration(uint8_t *dst, msg_type_t type, void *payload, 
                                         uint16_t len, uint16_t msg_id, bool is_broadcast) {
    // Для широковещательных сообщений - дополнительная задержка
    if (is_broadcast) {
        // Случайная задержка для предотвращения коллизий
        uint32_t broadcast_delay = rand() % 50;  // 0-50 мс
        vTaskDelay(pdMS_TO_TICKS(broadcast_delay));
    }
    
    // Ждем семафор доступа к шине
    if (xSemaphoreTake(bus_access_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire bus access");
        return false;
    }
    
    bool result = false;
        
    if (wait_for_bus_idle(200)) {
        // Отправляем данные
        rs485_send(dst, type, payload, len, msg_id);        
        // Обновляем время последней передачи
        //last_rx_time = xTaskGetTickCount();
        bus_busy = false;  // Снова помечаем шину как свободную        
        result = true;
    } else {
        ESP_LOGW(TAG, "Bus busy timeout");
    }
    
    // Освобождаем семафор
    xSemaphoreGive(bus_access_sem);
    
    return result;
}

static void senderTask(void *arg) {
    queued_msg_t *msg;
    uint32_t min_send_interval = pdMS_TO_TICKS(5);
    
    while (1) {
        if (xQueueReceive(msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            int retries = 0;            
            bool is_broadcast = (memcmp(msg->dst, BROADCAST_MAC, 6) == 0);
            if (!is_broadcast)
                retries = 2;

            if (msg->require_ack) {
                msg->waiting_task = xTaskGetCurrentTaskHandle();
                //if (msg->require_ack && msg->waiting_task != NULL) {
                    xSemaphoreTake(pending_mux, portMAX_DELAY);        
                    for (int i = 0; i < MAX_PENDING; i++) {
                        if (pending[i].task == NULL) {
                            ESP_LOGI(TAG, "Adding pending i %d", i);
                            pending[i].msg_id = msg->msg_id;
                            pending[i].task = msg->waiting_task;
                            break;
                        }
                    }
                    xSemaphoreGive(pending_mux);                
            }

            for (int retry = 0; retry <= retries; retry++) {
                if (retry > 0) {
                    // Увеличенная задержка перед повторной отправкой
                    uint32_t backoff = 50 + (rand() % 100);  // 50-150 мс
                    vTaskDelay(pdMS_TO_TICKS(backoff));
                    ESP_LOGI(TAG, "Retry #%d for msg_id %d", retry, msg->msg_id);
                }
                
                // Отправляем с арбитражем
                if (rs485_send_with_arbitration(msg->dst, msg->type, msg->payload,
                                                msg->payload_len, msg->msg_id, is_broadcast)) {
                    
                    // Для широковещательных или сообщений без подтверждения
                    if (!msg->require_ack || is_broadcast) {
                        //ack_received = true;
                        break;
                    }
                    
                    // Ждем подтверждение с таймаутом
                    uint32_t notified;
                    if (msg->waiting_task)
                            ESP_LOGI(TAG, "Waiting have task. ACK for msg_id %d from task %s", msg->msg_id, pcTaskGetName(msg->waiting_task));
                    if (msg->waiting_task && xTaskNotifyWait(0, UINT32_MAX, &notified, pdMS_TO_TICKS(MAX_WAITING_TIME)) == pdTRUE) {
                        ESP_LOGI(TAG, "notify ACK received for msg_id %d", msg->msg_id);
                        //ack_received = true;
                        break;
                    }
                }                
            }
            
            //if (msg->payload) free(msg->payload);
            
            if (msg->require_ack && msg->waiting_task) {
                ESP_LOGI(TAG, "Removing pending msg_id %d for task %s", msg->msg_id, pcTaskGetName(msg->waiting_task));
                xSemaphoreTake(pending_mux, portMAX_DELAY);
                for (int i = 0; i < MAX_PENDING; i++) {
                    if (pending[i].msg_id == msg->msg_id) {
                        pending[i].task = NULL;
                        pending[i].msg_id = 0;
                        break;
                    }
                }
                xSemaphoreGive(pending_mux);
            }
            if (msg->payload_len) free(msg->payload);
            free(msg);
            vTaskDelay(min_send_interval);
        }
    }
}

void addQueue(queued_msg_t *msg) {
    if (msg == NULL || msg_queue == NULL) return;    
    msg->timestamp = xTaskGetTickCount();
    
    // if (msg->require_ack && msg->waiting_task != NULL) {
    //     xSemaphoreTake(pending_mux, portMAX_DELAY);        
    //     for (int i = 0; i < MAX_PENDING; i++) {
    //         if (pending[i].task == NULL) {
    //             ESP_LOGI(TAG, "Adding pending i %d", i);
    //             pending[i].msg_id = msg->msg_id;
    //             pending[i].task = msg->waiting_task;
    //             break;
    //         }
    //     }
    //     xSemaphoreGive(pending_mux);
    // }
    
    ESP_LOGI(TAG, "Queueing msg_id %d type %s, payload len %d", 
             msg->msg_id, msg_type_to_str(msg->type), msg->payload_len);
    if (xQueueSend(msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Queue full, message dropped");
        if (msg->payload) free(msg->payload);
        free(msg);
    }
}

void send_msg(uint8_t *dst, msg_type_t type, void *payload, uint16_t len) {
    queued_msg_t *msg = malloc(sizeof(queued_msg_t));
    if (!msg) return;

    memcpy(msg->dst, dst, 6);
    msg->type = type;
    msg->msg_id = next_msg_id();
    msg->require_ack = true;
    msg->waiting_task = NULL;

    if (len > 0 && payload) {
        msg->payload = malloc(len);
        if (!msg->payload) {
            free(msg);
            return;
        }
        memcpy(msg->payload, payload, len);
        msg->payload_len = len;
    } else {
        msg->payload = NULL;
        msg->payload_len = 0;
    }

    addQueue(msg);
}


// void send_msg(uint8_t *dst, msg_type_t type, void *payload, uint16_t len) {
//     queued_msg_t *msg = malloc(sizeof(queued_msg_t));
//     memcpy(msg->dst, dst, 6);
//     msg->type = type;
//     msg->msg_id = next_msg_id();
//     msg->payload = payload;
//     msg->payload_len = len;    
//     msg->require_ack = true;
//     msg->waiting_task = NULL;
    
//     addQueue(msg);
// }

static void handle_frame(frame_t *f) {    
    //ESP_LOGI(TAG, "Received frame type %s, len %d", msg_type_to_str(f->type), f->len);
    ESP_LOGI(TAG, "RX %s", printFrame(f));

    if (f->magic != MAGIC) return;

    // если пакет не нам, то игнорим
    if (!mac_equal(f->dst, self_node.mac) && memcmp(f->dst, BROADCAST_MAC, 6) != 0)
        return;

    uint16_t outputStates = 0;
    uint16_t inputStates = 0;
    bool isonline = false;
    model_t model = UNKNOWN;

    switch (f->type) {
        case MSG_HELLO:
            model = f->payload[0];
            outputStates = f->payload[1] | (f->payload[2] << 8);
            inputStates = f->payload[3] | (f->payload[4] << 8);         
            isonline = f->payload[5] != 0;   
            add_or_update_node(f->src, model, outputStates, inputStates, isonline); 
            //rs485_send(f->src, MSG_HELLO_ACK, NULL, 0, next_msg_id());
            break;

        case MSG_HELLO_ACK:
            ESP_LOGI(TAG, "HELLO_ACK received");
            break;

        case MSG_STATUS:
            ESP_LOGI(TAG, "MSG_STATUS received");
            update_node_status(f->src, isonline); 
            break;    

        case MSG_EVENT:            
            // может измениться состояние как входа так и выхода на удаленной ноде, поэтому обновляем их в списке нод и отправляем событие в core.
            outputStates = f->payload[3] | (f->payload[4] << 8);
            inputStates = f->payload[5] | (f->payload[6] << 8);         
            isonline = f->payload[7] != 0;
            model = f->payload[8];
            
            add_or_update_node(f->src, model, outputStates, inputStates, isonline); 

            // publish BEVT_IOEVENT
            static bus_event_t evt;
            evt.event = BEVT_IOEVENT;
            evt.io_event.io_type = f->payload[0];
            evt.io_event.io_id = f->payload[1];
            evt.io_event.state = f->payload[2];
            evt.inputStates = inputStates;
            evt.outputStates = outputStates;
            evt.online = isonline;
            memcpy(evt.io_event.node.mac, f->src, 6);
            if (busevent) busevent(evt);
            break;

        case MSG_ACTION:
            // action for this node
            if (f->len == 2) {
                ESP_LOGI(TAG, "ACTION received to output %d action %d", f->payload[0], f->payload[1]);
                // TODO : send event to handle
                static bus_event_t evt;
                evt.event = BEVT_ACTION;
                evt.io_action.io_id = f->payload[0];
                evt.io_action.action = f->payload[1];
                memcpy(evt.io_action.node.mac, self_node.mac, 6);
                // evt.io_event.io_id = f->payload[0];
                // evt.io_event.io_type = 0;
                // evt.io_event.action = f->payload[1];                
                //memcpy(evt.io_event.node.mac, self_node.mac, 6);
                if (busevent) busevent(evt);                
                // ack отправляет тот же msgId, но не для бродкастовых пакетов
                if (memcmp(f->dst, BROADCAST_MAC, 6) != 0)
                    rs485_send(f->src, MSG_ACK, NULL, 0, f->msg_id);                
            }            
            break;
         
        case MSG_ACK:
            xSemaphoreTake(pending_mux, portMAX_DELAY);
            for (int i = 0; i < MAX_PENDING; i++) {
                if (pending[i].msg_id == f->msg_id && pending[i].task != NULL) {
                    ESP_LOGI(TAG, "found pending. Received ACK for msg_id %d", f->msg_id);
                    xTaskNotify(pending[i].task, 1, eSetValueWithOverwrite);
                    pending[i].task = NULL;
                    break;
                }
            }
            xSemaphoreGive(pending_mux);
            break;    

        case MSG_CFG_CHUNK: {
            cfg_chunk_t *c = (cfg_chunk_t*)f->payload;
            memcpy(rx.buffer + c->seq * rx.chunk_size, c->data, c->len);
            rx.received_chunks++;
            rs485_send(f->src, MSG_ACK, NULL, 0, f->msg_id);
            break;
        }

        case MSG_CFG_START: {
            cfg_start_t *s = (cfg_start_t*)f->payload;

            rx.total_size = s->total_size;
            rx.expected_crc = s->crc16;
            rx.chunk_size = s->chunk_size;
            rx.received_chunks = 0;

            if (rx.buffer) {
                ESP_LOGE(TAG, "rx.buffer for receive config already allocated. Free it...");
                free(rx.buffer);
                rx.buffer = NULL;
            }                
            rx.buffer = malloc(rx.total_size);
            if (!rx.buffer) {
                ESP_LOGE(TAG, "Error allocating buffer for receive config");
            }
            rs485_send(f->src, MSG_ACK, NULL, 0, f->msg_id);                
            break;
        }    

        case MSG_CFG_END: {
            uint16_t crc = CRC16(rx.buffer, rx.total_size);

            if (crc == rx.expected_crc) {
                ESP_LOGI(TAG, "CFG OK");
                //apply_config(rx.buffer);
                io_cfg_t *newCfg = (io_cfg_t*)rx.buffer;
                updateLocalConfig(newCfg);
                send_msg(f->src, MSG_ACK, NULL, 0);
            } else {
                ESP_LOGE(TAG, "CFG CRC ERROR");
                send_msg(f->src, MSG_CFG_NACK, NULL, 0);
            }

            if (rx.buffer) {
                free(rx.buffer);
                rx.buffer = NULL;
            }
            rs485_send(f->src, MSG_ACK, NULL, 0, f->msg_id);                
            break;
        }

        case MSG_CFG_NACK:
            ESP_LOGE(TAG, "Config NACK received");
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
        last_rx_time = xTaskGetTickCount();

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

void sendNodeAction(action_cfg_t *act) {
    queued_msg_t *msg = malloc(sizeof(queued_msg_t));
    if (!msg) return;
    
    uint8_t *payload = malloc(2);
    if (!payload) {
        free(msg);
        return;
    }
    
    payload[0] = act->output_id;
    payload[1] = act->action;
    
    memcpy(msg->dst, act->target_node.mac, 6);
    msg->type = MSG_ACTION;
    msg->msg_id = next_msg_id();
    msg->payload = payload;
    msg->payload_len = 2;    
    msg->require_ack = true;
    //msg->waiting_task = xTaskGetCurrentTaskHandle();
    
    addQueue(msg);
}

void sendNodeEvent(node_io_event_t event, uint16_t inputStates, uint16_t outputStates) {
    // отправка события изменения IO на все ноды
    // передавать события изменения IO вместе с их текущим состояием, а далее если нода офлайн то отправить от кого-то кто онлайн
    queued_msg_t *msg = malloc(sizeof(queued_msg_t));
    if (!msg) {
        ESP_LOGE(TAG, "Can't allocate memory for nodeevent");
        return;
    }

    device_payload_t *payload = malloc(sizeof(device_payload_t));
    if (payload != NULL) {
        payload->io_type       = event.io_type;
        payload->io_id         = event.io_id;
        payload->state         = event.state;
        payload->output_states = outputStates;
        payload->input_states  = inputStates;
        payload->online        = node_online ? 1 : 0;
        payload->model         = controllerType;
    }
        
    // size_t payload_size = 9; // io_type(1) + io_id(1) + state(1) + outputStates(2) + inputStates(2) + online(1) + model(1)
    // uint8_t *payload = malloc(payload_size);
    // payload[0] = event.io_type;
    // payload[1] = event.io_id;
    // payload[2] = event.state;
    // payload[3] = outputStates & 0xFF;
    // payload[4] = (outputStates >> 8) & 0xFF;
    // payload[5] = inputStates & 0xFF;
    // payload[6] = (inputStates >> 8) & 0xFF;
    // payload[7] = node_online ? 1 : 0;
    // payload[8] = controllerType;

    memcpy(msg->dst, BROADCAST_MAC, 6);
    msg->type = MSG_EVENT;
    msg->msg_id = next_msg_id();
    msg->payload = payload;
    msg->payload_len = sizeof(device_payload_t);
    msg->require_ack = false;
    msg->waiting_task = NULL;
    
    addQueue(msg);
}

void sendNodeStatus(bool online) {
    node_online = online;
    queued_msg_t *msg = malloc(sizeof(queued_msg_t));
    if (!msg) return;
    
    size_t payload_size = 1; // online(1)
    uint8_t *payload = malloc(payload_size);
    payload[0] = online ? 1 : 0;
    
    memcpy(msg->dst, BROADCAST_MAC, 6);
    msg->type = MSG_STATUS;
    msg->msg_id = next_msg_id();
    msg->payload = payload;
    msg->payload_len = payload_size;    
    msg->require_ack = false;
    msg->waiting_task = NULL;
    
    addQueue(msg);
}

void sendFullConfig(uint8_t *dst, uint8_t *cfg, uint32_t size) {
    uint16_t chunk_size = 128;
    uint16_t crc = CRC16(cfg, size);

    cfg_start_t start = {
        .total_size = size,
        .crc16 = crc,
        .chunk_size = chunk_size,        
    };

    send_msg(dst, MSG_CFG_START, &start, sizeof(start));

    uint16_t seq = 0;
    for (uint32_t offset = 0; offset < size; offset += chunk_size) {
        //uint16_t len = MIN(chunk_size, size - offset);
        uint16_t len = chunk_size;
        if (len > size - offset)
            len = size - offset;

        uint8_t buf[sizeof(cfg_chunk_t) + 128];
        cfg_chunk_t *c = (cfg_chunk_t *)buf;

        c->seq = seq++;
        c->len = len;
        memcpy(c->data, cfg + offset, len);

        send_msg(dst, MSG_CFG_CHUNK, buf, sizeof(cfg_chunk_t) + len);
    }

    cfg_end_t end = {.last_seq = seq - 1};
    send_msg(dst, MSG_CFG_END, &end, sizeof(end));
}

static void discoveryTask(void *arg) {
    ESP_LOGI(TAG, "Starting RS485 discovery task...");
    
    while (1) {        
        queued_msg_t *msg = malloc(sizeof(queued_msg_t));    
        msg->type = MSG_HELLO;    
        memcpy(msg->dst, BROADCAST_MAC, 6);        
        //msg->payload = &controllerType;
        uint8_t *payload = malloc(6);
        if (payload) {
            uint16_t outputStates = getOutputs();
            uint16_t inputStates = getInputs();
            payload[0] = controllerType;
            payload[1] = outputStates & 0xFF;
            payload[2] = (outputStates >> 8) & 0xFF;
            payload[3] = inputStates & 0xFF;
            payload[4] = (inputStates >> 8) & 0xFF;
            payload[5] = node_online;
            msg->payload = payload;
        } // TODO : when it free?
        msg->payload_len = 6;
        msg->require_ack = false;
        msg->waiting_task = NULL;
        msg->msg_id = next_msg_id();    
        addQueue(msg);
        vTaskDelay(pdMS_TO_TICKS(1000 * 30));
    }
}

void registerBUSHandler(TBusEvent event) {
    busevent = event;
}

void rs485_init() {    
    getMac(self_node.mac);    
    
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_param_config(UART_PORT, &cfg);
    uint8_t rts_pin = UART_RTS;
    if (isOldControllerType())
        rts_pin = UART_RTS_OLD;
    ESP_LOGI(TAG, "Initing RS485. Mac is %02X%02X%02X%02X%02X%02X. RTS pin %d", 
             self_node.mac[0], self_node.mac[1], self_node.mac[2], 
             self_node.mac[3], self_node.mac[4], self_node.mac[5], rts_pin);
    
    uart_set_pin(UART_PORT, UART_TX, UART_RX, rts_pin, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT, UART_BUF, UART_BUF, 0, NULL, 0);
    uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX);  
    
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);    
    pending_mux = xSemaphoreCreateMutex();

    srand((unsigned int)esp_timer_get_time());
    msg_queue = xQueueCreate(QUEUE_SIZE, sizeof(queued_msg_t*));
    bus_mutex = xSemaphoreCreateMutex();    
    bus_access_sem = xSemaphoreCreateMutex();
    rx.buffer = NULL;
        
    xTaskCreate(senderTask, "sender_task", 4096, NULL, 6, NULL);    
    xTaskCreate(discoveryTask, "discovery", 2048, NULL, 5, NULL);
}
