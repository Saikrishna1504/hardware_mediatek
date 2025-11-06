/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __WIFI_RTT_H__
#define __WIFI_RTT_H__

#include "common.h"
#include "cpp_bindings.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern wifi_error wifi_rtt_range_request(wifi_request_id id, wifi_interface_handle iface,
                                         unsigned num_rtt_config, wifi_rtt_config rtt_config[],
                                         wifi_rtt_event_handler handler);
extern wifi_error wifi_rtt_range_cancel(wifi_request_id id, wifi_interface_handle iface,
                                        unsigned num_devices, mac_addr addr[]);
extern wifi_error wifi_get_rtt_capabilities(wifi_interface_handle iface,
                                            wifi_rtt_capabilities* capabilities);

typedef enum {

    RTT_SUBCMD_SET_CONFIG = ANDROID_NL80211_SUBCMD_RTT_RANGE_START,
    RTT_SUBCMD_CANCEL_CONFIG,
    RTT_SUBCMD_GETCAPABILITY,
    RTT_SUBCMD_GETAVAILCHANNEL,
    RTT_SUBCMD_SET_RESPONDER,
    RTT_SUBCMD_CANCEL_RESPONDER,
} RTT_SUB_COMMAND;

typedef enum {
    RTT_ATTRIBUTE_TARGET_INVALID = 0,
    RTT_ATTRIBUTE_TARGET_CNT = 1,
    RTT_ATTRIBUTE_TARGET_INFO = 2,
    RTT_ATTRIBUTE_TARGET_MAC = 3,
    RTT_ATTRIBUTE_TARGET_TYPE = 4,
    RTT_ATTRIBUTE_TARGET_PEER = 5,
    RTT_ATTRIBUTE_TARGET_CHAN = 6,
    RTT_ATTRIBUTE_TARGET_PERIOD = 7,
    RTT_ATTRIBUTE_TARGET_NUM_BURST = 8,
    RTT_ATTRIBUTE_TARGET_NUM_FTM_BURST = 9,
    RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTM = 10,
    RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTMR = 11,
    RTT_ATTRIBUTE_TARGET_LCI = 12,
    RTT_ATTRIBUTE_TARGET_LCR = 13,
    RTT_ATTRIBUTE_TARGET_BURST_DURATION = 14,
    RTT_ATTRIBUTE_TARGET_PREAMBLE = 15,
    RTT_ATTRIBUTE_TARGET_BW = 16,
    RTT_ATTRIBUTE_RESULTS_COMPLETE = 30,
    RTT_ATTRIBUTE_RESULTS_PER_TARGET = 31,
    RTT_ATTRIBUTE_RESULT_CNT = 32,
    RTT_ATTRIBUTE_RESULT = 33,
    RTT_ATTRIBUTE_RESUTL_DETAIL = 34,
    /* Add any new RTT_ATTRIBUTE prior to RTT_ATTRIBUTE_MAX */
    RTT_ATTRIBUTE_MAX
} RTT_ATTRIBUTE;

struct dot11_rm_ie {
    u8 id;
    u8 len;
    u8 token;
    u8 mode;
    u8 type;
} __attribute__((packed));
typedef struct dot11_rm_ie dot11_rm_ie_t;
#define DOT11_HDR_LEN 2
#define DOT11_RM_IE_LEN 5
#define DOT11_MNG_MEASURE_REQUEST_ID 38 /* 11H MeasurementRequest */
#define DOT11_MNG_MEASURE_REPORT_ID 39  /* 11H MeasurementResponse */
#define DOT11_MEASURE_TYPE_LCI 8        /* d11 measurement LCI type */
#define DOT11_MEASURE_TYPE_CIVICLOC 11  /* d11 measurement location civic */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __WIFI_RTT_H__ */
