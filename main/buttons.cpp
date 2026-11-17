// Buttons are active low with pull-ups (stock firmware button.py). Polled every 10 ms, a press is
// three low samples in a row after a high one. Presses outside the countdown are ignored.

#include "buttons.h"

#include <cstring>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "browser.h"

static const char* TAG = "buttons";

static const gpio_num_t USER_A = GPIO_NUM_39; // SW4
static const gpio_num_t USER_B = GPIO_NUM_16; // SW7
static const gpio_num_t PINS[] = {USER_A, USER_B};
static const char* const NAMES[] = {"User A", "User B"};
static const int         NPINS   = 2;

static void buttons_task(void*)
{
    int  low_count[NPINS] = {0, 0};
    bool pressed[NPINS]   = {false, false};
    for(;;)
    {
        for(int i = 0; i < NPINS; i++)
        {
            bool low = gpio_get_level(PINS[i]) == 0;
            low_count[i] = low ? low_count[i] + 1 : 0;
            if(low_count[i] == 3 && !pressed[i])
            {
                pressed[i] = true;
                if(strcmp(browser_state(), "wait") == 0)
                {
                    ESP_LOGI(TAG, "%s: %s tab", NAMES[i], i == 0 ? "previous" : "next");
                    browser_step(i == 0 ? -1 : 1);
                } else
                    ESP_LOGI(TAG, "%s ignored, state %s", NAMES[i], browser_state());
            }
            if(!low) pressed[i] = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void buttons_start()
{
    gpio_config_t io = {};
    io.pin_bit_mask  = (1ULL << USER_A) | (1ULL << USER_B);
    io.mode          = GPIO_MODE_INPUT;
    io.pull_up_en    = GPIO_PULLUP_ENABLE;
    io.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io.intr_type     = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    if(xTaskCreate(buttons_task, "buttons", 3072, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGE(TAG, "buttons task not created");
}
