// Polls the four hardware buttons in a task and turns User A and User B presses into browser tab
// moves through browser_step.

#include "buttons.h"

#include <atomic>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "browser.h"
#include "leds.h"
#include "power_logic.h"

static const char* TAG = "buttons";

struct button
{
    gpio_num_t  pin;
    const char* name;
    int         step; // 0: a Menu button, which holds and resumes
};

static const button BUTTONS[] = {
    {GPIO_NUM_39, "User A", -1},
    {GPIO_NUM_40, "Menu +", 0},
    {static_cast<gpio_num_t>(POWER_PIN_MENU_MINUS), "Menu -", 0},
    {static_cast<gpio_num_t>(POWER_PIN_USER_B), "User B", 1},
};
static const int NBUTTONS = sizeof BUTTONS / sizeof BUTTONS[0];

static std::atomic<uint64_t> s_released{0};

static void press(const button& b)
{
    if(b.step == 0)
    {
        int held = browser_hold_toggle();
        if(held < 0) ESP_LOGI(TAG, "%s ignored, state %s", b.name, browser_state());
        else ESP_LOGI(TAG, "%s: %s", b.name, held ? "hold" : "resume");
        return;
    }
    int tab = browser_step(b.step);
    if(tab >= 0) ESP_LOGI(TAG, "%s: tab %d", b.name, tab + 1);
    else ESP_LOGI(TAG, "%s ignored, state %s", b.name, browser_state());
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
            if(!low)
            {
                pressed[i] = false;
                s_released.fetch_or(1ULL << BUTTONS[i].pin);
            }
        }
        leds_tick();
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
    if(xTaskCreate(buttons_task, "buttons", 4096, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGE(TAG, "buttons task not created");
}

uint64_t buttons_released()
{
    return s_released.load();
}
