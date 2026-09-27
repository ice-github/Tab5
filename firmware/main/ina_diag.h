/* INA226 power-monitor diagnostic (Tab5, I2C 0x41).
 *
 * Records raw bus/shunt/calibration register values + timestamp + error
 * into a small DRAM ring at 1Hz. No current/power conversion: the shunt
 * value and monitored rail are not confirmed from schematics yet, so raw
 * values only. Disabled automatically when 0x41 does not answer.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int64_t t_us;       /* esp_timer_get_time() at sample */
    uint16_t bus_raw;   /* reg 0x02, LSB 1.25mV (if bus rail as assumed) */
    uint16_t shunt_raw; /* reg 0x01, LSB 2.5uV */
    uint16_t cal_raw;   /* reg 0x05, reserved for future current conversion */
    esp_err_t err;      /* ESP_OK or I2C error */
} ina_sample_t;

#define INA_DIAG_N 256 /* 1Hz x 256 = ~4min window, ~4KB DRAM */

void ina_diag_init(void);     /* probe 0x41, add device, spawn sampler task */
void ina_diag_dump(void);     /* log the whole ring (call after USB reconnect) */
bool ina_diag_latest(ina_sample_t *out); /* newest sample for prof piggyback */

/* Charge control (IP2326 via IO expander U7 @0x44). Factory FW enables
 * charging after init; power-on default is OFF, so without this the
 * battery never charges. */
void power_charge_enable(void); /* CHG_EN=1, nCHG_QC_EN=0 (factory order) */
int power_charge_level(void);   /* U7 P6 (CHG_STAT net): 0=trickle charging,
                                 * 1=not-trickle, -1=read error */
int32_t ina_bus_mv(void);       /* latest bus reg as mV (1.25mV/LSB), -1 N/A */
