/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 */

#ifndef SHS01_H
#define SHS01_H

#include "esp_zigbee_core.h"
#include "driver/gpio.h"
#include "driver/uart.h"

/* ---------------- Zigbee device & endpoints ---------------- */
#define SHS_MAX_CHILDREN                10
#define SHS_INSTALLCODE_POLICY_ENABLE   false

/* Endpoints */
#define SHS_EP_LIGHT                    1   /* genOnOff Light + Config */
#define SHS_EP_OCC                      2   /* Occupancy Sensing cluster (moving, static, overall) */

/* Router device config */
#define SHS_ZR_CONFIG()                                         \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,               \
        .install_code_policy = SHS_INSTALLCODE_POLICY_ENABLE,   \
        .nwk_cfg.zczr_cfg = {                                   \
            .max_children = SHS_MAX_CHILDREN,                   \
        },                                                      \
    }

/* Channels */
#define SHS_PRIMARY_CHANNEL_MASK        ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/* Manufacturer / Model strings for Basic cluster (length-prefixed ASCII) */
#define SHS_MANUFACTURER_NAME           "\x0E""SmartHomeScene"
#define SHS_MODEL_IDENTIFIER            "\x05""SHS01"

/* Optional Basic metadata (length-prefixed ZCL char strings) */
#define SHS_BASIC_DATE_CODE             "\x0A""2025-08-29"      /* YYYY-MM-DD (10) */
#define SHS_BASIC_SW_BUILD_ID           "\x0B""SHS01-1.0.0"     /* adjust as needed */

/* ---------------- LD2410C UART pins ---------------- */
#define SHS_LD2410_UART_NUM             (UART_NUM_1)
#define SHS_LD2410_UART_RX_PIN          (GPIO_NUM_4)
#define SHS_LD2410_UART_TX_PIN          (GPIO_NUM_5)

/* Increase buffers for robustness under bursty frames */
#define SHS_UART_BUF_SIZE               (512)   /* per read() temp */
#define SHS_UART_ACC_BUF_SIZE           (1024)  /* accumulator size */

/* ---------------- LD2410C constants ---------------- */
#define SHS_LD2410_HDR_TX0              0xFD
#define SHS_LD2410_HDR_TX1              0xFC
#define SHS_LD2410_HDR_TX2              0xFB
#define SHS_LD2410_HDR_TX3              0xFA
#define SHS_LD2410_TAIL_TX0             0x04
#define SHS_LD2410_TAIL_TX1             0x03
#define SHS_LD2410_TAIL_TX2             0x02
#define SHS_LD2410_TAIL_TX3             0x01

#define SHS_LD2410_HDR_RX0              0xF4
#define SHS_LD2410_HDR_RX1              0xF3
#define SHS_LD2410_HDR_RX2              0xF2
#define SHS_LD2410_HDR_RX3              0xF1
#define SHS_LD2410_TAIL_RX0             0xF8
#define SHS_LD2410_TAIL_RX1             0xF7
#define SHS_LD2410_TAIL_RX2             0xF6
#define SHS_LD2410_TAIL_RX3             0xF5

#define SHS_LD2410_MIN_FRAME_BYTES      9

/* Commands */
#define SHS_LD2410_CMD_BEGIN_CONFIG     0x00FF
#define SHS_LD2410_CMD_SET_PARAMS       0x0060
#define SHS_LD2410_CMD_SET_SENSITIVITY  0x0064
#define SHS_LD2410_CMD_END_CONFIG       0x00FE

/* Parameters */
#define SHS_LD2410_PW_MAX_MOVE_GATE     0x0000
#define SHS_LD2410_PW_MAX_STATIC_GATE   0x0001
#define SHS_LD2410_PW_NO_ONE_DURATION   0x0002
#define SHS_LD2410_GATE_ALL             0xFFFF

/* ---------------- Custom Config Cluster ---------------- */
#define SHS_CL_CFG_ID                   0xFDCD

#define SHS_ATTR_MOVEMENT_COOLDOWN      0x0001
#define SHS_ATTR_OCC_CLEAR_COOLDOWN     0x0002
#define SHS_ATTR_MOVING_SENS_0_10       0x0003
#define SHS_ATTR_STATIC_SENS_0_10       0x0004
#define SHS_ATTR_MOVING_MAX_GATE        0x0005
#define SHS_ATTR_STATIC_MAX_GATE        0x0006

/* ---------------- Occupancy custom attributes ---------------- */
#define SHS_ATTR_OCC_MOVING_TARGET      0xF001
#define SHS_ATTR_OCC_STATIC_TARGET      0xF002

/* ---------------- Occupancy Sensing cluster optional attr IDs ---------------- */
#define SHS_ZCL_ATTR_OCC_PIR_OU_DELAY   0x0010

/* ---------------- BOOT button ---------------- */
#define SHS_BOOT_BUTTON_GPIO            GPIO_NUM_9
#define SHS_FACTORY_RESET_LONGPRESS_MS  6000

/* ---------------- Debounce ---------------- */
#define SHS_NVS_DEBOUNCE_MS             500
#define SHS_COOLDOWN_MAX_SEC            300

#endif /* SHS01_H */
