#include "bconfig.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "i2cdev.h"
#include "pca9685.h"
#include "esp_log.h"
#include "pcf8574.h"
#include "pcf8563.h"
#include "utils.h"
#include "hardware.h"
#include "config.h"
#include "esp_timer.h"
#include "rs485.h"

#define SDA 32
#define SCL 33

#define IO_CLK   32
#define IO_LA595 33
#define IO_DI    34
#define IO_DO    12
#define IO_LA165 16
#define IO_EN    0
#define IO_REN   16
#define I2CPORT  0
#define MAXFREQ  1526
#define MAXPWM   4096
#define MINPWMLed 1500
#define MAXRELAYS 16
#define MAXLEDS  16

#define MAXTASKNAME 15
#define EVT_HW_UPDATE   (1 << 0)

static const char *TAG = "HARDWARE";

static EventGroupHandle_t hw_evt;
typedef enum {
    RELAY_OFF = 0,
    RELAY_ON_FULL,
    RELAY_ON_HOLD
} relay_state_t;
static relay_state_t relay_state[MAXRELAYS];
static uint16_t relay_target_bits;   
static uint16_t relay_current_bits; 
static portMUX_TYPE relay_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    uint16_t outputs;
    uint16_t inputs_leds;
    uint16_t outputs_leds;
} hw_state_t;
static hw_state_t hw_target;
static hw_state_t hw_current;
static portMUX_TYPE hw_mux = portMUX_INITIALIZER_UNLOCKED;

// static uint16_t relayValues = 0; // значения для реле
// static uint16_t relayPrepareBits = 0; // флаг снижения скважности
// static uint16_t relayBits = 0; // флаг снижения скважности
static bool i2c = false;
static bool clockPresent = false;
static uint16_t relPWM = 2000;
model_t controllerType = UNKNOWN;
static TIOEvent ioevent = NULL;
static uint8_t self_node[6];

typedef struct {
    i2c_dev_t device;
    uint8_t address;
} device_t;
static device_t devices[9];

typedef enum {
    I2CCLOCK = 0,
    I2CINPUTS,
    I2CFACEBUTTONS,
    I2CRELAYS,
    I2CFACELEDS1,
    // big
    I2CINPUTS2, // 8574 (relay board)
    I2CFACEBUTTONS2,// 8574 (face board)
    I2CFACELEDS2,// 9685 (face board)
    // medium
    I2C8// 8574 (face board)
} i2c_devices_t;

uint32_t serviceButtonsTime[16] = {0}; // in ms
uint32_t inputButtonsTime[32] = {0}; // in ms

bool isLocalNode(const node_uid_t *b) {
    return memcmp(&self_node, b->mac, sizeof(b->mac)) == 0;
}

void sendTo595(uint8_t *values, uint8_t count) {
    // Функция просто отправит данные в 595 
    uint8_t value;
    uint8_t mask;
    for (uint8_t c=0;c<count;c++) {
        value = values[c];
        mask = 0b10000000; // старший бит
        //mask = 0b00000001; // сначала младший бит
        for (uint8_t i=0;i<8;i++) {
            gpio_set_level(IO_CLK, 0);                        
            gpio_set_level(IO_DO, 0);
            if (value & mask) {                
                gpio_set_level(IO_DO, 1);
            }
            gpio_set_level(IO_CLK, 1);            
            mask >>= 1;
            //mask <<= 1;
        }
    }
    gpio_set_level(IO_CLK, 0);
    gpio_set_level(IO_LA595, 1);
    gpio_set_level(IO_LA595, 0);    
    // #ifdef IO_EN
    gpio_set_level(IO_EN, 0); // OE enable                                       
    // #endif
}

void readFrom165(uint8_t *values, uint8_t count)
{
    gpio_set_level(IO_LA165, 0);
    gpio_set_level(IO_LA165, 1);
    for (uint8_t d=count; d>0; d--) {        
        values[d-1] = 0;
        for (uint8_t i=0; i<8; ++i) {
            gpio_set_level(IO_CLK, 0);
            //values[d-1] |= gpio_get_level(IO_DI) << (7 - i);            
            values[d-1] |= gpio_get_level(IO_DI) << i;            
            gpio_set_level(IO_CLK, 1);
        }        
    }
}

void setGPIOOut(uint8_t gpio) {
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);

    // gpio_config_t cfg = {
    //     .pin_bit_mask = 1ULL << IO_EN,
    //     .mode = GPIO_MODE_INPUT_OUTPUT,//GPIO_MODE_OUTPUT,
    //     .pull_up_en = GPIO_PULLUP_DISABLE,
    //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
    //     .intr_type = GPIO_INTR_DISABLE
    // };
    // gpio_config(&cfg);   
    //gpio_dump_io_configuration(stdout, (1ULL << IO_EN)); 
}

void setGPIOIn(uint8_t gpio) {
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);    
}

void i2cScan(uint8_t found[], uint8_t *size) {
    i2c_dev_t dev = { 0 };
    dev.cfg.sda_io_num = SDA;
    dev.cfg.scl_io_num = SCL;
    dev.cfg.master.clk_speed = 100000;
    // uint8_t found[20];
    // uint8_t fCnt = 0;
    esp_err_t res;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    printf("00:         ");
    for (uint8_t addr = 3; addr < 0x78; addr++)
    {
        if (addr % 16 == 0)
            printf("\n%.2x:", addr);

        dev.addr = addr;
        res = i2c_dev_probe(&dev, I2C_DEV_WRITE);

        if (res == 0) {
            found[(*size)++] = addr;
            printf(" %.2x", addr);
        }
        else
            printf(" --");
    }
    printf("\n");
}

bool isMatched(uint8_t *array, uint8_t arraySize, uint8_t *pattern, uint8_t patternSize) {
    for (uint8_t i = 0; i < patternSize; i++) {
        bool found = false;
        for (uint8_t j = 0; j < arraySize; j++) {
            if (array[j] == pattern[i]) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool isInArray(uint8_t *array, uint8_t arraySize, uint8_t element) {
    for (uint8_t i = 0; i < arraySize; i++) {
        if (array[i] == element)
            return true;        
    }
    return false;
}

esp_err_t initPCA9685hw(i2c_dev_t dev, bool setValues) {
    esp_err_t err = ESP_FAIL;
    uint16_t freq = MAXFREQ;
    uint16_t nFreq = getConfigValueNumber("hw/freq");    
    if (nFreq > 0) //  TODO : && nFreq < MAXFREQ
        freq = nFreq;
    uint16_t nRelPWM = getConfigValueNumber("hw/pwm");    
    if (nRelPWM > 0) {
        relPWM = nRelPWM;
    }
    //if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {        
        err = pca9685_init(&dev);
        if (err == ESP_OK) {
            ESP_ERROR_CHECK(pca9685_restart(&dev));
            pca9685_set_pwm_frequency(&dev, freq);
            // set all to off
            if (setValues) {
                for (int i=0;i<16;i++) {
                    pca9685_set_pwm_value(&dev, i, 0);    	
                }
            }
        }        
    //    xSemaphoreGive(xMutex); 
    //}   
    return err;
    // err = pca9685_set_prescaler(&dev_out1, 10);
    // if (err != ESP_OK) {
    // 	ESP_LOGE(TAG, "Can't set prescaler");
    // 	return err;
    // }
}

esp_err_t initPCA9685(uint8_t devNum, uint8_t *foundDevices, uint8_t devicesCount) {
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (isInArray(foundDevices, devicesCount, devices[devNum].address)) {
        pca9685_init_desc(&devices[devNum].device, devices[devNum].address, I2CPORT, SDA, SCL);
        err = initPCA9685hw(devices[devNum].device, true);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "PCA9685 with address 0x%x inited OK", devices[devNum].address);
        else
            ESP_LOGE(TAG, "Can't init PCA9685 address 0x%x", devices[devNum].address);
    }
    return err;
}

void setI2COut(uint8_t adr, uint8_t num, uint16_t value) {
    if (!i2c) return;
//return;    
    if (value > MAXPWM) {
        value = MAXPWM;
    }
    pca9685_set_pwm_value(&devices[adr].device, num, value);    
}

static void updateIndicators(const hw_state_t *s) {
    uint8_t values[6];
    uint16_t i2cValues[MAXLEDS];

    // for old controllers relay also here
    switch (controllerType) {
        case RCV1S:
            values[1] = revByte(s->outputs << 2);
            values[0] = ((s->inputs_leds & 0xF0) >> 4)
                        | (s->outputs_leds << 4);
            values[0] = revByte(values[0]);
            sendTo595(values, 2);
            break;

        case RCV1B:
            values[5] = s->outputs >> 8;
            values[4] = s->outputs;
            values[3] = s->inputs_leds;
            values[2] = s->inputs_leds >> 8;
            values[1] = s->outputs_leds;
            values[0] = s->outputs_leds >> 8;
            sendTo595(values, 6);
            break;

        case RCV2S:
            //updatePCAFaceLeds1(s);
            for (int i = 0; i < 6; i++) {
                i2cValues[i] = (s->inputs_leds & (1 << i)) ? 0 : MAXPWM;
            }
            for (int i = 0; i < 4; i++) {
                i2cValues[i + 6] = (s->outputs_leds & (1 << i)) ? 0 : MAXPWM;
            }
            pca9685_set_pwm_values(&devices[I2CFACELEDS1].device,
                                0, 10, i2cValues);
            break;

        case RCV2B:
            // inputs leds
            for (uint8_t i=0;i<MAXLEDS;i++) {
                i2cValues[i] = (s->inputs_leds & (0x1 << i)) > 0 ? 0 : MAXPWM;            
            }
            pca9685_set_pwm_values(&devices[I2CFACELEDS1].device, 0, MAXLEDS, i2cValues);
            // relay leds
            for (uint8_t i=0; i<12; i++) {
                i2cValues[i] = (s->outputs_leds & (0x1 << i) ) > 0 ? 0 : MAXPWM;            
            }
            pca9685_set_pwm_values(&devices[I2CFACELEDS2].device, 0, 12, i2cValues);
            break;

        default:
            break;
    }
}

void relayTask(void *pvParameter) {
    uint16_t pwm[MAXRELAYS];
    memset(pwm, 0, sizeof(pwm));

    while (1) {
        taskENTER_CRITICAL(&relay_mux);
        uint16_t target = relay_target_bits;
        taskEXIT_CRITICAL(&relay_mux);

        for (uint8_t i = 0; i < MAXRELAYS; i++) {
            bool want_on = testbit(target, i);            
            switch (relay_state[i]) {
                case RELAY_OFF:
                    if (want_on) {
                        pwm[i] = MAXPWM;
                        relay_state[i] = RELAY_ON_FULL;
                        setbit(relay_current_bits, i);
                    }
                    break;
                case RELAY_ON_FULL:
                    pwm[i] = relPWM;
                    relay_state[i] = RELAY_ON_HOLD;
                    break;
                case RELAY_ON_HOLD:
                    if (!want_on) {
                        pwm[i] = 0;
                        relay_state[i] = RELAY_OFF;
                        clrbit(relay_current_bits, i);
                    }
                    break;
            }
        }        
        pca9685_set_pwm_values(&devices[I2CRELAYS].device,
                                0, MAXRELAYS, pwm);
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void hwTask(void *arg) {
    while (1) {
        xEventGroupWaitBits(hw_evt,
                            EVT_HW_UPDATE,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        taskENTER_CRITICAL(&hw_mux);
        hw_state_t target = hw_target;
        taskEXIT_CRITICAL(&hw_mux);

        // relays
        if (target.outputs != hw_current.outputs) {
            taskENTER_CRITICAL(&relay_mux);
            relay_target_bits = target.outputs;
            taskEXIT_CRITICAL(&relay_mux);
            hw_current.outputs = target.outputs;
        }

        // leds
        updateIndicators(&target);

        hw_current.inputs_leds  = target.inputs_leds;
        hw_current.outputs_leds = target.outputs_leds;
    }
}


esp_err_t initI2Cdevices(model_t *ctrlType) {    
    esp_err_t err = ESP_FAIL;  
    *ctrlType = UNKNOWN; 

    i2cdev_init();
    uint8_t foundDevices[20];
    uint8_t devicesCount = 0;
    i2cScan(foundDevices, &devicesCount);
    
    uint8_t aRCV2S[] = {0x20, 0x21, 0x40, 0x41};
    uint8_t aRCV2M[] = {0x20, 0x21, 0x22, 0x23, 0x27, 0x40, 0x41, 0x42};
    uint8_t aRCV2B[] = {0x20, 0x21, 0x22, 0x23, 0x40, 0x41, 0x42};
        
    if (isMatched(foundDevices, devicesCount, aRCV2M, sizeof(aRCV2M)/sizeof(uint8_t)))
        *ctrlType = RCV2M;
    else if (isMatched(foundDevices, devicesCount, aRCV2B, sizeof(aRCV2B)/sizeof(uint8_t)))
        *ctrlType = RCV2B;
    else if (isMatched(foundDevices, devicesCount, aRCV2S, sizeof(aRCV2S)/sizeof(uint8_t)))
        *ctrlType = RCV2S;    

    if (!devicesCount || *ctrlType == UNKNOWN) {
        ESP_LOGW(TAG, "No i2c device found or controllerType is unknown");
        return err;
    }
    
    /*
    3 RELAY_1

    // должна вернуть успех если есть минимальный набор
	RCV2S
	0x51 - clock
	0x20 - 8574 (relay)
    0x21 - 8574 (face)
	0x40 - 9685 (relay) 15 bit to 7 bit 8574
    0x41 - 9685 (face)	15 bit to 7 bit 8574
    ---
    RCV2B
	0x51 - clock
	0x20 - 8574 (relay)
    0x21 - 8574 (face)
    0x22 - 8574 (relay)
    0x23 - 8574 (face)
	0x40 - 9685 (relay) 
    0x41 - 9685 (face)	
    0x42 - 9685 (face)	
    ---
    RCV2M
    0x20 - 8574 (relay)
    0x21 - 8574 (face)
    0x22 - 8574 (relay)
    0x23 - 8574 (face)
    0x27 - 8574 (face rgb)  
    0x40 - 9685 (relay)  
    0x41 - 9685 (face)  
    0x42 - 9685 (face)   

    0x25 - pca9555 (face1)
    0x26 - pca9555 (face2)
    ---
    0x18 - i2c-ow DS2484
	*/
    for (uint8_t i=0; i<9;i++)
        memset(&devices[i].device, 0, sizeof(i2c_dev_t));
    // addresses
    devices[0].address = 0x51; // 8563 (clock)
    devices[1].address = 0x20; // 8574 (relay board)
    devices[2].address = 0x21; // 8574 (face board)
    devices[3].address = 0x40; // 9685 (relay board)
    devices[4].address = 0x41; // 9685 (face board)
    // big
    devices[5].address = 0x22; // 8574 (relay board)
    devices[6].address = 0x23; // 8574 (face board)
    devices[7].address = 0x42; // 9685 (face board)
    // medium
    devices[8].address = 0x27; // 8574 (face board)

    // у маленького и на плате реле и на плате индикации 15 bit 9685 соединен с 7 bit 8574

    // TODO : часы сделать
    if (isInArray(foundDevices, devicesCount, devices[0].address)) {
        pcf8563_init_desc(&devices[0].device, 0, SDA, SCL);        
        struct tm timeinfo;
        bool valid;
        if (pcf8563_get_time(&devices[0].device, &timeinfo, &valid) == ESP_OK) {
            clockPresent = true;
            ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02d, %s\n", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                     timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, valid ? "VALID" : "NOT VALID");
            if (valid && (timeinfo.tm_year + 1900 >= 2025)) {
                time_t now;
                //struct tm timeinfo;
                time(&now);
                
                //setenv("TZ", getSettingsValueString("ntpTZ"), 1);
                //tzset();
                localtime_r(&now, &timeinfo);
                ESP_LOGI(TAG, "Clock set");
            }
            // if (!valid) {
            //         struct tm time = {
            //         .tm_year = 120, // years since 1900
            //         .tm_mon  = 3,   // months since January
            //         .tm_mday = 3,
            //         .tm_hour = 12,
            //         .tm_min  = 35,
            //         .tm_sec  = 10,
            //         .tm_wday = 0    // days since Sunday
            //     };
            //     ESP_ERROR_CHECK(pcf8563_set_time(&dev, &time));

            // }
        } else {
            ESP_LOGI(TAG, "No clock chip present");
        }
    }

    pcf8574_init_desc(&devices[1].device, devices[1].address, I2CPORT, SDA, SCL);
    pcf8574_init_desc(&devices[2].device, devices[2].address, I2CPORT, SDA, SCL);
    pcf8574_init_desc(&devices[5].device, devices[5].address, I2CPORT, SDA, SCL);
    pcf8574_init_desc(&devices[6].device, devices[6].address, I2CPORT, SDA, SCL);
    //pcf8574_init_desc(&devices[8].device, devices[8].address, I2CPORT, SDA, SCL);

    //3,4,7    
    initPCA9685(4, foundDevices, devicesCount);
    initPCA9685(7, foundDevices, devicesCount);
    initPCA9685(3, foundDevices, devicesCount);
 
    
    setRGBFace("yellow");  
    gpio_set_level(IO_REN, 0);
    ESP_LOGI("GPIO0 before", "level=%d", gpio_get_level(IO_EN));
    gpio_set_level(IO_EN, 0);
    ESP_LOGI("GPIO0 after", "level=%d", gpio_get_level(IO_EN));
gpio_dump_io_configuration(stdout, (1ULL << IO_EN));
    
    xTaskCreate(&relayTask, "relayTask", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void determinateControllerType() {
    if (controllerType == UNKNOWN) {
        char *cType = getConfigValueString("controllerType");
        ESP_LOGI(TAG, "getConfigValueString %s", cType == NULL ? "NULL" : cType);
        if (cType != NULL) {
            if (!strcmp(cType, "small") || !strcmp(cType, "RCV1S")) {
                controllerType = RCV1S;
            } else if (!strcmp(cType, "big") || !strcmp(cType, "RCV1B")) {
                controllerType = RCV1B;
            }            
        } else {
            controllerType = RCV1S; // by default
        }
        // write to config
        setConfigValueString("controllerType", controllerTypesData[controllerType].name);
        //ESP_LOGI(TAG, "Controllertype is %s", controllerTypesData[controllerType].name);
    }        
}

uint8_t readFrom8574(uint8_t adr) {
    if (!i2c) return 0;
//return 0;    
    uint8_t inputs;
    //adr 1,2,5,6
    pcf8574_port_read(&devices[adr].device, &inputs);
    return inputs;
}

void setRGBFaceValue(uint16_t r, uint16_t g, uint16_t b) {
    if (!i2c) return;
    uint8_t dev = 0;
    uint8_t rNum = 0, gNum = 0, bNum = 0;  
    if (controllerType == RCV2S) {
        dev = 4;
        rNum = 10;
        gNum = 11;
        bNum = 12;
    } else if (controllerType == RCV2B) {
        dev = 7;
        rNum = 13;
        gNum = 14;
        bNum = 15;    
    }
    if (!dev) return;
    setI2COut(dev, rNum, b);
    setI2COut(dev, gNum, g);
    setI2COut(dev, bNum, r);
}

void setRGBFace(char *color) {    
    if (!strcmp(color, "red")) {            
        setRGBFaceValue(3000, MAXPWM, MAXPWM);        
    } else if (!strcmp(color, "yellow")) {            
        setRGBFaceValue(2000, 3400, MAXPWM);                
    } else if (!strcmp(color, "green")) {            
        setRGBFaceValue(MAXPWM, 3000, MAXPWM);                        
    } else {
        setRGBFaceValue(2000, 2000, 2000);                                
    }    
}
/*
void setRGBFace(char* color) {
    if (!i2c) return;
    uint16_t min = 1500;  
    uint8_t dev = 0;
    uint8_t r = 0, g = 0, b = 0;  
    if (controllerType == RCV2S) {
        dev = 4;
        r = 10;
        g = 11;
        b = 12;
    } else if (controllerType == RCV2B) {
        dev = 7;
        r = 13;
        g = 14;
        b = 15;    
    }

    if (!strcmp(color, "red")) {            
        setI2COut(dev, r, MAXPWM);        
        setI2COut(dev, g, MAXPWM);        
        setI2COut(dev, b, min);                    
    } else if (!strcmp(color, "yellow")) {            
        setI2COut(dev, r, MAXPWM);        
        setI2COut(dev, g, min);        
        setI2COut(dev, b, min);                    
    } else if (!strcmp(color, "green")) {            
        setI2COut(dev, r, MAXPWM);
        setI2COut(dev, g, min);        
        setI2COut(dev, b, MAXPWM);                        
    } else {
        setI2COut(dev, r, MAXPWM);        
        setI2COut(dev, g, MAXPWM);        
        setI2COut(dev, b, MAXPWM);
    }    
}*/

esp_err_t setClock() {
    if (!clockPresent) return ESP_ERR_NOT_FOUND;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "setClock is %s", asctime(&timeinfo));    
    return pcf8563_set_time(&devices[0].device, &timeinfo);
}

esp_err_t getClock(struct tm time) {
    bool valid;
    esp_err_t ret = pcf8563_get_time(&devices[0].device, &time, &valid);     
    if (!valid)
        return ESP_FAIL;
    return ret;
}

void updateStateHW(uint16_t outputs,
                   uint16_t inputsLeds,
                   uint16_t outputsLeds) {
    taskENTER_CRITICAL(&hw_mux);
    hw_target.outputs      = outputs;
    hw_target.inputs_leds  = inputsLeds;
    hw_target.outputs_leds = outputsLeds;
    taskEXIT_CRITICAL(&hw_mux);

    xEventGroupSetBits(hw_evt, EVT_HW_UPDATE);
}

// void updateStateHW(uint16_t outputs, uint16_t inputsLeds, uint16_t outputsLeds) {
//     // обновление состояния выходов, индикации
//     // outputs - выходы реле, inputsLeds - индикация входов, outputsLeds - индикация выходов
//     uint8_t values[6] = {0};
//     uint16_t i2cValues[MAXLEDS];

//     switch (controllerType) {
//         case RCV1S:
//             values[1] = revByte(outputs << 2);    // TODO : проверить не перепутал ли я выходы и индикацию    
//             values[0] = (inputsLeds & 0xF0) >> 4;        
//             values[0] |= outputsLeds << 4;
//             values[0] = revByte(values[0]);
//             sendTo595(values, 2);   
//             break;
//         case RCV1B:
//             values[5] = outputs >> 8;       // TODO : проверить не перепутал ли я выходы и индикацию    
//             values[4] = outputs;
//             values[3] = inputsLeds;
//             values[2] = inputsLeds >> 8;
//             values[1] = outputsLeds;
//             values[0] = outputsLeds >> 8;
//             sendTo595(values, 6);      
//             break;
//         case RCV2S:
//             setRelayValues(outputs);
//             // inputs leds
//             for (uint8_t i=0; i<6; i++) {
//                 i2cValues[i] = (inputsLeds & (0x1 << i)) > 0 ? 0 : MAXPWM;                                
//             }
//             // relay leds
//             for (uint8_t i=0; i<4; i++) {
//                 i2cValues[i+6] = (outputsLeds & (0x1 << i)) > 0 ? 0 : MAXPWM;                
//             }            
//             pca9685_set_pwm_values(&devices[I2CFACELEDS1].device, 0, 10, i2cValues);
//             break;
//         case RCV2B:
//             setRelayValues(outputs);        
//             // inputs leds
//             for (uint8_t i=0;i<MAXLEDS;i++) {
//                 i2cValues[i] = (inputsLeds & (0x1 << i)) > 0 ? 0 : MAXPWM;            
//             }
//             pca9685_set_pwm_values(&devices[I2CFACELEDS1].device, 0, MAXLEDS, i2cValues);
//             // relay leds
//             for (uint8_t i=0; i<12; i++) {
//                 i2cValues[i] = (outputsLeds & (0x1 << i) ) > 0 ? 0 : MAXPWM;            
//             }
//             pca9685_set_pwm_values(&devices[I2CFACELEDS2].device, 0, 12, i2cValues);
//             break;
//         case RCV2M:
//             setRelayValues(outputs);        
//             // TODO : сделать индикацию
//             break;    
//         default:            
//     }    
// }

void readInputs(uint8_t *values, uint8_t count) {
    // чтение входов
    //ESP_LOGI(TAG, "readInputs cnt %d type %d", count, controllerType);
    if (controllerType == RCV1S || controllerType == RCV1B) {    
        readFrom165(values, count);        
    } else if (controllerType == RCV2S || controllerType == RCV2B || controllerType == RCV2M) {
        if (count == 2) {
            values[0] = readFrom8574(I2CINPUTS) & 0x3F;
            values[1] = readFrom8574(I2CFACEBUTTONS) & 0x0F;
            //ESP_LOGI(TAG, "Inputs2 %d %d", values[0], values[1]);
        } else if (count == 4) {
            values[0] = readFrom8574(I2CINPUTS); // relay board inputs
            values[1] = readFrom8574(I2CINPUTS2); // relay board inputs
            values[2] = readFrom8574(I2CFACEBUTTONS); // face
            values[3] = readFrom8574(I2CFACEBUTTONS2); // face
        }
    } else {
        // заглушка для неизвестных типов
        for (uint8_t i=0;i<count;i++)
            values[i] = 0xFF;
    }   
}

uint16_t readServiceButtons() { 
    // чтение сервисных кнопок (только при включении)
    // TODO : должен читать только сервисные кнопки
    // вернет 2 байта с соответствующими значениями, справа 0 бит. 1 кнопка нажата, 0 нет
    uint8_t inputs[BINPUTS];
    uint16_t buttons = 0x0;
    if (controllerType == RCV1S) {    
        readFrom165(inputs, 2);
        //ESP_LOGI(TAG, "readFrom165 %d %d", inputs[0], inputs[1]);
        buttons = inputs[0]>>4;
        buttons |= 0xFFF0;
        buttons = ~buttons;        
    } else if (controllerType == RCV1B) {
        readFrom165(inputs, 4);
        buttons = inputs[0]; // первые 8 левых кнопок байт 0
        buttons<<=2;
        buttons |= (inputs[1] & 0xC0) >> 6; // две кнопки справа. маска т.к. входы в воздухе
        buttons |= 0xFFC0; // чтобы заполнить 6 первых бит единицами после сдвига на 6
        buttons = ~buttons;
    } else if (controllerType == RCV2B) {
        inputs[0] = readFrom8574(2); // face
        inputs[1] = readFrom8574(6); // face
        buttons = inputs[1];
        buttons<<=8;
        buttons |= inputs[0];
        buttons = ~buttons;          
    } else {
        // TODO : добавить чтение значений кнопок для новых контроллеров
    }
    ESP_LOGI(TAG, "readServiceButtons inputs 0x%02x%02x buttons 0x%02x", inputs[1], inputs[0], buttons);    
    return buttons;
}

char* getControllerTypeText(uint8_t type) {
    switch (type) {
        case RCV1S:
            return "RCV1S";//"small";
        case RCV1B:    
            return "RCV1B";// "big";
        case RCV2S:    
            return "RCV2S";
        case RCV2M:    
            return "RCV2M";
        case RCV2B:    
            return "RCV2B";        
    }
    return "UNKNOWN";
}

uint16_t getOutputs() {
    uint16_t outputs = 0;    
    uint8_t outputsCount = getConfigOutputsCount();
    if (outputsCount > 4)
        ESP_LOGW(TAG, "Too many outputs in config %d, max is 4", outputsCount);
    //outputsCount &= 0x10;
    for (uint8_t i = 0; i < outputsCount; i++) {
        //const output_cfg_t *o = &gCfg->outputs[i];
        const output_cfg_t *o = getConfigOutput(i);// &cfg_outputs(gCfg)[i];
        if (o->is_on) 
            setbit(outputs, o->id & 0xFFFF);
        else
            clrbit(outputs, o->id & 0xFFFF);        
    }
    return outputs;
}

uint16_t getInputs() {
    uint16_t inputs= 0;
    // if (!gCfg) return 0;
    uint8_t inputsCount = getConfigInputsCount();
    for (uint8_t i = 0; i < inputsCount; i++) {
        //const input_cfg_t *in = &gCfg->inputs[i];
        // const input_cfg_t *in = &cfg_inputs(gCfg)[i]; 
        const input_cfg_t *in = getConfigInput(i);
        if (in->is_on) 
            setbit(inputs, in->id & 0xFFFF);
        else
            clrbit(inputs, in->id & 0xFFFF);        
    }
    return inputs;
}

void setOutput(action_cfg_t *act) {
    ESP_LOGI(TAG, "setOutput action %d output %d node %s", 
        act->action, act->output_id, strNode(&act->target_node));
    
    if (act->action == ACT_ALLOFF) {
        setAllOff();  
        // TODO : inform backend about alloff
        io_event_t e;
        e.io_type = IO_OUT;
        //e.io_id = act->output_id; // ?????
        e.state = false;
        e.timer = 0;
        e.node = act->target_node;        
        e.outputsStates = getOutputs();
        e.inputStates = getInputs();
        if (ioevent) ioevent(e);
        sendNodeAction(act);    
        return;
    }


    if (isLocalNode(&act->target_node)) {        
        output_cfg_t *output = findOutput(act->output_id);
        if (output == NULL) {
            ESP_LOGE(TAG, "Output %d not found", act->output_id);
            return;
        }
        switch (act->action) {
            case ACT_ON:
                output->is_on = true;
                break;
            case ACT_OFF:
                output->is_on = false;
                break;
            case ACT_TOGGLE:
                output->is_on = !output->is_on;
                break;
            case ACT_ALLOFF:
                // TODO : realize it    
                     
                break;
            default:
                break;            
        }      
        
        // publish to core
        io_event_t e;
        e.io_type = IO_OUT;
        e.io_id = act->output_id;
        e.state = output->is_on;
        e.timer = output->timer;        
        e.node = act->target_node;
        e.outputsStates = getOutputs();
        e.inputStates = getInputs();
        if (ioevent) ioevent(e);

        // sending current state to bus
        node_io_event_t ioEvnt;
        ioEvnt.io_type = IO_OUT;
        ioEvnt.io_id = output->id;
        ioEvnt.state = output->is_on;       
        sendNodeEvent(ioEvnt, getInputs(), getOutputs());
    } else {
        // remote node
        // TODO : продумать как послать состояние выхода удаленной ноды
        sendNodeAction(act);
    }    
}

void actionsTask(void *pvParameter) {
    input_event_cfg_t *evt = (input_event_cfg_t*)pvParameter;
    uint16_t durationCnt = 0;
    uint8_t i = 0;
    while (1) {
        if (i >= evt->actions_count)
            break;

        if (durationCnt) {
            durationCnt--;            
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            //action_cfg_t *act = &cfg_actions(gCfg)[evt->actions_offset + i];            
            action_cfg_t *act = getConfigAction(evt->actions_offset + i);
            
            //ESP_LOGI(TAG, "process action %d output %d", act->action, act->output_id);            
            // TODO : print node
            //ESP_LOGI(TAG, "process action %d on output %d node %s", act->action, act->output_id, strNode(&act->target_node));
            if (act != NULL) {
                if (act->action == ACT_WAIT) {
                    durationCnt = act->duration_sec;
                } else {
                    setOutput(act);                
                }
            }
        }            
        i++;
    }
    vTaskDelete(NULL);
}

uint8_t correctInput(uint8_t pInput) {
    uint8_t cInput = 255;
    switch (controllerType) {
        case RCV1S:
            if ((pInput >= 4) && (pInput <= 7)) {
                cInput = pInput+12;
            } else if ((pInput >= 12) && (pInput <= 15)) {
                cInput = pInput-12;
            }
            break;
        case RCV1B:
            if ((pInput >= 16) && (pInput <= 31)) {
                cInput = pInput-16;
            } else if (pInput <= 7) { 
                cInput = pInput+18;
            } else if ((pInput >= 14) && (pInput <= 15)) {
                cInput = pInput+2;
            }
            break;
        case RCV2S:
            if (pInput >=8)
                cInput = pInput+8;
            else
                cInput = pInput;
            break;
        case RCV2M:
                // TODO: inplement
            break;
        case RCV2B:
                cInput = pInput;
            break;
        default:
            cInput = 255;
    }
    return cInput;
}

void processInput(uint8_t input_id, uint8_t event_id) {
    // find config entity    
    uint8_t inputId = correctInput(input_id);
    ESP_LOGI(TAG, "processInput. orig %d, corrected %d", input_id, inputId);
    input_cfg_t *input = findInput(inputId);
    if (input == NULL) {
        ESP_LOGE(TAG, "No input found in config");
        return;
    }
    event_type_t event = EVT_UNKNOWN;

    switch (input->type) {
        case INPUT_BTN:
            if (event_id == 1) {
                // button is pressed, save timer
                inputButtonsTime[inputId] = esp_timer_get_time() / 1000 & 0xFFFFFFFF;					                
            } else {
                // button is released, get time
                if ((esp_timer_get_time() / 1000 & 0xFFFFFFFF) - inputButtonsTime[inputId] < 1000) {
                    // click
                    event = EVT_TOGGLE;
                } else {
                    // long press
                    event = EVT_LONGPRESS;
                }					
                ESP_LOGI(TAG, "Button %d %s", inputId, eventStr(event)); 
            }
            break;
        case INPUT_SWITCH_INV:
            // TODO : переключатель со счетчиком. для Саши делали. 
            event = EVT_TOGGLE;        
            break;
        case INPUT_SWITCH:
            if (event_id == 1)
                event = EVT_ON;
            else
                event = EVT_OFF;
            break;
        default:
            event = EVT_UNKNOWN;
    }
    
    // process events
    if (event != EVT_UNKNOWN) {        
        // publish core only for switches and inverted switches
        if (input->type == INPUT_SWITCH || input->type == INPUT_SWITCH_INV) {
            // sending current state to bus
            node_io_event_t ioEvnt;
            ioEvnt.io_type = 1;
            ioEvnt.io_id = inputId;
            ioEvnt.state = event_id;        
            sendNodeEvent(ioEvnt, getInputs(), getOutputs());
            
            // publish to core
            io_event_t e;
            e.io_type = 1;
            e.io_id = inputId;
            e.state = event_id;            
            memcpy(e.node.mac, self_node, 6);
            if (ioevent) ioevent(e);
        }
        
        // const io_cfg_t *cfg = gCfg;    
        const input_event_cfg_t *evt = NULL;
        for (uint8_t i = 0; i < input->events_count; i++) {
            // const input_event_cfg_t *e =
            //     &cfg->events[input->events_offset + i];
            // const input_event_cfg_t *e =
            //     &cfg_events(gCfg)[input->events_offset + i];                    
            const input_event_cfg_t *e = getConfigEvent(input->events_offset + i);    
            if (e->event == event) {
                evt = e;
                break;
            }
        }

        if(evt == NULL) {
            ESP_LOGE(TAG, "No event in config for input %d, event %s", inputId, eventStr(event));
            return;
        }

        ESP_LOGI(TAG, "Input %d event %s → %d actions", inputId, eventStr(event), evt->actions_count);

        if (evt != NULL && evt->actions_count > 0) {
            char taskName[MAXTASKNAME];
            snprintf(taskName, MAXTASKNAME, "actiontask_%d", inputId);
            TaskHandle_t xHandle = xTaskGetHandle(taskName);
            if (xHandle != NULL) {
                // если по данному входу уже работает таск то удалить его
                vTaskDelete(xHandle);
            }    
            xTaskCreate(&actionsTask, taskName, 4096, (void *)evt, 5, NULL);
        }
    }
}

void outputTimer() {        
    // if (!gCfg) return;    
    // const io_cfg_t *cfg = gCfg;  
    uint8_t outputsCount = getConfigOutputsCount();  
    for (uint8_t i = 0; i < outputsCount; i++) {
        //output_cfg_t *o = &cfg->outputs[i];        
        //output_cfg_t *o = &cfg_outputs(gCfg)[i];                
        output_cfg_t *o = getConfigOutput(i);
        if (o->type == OUTPUT_TIMED) {
            if (--o->timer == 0) {
                if (o->is_on) {                
                    o->is_on = false;
                    o->timer = o->timed.off_sec;
                } else {
                    o->is_on = true;
                    o->timer = o->timed.on_sec;
                }
            }
        }
    }
}

void IOTask() {
    ESP_LOGI(TAG, "Starting IO task");    
	uint8_t cnt_timer = 0;
    uint8_t sch_timer = 0;
	uint8_t inputsNew[BINPUTS];    
    uint8_t inputsOld[BINPUTS];// = {0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t diff[BINPUTS];
	uint8_t inputsCnt = 2; // for RCV1S, RCV2S
	if ((controllerType == RCV1B) || (controllerType == RCV2B)) {
		inputsCnt = 4;
    } else if ((controllerType == RCV1S) || (controllerType == RCV2S)) {
        inputsCnt = 2;
    }
        
    readInputs(inputsOld, inputsCnt); // for ignore startup values   

    uint16_t oldOuts = 0;
	while (1) {
		// опрос изменения входов и кнопок
        if (xSemaphoreTake(gMutex, portMAX_DELAY) == pdTRUE) {
			// анализ входов
			readInputs(inputsNew, inputsCnt);
            for (uint8_t i=0; i<inputsCnt; i++) {
                diff[i] = inputsNew[i] ^ inputsOld[i]; // единички где что-то поменялось
                if (diff[i] > 0) {                                        
                    for (uint8_t b=0; b<8; b++) {
                        if (diff[i] >> b & 0x01) {                        
                            // ESP_LOGI(TAG, "i %d b %d", i, b);
                            processInput(i*8 + b, inputsOld[i] >> b & 0x01);
                        }
                    }
                    inputsOld[i] = inputsNew[i];                    
                }            
            }

            //outputsTimerShot();
            if (++cnt_timer >= 10) {
                cnt_timer = 0;
                outputTimer();

                // if (++sch_timer >= 60) {
                //     sch_timer = 0;
                //     processScheduler();
                //     sendInfo();
                // }
            }

            uint16_t outs = getOutputs();
            uint16_t ins = getInputs();
            if (oldOuts != outs) {
                ESP_LOGW(TAG, "New outputs %d, old outputs %d", outs, oldOuts);
                oldOuts = outs;                                
            }
            updateStateHW(outs, ins, outs);            
            xSemaphoreGive(gMutex);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void initHardware() {
    ESP_LOGI(TAG, "Init hardware");
    getMac(self_node);
    setGPIOOut(IO_EN);
    setGPIOOut(IO_REN);
    gpio_set_level(IO_EN, 1);    
    gpio_set_level(IO_REN, 1);     
    hw_evt = xEventGroupCreate();
    if (initI2Cdevices(&controllerType) == ESP_OK) {
        i2c = true;
        ESP_LOGI(TAG, "I2C inited.");
    } else {
        ESP_LOGI(TAG, "I2C not inited. Probably old controller 1.0");
        setGPIOOut(IO_CLK);
        setGPIOOut(IO_LA595);
        setGPIOOut(IO_LA165);
        setGPIOOut(IO_DO);
        setGPIOIn(IO_DI);        
        // для старых контроллеров нужно определить тип контроллера из конфигурации
        controllerType = UNKNOWN;
        determinateControllerType();
    }    
    ESP_LOGI(TAG, "Controller type is %s", getControllerTypeText(controllerType));
    
    xTaskCreate(&hwTask, "hwTask", 4096, NULL, 5, NULL);    
}

// void testTask() {
//     vTaskDelay(4000 / portTICK_PERIOD_MS);
//     while (1) {
//         // action_cfg_t ac;
//         // ac.action = ACT_TOGGLE;
//         // ac.output_id = 0;
//         // #define TRG  ((node_uid_t){ .mac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff} })
//         // ac.target_node = TRG;
//         // sendNodeAction(&ac);
//         input_event_t ev;
//         ev.input_id = 0;
//         ev.event = EVT_TOGGLE;
//         sendNodeEvent(ev);
//         vTaskDelay(10000 / portTICK_PERIOD_MS);
//     }    
// }

void registerIOHandler(TIOEvent event) {
    ioevent = event;
}

void startIOTask() {
    xTaskCreate(&IOTask, "IOTask", 4096, NULL, 5, NULL);    
    //xTaskCreate(&testTask, "IOTask", 4096, NULL, 5, NULL);    
}