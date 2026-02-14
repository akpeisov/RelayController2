#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "core.h"
#include "rs485.h"
#include "utils.h"
#include "hardware.h"
#include "config.h"
#include "core.h"
#include "storage.h"
#include "bconfig.h"
#include "network.h"
#include "webserver.h"

static const char *TAG = "MAIN";

void networkHandler(uint8_t event, uint32_t address) {
	static bool inited = false;
    ESP_LOGI(TAG, "event %d, address %d.%d.%d.%d", event, 
		     address & 0xFF, (address & 0xFFFF) >> 8, 
		     (address & 0xFFFFFF) >> 16, address >> 24);
    if (event == WIFI_AP_START) {
		ESP_LOGI(TAG, "Starting webserver for AP");
		initWebServer(0);
    } else if (!inited && (event == WIFI_CONNECTED || event == ETH_CONNECTED)) {
		inited = true;
		//otaCheck(getSettingsValueString("otaurl"), cert_pem_start);
		runWebServer();
        initWS();
    } else if (event == WIFI_DISCONNECTED || event == ETH_DISCONNECTED) {
        inited = false;
    }	
}

void app_main(void) {
    ESP_LOGI(TAG, "--= RelayController 2.0 =--");
    initMutex();    
    // init storage
    initStorage();
    // load config  
    loadConfig();    
    // init hardware    
    initHardware(); // controllerType is not identified for old devices
    loadBConfig();
        
    // get controller model
    rs485_init();
    startIOTask();
    setRGBFace("green");
    initNetwork(&networkHandler);
    initCore();
    // create core tasks
    // run webserver
    // run websocket	
    // init scheduler
}

// Chip is ESP32-D0WD-V3 (revision v3.0)   on new
// Chip is ESP32-D0WD-V3 (revision v3.0)   on old