/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "includes.h"
#include "common.h"
#include "wpa_supplicant_i.h"
#include "bss.h"

#include "wapi.h"
#include <dlfcn.h>
#include "config.h"
#include "driver_i.h"
#include "eloop.h"
#include "notify.h"
#include "scan.h"
#include "wpa_debug.h"

#ifndef ETH_P_WAI
#define ETH_P_WAI 0x88B4
#endif

#define LIB_FULL_NAME "libwapi.so"

static struct WAPI_CONTEXT_T* wapi_context = NULL;

/* Begin: wrapper definition for wapi interface*/
static int wapi_tx_wai(void* ctx, const u8* pbuf, int length) {
    struct wpa_supplicant* wpa_s = (struct wpa_supplicant*)ctx;
    int ret = -1;

    if (wapi_context && wapi_context->l2_wai)
        ret = l2_packet_send(wapi_context->l2_wai, wpa_s->bssid, ETH_P_WAI, (const u8*)pbuf,
                             (unsigned int)length);

    return ret;
}

static int wapi_get_state_helper(void* ctx) {
    struct wpa_supplicant* wpa_s = (struct wpa_supplicant*)ctx;

    return wpa_s->wpa_state;
}

static void wapi_deauthenticate_helper(void* ctx, int reason_code) {
    struct wpa_supplicant* wpa_s = (struct wpa_supplicant*)ctx;

    wpa_supplicant_deauthenticate(wpa_s, reason_code);
}

static void wapi_set_state_helper(void* ctx, int state) {
    struct wpa_supplicant* wpa_s = (struct wpa_supplicant*)ctx;

    wpa_supplicant_set_state(wpa_s, (enum wpa_states)state);
}

static int wapi_set_key_helper(void* _wpa_s, int alg, const u8* addr, int key_idx, int set_tx,
                               const u8* seq, size_t seq_len, const u8* key, size_t key_len) {
    struct {
        const u8* addr;
        int key_idx;
        int set_tx;
        const u8* seq;
        size_t seq_len;
        const u8* key;
        size_t key_len;
    } wapi_key_param;
    wapi_key_param.addr = addr;
    wapi_key_param.key_idx = key_idx;
    wapi_key_param.set_tx = set_tx;
    wapi_key_param.seq = seq;
    wapi_key_param.seq_len = seq_len;
    wapi_key_param.key = key;
    wapi_key_param.key_len = key_len;

    return wpa_drv_driver_cmd((struct wpa_supplicant*)_wpa_s, "set-wapi-key",
                              (char*)&wapi_key_param, sizeof(wapi_key_param));
}

static int wapi_msg_send_helper(void* priv, const u8* msg_in, int msg_in_len, u8* msg_out,
                                int* msg_out_len) {
    struct {
        const u8* msg_in;
        int msg_in_len;
        u8* msg_out;
        int* msg_out_len;
    } wapi_msg_send_param;

    wapi_msg_send_param.msg_in = msg_in;
    wapi_msg_send_param.msg_in_len = msg_in_len;
    wapi_msg_send_param.msg_out = msg_out;
    wapi_msg_send_param.msg_out_len = msg_out_len;
    return wpa_drv_driver_cmd((struct wpa_supplicant*)priv, "wapi-msg-send",
                              (char*)&wapi_msg_send_param, sizeof(wapi_msg_send_param));
}
/* End: wrapper definition for wapi interface*/

static void wpa_supplicant_rx_wai(void* ctx, const u8* src_addr, const u8* buf, size_t len) {
    struct wpa_supplicant* wpa_s = (struct wpa_supplicant*)ctx;

    /*Drop packets to another interface*/
    if (os_memcmp(wpa_s->bssid, src_addr, ETH_ALEN) != 0) {
        return;
    }

    if (wapi_context && wapi_context->ops.wapi_set_rx_wai != NULL)
        wapi_context->ops.wapi_set_rx_wai(buf, len);
}

int wapi_init_l2(struct wpa_supplicant* wpa_s) {
    if (!wapi_context) {
        wpa_printf(MSG_ERROR, "[WAPI] wapi context is NULL");
        return -1;
    }
    wapi_context->l2_wai = l2_packet_init(wpa_s->ifname, wpa_drv_get_mac_addr(wpa_s), ETH_P_WAI,
                                          wpa_supplicant_rx_wai, wpa_s, 0);

    if (wapi_context->l2_wai && l2_packet_get_own_addr(wapi_context->l2_wai, wpa_s->own_addr)) {
        wpa_printf(MSG_ERROR, "[WAPI] Failed to get own L2 address for WAPI");
        return -1;
    }
    return 0;
}

int wapi_deinit_l2(struct wpa_supplicant* wpa_s) {
    if (wapi_context && wapi_context->l2_wai) {
        l2_packet_deinit(wapi_context->l2_wai);
        wapi_context->l2_wai = NULL;
        return 0;
    } else
        return -1;
}

int wapi_init(struct wpa_supplicant* wpa_s) {
    /*Only support Station mode*/
    if (os_strncmp(wpa_s->ifname, "wlan", 4) != 0) {
        wpa_printf(MSG_DEBUG, "[%s]skip wapi_init", wpa_s->ifname);
        return -1;
    }

    /*Initiated by another interface, do reinit*/
    if (wapi_context) {
        wapi_deinit(wpa_s);
    }

    wapi_context = os_zalloc(sizeof(struct WAPI_CONTEXT_T));

    if (wapi_context == NULL) {
        wpa_msg(wpa_s, MSG_ERROR, "[WAPI] allocate context fail");
        goto error_handle;
    }

    wapi_context->libwapi_handler = dlopen(LIB_FULL_NAME, RTLD_LAZY);

    if (wapi_context->libwapi_handler == NULL) {
        wpa_msg(wpa_s, MSG_ERROR, "[WAPI] dlopen %s failed:%s", LIB_FULL_NAME, dlerror());
        goto error_handle;
    }

    // reset errors
    dlerror();

    wapi_init_api(wpa_s);

    if (wapi_context->ops.wapi_lib_init != NULL &&
        wapi_context->ops.wapi_lib_init(&wapi_context->ctx_cb) != 0) {
        wpa_msg(wpa_s, MSG_ERROR, "[WAPI] wapi_lib_init fail");
        goto error_handle;
    }

    if (wapi_init_l2(wpa_s) < 0) {
        wpa_msg(wpa_s, MSG_ERROR, "[WAPI] wapi_init_l2 fail");
        goto error_handle;
    }
    wpa_msg(wpa_s, MSG_DEBUG, "[WAPI] wapi_init success");
    return 0;

error_handle:
    wapi_deinit(wpa_s);
    return -1;
}

int wapi_deinit(struct wpa_supplicant* wpa_s) {
    if (wpa_s) wapi_deinit_l2(wpa_s);

    if (wapi_context && wapi_context->ops.wapi_lib_exit) {
        wapi_context->ops.wapi_lib_exit();
        wapi_context->ops.wapi_lib_exit = NULL;
    }

    if (wapi_context && wapi_context->libwapi_handler) {
        dlclose(wapi_context->libwapi_handler);
        wapi_context->libwapi_handler = NULL;
    }

    if (wapi_context) {
        os_free(wapi_context);
        wapi_context = NULL;
    }

    wpa_msg(wpa_s, MSG_INFO, "[WAPI] wapi_deinit done");

    return 0;
}

int wapi_set_suites(struct wpa_supplicant* wpa_s, struct wpa_ssid* ssid, struct wpa_bss* bss,
                    u8* wpa_ie, size_t* wpa_ie_len) {
    CNTAP_PARA wapi_param;
    int wapi_set_param_result = 0;
    const u8* ie = NULL;
    if (wapi_init(wpa_s) < 0) return -1;

    wpa_s->wpa_proto = ssid->proto;
    wpa_s->key_mgmt = ssid->key_mgmt;
    /* Always use SMS4 as pairwise and group cipher suite */
    wpa_s->pairwise_cipher = WPA_CIPHER_SMS4;
    wpa_s->group_cipher = WPA_CIPHER_SMS4;
    os_memset(&wapi_param, 0, sizeof(wapi_param));
    if (wpa_s->key_mgmt == WPA_KEY_MGMT_WAPI_PSK) {
        /* it's not PMK actually but string */
        wapi_param.authType = AUTH_TYPE_WAPI_PSK;

        if (!ssid->passphrase) {
            wpa_printf(MSG_ERROR, "[WAPI] ssid->passphrase is NULL");
            return -1;
        }
        /* Align with AOSP psk style:
         * PSK can either be quoted ASCII passphrase or hex string for raw psk
         */
        if (ssid->passphrase[0] == '\"') {
            wapi_param.kt = KEY_TYPE_ASCII;
            wapi_param.kl = strlen((char*)ssid->passphrase) - 2;
            memcpy(wapi_param.kv, &ssid->passphrase[1], wapi_param.kl);
            wpa_printf(MSG_INFO, "[WAPI] Using Key mgmt WAPI Ascii PSK");
        } else {
            wapi_param.kt = KEY_TYPE_HEX;
            wapi_param.kl = strlen((char*)ssid->passphrase);
            memcpy(wapi_param.kv, ssid->passphrase, wapi_param.kl);
            wpa_printf(MSG_INFO, "[WAPI] Using Key mgmt WAPI Hex PSK");
        }
        wapi_param.kv[wapi_param.kl] = '\0';
    } else if (wpa_s->key_mgmt == WPA_KEY_MGMT_WAPI_CERT) {
        if (!ssid->wapi_cert_alias || !os_strcmp((char*)ssid->wapi_cert_alias, "NULL"))
            wapi_param.cert_list = NULL;
        else
            wapi_param.cert_list = (char*)ssid->wapi_cert_alias;
        wapi_param.authType = AUTH_TYPE_WAPI_CERT;
        wpa_printf(MSG_INFO, "[WAPI] Using Key mgmt WAPI Cert");
    }

    wapi_set_param_result = wapi_context->ops.wapi_set_user(&wapi_param);
    if (wapi_set_param_result < 0) {
        wpa_printf(MSG_WARNING, "[WAPI] wapi_set_user result %d", wapi_set_param_result);
        return -1;
    }

    /*
     * fill wapi ie for associate request
     */
    if (bss) ie = wpa_bss_get_ie(bss, WLAN_EID_WAPI);
    if (ie && ie[1] && wapi_context) {
        wapi_context->bss_wapi_ie_len = 2 + ie[1];
        wapi_context->bss_wapi_ie = ie;
    }
    *wpa_ie_len = wapi_context->ops.wapi_get_assoc_ie(wpa_ie);

    if (*wpa_ie_len <= 0) {
        wpa_printf(MSG_DEBUG, "[WAPI] Failed to generate WAPI IE.");
        return -1;
    }

    return 0;
}

void wapi_event_assoc(struct wpa_supplicant* wpa_s) {
    if (wapi_context == NULL || wapi_context->libwapi_handler == NULL) {
        return;
    }

    MAC_ADDRESS bssid_s;
    MAC_ADDRESS own_s;

    /*
     * stop WPA and other time out use WAPI time only
     */
    wpa_supplicant_cancel_auth_timeout(wpa_s);
    wpa_supplicant_cancel_scan(wpa_s);

    wpa_printf(MSG_DEBUG, "[WAPI] Associated, Bssid:" MACSTR " Own:" MACSTR, MAC2STR(wpa_s->bssid),
               MAC2STR(wpa_s->own_addr));

    if (is_zero_ether_addr(wpa_s->bssid)) {
        wpa_printf(MSG_DEBUG, "[WAPI] Zero bssid, Not to set msg to WAPI SM\n");
        /*
         * Have been disassociated with the WAPI AP
         */
        return;
    }

    memcpy(bssid_s.v, wpa_s->bssid, sizeof(bssid_s.v));
    memcpy(own_s.v, wpa_s->own_addr, sizeof(own_s.v));
    if (wapi_context->ops.wapi_set_msg)
        wapi_context->ops.wapi_set_msg(CONN_ASSOC, &bssid_s, &own_s, wapi_context->bss_wapi_ie,
                                       wapi_context->bss_wapi_ie_len);
}

void wapi_event_disassoc(struct wpa_supplicant* wpa_s, const u8* bssid) {
    if (wapi_context == NULL || wapi_context->libwapi_handler == NULL) {
        return;
    }

    if (!bssid) {
        wpa_printf(MSG_DEBUG, "[WAPI] wapi_event_disassoc, bssid is NULL!");
        return;
    }
    if (wpa_s->wpa_state == WPA_COMPLETED) wapi_deinit(wpa_s);
}

void wapi_init_api(struct wpa_supplicant* wpa_s) {
    void* libwapi_handle = wapi_context->libwapi_handler;

    if (libwapi_handle == NULL) {
        return;
    }

    wapi_context->ops.wapi_set_rx_wai = dlsym(libwapi_handle, "wapi_set_rx_wai");
    wapi_context->ops.wapi_lib_init = dlsym(libwapi_handle, "wapi_lib_init");
    wapi_context->ops.wapi_lib_exit = dlsym(libwapi_handle, "wapi_lib_exit");
    wapi_context->ops.wapi_set_user = dlsym(libwapi_handle, "wapi_set_user");
    wapi_context->ops.wapi_get_assoc_ie = dlsym(libwapi_handle, "wapi_get_assoc_ie");
    wapi_context->ops.wapi_set_msg = dlsym(libwapi_handle, "wapi_set_msg");

    wapi_context->ctx_cb.ctx = wpa_s;
    wapi_context->ctx_cb.mtu_len = 1500;
    wapi_context->ctx_cb.msg_send = wapi_msg_send_helper;
    wapi_context->ctx_cb.wpa_msg = wpa_msg;
    wapi_context->ctx_cb.get_state = wapi_get_state_helper;
    wapi_context->ctx_cb.deauthenticate = wapi_deauthenticate_helper;
    wapi_context->ctx_cb.ether_send = wapi_tx_wai;
    wapi_context->ctx_cb.set_key = wapi_set_key_helper;
    wapi_context->ctx_cb.set_state = wapi_set_state_helper;
    wapi_context->ctx_cb.cancel_timer = eloop_cancel_timeout;
    wapi_context->ctx_cb.set_timer = eloop_register_timeout;
    wapi_context->ctx_cb.get_certificate = wpas_get_certificate;

    wpa_msg(wpa_s, MSG_DEBUG, "[WAPI] Dlopen wapi APIs success");
}
