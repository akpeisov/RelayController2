#include <sys/types.h>
#define BOUTPUTS 6
#define BINPUTS 4
#include "bconfig.h"


typedef void (*TIOEvent) (io_event_t data);
void registerIOHandler(TIOEvent event);

void initHardware();
char* getControllerTypeText(uint8_t type);
void setRGBFace(char* color);
void startIOTask();
void setOutput(action_cfg_t *act);
uint16_t getOutputs();
uint16_t getInputs();