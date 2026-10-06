#include "power.h"
#include "epd_drv.h"
#include "settings.h"

#include "calendar.h"
#include "carousel.h"
#include "netif_ap.h"
#include "ota_web.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define AUTO_WAKE_WINDOW_S 180   /* 定时唤醒后等人的窗口（秒） */
#define MIN_ARM_S           30   /* 定时器兜底下限：避免到期未处理成功时 1 秒一醒 */

/* 电池分压 470k:100k => Vbat = Vpin * (470k+100k)/100k = Vpin * 5.7。 */
#define BAT_DIV_NUM        570
#define BAT_DIV_DEN        100
#define BAT_ADC_MAX        4095
#define BAT_FULLSCALE_MV   3100   /* 12dB 满量程，按板实测标定 */
/* 未连接判定：单节锂电物理范围 + 连续有效次数（见 power_battery_mv）。 */
#define BAT_MV_MIN         2400
#define BAT_MV_MAX         4600

static adc_oneshot_unit_handle_t s_adc;
static TickType_t s_last_activity;
static bool s_auto_wake = false;

/* 跨深睡计时三件套（RTC 慢时钟域，深睡保留、掉电由 IDF 清零）。见 power.h 说明。 */
RTC_DATA_ATTR static uint32_t s_mono_s;       /* 已入账的单调秒数 */
RTC_DATA_ATTR static uint32_t s_armed_s;      /* 上次入睡武装的定时秒数 */
RTC_DATA_ATTR static uint32_t s_hotspot_at_s; /* 上次「开热点等人」的时刻 */

void power_init(void)
{
    /* 定时唤醒 = 睡掉了 s_armed_s，把它折进累加器；其他唤醒源（按键 / 上电）不补。
       必须在任何节奏查询之前完成，所以放在 power_init 开头。 */
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        s_mono_s += s_armed_s;
        s_armed_s = 0;
    }

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << EPD_PIN_WAKE_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    /* GPIO0 = ADC1_CH0（ADC2 与 WiFi 共用，不可用）。 */
    adc_oneshot_unit_init_cfg_t u = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c));

    power_activity();
}

void power_activity(void)
{
    s_last_activity = xTaskGetTickCount();
}

void power_set_auto_wake(bool v) { s_auto_wake = v; }
bool power_auto_wake(void) { return s_auto_wake; }

uint32_t power_wakeup_causes(void)
{
    return esp_sleep_get_wakeup_causes();
}

uint32_t power_mono_s(void)
{
    return s_mono_s + (uint32_t)(esp_timer_get_time() / 1000000);
}

uint32_t power_hotspot_in_s(void)
{
    uint32_t itv = settings_get()->wake_s;
    if (itv == 0) return 0;
    uint32_t el = power_mono_s() - s_hotspot_at_s;
    return (el >= itv) ? 1 : (itv - el);
}

void power_mark_hotspot_wake(void)
{
    if (settings_get()->wake_s > 0) s_hotspot_at_s = power_mono_s();
}

uint32_t power_next_wake_s(void)
{
    uint32_t cad[3] = { power_hotspot_in_s(), carousel_next_in_s(), calendar_next_in_s() };
    uint32_t best = 0;
    for (int i = 0; i < 3; i++)
        if (cad[i] && (best == 0 || cad[i] < best)) best = cad[i];
    return best;
}

bool power_should_sleep(void)
{
    const moink_settings_t *s = settings_get();
    uint32_t limit = s->sleep_s;

    if (s_auto_wake) {
        /* 定时醒来：用短窗口等人，除非用户设了更短的空闲超时。 */
        if (limit == 0 || limit > AUTO_WAKE_WINDOW_S) limit = AUTO_WAKE_WINDOW_S;
    } else if (limit == 0 && power_next_wake_s() > 0) {
        /* 「不休眠」+ 有定时节奏 = R1.4.0 的死角：永不入睡 → 定时器从没被武装 →
           轮播 / 日历一次也不会推进。此时按短窗口入睡，把定时链路跑起来。
           注意语义变化：不休眠 + 开了节奏，空闲 3 分钟后仍会睡。 */
        limit = AUTO_WAKE_WINDOW_S;
    }
    if (limit == 0) return false;

    TickType_t idle = xTaskGetTickCount() - s_last_activity;
    return idle >= pdMS_TO_TICKS(limit * 1000);
}

void power_enter_deep_sleep(void)
{
    /* R1.5.4：睡 = 一次彻底重启。新镜像还没自证就睡下，下次开机 bootloader
       会把它标 ABORTED 永久回滚（R1.5.3 定时自醒路径实机踩过），所以睡前必须
       把确认窗口结掉。已确认时这里是空操作。 */
    ota_web_confirm_flush();
    /* 先立闸门：esp_wifi_stop() 会抛 STA_DISCONNECTED，回调若去重连会 abort（R1.5.0）。 */
    netif_sta_suspend_begin();
    esp_wifi_stop();
    epd_panel_deep_sleep();

    /* 唤醒键：GPIO5 上拉、按钮接 GND，低电平唤醒（已验证路径）。 */
    gpio_pullup_en((gpio_num_t)EPD_PIN_WAKE_BTN);
    gpio_pulldown_dis((gpio_num_t)EPD_PIN_WAKE_BTN);
    gpio_set_direction((gpio_num_t)EPD_PIN_WAKE_BTN, GPIO_MODE_INPUT);
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << EPD_PIN_WAKE_BTN, ESP_GPIO_WAKEUP_GPIO_LOW));

    /* 各独立节奏取最小：谁先到期谁醒，醒来只做到期的那件事。
       先把本次清醒时长折进累加器，再记下武装秒数供下次开机补账。 */
    s_mono_s = power_mono_s();
    uint32_t arm_s = power_next_wake_s();
    if (arm_s > 0 && arm_s < MIN_ARM_S) arm_s = MIN_ARM_S;  /* 到期却没处理成功时别 1 秒一醒 */
    s_armed_s = arm_s;
    if (arm_s > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)arm_s * 1000000ULL);
    }

    ESP_LOGI(TAG, "deep sleep (btn GPIO%d low, timer %lus)",
             EPD_PIN_WAKE_BTN, (unsigned long)arm_s);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_deep_sleep_start();
}

int power_battery_mv(void)
{
    /* 电池未连接检测：ADC 引脚悬空时读数靠杂散耦合乱漂（实测 3.2/3.3/17.6V 交替，
       17.6V 即漂到接近满量程的换算值）。单次采样无法区分，用跨请求状态机：
       读数必须落在单节锂电物理范围内，且连续 3 次 /api/info 轮询都有效才显示；
       任一次超范围立即清零 → 页面显示「未连接」。真电池接上后轮询 2~3 次即出电压。 */
    static int s_valid = 0;

    int raw = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) { s_valid = 0; return -1; }
    int mv = raw * BAT_FULLSCALE_MV / BAT_ADC_MAX * BAT_DIV_NUM / BAT_DIV_DEN;

    if (mv < BAT_MV_MIN || mv > BAT_MV_MAX) { s_valid = 0; return -1; }
    if (++s_valid < 3) return -1;
    return mv;
}
