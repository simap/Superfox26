// Buttons are active low with pull-ups (stock firmware button.py). Polled every 10 ms, a press is
// three low samples in a row after a high one. A button that is low when the task starts counts as
// held, so it needs a high sample first. Presses outside the countdown are ignored.

#include "buttons.h"

#include <cstring>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "browser.h"
#include "power_logic.h"

static const char* TAG = "buttons";

struct button
{
    gpio_num_t  pin;
    const char* name;
    int         step; // browser_step() delta, 0 for a button with no action
};

static const button BUTTONS[] = {
    {GPIO_NUM_39, "User A", -1}, // SW4
    {GPIO_NUM_40, "Menu +", 0},  // SW5
    {static_cast<gpio_num_t>(POWER_PIN_MENU_MINUS), "Menu -", 0}, // SW6, RTC pad, wakes from deep sleep
    {static_cast<gpio_num_t>(POWER_PIN_USER_B), "User B", 1},     // SW7, RTC pad, wakes from deep sleep
};
static const int NBUTTONS = sizeof BUTTONS / sizeof BUTTONS[0];

static void press(const button& b)
{
    if(b.step == 0)
        ESP_LOGI(TAG, "%s: no action", b.name);
    else if(strcmp(browser_state(), "wait") == 0)
    {
        ESP_LOGI(TAG, "%s: %s tab", b.name, b.step < 0 ? "previous" : "next");
        browser_step(b.step);
    } else
        ESP_LOGI(TAG, "%s ignored, state %s", b.name, browser_state());
}

static void buttons_task(void*)
{
    int  low_count[NBUTTONS] = {};
    bool pressed[NBUTTONS];
    for(bool& p : pressed) p = true;
    for(;;)
    {
        for(int i = 0; i < NBUTTONS; i++)
        {
            bool low = gpio_get_level(BUTTONS[i].pin) == 0;
            low_count[i] = low ? low_count[i] + 1 : 0;
            if(low_count[i] == 3 && !pressed[i])
            {
                pressed[i] = true;
                press(BUTTONS[i]);
            }
            if(!low) pressed[i] = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void buttons_start()
{
    gpio_config_t io = {};
    for(const button& b : BUTTONS) io.pin_bit_mask |= 1ULL << b.pin;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    if(xTaskCreate(buttons_task, "buttons", 3072, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGE(TAG, "buttons task not created");
}
