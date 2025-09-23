/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "shs01.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl_utility.h"
#include "light_driver.h"

/* Zigbee custom cluster helpers */
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"

#if !defined CONFIG_ZB_ZCZR
#error "Enable Router: set CONFIG_ZB_ZCZR=y (menuconfig)"
#endif

static const char *SHS_TAG = "SHS01";

/* ---------------- NVS keys ---------------- */
#define SHS_NVS_NAMESPACE       "cfg"
#define SHS_NVS_KEY_MV_CD       "mv_cd"     /* u16 */
#define SHS_NVS_KEY_OCC_CD      "occ_cd"    /* u16 */
#define SHS_NVS_KEY_MV_SENS     "mv_sens"   /* u8  0..100 */
#define SHS_NVS_KEY_ST_SENS     "st_sens"   /* u8  0..100 */
#define SHS_NVS_KEY_MV_GATE     "mv_gate"   /* u8  0..8   */
#define SHS_NVS_KEY_ST_GATE     "st_gate"   /* u8  2..8   */

/* ---------------- Backing store for config sliders ---------------- */
static uint16_t shs_movement_cooldown_sec = 0;  /* 0..300 */
static uint16_t shs_occupancy_clear_sec   = 0;  /* 0..65535 */

static uint8_t  shs_moving_sens_0_100     = 60;  /* 0..100 */
static uint8_t  shs_static_sens_0_100     = 50;  /* 0..100 */

/* Gates are 16-bit so EP1 attributes can point directly (U16 type) */
static uint16_t shs_moving_max_gate       = 8;   /* 0..8 (0..6.0 m) */
static uint16_t shs_static_max_gate       = 8;   /* 2..8 (0.75..6.0 m) */

/* Proxies exposed on EP1 for 0..10 slider READs (kept in sync with 0..100) */
static uint16_t shs_sens_mv_0_10          = 6;
static uint16_t shs_sens_st_0_10          = 5;

/* ---------------- Published states ---------------- */
static bool shs_moving_state    = false; /* moving target */
static bool shs_static_state    = false; /* static target */
static bool shs_occupancy_state = false; /* overall occupancy (moving || static) */

/* ---------------- Movement cooldown state (for moving target) ---------------- */
static bool     shs_mv_cooldown_active      = false;
static uint32_t shs_mv_cooldown_deadline_ms = 0;
static bool     shs_last_moving_sample      = false;

/* Zigbee stack ready flag: only write attrs when true */
static volatile bool shs_zb_ready = false;

/* ---------------- NVS save worker (debounce sliders) ---------------- */
typedef enum {
    SHS_SAVE_IMMEDIATE_U16,
    SHS_SAVE_DEBOUNCE_SENS_MOVE,   /* 0..100 */
    SHS_SAVE_DEBOUNCE_SENS_STATIC, /* 0..100 */
    SHS_SAVE_DEBOUNCE_GATE_MOVE,   /* 0..8 */
    SHS_SAVE_DEBOUNCE_GATE_STATIC, /* 2..8 */
} shs_save_evt_t;

typedef struct {
    shs_save_evt_t type;
    uint16_t       u16;   
} shs_save_msg_t;

static QueueHandle_t shs_save_q;

/* ---------------- Helpers ---------------- */
static inline bool shs_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void shs_cfg_save_u16(const char *key, uint16_t v)
{
    nvs_handle_t h;
    if (nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void shs_cfg_save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static inline void shs_save_enqueue(shs_save_evt_t t, uint16_t v)
{
    if (!shs_save_q) return;
    shs_save_msg_t m = {.type = t, .u16 = v};
    (void)xQueueSend(shs_save_q, &m, 0);
}

static void shs_cfg_sync_sens_proxies(void)
{
    shs_sens_mv_0_10 = (uint16_t)((shs_moving_sens_0_100 + 5) / 10);
    if (shs_sens_mv_0_10 > 10) shs_sens_mv_0_10 = 10;
    shs_sens_st_0_10 = (uint16_t)((shs_static_sens_0_100 + 5) / 10);
    if (shs_sens_st_0_10 > 10) shs_sens_st_0_10 = 10;
}

static void shs_cfg_load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SHS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(SHS_TAG, "NVS open RO failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    uint16_t u16tmp; uint8_t u8tmp;

    if (nvs_get_u16(h, SHS_NVS_KEY_MV_CD,  &u16tmp) == ESP_OK) shs_movement_cooldown_sec = (u16tmp > SHS_COOLDOWN_MAX_SEC) ? SHS_COOLDOWN_MAX_SEC : u16tmp;
    if (nvs_get_u16(h, SHS_NVS_KEY_OCC_CD, &u16tmp) == ESP_OK) shs_occupancy_clear_sec   = u16tmp;

    if (nvs_get_u8(h,  SHS_NVS_KEY_MV_SENS, &u8tmp) == ESP_OK) shs_moving_sens_0_100 = (u8tmp > 100) ? 100 : u8tmp;
    if (nvs_get_u8(h,  SHS_NVS_KEY_ST_SENS, &u8tmp) == ESP_OK) shs_static_sens_0_100 = (u8tmp > 100) ? 100 : u8tmp;

    if (nvs_get_u8(h,  SHS_NVS_KEY_MV_GATE, &u8tmp) == ESP_OK) shs_moving_max_gate = (u8tmp > 8) ? 8 : u8tmp;
    if (nvs_get_u8(h,  SHS_NVS_KEY_ST_GATE, &u8tmp) == ESP_OK) {
        if (u8tmp < 2) u8tmp = 2; else if (u8tmp > 8) u8tmp = 8;
        shs_static_max_gate = u8tmp;
    }
    nvs_close(h);

    shs_cfg_sync_sens_proxies();

    ESP_LOGI(SHS_TAG, "NVS loaded: mv_cd=%us, occ_cd=%us, mv_sens=%u, st_sens=%u, mv_gate=%u, st_gate=%u",
             (unsigned)shs_movement_cooldown_sec, (unsigned)shs_occupancy_clear_sec,
             (unsigned)shs_moving_sens_0_100, (unsigned)shs_static_sens_0_100,
             (unsigned)shs_moving_max_gate, (unsigned)shs_static_max_gate);
}

/* ---------------- Light driver init ---------------- */
static void shs_deferred_driver_init(void)
{
    light_driver_init(LIGHT_DEFAULT_OFF);
}

/* ---------------- LD2410C frame writers ---------------- */
static void shs_ld2410_write_cmd(const uint8_t *payload, uint16_t payload_len)
{
    /* Build contiguous buffer: hdr(4) + len(2 LE) + payload + tail(4) */
    uint16_t total = 4 + 2 + payload_len + 4;

    uint8_t stackbuf[4 + 2 + 256 + 4];
    uint8_t *p = stackbuf;
    p[0]=SHS_LD2410_HDR_TX0; p[1]=SHS_LD2410_HDR_TX1; p[2]=SHS_LD2410_HDR_TX2; p[3]=SHS_LD2410_HDR_TX3;
    p[4]=(uint8_t)(payload_len & 0xFF);
    p[5]=(uint8_t)((payload_len >> 8) & 0xFF);
    memcpy(&p[6], payload, payload_len);
    p[6 + payload_len + 0]=SHS_LD2410_TAIL_TX0;
    p[6 + payload_len + 1]=SHS_LD2410_TAIL_TX1;
    p[6 + payload_len + 2]=SHS_LD2410_TAIL_TX2;
    p[6 + payload_len + 3]=SHS_LD2410_TAIL_TX3;
    uart_write_bytes(SHS_LD2410_UART_NUM, (const char*)stackbuf, total);
}

static inline uint16_t shs_clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t  shs_clamp_u8 (uint8_t  v, uint8_t  lo, uint8_t  hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void shs_ld2410_apply_params_all(void)
{
    /* belt-and-suspenders clamp */
    uint16_t mv_gate = shs_clamp_u16(shs_moving_max_gate, 0, 8);
    uint16_t st_gate = shs_clamp_u16(shs_static_max_gate, 2, 8);
    uint16_t no_one  = shs_occupancy_clear_sec; /* 0..65535 */

    const uint8_t begin_cfg[] = { (SHS_LD2410_CMD_BEGIN_CONFIG & 0xFF), ((SHS_LD2410_CMD_BEGIN_CONFIG >> 8) & 0xFF), 0x01, 0x00 };
    shs_ld2410_write_cmd(begin_cfg, sizeof(begin_cfg));

    uint8_t set_params[2 + (2+4)*3]; int o = 0;
    set_params[o++] = (SHS_LD2410_CMD_SET_PARAMS & 0xFF); set_params[o++] = ((SHS_LD2410_CMD_SET_PARAMS >> 8) & 0xFF);

    /* max move gate (1 byte significant) */
    set_params[o++] = (SHS_LD2410_PW_MAX_MOVE_GATE & 0xFF); set_params[o++] = ((SHS_LD2410_PW_MAX_MOVE_GATE >> 8) & 0xFF);
    set_params[o++] = (uint8_t)(mv_gate & 0xFF); set_params[o++] = 0x00; set_params[o++] = 0x00; set_params[o++] = 0x00;

    /* max static gate (1 byte significant) */
    set_params[o++] = (SHS_LD2410_PW_MAX_STATIC_GATE & 0xFF); set_params[o++] = ((SHS_LD2410_PW_MAX_STATIC_GATE >> 8) & 0xFF);
    set_params[o++] = (uint8_t)(st_gate & 0xFF); set_params[o++] = 0x00; set_params[o++] = 0x00; set_params[o++] = 0x00;

    /* no one duration seconds (LE16) */
    set_params[o++] = (SHS_LD2410_PW_NO_ONE_DURATION & 0xFF); set_params[o++] = ((SHS_LD2410_PW_NO_ONE_DURATION >> 8) & 0xFF);
    set_params[o++] = (uint8_t)(no_one & 0xFF);
    set_params[o++] = (uint8_t)((no_one >> 8) & 0xFF);
    set_params[o++] = 0x00; set_params[o++] = 0x00;

    shs_ld2410_write_cmd(set_params, o);

    const uint8_t end_cfg[] = { (SHS_LD2410_CMD_END_CONFIG & 0xFF), ((SHS_LD2410_CMD_END_CONFIG >> 8) & 0xFF) };
    shs_ld2410_write_cmd(end_cfg, sizeof(end_cfg));

    ESP_LOGI(SHS_TAG, "Applied params: move_gate=%u, static_gate=%u, no_one=%us",
             (unsigned)mv_gate, (unsigned)st_gate, (unsigned)no_one);
}

static void shs_ld2410_apply_global_sensitivity(void)
{
    /* clamp & map */
    uint8_t mv = shs_clamp_u8(shs_moving_sens_0_100, 0, 100);
    uint8_t st = shs_clamp_u8(shs_static_sens_0_100, 0, 100);

    const uint8_t begin_cfg[] = { (SHS_LD2410_CMD_BEGIN_CONFIG & 0xFF), ((SHS_LD2410_CMD_BEGIN_CONFIG >> 8) & 0xFF), 0x01, 0x00 };
    shs_ld2410_write_cmd(begin_cfg, sizeof(begin_cfg));

    /* 0xFFFF = all gates */
    uint8_t sens[2 + 2 + 2 + 2]; int o = 0;
    sens[o++] = (SHS_LD2410_CMD_SET_SENSITIVITY & 0xFF); sens[o++] = ((SHS_LD2410_CMD_SET_SENSITIVITY >> 8) & 0xFF);
    sens[o++] = (SHS_LD2410_GATE_ALL & 0xFF);            sens[o++] = ((SHS_LD2410_GATE_ALL >> 8) & 0xFF);
    sens[o++] = mv;   sens[o++] = 0x00;  /* moving (LSB) */
    sens[o++] = st;   sens[o++] = 0x00;  /* static (LSB) */

    shs_ld2410_write_cmd(sens, o);

    const uint8_t end_cfg[] = { (SHS_LD2410_CMD_END_CONFIG & 0xFF), ((SHS_LD2410_CMD_END_CONFIG >> 8) & 0xFF) };
    shs_ld2410_write_cmd(end_cfg, sizeof(end_cfg));

    ESP_LOGI(SHS_TAG, "Applied sensitivity: move=%u, static=%u", (unsigned)mv, (unsigned)st);
}

static inline void shs_ld2410_apply_no_one_duration(uint16_t seconds)
{
    shs_occupancy_clear_sec = seconds;
    shs_ld2410_apply_params_all();
}

/* ---------------- ZCL helpers ---------------- */
static inline void shs_zb_set_occ_bitmap(uint8_t endpoint, bool occupied)
{
    if (!shs_zb_ready) return;
    uint8_t v = occupied ? 1 : 0; /* Occupancy (0x0000) is bitmap8; bit0=1 means occupied */
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(endpoint,
                                 ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                                 &v, false);
    esp_zb_lock_release();
}

static inline void shs_zb_set_bool_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr_id, bool value)
{
    if (!shs_zb_ready) return;
    bool v = value;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(endpoint,
                                 cluster,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 attr_id,
                                 &v, false);
    esp_zb_lock_release();
}

/* mirror occupied_to_unoccupied_delay (0x0010) as read-only on EP2 */
static inline void shs_zb_set_ou_delay_ep2(uint16_t seconds)
{
    if (!shs_zb_ready) return;
    uint16_t v = seconds;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(SHS_EP_OCC,
                                 ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 SHS_ZCL_ATTR_OCC_PIR_OU_DELAY,
                                 &v, false);
    esp_zb_lock_release();
}

/* ---------------- ZCL write callback to config cluster + OnOff ---------------- */
static esp_err_t shs_zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message) return ESP_OK;

    /* EP1: genOnOff (light) */
    if (message->info.dst_endpoint == SHS_EP_LIGHT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
            message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
            bool light_state = *(bool *)message->attribute.data.value;
            ESP_LOGI(SHS_TAG, "Light -> %s", light_state ? "ON" : "OFF");
            light_driver_set_power(light_state);
            return ESP_OK;
        }
    }

    /* EP1: custom config cluster (0xFDCD), all U16 */
    if (message->info.dst_endpoint == SHS_EP_LIGHT &&
        message->info.cluster == SHS_CL_CFG_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {

        uint16_t v = *(uint16_t *)message->attribute.data.value;

        switch (message->attribute.id) {
            case SHS_ATTR_MOVEMENT_COOLDOWN: {
                if (v > SHS_COOLDOWN_MAX_SEC) v = SHS_COOLDOWN_MAX_SEC;
                bool was_zero = (shs_movement_cooldown_sec == 0);
                shs_movement_cooldown_sec = v;
                shs_save_enqueue(SHS_SAVE_IMMEDIATE_U16, (SHS_ATTR_MOVEMENT_COOLDOWN<<8)|0);
                if (shs_movement_cooldown_sec == 0) {
                    shs_mv_cooldown_active = false;
                } else if (was_zero && shs_moving_state && !shs_mv_cooldown_active) {
                    shs_mv_cooldown_active = true;
                    shs_mv_cooldown_deadline_ms = esp_log_timestamp() + (uint32_t)shs_movement_cooldown_sec * 1000U;
                }
                ESP_LOGI(SHS_TAG, "Set Movement Clear Cooldown = %us", (unsigned)shs_movement_cooldown_sec);
                return ESP_OK;
            }
            case SHS_ATTR_OCC_CLEAR_COOLDOWN: {
                shs_ld2410_apply_no_one_duration(v);
                shs_zb_set_ou_delay_ep2(shs_occupancy_clear_sec);
                shs_save_enqueue(SHS_SAVE_IMMEDIATE_U16, (SHS_ATTR_OCC_CLEAR_COOLDOWN<<8)|0);
                ESP_LOGI(SHS_TAG, "Set Occupancy Clear Cooldown = %us", (unsigned)shs_occupancy_clear_sec);
                return ESP_OK;
            }
            case SHS_ATTR_MOVING_SENS_0_10: {
                if (v > 10) v = 10;
                shs_sens_mv_0_10 = v;
                shs_moving_sens_0_100 = (uint8_t)(v * 10);
                shs_ld2410_apply_global_sensitivity(); 
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_SENS_MOVE, shs_moving_sens_0_100); /* debounce NVS 500ms */
                ESP_LOGI(SHS_TAG, "Set Movement Detection Sensitivity = %u/100", (unsigned)shs_moving_sens_0_100);
                return ESP_OK;
            }
            case SHS_ATTR_STATIC_SENS_0_10: {
                if (v > 10) v = 10;
                shs_sens_st_0_10 = v;
                shs_static_sens_0_100 = (uint8_t)(v * 10);
                shs_ld2410_apply_global_sensitivity();
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_SENS_STATIC, shs_static_sens_0_100);
                ESP_LOGI(SHS_TAG, "Set Occupancy Detection Sensitivity = %u/100", (unsigned)shs_static_sens_0_100);
                return ESP_OK;
            }
            case SHS_ATTR_MOVING_MAX_GATE: {
                if (v > 8) v = 8;
                shs_moving_max_gate = v;
                shs_ld2410_apply_params_all();
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_GATE_MOVE, shs_moving_max_gate);
                ESP_LOGI(SHS_TAG, "Set Movement Detection Range (gate) = %u", (unsigned)shs_moving_max_gate);
                return ESP_OK;
            }
            case SHS_ATTR_STATIC_MAX_GATE: {
                if (v < 2) v = 2; else if (v > 8) v = 8;
                shs_static_max_gate = v;
                shs_ld2410_apply_params_all();
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_GATE_STATIC, shs_static_max_gate);
                ESP_LOGI(SHS_TAG, "Set Occupancy Detection Range (gate) = %u", (unsigned)shs_static_max_gate);
                return ESP_OK;
            }
            default:
                break;
        }
    }
    return ESP_OK;
}

static esp_err_t shs_zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return shs_zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    }
    return ESP_OK;
}

/* ---------------- Cooldown (moving) ---------------- */
static inline void shs_mv_cooldown_tick(uint32_t now_ms)
{
    if (shs_movement_cooldown_sec == 0 || !shs_mv_cooldown_active) return;

    if (shs_time_reached(now_ms, shs_mv_cooldown_deadline_ms)) {
        if (!shs_last_moving_sample) {
            if (shs_moving_state) {
                shs_moving_state = false;
                ESP_LOGI(SHS_TAG, "Moving Target cooldown end -> CLEAR");
                shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_MOVING_TARGET, false);
            }
            shs_mv_cooldown_active = false;
        } else {
            shs_mv_cooldown_deadline_ms = now_ms + (uint32_t)shs_movement_cooldown_sec * 1000U;
        }
    }
}

/* ---------------- UART task: LD2410 live frames ---------------- */
static void shs_process_sensor_state(uint8_t state_byte)
{
    bool moving   = (state_byte & 0x01) != 0;
    bool stat     = (state_byte & 0x02) != 0;
    bool presence = (state_byte & 0x03) != 0;

    shs_last_moving_sample = moving;

    if (shs_movement_cooldown_sec == 0) {
        if (moving != shs_moving_state) {
            shs_moving_state = moving;
            ESP_LOGI(SHS_TAG, "Moving Target -> %s", moving ? "DETECTED" : "CLEAR");
            shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_MOVING_TARGET, moving);
        }
    } else {
        uint32_t now = esp_log_timestamp();
        if (!shs_mv_cooldown_active) {
            if (moving && !shs_moving_state) {
                shs_moving_state = true;
                ESP_LOGI(SHS_TAG, "Moving Target START -> DETECTED (cooldown %us)", (unsigned)shs_movement_cooldown_sec);
                shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_MOVING_TARGET, true);
                shs_mv_cooldown_active = true;
                shs_mv_cooldown_deadline_ms = now + (uint32_t)shs_movement_cooldown_sec * 1000U;
            }
        } else {
            shs_mv_cooldown_tick(now);
        }
    }

    if (stat != shs_static_state) {
        shs_static_state = stat;
        ESP_LOGI(SHS_TAG, "Static Target -> %s", stat ? "DETECTED" : "CLEAR");
        shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_STATIC_TARGET, stat);
    }

    if (presence != shs_occupancy_state) {
        shs_occupancy_state = presence;
        ESP_LOGI(SHS_TAG, "Occupancy -> %s", presence ? "DETECTED" : "CLEAR");
        shs_zb_set_occ_bitmap(SHS_EP_OCC, presence);
    }
}

static void shs_ld2410_task(void *pvParameters)
{
    uint8_t rxbuf[SHS_UART_BUF_SIZE];
    static uint8_t acc[SHS_UART_ACC_BUF_SIZE];
    size_t start = 0, acc_len = 0;

    for (;;) {
        int len = uart_read_bytes(SHS_LD2410_UART_NUM, rxbuf, sizeof(rxbuf), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            if (start + acc_len + (size_t)len > sizeof(acc)) {
                if (acc_len > 0) memmove(acc, acc + start, acc_len);
                start = 0;
            }
            size_t copy = (size_t)len;
            if (start + acc_len + copy > sizeof(acc)) {
                size_t free_space = sizeof(acc) - (start + acc_len);
                memcpy(acc + start + acc_len, rxbuf, free_space);
                acc_len += free_space;
            } else {
                memcpy(acc + start + acc_len, rxbuf, copy);
                acc_len += copy;
            }

            size_t i = 0;
            while (acc_len - i >= SHS_LD2410_MIN_FRAME_BYTES) {
                size_t h = i;
                for (; h + 4 <= acc_len; ++h) {
                    if (acc[start + h]     == SHS_LD2410_HDR_RX0 &&
                        acc[start + h + 1] == SHS_LD2410_HDR_RX1 &&
                        acc[start + h + 2] == SHS_LD2410_HDR_RX2 &&
                        acc[start + h + 3] == SHS_LD2410_HDR_RX3) {
                        break;
                    }
                }
                if (h + SHS_LD2410_MIN_FRAME_BYTES > acc_len) { i = h; break; }
                if (h >= acc_len) { i = acc_len; break; }

                bool consumed = false;
                if (acc_len - h >= 10) {
                    uint16_t le_len = (uint16_t)acc[start + h + 4] | ((uint16_t)acc[start + h + 5] << 8);
                    size_t total = 4 + 2 + (size_t)le_len + 4;
                    if (h + total <= acc_len) {
                        if (acc[start + h + total - 4] == SHS_LD2410_TAIL_RX0 &&
                            acc[start + h + total - 3] == SHS_LD2410_TAIL_RX1 &&
                            acc[start + h + total - 2] == SHS_LD2410_TAIL_RX2 &&
                            acc[start + h + total - 1] == SHS_LD2410_TAIL_RX3) {

                            shs_process_sensor_state(acc[start + h + 8]);

                            size_t drop = h + total;
                            start += drop;
                            acc_len -= drop;
                            i = 0;
                            consumed = true;
                        }
                    }
                }

                if (!consumed) {
                    shs_process_sensor_state(acc[start + h + 8]);

                    size_t drop = h + SHS_LD2410_MIN_FRAME_BYTES;
                    start += drop;
                    acc_len -= drop;
                    i = 0;
                }
            }
        }

        shs_mv_cooldown_tick(esp_log_timestamp());
    }
}

/* ---------------- BOOT button (factory reset) ---------------- */
static void shs_boot_button_task(void *pv)
{
    const TickType_t poll = pdMS_TO_TICKS(25);
    const uint32_t required_ticks = SHS_FACTORY_RESET_LONGPRESS_MS / 25;
    uint32_t held = 0; bool armed = false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SHS_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    ESP_LOGI(SHS_TAG, "BOOT long-press enabled on GPIO%d (hold %u ms to factory reset Zigbee and rejoin)",
             SHS_BOOT_BUTTON_GPIO, (unsigned)SHS_FACTORY_RESET_LONGPRESS_MS);

    while (1) {
        int level = gpio_get_level(SHS_BOOT_BUTTON_GPIO); /* BOOT pulls to GND when pressed */
        if (level == 0) {
            if (held < required_ticks) held++;
            if (!armed && held > 4) { armed = true; ESP_LOGI(SHS_TAG, "BOOT press detected, hold to confirm..."); }
            if (held >= required_ticks) {
                ESP_LOGW(SHS_TAG, "BOOT long-press confirmed: factory resetting Zigbee state...");
                esp_zb_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else { held = 0; armed = false; }
        vTaskDelay(poll);
    }
}

/* ---------------- Commissioning helper ---------------- */
static void shs_bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    if (esp_zb_bdb_start_top_level_commissioning(mode_mask) != ESP_OK) {
        ESP_LOGW(SHS_TAG, "Failed to start Zigbee commissioning");
    }
}

/* Publish EP1 Basic metadata and set power source (mains) at runtime */
static void shs_basic_publish_metadata_ep1(void)
{
    if (!shs_zb_ready) return;

    const char *date_code = SHS_BASIC_DATE_CODE;
    const char *sw_build  = SHS_BASIC_SW_BUILD_ID;
    uint8_t power_src = 0x01;  // ZCL Basic Power Source: Mains (single phase)

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT,
                                 ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID,
                                 &power_src, false);

    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT,
                                 ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID,
                                 (void *)date_code, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT,
                                 ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
                                 (void *)sw_build, false);

    esp_zb_lock_release();
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(SHS_TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            shs_zb_ready = true;

            /* EP1 Basic metadata + power source (mains) */
            shs_basic_publish_metadata_ep1();

            shs_zb_set_ou_delay_ep2(shs_occupancy_clear_sec);

            shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_MOVING_TARGET, shs_moving_state);
            shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, SHS_ATTR_OCC_STATIC_TARGET, shs_static_state);
            shs_zb_set_occ_bitmap(SHS_EP_OCC, shs_occupancy_state);

            ESP_LOGI(SHS_TAG, "Device started up in%s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(SHS_TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(SHS_TAG, "Device rebooted");
            }
        } else {
            ESP_LOGW(SHS_TAG, "Failed to initialize Zigbee stack (%s)", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(SHS_TAG, "Joined network successfully (ExtPAN:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN:0x%04hx, Ch:%d, Short:0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGW(SHS_TAG, "Network steering not successful (%s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    default:
        ESP_LOGI(SHS_TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ---------------- Zigbee main task: endpoints ---------------- */
static void shs_zigbee_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = SHS_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    zcl_basic_manufacturer_info_t info =
        { .manufacturer_name = SHS_MANUFACTURER_NAME, .model_identifier = SHS_MODEL_IDENTIFIER, };

    esp_zb_ep_list_t *dev_ep_list = esp_zb_ep_list_create();

    /* EP1: genOnOff Light + Custom Config Cluster */
    {
        esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

        esp_zb_on_off_cluster_cfg_t on_off_cfg = { .on_off = ESP_ZB_ZCL_ON_OFF_ON_OFF_DEFAULT_VALUE };
        esp_zb_attribute_list_t *onoff = esp_zb_on_off_cluster_create(&on_off_cfg);

        esp_zb_cluster_list_add_basic_cluster(cl, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_cluster_list_add_identify_cluster(cl, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_cluster_list_add_on_off_cluster(cl, onoff, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        /* Custom Config Cluster (0xFDCD) on EP1 */
        esp_zb_attribute_list_t *cfg_cl = esp_zb_zcl_attr_list_create(SHS_CL_CFG_ID);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVEMENT_COOLDOWN,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_movement_cooldown_sec);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_OCC_CLEAR_COOLDOWN,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_occupancy_clear_sec);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVING_SENS_0_10,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_sens_mv_0_10);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_STATIC_SENS_0_10,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_sens_st_0_10);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVING_MAX_GATE,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_moving_max_gate);

        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_STATIC_MAX_GATE,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                              &shs_static_max_gate);

        esp_zb_cluster_list_add_custom_cluster(cl, cfg_cl, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = SHS_EP_LIGHT,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
            .app_device_version = 0
        };
        esp_zb_ep_list_add_ep(dev_ep_list, cl, ep_cfg);

        /* Attach manufacturer/model on EP1 only */
        esp_zcl_utility_add_ep_basic_manufacturer_info(dev_ep_list, SHS_EP_LIGHT, &info);
    }

    /* EP2: Occupancy Sensor (standard 0x0406) + manufacturer-specific attrs */
    {
        esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
        esp_zb_attribute_list_t *occ = esp_zb_occupancy_sensing_cluster_create(NULL);

        /* Add manufacturer-specific boolean attrs to the standard cluster */
        esp_zb_custom_cluster_add_custom_attr(occ, SHS_ATTR_OCC_MOVING_TARGET,
                                            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &shs_moving_state);
        esp_zb_custom_cluster_add_custom_attr(occ, SHS_ATTR_OCC_STATIC_TARGET,
                                            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &shs_static_state);


        esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = SHS_EP_OCC,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = 0x0107, /* Occupancy Sensor */
            .app_device_version = 0
        };
        esp_zb_ep_list_add_ep(dev_ep_list, cl, ep_cfg);
    }

    /* Register device and start */
    esp_zb_device_register(dev_ep_list);
    esp_zb_core_action_handler_register(shs_zb_action_handler);
    esp_zb_set_primary_network_channel_set(SHS_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ---------------- Save worker task ---------------- */
static void shs_save_worker(void *pv)
{
    TickType_t last_mv_sens = 0, last_st_sens = 0, last_mv_gate = 0, last_st_gate = 0;
    bool pend_mv_sens = false, pend_st_sens = false, pend_mv_gate = false, pend_st_gate = false;
    uint8_t mv_sens_val = shs_moving_sens_0_100, st_sens_val = shs_static_sens_0_100;
    uint8_t mv_gate_val = (uint8_t)shs_moving_max_gate, st_gate_val = (uint8_t)shs_static_max_gate;

    shs_save_msg_t m;
    for (;;) {
        if (xQueueReceive(shs_save_q, &m, pdMS_TO_TICKS(50))) {
            switch (m.type) {
                case SHS_SAVE_IMMEDIATE_U16:
                    if ((m.u16 >> 8) == SHS_ATTR_MOVEMENT_COOLDOWN) {
                        shs_cfg_save_u16(SHS_NVS_KEY_MV_CD, shs_movement_cooldown_sec);
                    } else if ((m.u16 >> 8) == SHS_ATTR_OCC_CLEAR_COOLDOWN) {
                        shs_cfg_save_u16(SHS_NVS_KEY_OCC_CD, shs_occupancy_clear_sec);
                    }
                    break;
                case SHS_SAVE_DEBOUNCE_SENS_MOVE:
                    mv_sens_val = (uint8_t)m.u16; pend_mv_sens = true; last_mv_sens = xTaskGetTickCount();
                    break;
                case SHS_SAVE_DEBOUNCE_SENS_STATIC:
                    st_sens_val = (uint8_t)m.u16; pend_st_sens = true; last_st_sens = xTaskGetTickCount();
                    break;
                case SHS_SAVE_DEBOUNCE_GATE_MOVE:
                    mv_gate_val = (uint8_t)m.u16; pend_mv_gate = true; last_mv_gate = xTaskGetTickCount();
                    break;
                case SHS_SAVE_DEBOUNCE_GATE_STATIC:
                    st_gate_val = (uint8_t)m.u16; pend_st_gate = true; last_st_gate = xTaskGetTickCount();
                    break;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (pend_mv_sens && (now - last_mv_sens) >= pdMS_TO_TICKS(SHS_NVS_DEBOUNCE_MS)) {
            shs_cfg_save_u8(SHS_NVS_KEY_MV_SENS, mv_sens_val);
            pend_mv_sens = false;
        }
        if (pend_st_sens && (now - last_st_sens) >= pdMS_TO_TICKS(SHS_NVS_DEBOUNCE_MS)) {
            shs_cfg_save_u8(SHS_NVS_KEY_ST_SENS, st_sens_val);
            pend_st_sens = false;
        }
        if (pend_mv_gate && (now - last_mv_gate) >= pdMS_TO_TICKS(SHS_NVS_DEBOUNCE_MS)) {
            shs_cfg_save_u8(SHS_NVS_KEY_MV_GATE, mv_gate_val);
            pend_mv_gate = false;
        }
        if (pend_st_gate && (now - last_st_gate) >= pdMS_TO_TICKS(SHS_NVS_DEBOUNCE_MS)) {
            shs_cfg_save_u8(SHS_NVS_KEY_ST_GATE, st_gate_val);
            pend_st_gate = false;
        }
    }
}

/* ---------------- app_main ---------------- */
void app_main(void)
{
    esp_err_t nvs_rc = nvs_flash_init();
    if (nvs_rc == ESP_ERR_NVS_NO_FREE_PAGES || nvs_rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_rc);

    /* Light driver: init immediately at boot */
    shs_deferred_driver_init();

    /* UART init */
    uart_config_t uart_config = {
        .baud_rate = 256000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk= UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SHS_LD2410_UART_NUM, SHS_UART_ACC_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SHS_LD2410_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SHS_LD2410_UART_NUM, SHS_LD2410_UART_TX_PIN, SHS_LD2410_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(SHS_TAG, "LD2410 UART driver initialized");

    /* Load settings & push to LD2410 */
    shs_cfg_load_from_nvs();
    shs_ld2410_apply_global_sensitivity();
    shs_ld2410_apply_params_all();

    /* Save worker (debounce + off-thread writes) */
    shs_save_q = xQueueCreate(8, sizeof(shs_save_msg_t));
    xTaskCreate(shs_save_worker, "shs_save_worker", 3072, NULL, 3, NULL);

    /* Tasks */
    xTaskCreate(shs_ld2410_task,      "shs_ld2410_task", 4096, NULL, 6, NULL);
    xTaskCreate(shs_boot_button_task, "shs_boot_button", 2048, NULL, 4, NULL);
    xTaskCreate(shs_zigbee_task,      "shs_zigbee_main", 4096, NULL, 5, NULL);
}
