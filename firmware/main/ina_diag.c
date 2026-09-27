/* INA226 diagnostic sampler. See ina_diag.h.
 * TRANSFER mode never calls ina_diag_init (USB protocol has priority).
 */
#include "ina_diag.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "ina_diag";

#define INA_ADDR      0x41
#define INA_REG_SHUNT 0x01
#define INA_REG_BUS   0x02
#define INA_REG_CAL   0x05

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static ina_sample_t s_ring[INA_DIAG_N];
static int s_idx, s_count;

static esp_err_t ina_read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t r = i2c_master_transmit_receive(s_dev, &reg, 1, rx, 2, 50);
    if (r == ESP_OK)
        *out = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]); /* big-endian */
    return r;
}

static void ina_sampler_task(void *arg)
{
    (void)arg;
    while (1) {
        /* Keep sampling while paused too: pause-vs-play power delta is
         * the main diagnostic goal. Short timeouts so a NACK never blocks. */
        ina_sample_t s = {
            .t_us = esp_timer_get_time(),
            .err = ESP_OK,
        };
        esp_err_t r = ina_read_reg(INA_REG_BUS, &s.bus_raw);
        if (r == ESP_OK)
            r = ina_read_reg(INA_REG_SHUNT, &s.shunt_raw);
        if (r == ESP_OK)
            r = ina_read_reg(INA_REG_CAL, &s.cal_raw);
        s.err = r;
        s_ring[s_idx] = s;
        s_idx = (s_idx + 1) % INA_DIAG_N;
        if (s_count < INA_DIAG_N)
            s_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ina_diag_init(void)
{
#if !CONFIG_TAB5_INA_DIAG_ENABLE
    ESP_LOGI(TAG, "disabled by Kconfig");
    return;
#endif
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus handle");
        return;
    }
    if (i2c_master_probe(bus, INA_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "INA226 0x41 not present, diagnostic off");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA_ADDR,
        .scl_speed_hz = 100000, /* shared generic bus: stay at standard mode */
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "add_device failed, diagnostic off");
        return;
    }
    s_present = true;
    /* Low priority on core 0: never disturb audio(12)/prefetch(5). */
    xTaskCreatePinnedToCore(ina_sampler_task, "ina_diag", 3072, NULL, 2,
                            NULL, 0);
    ESP_LOGI(TAG, "INA226 diagnostic on (raw values only)");
}

void ina_diag_dump(void)
{
    if (!s_present) {
        ESP_LOGI(TAG, "dump: no device");
        return;
    }
    ESP_LOGI(TAG, "dump start n=%d", s_count);
    for (int i = 0; i < s_count; i++) {
        const ina_sample_t *s =
            &s_ring[(s_idx - s_count + i + INA_DIAG_N * 2) % INA_DIAG_N];
        ESP_LOGI(TAG, "ina t=%lld bus=%u shunt=%u cal=%u err=%s",
                 (long long)s->t_us, s->bus_raw, s->shunt_raw, s->cal_raw,
                 esp_err_to_name(s->err));
    }
    ESP_LOGI(TAG, "dump end");
}

bool ina_diag_latest(ina_sample_t *out)
{
    if (!s_present || s_count == 0)
        return false;
    *out = s_ring[(s_idx - 1 + INA_DIAG_N) % INA_DIAG_N];
    return true;
}

int32_t ina_bus_mv(void)
{
    ina_sample_t s;
    if (!ina_diag_latest(&s) || s.err != ESP_OK)
        return -1;
    return (int32_t)s.bus_raw * 125 / 100; /* 1.25mV/LSB */
}

/* IP2326 charge control via U7 (0x44): P7=CHG_EN (high=run),
 * P5=nCHG_QC_EN (low=QC enable), P6=CHG_STAT net (input).
 * Same order as factory FW: QC first, then CHG_EN. */
void power_charge_enable(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if (io == NULL) {
        ESP_LOGW(TAG, "charge: no expander1 handle");
        return;
    }
    esp_err_t r = ESP_OK;
    r |= esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_5,
                                 IO_EXPANDER_OUTPUT);
    r |= esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_5, 0);
    r |= esp_io_expander_set_output_mode(io, IO_EXPANDER_PIN_NUM_5,
                                         IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    r |= esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_7,
                                 IO_EXPANDER_OUTPUT);
    r |= esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_7, 1);
    r |= esp_io_expander_set_output_mode(io, IO_EXPANDER_PIN_NUM_7,
                                         IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    ESP_LOGI(TAG, "charge enable rs=%s", esp_err_to_name(r));
}

int power_charge_level(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    uint32_t mask = 0;
    if (io == NULL ||
        esp_io_expander_get_level(io, IO_EXPANDER_PIN_NUM_6, &mask) != ESP_OK)
        return -1;
    return (mask & IO_EXPANDER_PIN_NUM_6) ? 1 : 0;
}
