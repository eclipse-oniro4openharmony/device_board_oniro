/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  Filename:      bt_vendor_brcm.c
 *
 *  Description:   Broadcom vendor specific library implementation
 *
 ******************************************************************************/

#define LOG_TAG "bt_vendor"

#include <utils/Log.h>
#include <string.h>
#include "upio.h"
#include "userial_vendor.h"
#include "bt_vendor_brcm.h"

#ifndef BTVND_DBG
#define BTVND_DBG FALSE
#endif

#if (BTVND_DBG == TRUE)
#define BTVNDDBG(param, ...)        \
    {                               \
        HILOGD(param, ##__VA_ARGS__); \
    }
#else
#define BTVNDDBG(param, ...)        \
    {                               \
        HILOGD(param, ##__VA_ARGS__); \
    }
#endif

/******************************************************************************
**  Externs
******************************************************************************/

void hw_config_start(void);
uint8_t hw_lpm_enable(uint8_t turn_on);
uint32_t hw_lpm_get_idle_timeout(void);
void hw_lpm_set_wake_state(uint8_t wake_assert);
#if (SCO_CFG_INCLUDED == TRUE)
void hw_sco_config(void);
#endif
void vnd_load_conf(const char *p_path);
#if (HW_END_WITH_HCI_RESET == TRUE)
void hw_epilog_process(void);
#endif

/******************************************************************************
**  Variables
******************************************************************************/

bt_vendor_callbacks_t *bt_vendor_cbacks = NULL;
uint8_t vnd_local_bd_addr[BD_ADDR_LEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/******************************************************************************
**  Local type definitions
******************************************************************************/

/******************************************************************************
**  Static Variables
******************************************************************************/

static const tUSERIAL_CFG userial_init_cfg = {
    (USERIAL_DATABITS_8 | USERIAL_PARITY_NONE | USERIAL_STOPBITS_1),
    USERIAL_BAUD_115200
};

/******************************************************************************
**  Functions
******************************************************************************/

/*****************************************************************************
**
**   BLUETOOTH VENDOR INTERFACE LIBRARY FUNCTIONS
**
*****************************************************************************/
/** LPM disable/enable request */
typedef enum {
    BT_VND_LPM_DISABLE,
    BT_VND_LPM_ENABLE,
} bt_vendor_lpm_mode_t;

static int init(const bt_vendor_callbacks_t *p_cb, unsigned char *local_bdaddr)
{
    HILOGI("init, bdaddr:%02x%02x:%02x%02x:%02x%02x", local_bdaddr[0], local_bdaddr[1], local_bdaddr[2],
        local_bdaddr[3], local_bdaddr[4], local_bdaddr[5]);

    if (p_cb == NULL) {
        HILOGE("init failed with no user callbacks!");
        return -1;
    }

#if (VENDOR_LIB_RUNTIME_TUNING_ENABLED == TRUE)
    HILOGW("*****************************************************************");
    HILOGW("*****************************************************************");
    HILOGW("** Warning - BT Vendor Lib is loaded in debug tuning mode!");
    HILOGW("**");
    HILOGW("** If this is not intentional, rebuild libbt-vendor.so ");
    HILOGW("** with VENDOR_LIB_RUNTIME_TUNING_ENABLED=FALSE and ");
    HILOGW("** check if any run-time tuning parameters needed to be");
    HILOGW("** carried to the build-time configuration accordingly.");
    HILOGW("*****************************************************************");
    HILOGW("*****************************************************************");
#endif

    userial_vendor_init();
    upio_init();

    vnd_load_conf(VENDOR_LIB_CONF_FILE);

    /* store reference to user callbacks */
    bt_vendor_cbacks = (bt_vendor_callbacks_t *)p_cb;

#if (BRCM_A2DP_OFFLOAD == TRUE)
    brcm_vnd_a2dp_init(bt_vendor_cbacks);
#endif

    /* This is handed over from the stack */
    return memcpy_s(vnd_local_bd_addr, BD_ADDR_LEN, local_bdaddr, BD_ADDR_LEN);
}

/** Requested operations */
static int op(bt_opcode_t opcode, void *param)
{
    int retval = 0;
    
    switch (opcode) {
        case BT_OP_POWER_ON: // BT_VND_OP_POWER_CTRL
            upio_set_bluetooth_power(UPIO_BT_POWER_OFF);
            upio_set_bluetooth_power(UPIO_BT_POWER_ON);
            break;

        case BT_OP_POWER_OFF: // BT_VND_OP_POWER_CTRL
            upio_set_bluetooth_power(UPIO_BT_POWER_OFF);
            hw_lpm_set_wake_state(false);
            break;

        case BT_OP_HCI_CHANNEL_OPEN: { // BT_VND_OP_USERIAL_OPEN
            int(*fd_array)[] = (int(*)[])param;
            int fd;
            fd = userial_vendor_open((tUSERIAL_CFG *)&userial_init_cfg);
            if (fd != -1) {
                for (int idx = 0; idx < HCI_MAX_CHANNEL; idx++)
                    (*fd_array)[idx] = fd;

                retval = 1;
            }
            /* retval contains numbers of open fd of HCI channels */
            break;
        }
        case BT_OP_HCI_CHANNEL_CLOSE: // BT_VND_OP_USERIAL_CLOSE
            userial_vendor_close();
            break;

        case BT_OP_INIT: // BT_VND_OP_FW_CFG
            hw_config_start();
            break;

        case BT_OP_GET_LPM_TIMER: { // BT_VND_OP_GET_LPM_IDLE_TIMEOUT
            uint32_t *timeout_ms = (uint32_t *)param;
            *timeout_ms = hw_lpm_get_idle_timeout();
            break;
        }

        case BT_OP_LPM_ENABLE:
            retval = hw_lpm_enable(BT_VND_LPM_ENABLE);
            break;

        case BT_OP_LPM_DISABLE:
            retval = hw_lpm_enable(BT_VND_LPM_DISABLE);
            break;

        case BT_OP_WAKEUP_LOCK:
            hw_lpm_set_wake_state(TRUE);
            break;
        case BT_OP_WAKEUP_UNLOCK:
            hw_lpm_set_wake_state(FALSE);
            break;
        case BT_OP_EVENT_CALLBACK:
            hw_process_event((HC_BT_HDR *)param);
            break;
    }

    return retval;
}

/** Closes the interface */
static void cleanup(void)
{
    BTVNDDBG("cleanup");
    upio_cleanup();
    bt_vendor_cbacks = NULL;
}

// Entry point of DLib
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    init,
    op,
    cleanup};
