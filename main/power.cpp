// Wake cause, rail voltage, USB presence and deep sleep. The decisions are in power_logic.h.

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
#include "freertos/task.h"

#include "epd97.h"
#include "leds.h"

static const char* TAG = "power";

static const adc_unit_t    BAT_UNIT    = ADC_UNIT_2;
static const adc_channel_t BAT_CHANNEL = ADC_CHANNEL_3; // BAT_SENS, GPIO14
static const adc_atten_t   BAT_ATTEN   = ADC_ATTEN_DB_12;
static const int           SAMPLES     = 8;
// ADC2 is shared with WiFi. A read that loses to WiFi returns ESP_ERR_TIMEOUT and the next try waits a
// tick.
static const int TRIES = 32;

static const gpio_num_t WAKE_PINS[] = {static_cast<gpio_num_t>(POWER_PIN_MENU_MINUS),
                                       static_cast<gpio_num_t>(POWER_PIN_USER_B)};
static const gpio_num_t LED_PIN     = static_cast<gpio_num_t>(LEDS_DATA_GPIO);
// A button only sleep waits up to this long for held wake buttons to be released.
static const int RELEASE_WAIT_MS = 10000;

// usb_serial_jtag_is_connected() is true from boot until about 4 ticks of the running scheduler have
// seen no SOF packet from a host. power_init() runs with the scheduler running, so 10 ms after it the
// value is settled.
static const int64_t USJ_SETTLE_US = 10000;

static const char* const WAKE_NAMES[] = {"power on or reset", "timer", "Menu -", "User B"};

static power_wake                s_wake = POWER_WAKE_RESET;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int64_t                   s_init_us;

static void usj_settle()
{
    int64_t wait_us = s_init_us + USJ_SETTLE_US - esp_timer_get_time();
    if(wait_us > 0) vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(wait_us / 1000)) + 1);
}

void power_init()
{
    s_init_us       = esp_timer_get_time();
    uint32_t causes = esp_sleep_get_wakeup_causes();
    uint64_t pins   = esp_sleep_get_ext1_wakeup_status();
    s_wake          = power_decode_wake(causes, pins);
    ESP_LOGI(TAG, "wake: %s (causes 0x%lx, ext1 pins 0x%llx)", WAKE_NAMES[s_wake], static_cast<unsigned long>(causes),
             static_cast<unsigned long long>(pins));

    // After a deep sleep wake the wake pads are still in RTC function. power_sleep() holds the LED data
    // pad low. The pad is set to drive low before its hold is released, so the line stays low until
    // leds_init() takes it. After a power on there is no hold and the line goes low here.
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

// The wake pins that read low now, as an ext1 mask.
static uint64_t wake_pins_low()
{
    uint64_t low = 0;
    for(gpio_num_t pin : WAKE_PINS)
        if(gpio_get_level(pin) == 0) low |= 1ULL << pin;
    return low;
}

// The system rail in mV, the average of up to SAMPLES samples. Negative when every sample failed.
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
    // The divider halves the rail.
    return 2 * ((sum + good / 2) / good);
}

power_reading power_read(bool was_low)
{
    usj_settle();
    bool          host = usb_serial_jtag_is_connected();
    power_reading r    = power_judge(host, rail_mv(), was_low);
    ESP_LOGI(TAG, "rail %d mV, %s, battery %s", r.rail_mv, host ? "USB host" : r.usb ? "USB" : "battery",
             r.low ? "low" : "ok");
    return r;
}

void power_sleep(int seconds)
{
    // The WS2812s keep their colour while powered. led_strip_clear() returns after the RMT has sent the
    // dark frame and its reset gap.
    leds_off();
    // A floating data line in deep sleep can clock noise into the chain. Drive it low and hold it.
    gpio_set_level(LED_PIN, 0);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_hold_en(LED_PIN);
    // epd_deep_sleep_prepare() latches the panel supply off. gpio_deep_sleep_hold_en() makes the pad holds
    // of GPIO48 and LED_PIN last through deep sleep.
    epd_deep_sleep_prepare();
    gpio_deep_sleep_hold_en();

    // A wake button that is down now would wake the chip at once, since ext1 wakes on a low level.
    // power_sleep_arm() leaves such a pin out. A button only sleep first waits up to RELEASE_WAIT_MS for
    // every held wake button to be released, so the button that woke the badge can wake it again. A pin
    // still low after that stays out for this sleep. With both pins out the timer wakes the badge after
    // POWER_STUCK_WAKE_S.
    for(gpio_num_t pin : WAKE_PINS)
    {
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_pullup_en(pin);
    }
    vTaskDelay(1);
    uint64_t low = wake_pins_low();
    if(seconds <= 0 && low)
    {
        ESP_LOGI(TAG, "wake button held, waiting up to %d ms for its release", RELEASE_WAIT_MS);
        for(int ms = 0; ms < RELEASE_WAIT_MS && low; ms += 10)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            low = wake_pins_low();
        }
    }
    power_arm arm = power_sleep_arm(seconds, low);

    // The buttons pull to ground and the board has no pull-up resistors. The RTC pull-ups keep the pads
    // high. The RTC peripherals domain powers down in this sleep. IDF holds the pads in the ext1 mask with
    // their pulls. A pin left out of the mask gets no hold, so its pull is lost in sleep and the pad
    // floats. It is not a wake source, so its level cannot wake the chip.
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
