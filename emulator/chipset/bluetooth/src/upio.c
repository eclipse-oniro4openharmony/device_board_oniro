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
 *  Filename:      upio.c
 *
 *  Description:   Contains I/O functions, like
 *                      rfkill control
 *                      BT_WAKE/HOST_WAKE control
 *
 ******************************************************************************/

#define LOG_TAG "bt_upio"

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <utils/Log.h>
#include "bt_vendor_brcm.h"
#include "userial_vendor.h"
#include "upio.h"

/******************************************************************************
**  Constants & Macros
******************************************************************************/

#ifndef UPIO_DBG
#define UPIO_DBG FALSE
#endif

#if (UPIO_DBG == TRUE)
#define UPIODBG(param, ...)         \
    {                               \
        HILOGD(param, ##__VA_ARGS__); \
    }
#else
#define UPIODBG(param, ...)         \
    {                               \
        HILOGD(param, ##__VA_ARGS__); \
    }
#endif

/******************************************************************************
**  Local type definitions
******************************************************************************/

#if (BT_WAKE_VIA_PROC == TRUE)

/* proc fs node for enable/disable lpm mode */
#ifndef VENDOR_LPM_PROC_NODE
#define VENDOR_LPM_PROC_NODE "/proc/bluetooth/sleep/lpm"
#endif

/* proc fs node for notifying write request */
#ifndef VENDOR_BTWRITE_PROC_NODE
#define VENDOR_BTWRITE_PROC_NODE "/proc/bluetooth/sleep/btwrite"
#endif

/*
 * Maximum btwrite assertion holding time without consecutive btwrite kicking.
 * This value is correlative(shorter) to the in-working timeout period set in
 * the bluesleep LPM code. The current value used in bluesleep is 10sec.
 */
#ifndef PROC_BTWRITE_TIMER_TIMEOUT_MS
#define PROC_BTWRITE_TIMER_TIMEOUT_MS 8000
#endif

/* lpm proc control block */
typedef struct {
    uint8_t btwrite_active;
    uint8_t timer_created;
    timer_t timer_id;
    uint32_t timeout_ms;
} vnd_lpm_proc_cb_t;

static vnd_lpm_proc_cb_t lpm_proc_cb;
#endif

/******************************************************************************
**  Static variables
******************************************************************************/

static uint8_t upio_state[UPIO_MAX_COUNT];
static int rfkill_id = -1;
static int bt_emul_enable = 0;
static char rfkill_state_path[128];

/******************************************************************************
**  Static functions
******************************************************************************/

/* for friendly debugging outpout string */
static char *lpm_mode[] = {
    "UNKNOWN",
    "disabled",
    "enabled"
};

static char *lpm_state[] = {
    "UNKNOWN",
    "de-asserted",
    "asserted"
};

/*****************************************************************************
**   Bluetooth On/Off Static Functions
*****************************************************************************/
static int is_emulator_context(void)
{
    return 0;
}

static int is_rfkill_disabled(void)
{
    return UPIO_BT_POWER_OFF;
}

static int init_rfkill(void)
{
    char path[64];
    char buf[16];
    int fd, sz, id;

    for (id = 0;; id++) {
        if (snprintf_s(path, sizeof(path), sizeof(path), "/sys/class/rfkill/rfkill%d/type", id) < 0) {
            return -1;
        }
        
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            HILOGE("init_rfkill : open(%s) failed: %s (%d)\n",
                path, strerror(errno), errno);
            return -1;
        }

        sz = read(fd, &buf, sizeof(buf));
        close(fd);

        if (sz >= (int)strlen("bluetooth") && memcmp(buf, "bluetooth", strlen("bluetooth")) == 0) {
            rfkill_id = id;
            break;
        }
    }

    (void)sprintf_s(rfkill_state_path, sizeof(rfkill_state_path), "/sys/class/rfkill/rfkill%d/state", rfkill_id);
    return 0;
}

/*****************************************************************************
**   LPM Static Functions
*****************************************************************************/

#if (BT_WAKE_VIA_PROC == TRUE)
/*******************************************************************************
**
** Function        proc_btwrite_timeout
**
** Description     Timeout thread of proc/.../btwrite assertion holding timer
**
** Returns         None
**
*******************************************************************************/
static void proc_btwrite_timeout(union sigval arg)
{
    UPIODBG("..%s..", __FUNCTION__);
    lpm_proc_cb.btwrite_active = FALSE;
    /* drive LPM down; this timer should fire only when BT is awake; */
    upio_set(UPIO_BT_WAKE, UPIO_DEASSERT, 1);
}

/******************************************************************************
 **
 ** Function      upio_start_stop_timer
 **
 ** Description   Arm user space timer in case lpm is left asserted
 **
 ** Returns       None
 **
 *****************************************************************************/
void upio_start_stop_timer(int action)
{
    struct itimerspec ts;

    if (action == UPIO_ASSERT) {
        lpm_proc_cb.btwrite_active = TRUE;
        if (lpm_proc_cb.timer_created == TRUE) {
            ts.it_value.tv_sec = PROC_BTWRITE_TIMER_TIMEOUT_MS / BT_VENDOR_TIME_RAIDX;
            ts.it_value.tv_nsec = BT_VENDOR_TIME_RAIDX * BT_VENDOR_TIME_RAIDX *
                (PROC_BTWRITE_TIMER_TIMEOUT_MS % BT_VENDOR_TIME_RAIDX);
            ts.it_interval.tv_sec = 0;
            ts.it_interval.tv_nsec = 0;
        }
    } else {
        /* unarm timer if writing 0 to lpm; reduce unnecessary user space wakeup */
        (void)memset_s(&ts, sizeof(ts), 0, sizeof(ts));
    }

    if (timer_settime(lpm_proc_cb.timer_id, 0, &ts, 0) == 0) {
        UPIODBG("%s : timer_settime success", __FUNCTION__);
    } else {
        UPIODBG("%s : timer_settime failed", __FUNCTION__);
    }
}
#endif

/*****************************************************************************
**   UPIO Interface Functions
*****************************************************************************/

/*******************************************************************************
**
** Function        upio_init
**
** Description     Initialization
**
** Returns         None
**
*******************************************************************************/
void upio_init(void)
{
    memset_s(upio_state, sizeof(upio_state), UPIO_UNKNOWN, UPIO_MAX_COUNT);
#if (BT_WAKE_VIA_PROC == TRUE)
    memset_s(&lpm_proc_cb, sizeof(vnd_lpm_proc_cb_t), 0, sizeof(vnd_lpm_proc_cb_t));
#endif
}

/*******************************************************************************
**
** Function        upio_cleanup
**
** Description     Clean up
**
** Returns         None
**
*******************************************************************************/
void upio_cleanup(void)
{
#if (BT_WAKE_VIA_PROC == TRUE)
    if (lpm_proc_cb.timer_created == TRUE)
        timer_delete(lpm_proc_cb.timer_id);

    lpm_proc_cb.timer_created = FALSE;
#endif
}

/*******************************************************************************
**
** Function        upio_set_bluetooth_power
**
** Description     Interact with low layer driver to set Bluetooth power
**                 on/off.
**
** Returns         0  : SUCCESS or Not-Applicable
**                 <0 : ERROR
**
*******************************************************************************/
int upio_set_bluetooth_power(int on)
{
    int sz;
    int fd = -1;
    int ret = -1;
    char buffer = '0';

    switch (on) {
        case UPIO_BT_POWER_OFF:
            buffer = '0';
            break;

        case UPIO_BT_POWER_ON:
            buffer = '1';
            break;
        default:
            return 0;
    }

    if (is_emulator_context()) {
        /* if new value is same as current, return -1 */
        if (bt_emul_enable == on) {
            return ret;
        }

        UPIODBG("set_bluetooth_power [emul] %d", on);
        bt_emul_enable = on;
        return 0;
    }

    /* check if we have rfkill interface */
    if (is_rfkill_disabled()) {
        return 0;
    }

    if (rfkill_id == -1) {
        if (init_rfkill()) {
            return ret;
        }
    }

    fd = open(rfkill_state_path, O_WRONLY);
    if (fd < 0) {
        HILOGE("set_bluetooth_power : open(%s) for write failed: %s (%d)",
            rfkill_state_path, strerror(errno), errno);
        return ret;
    }

    sz = write(fd, &buffer, 1);
    if (sz < 0) {
        HILOGE("set_bluetooth_power : write(%s) failed: %s (%d)",
            rfkill_state_path, strerror(errno), errno);
    } else {
        ret = 0;
    }

    if (fd >= 0) {
        close(fd);
    }

    return ret;
}

/*******************************************************************************
**
** Function        upio_set
**
** Description     Set i/o based on polarity
**
** Returns         None
**
*******************************************************************************/
void upio_set(uint8_t pio, uint8_t action, uint8_t polarity)
{
    int rc;
#if (BT_WAKE_VIA_PROC == TRUE)
    int fd = -1;
    char buffer;
#endif

    UPIODBG("%s : pio %d action %d, polarity %d", __FUNCTION__, pio, action, polarity);
    switch (pio) {
        case UPIO_LPM_MODE:
            if (upio_state[UPIO_LPM_MODE] == action) {
                UPIODBG("LPM is %s already", lpm_mode[action]);
                return;
            }

            upio_state[UPIO_LPM_MODE] = action;

#if (BT_WAKE_VIA_PROC == TRUE)
            fd = open(VENDOR_LPM_PROC_NODE, O_WRONLY);
            if (fd < 0) {
                LOGE("upio_set : open(%s) for write failed: %s (%d)",
                VENDOR_LPM_PROC_NODE, strerror(errno), errno);
                return;
            }

            if (action == UPIO_ASSERT) {
                buffer = '1';
            } else {
                buffer = '0';

                // delete btwrite assertion holding timer
                if (lpm_proc_cb.timer_created == TRUE) {
                    timer_delete(lpm_proc_cb.timer_id);
                    lpm_proc_cb.timer_created = FALSE;
                }
            }

            if (write(fd, &buffer, 1) < 0) {
                LOGE("upio_set : write(%s) failed: %s (%d)",
                VENDOR_LPM_PROC_NODE, strerror(errno),errno);
            }
#if (PROC_BTWRITE_TIMER_TIMEOUT_MS != 0)
            else {
                if (action == UPIO_ASSERT) {
                    // create btwrite assertion holding timer
                    if (lpm_proc_cb.timer_created == FALSE) {
                        int status;
                        struct sigevent se;

                        se.sigev_notify = SIGEV_THREAD;
                        se.sigev_value.sival_ptr = &lpm_proc_cb.timer_id;
                        se.sigev_notify_function = proc_btwrite_timeout;
                        se.sigev_notify_attributes = NULL;

                        status = timer_create(CLOCK_MONOTONIC, &se,
                                              &lpm_proc_cb.timer_id);
                        if (status == 0)
                            lpm_proc_cb.timer_created = TRUE;
                    }
                }
            }
#endif

            if (fd >= 0)
                close(fd);
#endif
            break;
            
        case UPIO_BT_WAKE:
            if (upio_state[UPIO_BT_WAKE] == action) {
                UPIODBG("BT_WAKE is %s already", lpm_state[action]);

#if (BT_WAKE_VIA_PROC == TRUE)
                if (lpm_proc_cb.btwrite_active == TRUE)
                    /*
                     * The proc btwrite node could have not been updated for
                     * certain time already due to heavy downstream path flow.
                     * In this case, we want to explicity touch proc btwrite
                     * node to keep the bt_wake assertion in the LPM kernel
                     * driver. The current kernel bluesleep LPM code starts
                     * a 10sec internal in-activity timeout timer before it
                     * attempts to deassert BT_WAKE line.
                     */
                    return;
#else
                return;
#endif
            }

            upio_state[UPIO_BT_WAKE] = action;

#if (BT_WAKE_VIA_USERIAL_IOCTL == TRUE)

            userial_vendor_ioctl( ( (action==UPIO_ASSERT) ? \
                      USERIAL_OP_ASSERT_BT_WAKE : USERIAL_OP_DEASSERT_BT_WAKE),\
                                  NULL);

#elif (BT_WAKE_VIA_PROC == TRUE)

            /*
             *  Kick proc btwrite node only at UPIO_ASSERT
             */
#if (BT_WAKE_VIA_PROC_NOTIFY_DEASSERT == FALSE)
            if (action == UPIO_DEASSERT)
                return;
#endif
            fd = open(VENDOR_BTWRITE_PROC_NODE, O_WRONLY);
            if (fd < 0) {
                LOGE("upio_set : open(%s) for write failed: %s (%d)",
                VENDOR_BTWRITE_PROC_NODE, strerror(errno), errno);
                return;
            }
#if (BT_WAKE_VIA_PROC_NOTIFY_DEASSERT == TRUE)
            if (action == UPIO_DEASSERT)
                buffer = '0';
            else
#endif
                buffer = '1';

            if (write(fd, &buffer, 1) < 0) {
                LOGE("upio_set : write(%s) failed: %s (%d)",
                VENDOR_BTWRITE_PROC_NODE, strerror(errno), errno);
            }
#if (PROC_BTWRITE_TIMER_TIMEOUT_MS != 0)
            else {
                /* arm user space timer based on action */
                upio_start_stop_timer(action);
            }
#endif

#if (BT_WAKE_VIA_PROC_NOTIFY_DEASSERT == TRUE)
            lpm_proc_cb.btwrite_active = TRUE;
#endif

            UPIODBG("%s: proc btwrite assertion, buffer: %c, timer_armed %d %d",
                    __FUNCTION__, buffer, lpm_proc_cb.btwrite_active, lpm_proc_cb.timer_created);

            if (fd >= 0)
                close(fd);
#endif

            break;

        case UPIO_HOST_WAKE:
            UPIODBG("upio_set: UPIO_HOST_WAKE");
            break;
    }
}
