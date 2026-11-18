// The ADC unit s_adc is a shared resource.
// After power_init, any read of it runs under s_adc_lock, so concurrent callers never read the rail
// at the same time.

#include "power.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "epd97.h"
#include "leds.h"

static const char* TAG = "power";

static const adc_unit_t    BAT_UNIT    = ADC_UNIT_2;
static const adc_channel_t BAT_CHANNEL = ADC_CHANNEL_3;
static const adc_atten_t   BAT_ATTEN   = ADC_ATTEN_DB_12;
static const int           SAMPLES     = 8;
static const int TRIES = 32;

static const gpio_num_t WAKE_PINS[] = {static_cast<gpio_num_t>(POWER_PIN_MENU_MINUS),
                                       static_cast<gpio_num_t>(POWER_PIN_USER_B)};
static const gpio_num_t LED_PIN     = static_cast<gpio_num_t>(LEDS_DATA_GPIO);
static const int RELEASE_WAIT_MS = 10000;

static const int64_t USJ_SETTLE_US = 10000;

static const char* const WAKE_NAMES[] = {"power on or reset", "timer", "Menu -", "User B"};

static power_wake                s_wake = POWER_WAKE_RESET;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int64_t                   s_init_us;
static SemaphoreHandle_t         s_adc_lock;

static void usj_settle()
{
    int64_t wait_us = s_init_us + USJ_SETTLE_US - esp_timer_get_time();
    if(wait_us > 0) vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(wait_us / 1000)) + 1);
}

void power_init()
{
    s_init_us       = esp_timer_get_time();
    s_adc_lock      = xSemaphoreCreateMutex();
    uint32_t causes = esp_sleep_get_wakeup_causes();
    uint64_t pins   = esp_sleep_get_ext1_wakeup_status();
    s_wake          = power_decode_wake(causes, pins);
    ESP_LOGI(TAG, "wake: %s (causes 0x%lx, ext1 pins 0x%llx)", WAKE_NAMES[s_wake], static_cast<unsigned long>(causes),
             static_cast<unsigned long long>(pins));

    for(gpio_num_t pin : WAKE_PINS) rtc_gpio_deinit(pin);
    gpio_set_level(LED_PIN, 0);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_hold_dis(LED_PIN);

    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id                     = BAT_UNIT;
    esp_err_t err                    = adc_oneshot_new_unit(&unit, &s_adc);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s, rail reads fail", esp_err_to_name(err));
        s_adc = nullptr;
        return;
    }
    adc_oneshot_chan_cfg_t chan = {};
    chan.atten                  = BAT_ATTEN;
    chan.bitwidth               = ADC_BITWIDTH_DEFAULT;
    err                         = adc_oneshot_config_channel(s_adc, BAT_CHANNEL, &chan);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s, rail reads fail", esp_err_to_name(err));
        return;
    }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id                         = BAT_UNIT;
    cal.chan                            = BAT_CHANNEL;
    cal.atten                           = BAT_ATTEN;
    cal.bitwidth                        = ADC_BITWIDTH_DEFAULT;
    err                                 = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_cali_create_scheme_curve_fitting: %s, rail reads fail", esp_err_to_name(err));
        s_cali = nullptr;
    }
}

power_wake power_wake_cause()
{
    return s_wake;
}

const char* power_wake_name(power_wake w)
{
    return WAKE_NAMES[w];
}

static uint64_t wake_pins_low()
{
    uint64_t low = 0;
    for(gpio_num_t pin : WAKE_PINS)
        if(gpio_get_level(pin) == 0) low |= 1ULL << pin;
    return low;
}

static int rail_mv()
{
    if(!s_adc || !s_cali) return -1;
    int sum = 0;
    int good = 0;
    for(int t = 0; t < TRIES && good < SAMPLES; t++)
    {
        int raw = 0;
        int mv  = 0;
        if(adc_oneshot_read(s_adc, BAT_CHANNEL, &raw) != ESP_OK)
        {
            vTaskDelay(1);
            continue;
        }
        if(adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        sum += mv;
        good++;
    }
    if(good == 0)
    {
        ESP_LOGW(TAG, "rail read failed, %d tries", TRIES);
        return -1;
    }
    return 2 * ((sum + good / 2) / good);
}

power_reading power_read(bool was_low)
{
    usj_settle();
    bool host = usb_serial_jtag_is_connected();
    if(s_adc_lock) xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    const int mv = rail_mv();
    if(s_adc_lock) xSemaphoreGive(s_adc_lock);
    power_reading r = power_judge(host, mv, was_low);
    ESP_LOGI(TAG, "rail %d mV, %s, battery %s", r.rail_mv, host ? "USB host" : r.usb ? "USB" : "battery",
             r.low ? "low" : "ok");
    return r;
}

void power_sleep(int seconds, uint64_t released)
{
    leds_stop();
    gpio_set_level(LED_PIN, 0);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_hold_en(LED_PIN);
    epd_deep_sleep_prepare();
    gpio_deep_sleep_hold_en();

    for(gpio_num_t pin : WAKE_PINS)
    {
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_pullup_en(pin);
    }
    vTaskDelay(1);
    uint64_t held = power_held_pins(wake_pins_low(), released);
    if(held)
    {
        ESP_LOGI(TAG, "wake button held, waiting up to %d ms for its release", RELEASE_WAIT_MS);
        for(int ms = 0; ms < RELEASE_WAIT_MS && held; ms += 10)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            held &= wake_pins_low();
        }
    }
    power_arm arm = power_sleep_arm(seconds, held);

    for(gpio_num_t pin : WAKE_PINS)
    {
        rtc_gpio_init(pin);
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_pullup_en(pin);
    }
    if(arm.pins) ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(arm.pins, ESP_EXT1_WAKEUP_ANY_LOW));
    if(arm.seconds > 0) ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(arm.seconds) * 1000000ULL));
    ESP_LOGI(TAG, "deep sleep, timer %d s (0 none), wake on Menu - %s, on User B %s", arm.seconds,
             arm.pins & (1ULL << POWER_PIN_MENU_MINUS) ? "yes" : "no, held", arm.pins & (1ULL << POWER_PIN_USER_B) ? "yes" : "no, held");
    esp_deep_sleep_start();
}
