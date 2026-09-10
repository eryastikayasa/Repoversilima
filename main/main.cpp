#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    printf("Repo 5 - clean foundation\n");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
