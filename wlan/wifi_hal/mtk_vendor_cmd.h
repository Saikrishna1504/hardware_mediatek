/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MTK_VENDOR_H
#define MTK_VENDOR_H

#define OUI_MTK 0x000CE7

/**
 * Vendor subcmds defined in drvier.
 */
enum mtk_nl80211_vendor_subcmds {
    MTK_SUBCMD_TRIGGER_RESET = 1,
    MTK_SUBCMD_GET_RADIO_COMBO_MATRIX = 2,
    MTK_SUBCMD_NAN = 12,
    MTK_SUBCMD_SET_SCAN_PARAM = 13,
    MTK_SUBCMD_GET_APF_CAPABILITIES = 14,
    MTK_SUBCMD_SET_PACKET_FILTER = 15,
    MTK_SUBCMD_READ_PACKET_FILTER = 16,
    MTK_SUMCMD_CSI = 17,
    MTK_SUBCMD_GET_TRX_STATS = 48,
    MTK_SUBCMD_NDP = 81,
    MTK_SUBCMD_GET_USABLE_CHANNEL = 82,
    MTK_SUBCMD_GET_CHIP_CAPABILITIES = 83,
    MTK_SUBCMD_GET_CHIP_CONCURRENCY_MATRIX = 84,
};

enum mtk_nl80211_vendor_events {
    MTK_EVENT_DRIVER_ERROR = 8,
    MTK_EVENT_GENERIC_RESPONSE = 10,
    MTK_EVENT_BIGDATA_PIP = 11,
    MTK_EVENT_OP_MODE_CHANGE = 12,
    // TODO: sync with NAN driver to fix the conflict
    MTK_EVENT_NAN = 12,
    MTK_EVENT_RESET_TRIGGERED = 15,
    MTK_EVENT_CSI = 17,
    MTK_EVENT_NDP = 18,
};

enum mtk_wlan_vendor_attr {
    MTK_WLAN_VENDOR_ATTR_NAN = 2,
    MTK_WLAN_VENDOR_ATTR_CSI = 5,

    /* keep last */
    MTK_WLAN_VENDOR_ATTR_AFTER_LAST,
    MTK_WLAN_VENDOR_ATTR_MAX = MTK_WLAN_VENDOR_ATTR_AFTER_LAST - 1,
};

/**
 * used by MTK_SUBCMD_NDP
 */
enum mtk_wlan_vendor_attr_ndp_params {
    MTK_WLAN_VENDOR_ATTR_NDP_PARAM_INVALID = 0,
    /* Unsigned 32-bit value */
    MTK_WLAN_VENDOR_ATTR_NDP_SUBCMD,
    /* Unsigned 16-bit value */
    MTK_WLAN_VENDOR_ATTR_NDP_TRANSACTION_ID,
    /* NL attributes for data used NDP SUB cmds */
    /* Unsigned 32-bit value indicating a service info */
    MTK_WLAN_VENDOR_ATTR_NDP_SERVICE_INSTANCE_ID,
    /* Unsigned 32-bit value; channel frequency in MHz */
    MTK_WLAN_VENDOR_ATTR_NDP_CHANNEL,
    /* Interface Discovery MAC address. An array of 6 Unsigned int8 */
    MTK_WLAN_VENDOR_ATTR_NDP_PEER_DISCOVERY_MAC_ADDR,
    /* Interface name on which NDP is being created */
    MTK_WLAN_VENDOR_ATTR_NDP_IFACE_STR,
    /* Unsigned 32-bit value for security */
    MTK_WLAN_VENDOR_ATTR_NDP_CONFIG_SECURITY,
    /* Unsigned 32-bit value for QoS */
    MTK_WLAN_VENDOR_ATTR_NDP_CONFIG_QOS,
    /* Array of u8 */
    MTK_WLAN_VENDOR_ATTR_NDP_APP_INFO,
    /* Unsigned 32-bit value for NDP instance Id */
    MTK_WLAN_VENDOR_ATTR_NDP_INSTANCE_ID,
    /* Array of instance Ids */
    MTK_WLAN_VENDOR_ATTR_NDP_INSTANCE_ID_ARRAY,
    /* Unsigned 32-bit value for initiator/responder NDP response code */
    MTK_WLAN_VENDOR_ATTR_NDP_RESPONSE_CODE,
    /* NDI MAC address. An array of 6 Unsigned int8 */
    MTK_WLAN_VENDOR_ATTR_NDP_NDI_MAC_ADDR,
    /* Unsigned 32-bit value errors types returned by driver
     * The wifi_nan.h in AOSP project platform/hardware/libhardware_legacy
     * NanStatusType includes these values.
     */
    MTK_WLAN_VENDOR_ATTR_NDP_DRV_RESPONSE_STATUS_TYPE,
    /* Unsigned 32-bit value error values returned by driver */
    MTK_WLAN_VENDOR_ATTR_NDP_DRV_RETURN_VALUE,
    /* Unsigned 32-bit value for Channel setup configuration
     * The wifi_nan.h in AOSP project platform/hardware/libhardware_legacy
     * NanDataPathChannelCfg includes these values.
     */
    MTK_WLAN_VENDOR_ATTR_NDP_CHANNEL_CONFIG,
    /* Unsigned 32-bit value for Cipher Suite Shared Key Type */
    MTK_WLAN_VENDOR_ATTR_NDP_CSID,
    /* Array of u8: len = NAN_PMK_INFO_LEN 32 bytes */
    MTK_WLAN_VENDOR_ATTR_NDP_PMK,
    /* Security Context Identifier that contains the PMKID
     * Array of u8: len = NAN_SCID_BUF_LEN 1024 bytes
     */
    MTK_WLAN_VENDOR_ATTR_NDP_SCID,
    /* Array of u8: len = NAN_SECURITY_MAX_PASSPHRASE_LEN 63 bytes */
    MTK_WLAN_VENDOR_ATTR_NDP_PASSPHRASE,
    /* Array of u8: len = NAN_MAX_SERVICE_NAME_LEN 255 bytes */
    MTK_WLAN_VENDOR_ATTR_NDP_SERVICE_NAME,
    /* Unsigned 32-bit bitmap indicating schedule update
     * BIT_0: NSS Update
     * BIT_1: Channel list update
     */
    MTK_WLAN_VENDOR_ATTR_NDP_SCHEDULE_UPDATE_REASON,
    /* Unsigned 32-bit value for NSS */
    MTK_WLAN_VENDOR_ATTR_NDP_NSS,
    /* Unsigned 32-bit value for NUMBER NDP CHANNEL */
    MTK_WLAN_VENDOR_ATTR_NDP_NUM_CHANNELS,
    /* Unsigned 32-bit value for CHANNEL BANDWIDTH
     * 0:20 MHz, 1:40 MHz, 2:80 MHz, 3:160 MHz
     */
    MTK_WLAN_VENDOR_ATTR_NDP_CHANNEL_WIDTH,
    /* Array of channel/band width */
    MTK_WLAN_VENDOR_ATTR_NDP_CHANNEL_INFO,
    /* IPv6 address used by NDP (in network byte order), 16 bytes array.
     * This attribute is used and optional for ndp request, ndp response,
     * ndp indication, and ndp confirm.
     */
    MTK_WLAN_VENDOR_ATTR_NDP_IPV6_ADDR = 27,
    /* Unsigned 16-bit value indicating transport port used by NDP.
     * This attribute is used and optional for ndp response, ndp indication,
     * and ndp confirm.
     */
    MTK_WLAN_VENDOR_ATTR_NDP_TRANSPORT_PORT = 28,
    /* Unsigned 8-bit value indicating protocol used by NDP and assigned by
     * the Internet Assigned Numbers Authority (IANA) as per:
     * https://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml
     * This attribute is used and optional for ndp response, ndp indication,
     * and ndp confirm.
     */
    MTK_WLAN_VENDOR_ATTR_NDP_TRANSPORT_PROTOCOL = 29,

    /* keep last */
    MTK_WLAN_VENDOR_ATTR_NDP_PARAMS_AFTER_LAST,
    MTK_WLAN_VENDOR_ATTR_NDP_PARAMS_MAX = MTK_WLAN_VENDOR_ATTR_NDP_PARAMS_AFTER_LAST - 1,
};

enum mtk_wlan_ndp_sub_cmd {
    MTK_WLAN_VENDOR_ATTR_NDP_INVALID = 0,
    /* Command to create a NAN data path interface */
    MTK_WLAN_VENDOR_ATTR_NDP_INTERFACE_CREATE = 1,
    /* Command to delete a NAN data path interface */
    MTK_WLAN_VENDOR_ATTR_NDP_INTERFACE_DELETE = 2,
    /* Command to initiate a NAN data path session */
    MTK_WLAN_VENDOR_ATTR_NDP_INITIATOR_REQUEST = 3,
    /* Command to notify if the NAN data path session was sent */
    MTK_WLAN_VENDOR_ATTR_NDP_INITIATOR_RESPONSE = 4,
    /* Command to respond to NAN data path session */
    MTK_WLAN_VENDOR_ATTR_NDP_RESPONDER_REQUEST = 5,
    /* Command to notify on the responder about the response */
    MTK_WLAN_VENDOR_ATTR_NDP_RESPONDER_RESPONSE = 6,
    /* Command to initiate a NAN data path end */
    MTK_WLAN_VENDOR_ATTR_NDP_END_REQUEST = 7,
    /* Command to notify the if end request was sent */
    MTK_WLAN_VENDOR_ATTR_NDP_END_RESPONSE = 8,
    /* Command to notify the peer about the end request */
    MTK_WLAN_VENDOR_ATTR_NDP_REQUEST_IND = 9,
    /* Command to confirm the NAN data path session is complete */
    MTK_WLAN_VENDOR_ATTR_NDP_CONFIRM_IND = 10,
    /* Command to indicate the peer about the end request being received */
    MTK_WLAN_VENDOR_ATTR_NDP_END_IND = 11,
    /* Command to indicate the peer of schedule update */
    MTK_WLAN_VENDOR_ATTR_NDP_SCHEDULE_UPDATE_IND = 12
};

enum mtk_wlan_vendor_attr_ndp_cfg_security {
    /* Security info will be added when proposed in the specification */
    MTK_WLAN_VENDOR_ATTR_NDP_SECURITY_TYPE = 1,
};

enum mtk_wlan_subsystem_reset_attr {
    MTK_SSR_ATTRIBUTE_RESET_REASON = 1,
};

enum mtk_wlan_apf_attr {
    MTK_APF_ATTRIBUTE_VERSION = 1,
    MTK_APF_ATTRIBUTE_MAX_LEN,
    MTK_APF_ATTRIBUTE_PROGRAM,
    MTK_APF_ATTRIBUTE_PROGRAM_LEN,
};

enum mtk_wlan_apf_request_type {
    GET_APF_CAPABILITIES,
    SET_APF_PROGRAM,
    READ_APF_PROGRAM,
};

enum wifi_radio_combinations_matrix_attributes {
    WIFI_ATTRIBUTE_RADIO_COMBINATIONS_MATRIX_INVALID = 0,
    WIFI_ATTRIBUTE_RADIO_COMBINATIONS_MATRIX_MATRIX = 1,
    /* Add more attribute here */
    WIFI_ATTRIBUTE_RADIO_COMBINATIONS_MATRIX_MAX
};

typedef enum {
    WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_INVALID,
    WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_MLO_ASSOCIATION_LINK_COUNT,
    WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_MLO_STR_LINK_COUNT,
    WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_CONCURRENT_TDLS_SESSION_COUNT,
    WIFI_ATTRIBUTE_CHIP_CAPABILITIES_MAX
} WIFI_CHIP_CAPABILITIES_RESP_ATTRIBUTE;

#endif /* MTK_VENDOR_H */
