/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include <linux/filter.h>
#include <linux/rtnetlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netpacket/packet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstddef>

#include <linux/pkt_sched.h>
#include <netlink/attr.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/object-api.h>
#include <netlink/socket.h>

#include <string>
#include <vector>

#include <dirent.h>
#include <net/if.h>
#include "linux/if.h"

#include "sync.h"

#define LOG_TAG "WifiHAL"

#include <utils/Log.h>

#include <hardware_legacy/link_layer_stats.h>
#include "common.h"
#include "cpp_bindings.h"
#include "wifi_rtt.h"

template <typename>
struct DummyFunction;

template <typename R, typename... Args>
struct DummyFunction<R (*)(Args...)> {
    static constexpr R invoke(Args...) { return WIFI_SUCCESS; }
};

template <typename... Args>
struct DummyFunction<void (*)(Args...)> {
    static constexpr void invoke(Args...) {}
};

template <typename T>
void populateDummyFor(T& val) {
    val = &DummyFunction<T>::invoke;
}

/*
 BUGBUG: normally, libnl allocates ports for all connections it makes; but
 being a static library, it doesn't really know how many other netlink connections
 are made by the same process, if connections come from different shared libraries.
 These port assignments exist to solve that problem - temporarily. We need to fix
 libnl to try and allocate ports across the entire process.
 */

#define WIFI_HAL_CMD_SOCK_PORT 644
#define WIFI_HAL_EVENT_SOCK_PORT 645

static int internal_no_seq_check(nl_msg* msg, void* arg);
static int internal_valid_message_handler(nl_msg* msg, void* arg);
static int wifi_get_multicast_id(wifi_handle handle, const char* name, const char* group);
static int wifi_add_membership(wifi_handle handle, const char* group);
static wifi_error wifi_init_interfaces(wifi_handle handle);
static wifi_error wifi_start_rssi_monitoring(wifi_request_id id, wifi_interface_handle iface,
                                             s8 max_rssi, s8 min_rssi, wifi_rssi_event_handler eh);
static wifi_error wifi_stop_rssi_monitoring(wifi_request_id id, wifi_interface_handle iface);

static wifi_error wifi_get_wake_reason_stats_dummy(
        wifi_interface_handle iface, WLAN_DRIVER_WAKE_REASON_CNT* wifi_wake_reason_cnt);
static wifi_error wifi_get_packet_filter_capabilities(wifi_interface_handle handle, u32* version,
                                                      u32* max_len);
static wifi_error wifi_set_packet_filter(wifi_interface_handle handle, const u8* program, u32 len);
static wifi_error wifi_read_packet_filter(wifi_interface_handle handle, u32 src_offset,
                                          u8* host_dst, u32 length);
static wifi_error wifi_get_ring_buffers_status_dummy(wifi_interface_handle iface, u32* num_rings,
                                                     wifi_ring_buffer_status* status);
static wifi_error wifi_get_logger_supported_feature_set_dummy(wifi_interface_handle iface,
                                                              unsigned int* support);
static wifi_error wifi_get_tx_pkt_fates_dummy(wifi_interface_handle handle,
                                              wifi_tx_report* tx_report_bufs,
                                              size_t n_requested_fates, size_t* n_provided_fates);
static wifi_error wifi_get_rx_pkt_fates_dummy(wifi_interface_handle handle,
                                              wifi_rx_report* rx_report_bufs,
                                              size_t n_requested_fates, size_t* n_provided_fates);
static wifi_error wifi_trigger_subsystem_restart(wifi_handle handle);
static wifi_error wifi_virtual_interface_create(wifi_handle handle, const char* ifname,
                                                wifi_interface_type iface_type);
static wifi_error wifi_virtual_interface_delete(wifi_handle handle, const char* ifname);
static void remove_all_hal_created_ifaces(wifi_handle handle);
static bool is_hal_created_iface(const char* ifname);
static wifi_error wifi_get_supported_radio_combinations_matrix(
        wifi_handle handle, u32 max_size, u32* size,
        wifi_radio_combination_matrix* radio_combination_matrix);
static wifi_error wifi_set_scan_mode(wifi_interface_handle handle, bool enable);
static wifi_error wifi_get_usable_channels(wifi_handle handle, u32 band_mask, u32 iface_mode_mask,
                                           u32 filter_mask, u32 max_size, u32* size,
                                           wifi_usable_channel* channels);
static wifi_error wifi_get_chip_capabilities(wifi_handle handle,
                                             wifi_chip_capabilities* chip_capabilities);
static wifi_error wifi_get_supported_iface_concurrency_matrix(
        wifi_handle handle, wifi_iface_concurrency_matrix* matrix);
/* Initialize/Cleanup */

void wifi_socket_set_local_port(struct nl_sock* sock, uint32_t port) {
    uint32_t pid = getpid() & 0x3FFFFF;
    nl_socket_set_local_port(sock, pid + (port << 22));
}

static nl_sock* wifi_create_nl_socket(int port) {
    ALOGV("Creating netlink socket, local port[%d]", port);
    struct nl_sock* sock = nl_socket_alloc();
    if (sock == NULL) {
        ALOGE("Could not create netlink socket: %s(%d)", strerror(errno), errno);
        return NULL;
    }

    wifi_socket_set_local_port(sock, port);

    ALOGV("Connecting to socket");
    if (nl_connect(sock, NETLINK_GENERIC)) {
        ALOGE("Could not connect to netlink socket: %s(%d)", strerror(errno), errno);
        nl_socket_free(sock);
        return NULL;
    }

    return sock;
}
/* Initialize vendor function pointer table with MTK HAL API */
wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn) {
    if (fn == NULL) {
        return WIFI_ERROR_UNKNOWN;
    }
    fn->wifi_initialize = wifi_initialize;
    fn->wifi_cleanup = wifi_cleanup;
    fn->wifi_event_loop = wifi_event_loop;
    fn->wifi_get_supported_feature_set = wifi_get_supported_feature_set;
    fn->wifi_get_ifaces = wifi_get_ifaces;
    fn->wifi_get_iface_name = wifi_get_iface_name;
    fn->wifi_set_country_code = wifi_set_country_code;
    fn->wifi_get_firmware_version = wifi_get_firmware_version;
    fn->wifi_get_driver_version = wifi_get_driver_version;
    fn->wifi_start_rssi_monitoring = wifi_start_rssi_monitoring;
    fn->wifi_stop_rssi_monitoring = wifi_stop_rssi_monitoring;
    fn->wifi_start_sending_offloaded_packet = wifi_start_sending_offloaded_packet;
    fn->wifi_stop_sending_offloaded_packet = wifi_stop_sending_offloaded_packet;
    fn->wifi_get_roaming_capabilities = wifi_get_roaming_capabilities;
    fn->wifi_configure_roaming = wifi_configure_roaming;
    fn->wifi_enable_firmware_roaming = wifi_enable_firmware_roaming;
    fn->wifi_select_tx_power_scenario = wifi_select_tx_power_scenario;
    fn->wifi_reset_tx_power_scenario = wifi_reset_tx_power_scenario;
    fn->wifi_set_scanning_mac_oui = wifi_set_scanning_mac_oui;
    fn->wifi_get_valid_channels = wifi_get_valid_channels;

    fn->wifi_get_ring_buffers_status = wifi_get_ring_buffers_status_dummy;
    fn->wifi_get_logger_supported_feature_set = wifi_get_logger_supported_feature_set_dummy;
    fn->wifi_get_tx_pkt_fates = wifi_get_tx_pkt_fates_dummy;
    fn->wifi_get_rx_pkt_fates = wifi_get_rx_pkt_fates_dummy;
    fn->wifi_get_packet_filter_capabilities = wifi_get_packet_filter_capabilities;
    fn->wifi_set_packet_filter = wifi_set_packet_filter;
    fn->wifi_read_packet_filter = wifi_read_packet_filter;
    fn->wifi_get_wake_reason_stats = wifi_get_wake_reason_stats_dummy;
    fn->wifi_get_driver_memory_dump = wifi_get_driver_memory_dump;

    fn->wifi_trigger_subsystem_restart = wifi_trigger_subsystem_restart;
    fn->wifi_set_subsystem_restart_handler = wifi_set_subsystem_restart_handler;

    fn->wifi_virtual_interface_create = wifi_virtual_interface_create;
    fn->wifi_virtual_interface_delete = wifi_virtual_interface_delete;

    fn->wifi_multi_sta_set_primary_connection = wifi_multi_sta_set_primary_connection;
    fn->wifi_multi_sta_set_use_case = wifi_multi_sta_set_use_case;

    /* nan */
    fn->wifi_nan_enable_request = nan_enable_request;
    fn->wifi_nan_disable_request = nan_disable_request;
    fn->wifi_nan_publish_request = nan_publish_request;
    fn->wifi_nan_publish_cancel_request = nan_publish_cancel_request;
    fn->wifi_nan_subscribe_request = nan_subscribe_request;
    fn->wifi_nan_subscribe_cancel_request = nan_subscribe_cancel_request;
    fn->wifi_nan_transmit_followup_request = nan_transmit_followup_request;
    fn->wifi_nan_stats_request = nan_stats_request;
    fn->wifi_nan_config_request = nan_config_request;
    fn->wifi_nan_tca_request = nan_tca_request;
    fn->wifi_nan_beacon_sdf_payload_request = nan_beacon_sdf_payload_request;
    fn->wifi_nan_register_handler = nan_register_handler;
    fn->wifi_nan_get_version = nan_get_version;
    fn->wifi_nan_get_capabilities = nan_get_capabilities;
    fn->wifi_nan_data_interface_create = nan_data_interface_create;
    fn->wifi_nan_data_interface_delete = nan_data_interface_delete;
    fn->wifi_nan_data_request_initiator = nan_data_request_initiator;
    fn->wifi_nan_data_indication_response = nan_data_indication_response;
    fn->wifi_nan_data_end = nan_data_end;

    fn->wifi_rtt_range_request = wifi_rtt_range_request;
    fn->wifi_rtt_range_cancel = wifi_rtt_range_cancel;
    fn->wifi_get_rtt_capabilities = wifi_get_rtt_capabilities;

    fn->wifi_get_link_stats = wifi_get_link_stats;
    fn->wifi_set_link_stats = wifi_set_link_stats;
    fn->wifi_clear_link_stats = wifi_clear_link_stats;

    fn->wifi_get_supported_radio_combinations_matrix = wifi_get_supported_radio_combinations_matrix;

    fn->wifi_set_scan_mode = wifi_set_scan_mode;
    fn->wifi_get_usable_channels = wifi_get_usable_channels;
    fn->wifi_get_chip_capabilities = wifi_get_chip_capabilities;
    fn->wifi_get_supported_iface_concurrency_matrix = wifi_get_supported_iface_concurrency_matrix;

    populateDummyFor(fn->wifi_wait_for_driver_ready);
    populateDummyFor(fn->wifi_set_nodfs_flag);
    populateDummyFor(fn->wifi_set_log_handler);
    populateDummyFor(fn->wifi_reset_log_handler);
    populateDummyFor(fn->wifi_configure_nd_offload);
    populateDummyFor(fn->wifi_start_pkt_fate_monitoring);
    populateDummyFor(fn->wifi_get_ring_data);
    populateDummyFor(fn->wifi_start_logging);

    return WIFI_SUCCESS;
}

wifi_error wifi_initialize(wifi_handle* handle) {
    hal_info* info = (hal_info*)malloc(sizeof(hal_info));
    if (info == NULL) {
        ALOGE("Could not allocate hal_info");
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    memset(info, 0, sizeof(*info));

    ALOGI("Creating socket");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, info->cleanup_socks) < 0) {
        ALOGE("Could not create cleanup sockets");
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }

    struct nl_sock* cmd_sock = wifi_create_nl_socket(WIFI_HAL_CMD_SOCK_PORT);
    if (cmd_sock == NULL) {
        ALOGE("Could not create handle");
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }

    struct nl_sock* event_sock = wifi_create_nl_socket(WIFI_HAL_EVENT_SOCK_PORT);
    if (event_sock == NULL) {
        ALOGE("Could not create handle");
        nl_socket_free(cmd_sock);
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }

    struct nl_cb* cb = nl_socket_get_cb(event_sock);
    if (cb == NULL) {
        ALOGE("Could not create handle");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }

    // ALOGI("cb->refcnt = %d", cb->cb_refcnt);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, internal_no_seq_check, info);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, internal_valid_message_handler, info);
    nl_cb_put(cb);

    info->cmd_sock = cmd_sock;
    info->event_sock = event_sock;
    info->clean_up = false;
    info->in_event_loop = false;

    info->event_cb = (cb_info*)malloc(sizeof(cb_info) * DEFAULT_EVENT_CB_SIZE);
    info->alloc_event_cb = DEFAULT_EVENT_CB_SIZE;
    info->num_event_cb = 0;
    if (info->event_cb == NULL) {
        ALOGE("Could not allocate cb_info array");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    info->cmd = (cmd_info*)malloc(sizeof(cmd_info) * DEFAULT_CMD_SIZE);
    info->alloc_cmd = DEFAULT_CMD_SIZE;
    info->num_cmd = 0;
    if (info->cmd == NULL) {
        ALOGE("Could not allocate cmd_info array");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info->event_cb);
        free(info);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    ALOGI("Get nl80211_family_id");
    info->nl80211_family_id = genl_ctrl_resolve(cmd_sock, "nl80211");
    if (info->nl80211_family_id < 0) {
        ALOGE("Could not resolve nl80211 familty id");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info->event_cb);
        free(info->cmd);
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }

    pthread_mutex_init(&info->cb_lock, NULL);

    if (wifi_init_interfaces((wifi_handle)info) != WIFI_SUCCESS) {
        ALOGE("No wifi interface found");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info->event_cb);
        free(info->cmd);
        pthread_mutex_destroy(&info->cb_lock);
        free(info);
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    if ((wifi_add_membership((wifi_handle)info, "scan") < 0) ||
        (wifi_add_membership((wifi_handle)info, "mlme") < 0) ||
        (wifi_add_membership((wifi_handle)info, "regulatory") < 0) ||
        (wifi_add_membership((wifi_handle)info, "vendor") < 0)) {
        ALOGE("Add membership failed");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info->event_cb);
        free(info->cmd);
        pthread_mutex_destroy(&info->cb_lock);
        free(info);
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    *handle = (wifi_handle)info;

    ALOGV("Wifi HAL initialized successfully: nl80211_family_id=%d, found %d interfaces",
          info->nl80211_family_id, info->num_interfaces);
    return WIFI_SUCCESS;
}

static int wifi_add_membership(wifi_handle handle, const char* group) {
    hal_info* info = getHalInfo(handle);

    int id = wifi_get_multicast_id(handle, "nl80211", group);
    if (id < 0) {
        ALOGE("Could not find group %s", group);
        return id;
    }

    int ret = nl_socket_add_membership(info->event_sock, id);
    if (ret < 0) {
        ALOGE("Could not add membership to group %s", group);
    }

    ALOGV("Add membership for group %s successfully", group);
    return ret;
}

static void internal_cleaned_up_handler(wifi_handle handle) {
    hal_info* info = getHalInfo(handle);
    wifi_cleaned_up_handler cleaned_up_handler = info->cleaned_up_handler;

    if (info->cmd_sock != 0) {
        close(info->cleanup_socks[0]);
        close(info->cleanup_socks[1]);
        nl_socket_free(info->cmd_sock);
        nl_socket_free(info->event_sock);
        info->cmd_sock = NULL;
        info->event_sock = NULL;
    }

    if (info->interfaces) {
        int i = 0;
        for (; i < info->num_interfaces; i++) free(info->interfaces[i]);
        free(info->interfaces);
    }

    (*cleaned_up_handler)(handle);
    pthread_mutex_destroy(&info->cb_lock);
    free(info);

    ALOGI("Internal cleanup completed");
}

void wifi_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler) {
    hal_info* info = getHalInfo(handle);

    // 1. Setup cleanup handler.
    info->cleaned_up_handler = handler;

    pthread_mutex_lock(&info->cb_lock);

    // 2. Cancel pending commands.
    int bad_commands = 0;
    for (int i = 0; i < info->num_event_cb; i++) {
        cb_info* cbi = &(info->event_cb[i]);
        WifiCommand* cmd = (WifiCommand*)cbi->cb_arg;
        ALOGI("Command left in event_cb %p:%s", cmd, (cmd ? cmd->getType() : ""));
    }

    while (info->num_cmd > bad_commands) {
        int num_cmd = info->num_cmd;
        cmd_info* cmdi = &(info->cmd[bad_commands]);
        WifiCommand* cmd = cmdi->cmd;
        if (cmd != NULL) {
            ALOGI("Cancelling command %p:%s", cmd, cmd->getType());
            pthread_mutex_unlock(&info->cb_lock);
            cmd->cancel();
            pthread_mutex_lock(&info->cb_lock);
            if (num_cmd == info->num_cmd) {
                ALOGI("Cancelling command %p:%s did not work", cmd, (cmd ? cmd->getType() : ""));
                bad_commands++;
            }
            /* release reference added when command is saved */
            cmd->releaseRef();
        }
    }

    for (int i = 0; i < info->num_event_cb; i++) {
        cb_info* cbi = &(info->event_cb[i]);
        WifiCommand* cmd = (WifiCommand*)cbi->cb_arg;
        ALOGE("Leaked command %p", cmd);
    }

    // 3. Remove all ifaces
    remove_all_hal_created_ifaces(handle);

    pthread_mutex_unlock(&info->cb_lock);

    // 4. Tell event loop to exit.
    info->clean_up = true;
    if (TEMP_FAILURE_RETRY(write(info->cleanup_socks[0], "Exit", 4)) < 1) {
        // As a fallback set the cleanup flag to TRUE
        ALOGE("could not write to the cleanup socket");
    }
    ALOGI("wifi_cleanup done");
}

static int internal_pollin_handler(wifi_handle handle) {
    hal_info* info = getHalInfo(handle);
    struct nl_cb* cb = nl_socket_get_cb(info->event_sock);
    int res = nl_recvmsgs(info->event_sock, cb);
    ALOGV("nl_recvmsgs returned %d", res);
    nl_cb_put(cb);
    return res;
}

/* Run event handler */
void wifi_event_loop(wifi_handle handle) {
    hal_info* info = getHalInfo(handle);
    if (info->in_event_loop) {
        return;
    } else {
        info->in_event_loop = true;
    }

    pollfd pfd[2];
    memset(&pfd[0], 0, sizeof(pollfd) * 2);

    pfd[0].fd = nl_socket_get_fd(info->event_sock);
    pfd[0].events = POLLIN;
    pfd[1].fd = info->cleanup_socks[1];
    pfd[1].events = POLLIN;

    char buf[2048];

    do {
        int timeout = -1; /* Infinite timeout */
        pfd[0].revents = 0;
        pfd[1].revents = 0;
        // ALOGI("Polling socket");
        int result = TEMP_FAILURE_RETRY(poll(pfd, 2, timeout));
        if (result < 0) {
            // ALOGE("Error polling socket");
        } else if (pfd[0].revents & POLLERR) {
            ALOGE("POLL Error; error no = %d (%s)", errno, strerror(errno));
            ssize_t result2 = TEMP_FAILURE_RETRY(read(pfd[0].fd, buf, sizeof(buf)));
            ALOGE("Read after POLL returned %zd, error no = %d (%s)", result2, errno,
                  strerror(errno));
        } else if (pfd[0].revents & POLLHUP) {
            ALOGE("Remote side hung up");
            break;
        } else if (pfd[0].revents & POLLIN && !info->clean_up) {
            // ALOGI("Found some events!!!");
            internal_pollin_handler(handle);
        } else if (pfd[1].revents & POLLIN) {
            ALOGI("Got a signal to exit!!!");
        } else {
            ALOGE("Unknown event - %0x, %0x", pfd[0].revents, pfd[1].revents);
        }
    } while (!info->clean_up);

    // Destroy socks and free mem
    internal_cleaned_up_handler(handle);
}

///////////////////////////////////////////////////////////////////////////////////////
static int internal_no_seq_check(struct nl_msg* msg, void* arg) {
    return NL_OK;
}

static int internal_valid_message_handler(nl_msg* msg, void* arg) {
    wifi_handle handle = (wifi_handle)arg;
    hal_info* info = getHalInfo(handle);

    WifiEvent event(msg);
    int res = event.parse();
    if (res < 0) {
        ALOGE("Failed to parse event: %d", res);
        return NL_SKIP;
    }

    int cmd = event.get_cmd();
    uint32_t vendor_id = 0;
    int subcmd = 0;

    if (cmd == NL80211_CMD_VENDOR) {
        vendor_id = event.get_u32(NL80211_ATTR_VENDOR_ID);
        subcmd = event.get_u32(NL80211_ATTR_VENDOR_SUBCMD);
        ALOGV("event received %s, vendor_id = 0x%0x, subcmd = 0x%0x", event.get_cmdString(),
              vendor_id, subcmd);
    } else {
        ALOGV("event received %s", event.get_cmdString());
    }

    pthread_mutex_lock(&info->cb_lock);

    for (int i = 0; i < info->num_event_cb; i++) {
        if (cmd == info->event_cb[i].nl_cmd) {
            if (cmd == NL80211_CMD_VENDOR && ((vendor_id != info->event_cb[i].vendor_id) ||
                                              (subcmd != info->event_cb[i].vendor_subcmd))) {
                /* event for a different vendor, ignore it */
                continue;
            }

            cb_info* cbi = &(info->event_cb[i]);
            nl_recvmsg_msg_cb_t cb_func = cbi->cb_func;
            void* cb_arg = cbi->cb_arg;
            WifiCommand* cmd = (WifiCommand*)cbi->cb_arg;
            if (cmd != NULL) {
                cmd->addRef();
            }

            pthread_mutex_unlock(&info->cb_lock);
            if (cb_func) (*cb_func)(msg, cb_arg);
            if (cmd != NULL) {
                cmd->releaseRef();
            }

            return NL_OK;
        }
    }

    pthread_mutex_unlock(&info->cb_lock);
    return NL_OK;
}

///////////////////////////////////////////////////////////////////////////////////

class GetMulticastIdCommand : public WifiCommand {
  private:
    const char* mName;
    const char* mGroup;
    int mId;

  public:
    GetMulticastIdCommand(wifi_handle handle, const char* name, const char* group)
        : WifiCommand("GetMulticastIdCommand", handle, 0) {
        mName = name;
        mGroup = group;
        mId = -1;
    }

    int getId() { return mId; }

    virtual int create() {
        int nlctrlFamily = genl_ctrl_resolve(mInfo->cmd_sock, "nlctrl");
        int ret = mMsg.create(nlctrlFamily, CTRL_CMD_GETFAMILY, 0, 0);
        if (ret < 0) {
            return ret;
        }
        ret = mMsg.put_string(CTRL_ATTR_FAMILY_NAME, mName);
        return ret;
    }

    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("handling reponse in %s", __func__);

        struct nlattr** tb = reply.attributes();
        struct nlattr* mcgrp = NULL;
        int i;

        if (!tb[CTRL_ATTR_MCAST_GROUPS]) {
            ALOGV("No multicast groups found");
            return NL_SKIP;
        } else {
            ALOGV("Multicast groups attr size = %d", nla_len(tb[CTRL_ATTR_MCAST_GROUPS]));
        }

        for_each_attr(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], i) {
            ALOGV("Processing group");
            struct nlattr* tb2[CTRL_ATTR_MCAST_GRP_MAX + 1];
            nla_parse(tb2, CTRL_ATTR_MCAST_GRP_MAX, (nlattr*)nla_data(mcgrp), nla_len(mcgrp), NULL);
            if (!tb2[CTRL_ATTR_MCAST_GRP_NAME] || !tb2[CTRL_ATTR_MCAST_GRP_ID]) {
                continue;
            }

            char* grpName = (char*)nla_data(tb2[CTRL_ATTR_MCAST_GRP_NAME]);
            int grpNameLen = nla_len(tb2[CTRL_ATTR_MCAST_GRP_NAME]);

            if (!grpName || grpNameLen == 0) {
                continue;
            }

            if (strncmp(grpName, mGroup, grpNameLen) != 0) continue;

            mId = nla_get_u32(tb2[CTRL_ATTR_MCAST_GRP_ID]);
            break;
        }

        return NL_SKIP;
    }
};

class SetCountryCodeCommand : public WifiCommand {
  private:
    const char* mCountryCode;

  public:
    SetCountryCodeCommand(wifi_interface_handle iface, const char* country_code)
        : WifiCommand("SetCountryCodeCommand", iface, 0) {
        mCountryCode = country_code;
    }
    virtual int create() {
        int ret;

        ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_SET_COUNTRY_CODE);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_string(WIFI_ATTRIBUTE_COUNTRY_CODE, mCountryCode);
        if (ret < 0) {
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

class SetRSSIMonitorCommand : public WifiCommand {
  private:
    s8 mMax_rssi;
    s8 mMin_rssi;
    wifi_rssi_event_handler mHandler;
    char mIfaceNameCache[MAX_IFACENAME_LEN + 1];
    int mIfaceIdCache;

  public:
    SetRSSIMonitorCommand(wifi_request_id id, wifi_interface_handle handle, s8 max_rssi,
                          s8 min_rssi, wifi_rssi_event_handler eh)
        : WifiCommand("SetRSSIMonitorCommand", handle, id),
          mMax_rssi(max_rssi),
          mMin_rssi(min_rssi),
          mHandler(eh) {
        static_assert(sizeof(mIfaceNameCache) == sizeof(mIfaceInfo->name),
                      "Inconsistent ifname buffer size");
        strncpy(mIfaceNameCache, mIfaceInfo->name, sizeof(mIfaceNameCache));
        mIfaceNameCache[sizeof(mIfaceInfo->name) - 1] = '\0';
        mIfaceIdCache = ifaceId();
    }

    int createRequest(WifiRequest& request, int enable) {
        int result = request.create(GOOGLE_OUI, WIFI_SUBCMD_SET_RSSI_MONITOR);
        if (result < 0) {
            return result;
        }

        ALOGI("set RSSI Monitor, mMax_rssi=%d, mMin_rssi=%d, enable=%d", mMax_rssi, mMin_rssi,
              enable);

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }
        result = request.put_u32(WIFI_ATTRIBUTE_RSSI_MONITOR_MAX_RSSI, (enable ? mMax_rssi : 0));
        if (result < 0) {
            return result;
        }
        ALOGV("create request");
        result = request.put_u32(WIFI_ATTRIBUTE_RSSI_MONITOR_MIN_RSSI, (enable ? mMin_rssi : 0));
        if (result < 0) {
            return result;
        }
        result = request.put_u32(WIFI_ATTRIBUTE_RSSI_MONITOR_START, enable);
        if (result < 0) {
            return result;
        }

        request.attr_end(data);
        return result;
    }

    int start() {
        ALOGI("SetRSSIMonitorCommand: start");
        WifiRequest request(familyId(), ifaceId());
        int result = createRequest(request, 1);
        if (result < 0) {
            return result;
        }
        result = requestResponse(request);
        if (result < 0) {
            ALOGE("Failed to set RSSI Monitor, result=%d", result);
            return result;
        }
        ALOGV("Successfully set RSSI monitoring");
        registerVendorHandler(GOOGLE_OUI, WIFI_EVENT_RSSI_MONITOR, mIfaceNameCache);

        if (result < 0) {
            ALOGE("Failed to register RSSI monitor handler");
            unregisterVendorHandler(GOOGLE_OUI, WIFI_EVENT_RSSI_MONITOR, mIfaceNameCache);
            return result;
        }
        ALOGI("SetRSSIMonitorCommand: started");
        return result;
    }

    virtual int cancel() {
        ALOGI("SetRSSIMonitorCommand: cancel");
        int iface_index = if_nametoindex(mIfaceNameCache);
        if (!iface_index || iface_index != mIfaceIdCache) {
            ALOGW("%s: iface %s was removed, ignore cancel", __FUNCTION__, mIfaceNameCache);
            unregisterVendorHandler(GOOGLE_OUI, WIFI_EVENT_RSSI_MONITOR, mIfaceNameCache);
            return WIFI_SUCCESS;
        }
        WifiRequest request(familyId(), iface_index);
        int result = createRequest(request, 0);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to create request, result=%d", result);
        } else {
            result = requestResponse(request);
            if (result != WIFI_SUCCESS) {
                ALOGE("Failed to stop RSSI monitoring, result=%d", result);
            }
        }
        unregisterVendorHandler(GOOGLE_OUI, WIFI_EVENT_RSSI_MONITOR, mIfaceNameCache);
        ALOGI("SetRSSIMonitorCommand: cancelled");
        return WIFI_SUCCESS;
    }

    virtual int handleResponse(WifiEvent& reply) {
        /* Nothing to do on response! */
        return NL_SKIP;
    }

    virtual int handleEvent(WifiEvent& event) {
        ALOGV("Got a RSSI monitor event");

        struct nlattr* vendor_data = (struct nlattr*)event.get_vendor_data();
        int len = event.get_vendor_data_len();

        if (vendor_data == NULL || len == 0) {
            ALOGE("RSSI monitor: no vendor data");
            return NL_SKIP;
        }
/* driver<->HAL event structure */
#define RSSI_MONITOR_EVT_VERSION 1
        typedef struct {
            u8 version;
            s8 cur_rssi;
            mac_addr BSSID;
        } rssi_monitor_evt;

        rssi_monitor_evt* data = NULL;
        if (vendor_data->nla_type == WIFI_EVENT_RSSI_MONITOR)
            data = (rssi_monitor_evt*)nla_data(vendor_data);
        else
            return NL_SKIP;

        if (!data) {
            ALOGE("RSSI monitor: no data");
            return NL_SKIP;
        }

        ALOGI("data: version=%d, cur_rssi=%d BSSID=" MACSTR "\r\n", data->version, data->cur_rssi,
              MAC2STR(data->BSSID));

        if (data->version != RSSI_MONITOR_EVT_VERSION) {
            ALOGE("Event version mismatch %d, expected %d", data->version,
                  RSSI_MONITOR_EVT_VERSION);
            return NL_SKIP;
        }

        if (*mHandler.on_rssi_threshold_breached) {
            (*mHandler.on_rssi_threshold_breached)(id(), data->BSSID, data->cur_rssi);
        } else {
            ALOGW("No RSSI monitor handler registered");
        }

        return NL_SKIP;
    }
};

class ConfigRoamingCommand : public WifiCommand {
  private:
    wifi_roaming_config* mConfig;

  public:
    ConfigRoamingCommand(wifi_interface_handle handle, wifi_roaming_config* config)
        : WifiCommand("ConfigRoamingCommand", handle, 0) {
        mConfig = config;
    }

    int createRequest(WifiRequest& request, int subcmd, wifi_roaming_config* config) {
        int result = request.create(GOOGLE_OUI, subcmd);
        if (result < 0) {
            return result;
        }

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        result = request.put_u32(WIFI_ATTRIBUTE_ROAMING_BLACKLIST_NUM, config->num_blacklist_bssid);
        if (result < 0) {
            return result;
        }

        mac_addr* bssid_list = config->blacklist_bssid;
        for (u32 i = 0; i < config->num_blacklist_bssid; i++) {
            result = request.put_addr(WIFI_ATTRIBUTE_ROAMING_BLACKLIST_BSSID, bssid_list[i]);
            ALOGI("Blacklist BSSID[%d] " MACSTR, i, MAC2STR(bssid_list[i]));
            if (result < 0) {
                return result;
            }
        }

        result = request.put_u32(WIFI_ATTRIBUTE_ROAMING_WHITELIST_NUM, config->num_whitelist_ssid);
        if (result < 0) {
            return result;
        }

        char ssid[MAX_SSID_LENGTH + 1];
        ssid_t* ssid_list = config->whitelist_ssid;
        for (u32 i = 0; i < config->num_whitelist_ssid; i++) {
            memcpy(ssid, ssid_list[i].ssid_str, ssid_list[i].length);
            ssid[ssid_list[i].length] = '\0';
            result = request.put(WIFI_ATTRIBUTE_ROAMING_WHITELIST_SSID, ssid,
                                 ssid_list[i].length + 1);
            ALOGI("Whitelist ssid[%d] : %s", i, ssid);
            if (result < 0) {
                return result;
            }
        }

        request.attr_end(data);
        return WIFI_SUCCESS;
    }

    int start() {
        ALOGV("Configure roaming");
        WifiRequest request(familyId(), ifaceId());
        int result = createRequest(request, WIFI_SUBCMD_CONFIG_ROAMING, mConfig);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to create request, result=%d", result);
            return result;
        }

        result = requestResponse(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("[WIFI HAL]Failed to configure roaming, result=%d", result);
        }

        return result;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("ConfigRoamingCommand complete!");
        /* Nothing to do on response! */
        return NL_SKIP;
    }
};

class EnableRoamingCommand : public WifiCommand {
  private:
    fw_roaming_state_t mState;

  public:
    EnableRoamingCommand(wifi_interface_handle handle, fw_roaming_state_t state)
        : WifiCommand("EnableRoamingCommand", handle, 0) {
        mState = state;
    }
    virtual int create() {
        int ret;

        ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_ENABLE_ROAMING);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_u32(WIFI_ATTRIBUTE_ROAMING_STATE, mState);
        if (ret < 0) {
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

class GetFeatureSetCommand : public WifiCommand {
  private:
    int feature_type;
    feature_set* fset;
    feature_set* feature_matrix;
    int* fm_size;
    int set_size_max;

  public:
    GetFeatureSetCommand(wifi_interface_handle handle, int feature, feature_set* set,
                         feature_set set_matrix[], int* size, int max_size)
        : WifiCommand("GetFeatureSetCommand", handle, 0) {
        feature_type = feature;
        fset = set;
        feature_matrix = set_matrix;
        fm_size = size;
        set_size_max = max_size;
    }

    virtual int create() {
        int ret;

        if (feature_type == WIFI_ATTRIBUTE_FEATURE_SET) {
            ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_GET_FEATURE_SET);
        } else if (feature_type == WIFI_ATTRIBUTE_ROAMING_CAPABILITIES) {
            ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_GET_ROAMING_CAPABILITIES);
        } else {
            ALOGE("Unknown feature type %d", feature_type);
            return -1;
        }

        if (ret < 0) {
            ALOGE("Can't create subcmd message to driver, ret=%d", ret);
        }

        return ret;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("In GetFeatureSetCommand::handleResponse for %d", feature_type);

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignore reply with cmd 0x%x", reply.get_cmd());
            return NL_SKIP;
        }

        int vendor_id = reply.get_vendor_id();
        int subcmd = reply.get_vendor_subcmd();
        ALOGV("vendor_id = 0x%x, subcmd = 0x%x", vendor_id, subcmd);

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();
        if (vendor_data == NULL || len == 0) {
            ALOGE("No vendor data in GetFeatureSetCommand response, ignore it");
            return NL_SKIP;
        }

        if (feature_type == WIFI_ATTRIBUTE_FEATURE_SET) {
            void* data = reply.get_vendor_data();
            if (!data) {
                ALOGE("Failed to get VENDOR_DATA");
                return NL_SKIP;
            }
            if (!fset) {
                ALOGE("feature_set pointer is not set");
                return NL_SKIP;
            }
            memcpy(fset, data, min(len, (int)sizeof(*fset)));
            ALOGI("feature_set=0x%" PRIx64, (uint64_t)(*fset));
        } else if (feature_type == WIFI_ATTRIBUTE_ROAMING_CAPABILITIES) {
            if (!feature_matrix || !fm_size) {
                ALOGE("feature_set pointer is not set");
                return NL_SKIP;
            }

            *fm_size = set_size_max;  // black and white
            wifi_roaming_capabilities cap;
            memset(&cap, 0, sizeof(cap));
            for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
                if (it.get_type() == WIFI_ATTRIBUTE_ROAMING_BLACKLIST_NUM) {
                    cap.max_blacklist_size =
                            it.get_u32() > MAX_BLACKLIST_BSSID ? MAX_BLACKLIST_BSSID : it.get_u32();
                } else if (it.get_type() == WIFI_ATTRIBUTE_ROAMING_WHITELIST_NUM) {
                    cap.max_whitelist_size =
                            it.get_u32() > MAX_WHITELIST_SSID ? MAX_WHITELIST_SSID : it.get_u32();
                } else {
                    ALOGW("Ignore invalid attribute type = %d", it.get_type());
                }
            }
            ALOGI("blacklist/whitelist: %d: %d", cap.max_blacklist_size, cap.max_whitelist_size);
            memcpy(feature_matrix, &cap, sizeof(wifi_roaming_capabilities));
        }

        return NL_OK;
    }
};

class SelectTxPowerCommand : public WifiCommand {
  private:
    int mScenario;

  public:
    SelectTxPowerCommand(wifi_interface_handle handle, int scenario)
        : WifiCommand("SelectTxPowerCommand", handle, 0) {
        mScenario = scenario;
    }
    virtual int create() {
        int ret;

        ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_SELECT_TX_POWER_SCENARIO);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_u32(WIFI_ATTRIBUTE_TX_POWER_SCENARIO, mScenario);
        if (ret < 0) {
            ALOGE("Failed to put TX_POWER_SCENARIO");
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

class SetScanMacOuiCommand : public WifiCommand {
  private:
    const byte* mScanMacOui;

  public:
    SetScanMacOuiCommand(wifi_interface_handle handle, const oui scan_oui)
        : WifiCommand("SetScanMacOuiCommand", handle, 0), mScanMacOui(scan_oui) {}
    virtual int create() {
        ALOGI("Set scan mac oui");
        int ret;

        ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_SET_PNO_RANDOM_MAC_OUI);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put(WIFI_ATTRIBUTE_PNO_RANDOM_MAC_OUI, (void*)mScanMacOui, sizeof(oui));
        if (ret < 0) {
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

class GetChannelListCommand : public WifiCommand {
  private:
    wifi_channel* mChannels;
    int mMaxChannels;
    int* mNumOfChannel;
    int mBand;

  public:
    GetChannelListCommand(wifi_interface_handle handle, int band, int max_channels,
                          wifi_channel* channels, int* num_channels)
        : WifiCommand("GetChannelListCommand", handle, 0) {
        mBand = band;
        mMaxChannels = max_channels;
        mChannels = channels;
        mNumOfChannel = num_channels;
        memset(mChannels, 0, sizeof(wifi_channel) * mMaxChannels);
    }

    virtual int create() {
        ALOGV("Creating message to get channel list; iface = %d", mIfaceInfo->id);

        int ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_GET_CHANNEL_LIST);
        if (ret < 0) {
            return ret;
        }

        ALOGI("In GetChannelList::mBand=%d", mBand);

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_u32(WIFI_ATTRIBUTE_BAND, mBand);
        if (ret < 0) {
            return ret;
        }

        mMsg.attr_end(data);
        return ret;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("In GetChannelList::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGE("Ignore reply with cmd 0x%x", reply.get_cmd());
            return NL_SKIP;
        }

        int vendor_id = reply.get_vendor_id();
        int subcmd = reply.get_vendor_subcmd();
        ALOGV("vendor_id = 0x%x, subcmd = 0x%x", vendor_id, subcmd);

        nlattr* vendor = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();
        if (vendor == NULL || len == 0) {
            ALOGE("No vendor data in GetChannelList response, ignore it");
            return NL_SKIP;
        }

        int num_channels = 0;
        for (nl_iterator it(vendor); it.has_next(); it.next()) {
            if (it.get_type() == WIFI_ATTRIBUTE_NUM_CHANNELS) {
                num_channels = it.get_u32();
                ALOGI("Get channel list with %d channels", num_channels);
                if (num_channels > mMaxChannels) num_channels = mMaxChannels;
                *mNumOfChannel = num_channels;
            } else if (it.get_type() == WIFI_ATTRIBUTE_CHANNEL_LIST && num_channels) {
                memcpy(mChannels, it.get_data(), sizeof(wifi_channel) * num_channels);
            } else {
                ALOGW("Ignore invalid attribute type = %d, size = %d", it.get_type(), it.get_len());
            }
        }

        ALOGV("mChannels[0]=%d mChannels[1]=%d", *mChannels, *(mChannels + 1));

        return NL_OK;
    }
};

/**
 * Add new iface.
 *
 * Attributes:
 *  NL80211_ATTR_IFINDEX    // control iface (wlan0) index
 *  NL80211_ATTR_IFNAME     // new iface name
 *  NL80211_ATTR_IFTYPE     // new iface type
 */
class AddInterfaceCommand : public WifiCommand {
  private:
    int mCtrlId;
    const char* mIfName;
    nl80211_iftype mType;

  public:
    AddInterfaceCommand(wifi_handle handle, int ctrlIndex, const char* ifname, nl80211_iftype type)
        : WifiCommand("AddInterfaceCommand", handle, 0),
          mCtrlId(ctrlIndex),
          mIfName(ifname),
          mType(type) {}

    virtual int create() {
        int ret = mMsg.create(NL80211_CMD_NEW_INTERFACE);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        mMsg.put_u32(NL80211_ATTR_IFINDEX, mCtrlId);
        mMsg.put_string(NL80211_ATTR_IFNAME, mIfName);
        mMsg.put_u32(NL80211_ATTR_IFTYPE, mType);
        return WIFI_SUCCESS;
    }
};

/**
 * Delete iface.
 *
 * Attributes:
 *  NL80211_ATTR_IFINDEX    // iface index to delete
 */
class DeleteInterfaceCommand : public WifiCommand {
  private:
    const char* mIfName;

  public:
    DeleteInterfaceCommand(wifi_handle handle, const char* ifname)
        : WifiCommand("DeleteInterfaceCommand", handle, 0), mIfName(ifname) {}

    virtual int create() {
        int ret = mMsg.create(NL80211_CMD_DEL_INTERFACE);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        unsigned ifindex = if_nametoindex(mIfName);
        if (ifindex == 0) {
            return WIFI_ERROR_UNKNOWN;
        }
        mMsg.put_u32(NL80211_ATTR_IFINDEX, ifindex);
        return WIFI_SUCCESS;
    }
};

/**
 * Set which iface is primary STA iface.
 *
 * Attributes:
 *  NL80211_ATTR_VENDOR_DATA {
 *      MULTISTA_ATTRIBUTE_PRIMARY_IFACE    // new primary iface index
 *  } // NL80211_ATTR_VENDOR_DATA
 */
class MultiStaSetPrimaryCommand : public WifiCommand {
  public:
    MultiStaSetPrimaryCommand(wifi_interface_handle handle)
        : WifiCommand("MultiStaSetPrimaryCommand", handle, 0) {}

    virtual int create() {
        int ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_SET_MULTISTA_PRIMARY_CONNECTION);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
            return ret;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_u32(MULTISTA_ATTRIBUTE_PRIMARY_IFACE, (uint32_t)mIfaceInfo->id);
        if (ret < 0) {
            ALOGE("Failed to add attribute primary iface: %d, result %d", mIfaceInfo->id, ret);
            mMsg.attr_end(data);
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

/**
 * Set Multi-STA use case.
 *
 * Attributes:
 *  NL80211_ATTR_IFINDEX    // control iface (wlan0) index
 *  NL80211_ATTR_VENDOR_DATA {
 *      MULTISTA_ATTRIBUTE_USE_CASE     // use case
 *  } // NL80211_ATTR_VENDOR_DATA
 */
class MultiStaSetUseCaseCommand : public WifiCommand {
  private:
    int mCtrlId;
    int mUseCase;

  public:
    MultiStaSetUseCaseCommand(wifi_handle handle, int ctrlIndex, int useCase)
        : WifiCommand("MultiStaSetUseCaseCommand", handle, 0),
          mCtrlId(ctrlIndex),
          mUseCase(useCase) {}

    virtual int create() {
        int ret = mMsg.create(GOOGLE_OUI, WIFI_SUBCMD_SET_MULTISTA_USE_CASE);
        if (ret < 0) {
            ALOGE("Can't create message %d to send to driver - %d",
                  WIFI_SUBCMD_SET_MULTISTA_USE_CASE, ret);
            return ret;
        }

        mMsg.put_u32(NL80211_ATTR_IFINDEX, mCtrlId);

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        ret = mMsg.put_u32(MULTISTA_ATTRIBUTE_USE_CASE, (uint32_t)mUseCase);
        if (ret < 0) {
            ALOGE("Failed to add attribute use case: %d, result %d", mUseCase, ret);
            mMsg.attr_end(data);
            return ret;
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }
};

class SetSubsystemRestartHandlerCommand : public WifiVendorCommand {
  private:
    wifi_subsystem_restart_handler mHandler;

  public:
    SetSubsystemRestartHandlerCommand(wifi_handle handle, wifi_subsystem_restart_handler eh)
        : WifiVendorCommand("SetSubsystemRestartHandlerCommand", handle, 0, OUI_MTK,
                            MTK_EVENT_RESET_TRIGGERED),
          mHandler(eh) {}

    int start() {
        ALOGI("register subsystem restart handler");
        int result = registerVendorHandler(OUI_MTK, MTK_EVENT_RESET_TRIGGERED);
        if (result < 0) {
            ALOGE("failed to register subsystem restart handler");
            unregisterVendorHandler(OUI_MTK, MTK_EVENT_RESET_TRIGGERED);
        }
        return result;
    }

    virtual int cancel() {
        ALOGI("unregister subsystem restart handler");
        unregisterVendorHandler(OUI_MTK, MTK_EVENT_RESET_TRIGGERED);
        wifi_unregister_cmd(wifiHandle(), id());
        return WIFI_SUCCESS;
    }

    virtual int handleResponse(WifiEvent& reply) {
        /* Nothing to do on response! */
        return NL_SKIP;
    }

    virtual int handleEvent(WifiEvent& event) {
        ALOGI("SetSubsystemRestartHandlerCommand: handleEvent");
        struct nlattr* vendor_data = (struct nlattr*)event.get_vendor_data();
        int len = event.get_vendor_data_len();
        int event_id = event.get_vendor_subcmd();
        ALOGI("Got event: %d", event_id);

        if (vendor_data == NULL || len == 0) {
            ALOGE("No Debug data found");
            return NL_SKIP;
        }

        uint32_t reason = UINT32_MAX;

        if (event_id == MTK_EVENT_RESET_TRIGGERED) {
            ALOGI("Handle MTK_EVENT_RESET_TRIGGERED");
            for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
                if (it.get_type() == MTK_SSR_ATTRIBUTE_RESET_REASON) {
                    reason = it.get_u32();
                } else {
                    ALOGW("Ignoring invalid attribute type = %d, size = %d", it.get_type(),
                          it.get_len());
                }
            }

            char reason_str[64];
            if (0 > snprintf(reason_str, sizeof(reason_str), "%u", reason)) {
                reason_str[0] = 0;
            }
            ALOGI("%s: Reset reason: %s", __FUNCTION__, reason_str);

            if (*mHandler.on_subsystem_restart) {
                (*mHandler.on_subsystem_restart)(reason_str);
            } else {
                ALOGW("No Restart handler registered");
            }
        }
        return NL_OK;
    }
};

class AndroidPacketFilterCommand : public WifiCommand {
  private:
    const u8* mProgram;
    u8* mReadProgram;
    u32 mProgramLen;
    u32* mVersion;
    u32* mMaxLen;
    int mReqType;

  public:
    AndroidPacketFilterCommand(wifi_interface_handle handle, u32* version, u32* max_len)
        : WifiCommand("AndroidPacketFilterCommand", handle, 0),
          mProgram(nullptr),
          mReadProgram(nullptr),
          mProgramLen(0),
          mVersion(version),
          mMaxLen(max_len),
          mReqType(GET_APF_CAPABILITIES) {}

    AndroidPacketFilterCommand(wifi_interface_handle handle, const u8* program, u32 len)
        : WifiCommand("AndroidPacketFilterCommand", handle, 0),
          mProgram(program),
          mReadProgram(nullptr),
          mProgramLen(len),
          mVersion(nullptr),
          mMaxLen(nullptr),
          mReqType(SET_APF_PROGRAM) {}

    AndroidPacketFilterCommand(wifi_interface_handle handle, u8* host_dst, u32 len)
        : WifiCommand("AndroidPacketFilterCommand", handle, 0),
          mProgram(nullptr),
          mReadProgram(host_dst),
          mProgramLen(len),
          mVersion(nullptr),
          mMaxLen(nullptr),
          mReqType(READ_APF_PROGRAM) {}

    int createRequest(WifiRequest& request) {
        if (mReqType == SET_APF_PROGRAM) {
            ALOGI("\n%s: APF set program request\n", __FUNCTION__);
            return createSetPktFilterRequest(request);
        } else if (mReqType == GET_APF_CAPABILITIES) {
            ALOGI("\n%s: APF get capabilities request\n", __FUNCTION__);
            return createGetPktFilterCapabilitesRequest(request);
        } else if (mReqType == READ_APF_PROGRAM) {
            ALOGI("\n%s: APF read packet filter request\n", __FUNCTION__);
            return createReadPktFilterRequest(request);
        } else {
            ALOGE("\n%s Unknown APF request\n", __FUNCTION__);
            return WIFI_ERROR_NOT_SUPPORTED;
        }
        return WIFI_SUCCESS;
    }

    int createSetPktFilterRequest(WifiRequest& request) {
        u8* program = new u8[mProgramLen];
        NULL_CHECK_RETURN(program, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
        int result = request.create(OUI_MTK, MTK_SUBCMD_SET_PACKET_FILTER);
        if (result < 0) {
            delete[] program;
            return result;
        }

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        memcpy(program, mProgram, mProgramLen);
        result = request.put(MTK_APF_ATTRIBUTE_PROGRAM, program, mProgramLen);
        request.attr_end(data);
        delete[] program;
        return result;
    }

    int createGetPktFilterCapabilitesRequest(WifiRequest& request) {
        int result = request.create(OUI_MTK, MTK_SUBCMD_GET_APF_CAPABILITIES);
        if (result < 0) {
            return result;
        }

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        request.attr_end(data);
        return result;
    }

    int createReadPktFilterRequest(WifiRequest& request) {
        int result = request.create(OUI_MTK, MTK_SUBCMD_READ_PACKET_FILTER);
        if (result < 0) {
            return result;
        }
        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        request.attr_end(data);
        return result;
    }

    int start() {
        WifiRequest request(familyId(), ifaceId());
        int result = createRequest(request);
        if (result < 0) {
            return result;
        }
        result = requestResponse(request);
        if (result < 0) {
            ALOGI("Request Response failed for APF, result = %d", result);
            return result;
        }
        ALOGI("Done!");
        return result;
    }

    int cancel() { return WIFI_SUCCESS; }

    int handleResponse(WifiEvent& reply) {
        ALOGE("In SetAPFCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGE("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        int id = reply.get_vendor_id();
        int subcmd = reply.get_vendor_subcmd();

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();

        ALOGV("Id = %0x, subcmd = %d, len = %d", id, subcmd, len);
        if (vendor_data == NULL || len == 0) {
            ALOGE("no vendor data in SetAPFCommand response; ignoring it");
            return NL_SKIP;
        }
        if (mReqType == SET_APF_PROGRAM) {
            ALOGE("Response received for set packet filter command\n");
        } else if (mReqType == GET_APF_CAPABILITIES) {
            *mVersion = 0;
            *mMaxLen = 0;
            ALOGE("Response received for get packet filter capabilities command\n");
            for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
                if (it.get_type() == MTK_APF_ATTRIBUTE_VERSION) {
                    *mVersion = it.get_u32();
                    ALOGI("APF version is %d\n", *mVersion);
                } else if (it.get_type() == MTK_APF_ATTRIBUTE_MAX_LEN) {
                    *mMaxLen = it.get_u32();
                    ALOGI("APF max len is %d\n", *mMaxLen);
                } else {
                    ALOGE("Ignoring invalid attribute type = %d, size = %d", it.get_type(),
                          it.get_len());
                }
            }
        } else if (mReqType == READ_APF_PROGRAM) {
            ALOGE("Read packet filter, mProgramLen = %d\n", mProgramLen);
            for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
                if (it.get_type() == MTK_APF_ATTRIBUTE_PROGRAM) {
                    u8* buffer = NULL;
                    buffer = (u8*)it.get_data();
                    memcpy(mReadProgram, buffer, mProgramLen);
                } else if (it.get_type() == MTK_APF_ATTRIBUTE_PROGRAM_LEN) {
                    int apf_length = it.get_u32();
                    ALOGV("apf program length = %d\n", apf_length);
                }
            }
        }
        return NL_OK;
    }

    int handleEvent(WifiEvent& event) {
        /* No Event to receive for APF commands */
        return NL_SKIP;
    }
};

/**
 * NOTE:
 *     For variable length structure, for example
 *     struct AAA {
 *         // ...
 *         int trailing_array[];
 *     };
 *     DO NOT use `sizeof(AAA)` to calculate the offset of trailing_array. USE `offsetof(AAA,
 * trailing_array)`.
 */
class GetLinkStatsCommand : public WifiCommand {
    wifi_stats_result_handler mHandler;

  public:
    GetLinkStatsCommand(wifi_interface_handle iface, wifi_stats_result_handler handler)
        : WifiCommand("GetLinkStatsCommand", iface, 0), mHandler(handler) {}

    virtual int create() {
        ALOGV("GetLinkStatsCommand::create");
        int ret = mMsg.create(GOOGLE_OUI, WIFI_LSTATS_SUBCMD_GET_INFO);
        if (ret < 0) {
            ALOGE("Can't create message %d to send to driver - %d", WIFI_LSTATS_SUBCMD_GET_INFO,
                  ret);
        }
        return ret;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("LLS RSP: enter");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("LLS RSP: ignore reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        int id = reply.get_vendor_id();
        void* data = reply.get_vendor_data();
        int len = reply.get_vendor_data_len();

        ALOGI("LLS RSP: data=%p, len=%d", data, len);

        // [wifi_iface_ml_stat][num_radios][wifi_radio_stat list][tx_time_per_levels list]
        wifi_iface_ml_stat* ml_stat_ptr = (wifi_iface_ml_stat*)data;
        uint32_t num_radios = 0;
        wifi_radio_stat* radio_stats_ptr = nullptr;

        // Quick sanity check and patch dangling pointers
        if (!check_data_sanity(data, len, num_radios, radio_stats_ptr)) {
            ALOGE("LLS RSP: invalid vendor data length");
            return NL_SKIP;
        }

        (*mHandler.on_multi_link_stats_results)(id, ml_stat_ptr, num_radios, radio_stats_ptr);

        return NL_OK;
    }

  private:
    /**
     * Check data sizes to avoid memory access violation quickly.
     * We can safely convert data to upper level representation
     * after this.
     */
    bool check_data_sanity(void* data, int len, uint32_t& num_radios,
                           wifi_radio_stat*& radio_stats_ptr) {
        if (data == nullptr || len < offsetof(wifi_iface_ml_stat, links)) {
            ALOGE("LLS %s: %d", __FUNCTION__, __LINE__);
            return false;
        }

        ALOGV("LLS %s: %d: data=%p, len=%d", __FUNCTION__, __LINE__, data, len);

        // wifi_iface_ml_stat header
        wifi_iface_ml_stat* ml_stat = (wifi_iface_ml_stat*)data;
        uint32_t confirmed_size = offsetof(wifi_iface_ml_stat, links);
        if (ml_stat->num_links <= 0) {
            ALOGE("LLS %s: %d No links found", __FUNCTION__, __LINE__);
            return false;
        }

        // trailing wifi_link_stat(s)
        wifi_link_stat* current_link_stat = ml_stat->links;
        ALOGV("LLS %s: %d: ml_stat->num_links=%d", __FUNCTION__, __LINE__, ml_stat->num_links);
        for (int i = 0; i < ml_stat->num_links; ++i) {
            // wifi_link_stat header
            if (confirmed_size + offsetof(wifi_link_stat, peer_info) > len) {
                ALOGE("LLS %s: %d", __FUNCTION__, __LINE__);
                return false;
            }
            confirmed_size += offsetof(wifi_link_stat, peer_info);
            // trailing wifi_peer_info(s)
            wifi_peer_info* current_peer_info = current_link_stat->peer_info;
            ALOGV("LLS %s: %d: current_link_stat(%p)->num_peers=%" PRIu32, __FUNCTION__, __LINE__,
                  current_link_stat, current_link_stat->num_peers);
            for (int j = 0; j < current_link_stat->num_peers; ++j) {
                // wifi_peer_info header
                if (confirmed_size + offsetof(wifi_peer_info, rate_stats) > len) {
                    ALOGE("LLS %s: %d", __FUNCTION__, __LINE__);
                    return false;
                }
                ALOGV("LLS %s: %d: current_peer_info(%p)->num_rate=%" PRIu32, __FUNCTION__,
                      __LINE__, current_peer_info, current_peer_info->num_rate);
                // trailing wifi_rate_stat(s)
                int expected_size = offsetof(wifi_peer_info, rate_stats) +
                                    current_peer_info->num_rate * sizeof(wifi_rate_stat);
                if (confirmed_size + expected_size > len) {
                    ALOGE("LLS %s: %d: [FAILED] check trailing wifi_rate_stat(s), "
                          "confirmed_size=%" PRIu32 ", expected_size=%d, len=%d",
                          __FUNCTION__, __LINE__, confirmed_size, expected_size, len);
                    return false;
                }
                confirmed_size += expected_size;
                current_peer_info = (wifi_peer_info*)(((u8*)current_peer_info) + expected_size);
            }

            current_link_stat = (wifi_link_stat*)(((u8*)data) + confirmed_size);
        }

        // num_radios
        if (confirmed_size + sizeof(uint32_t) > len) {
            ALOGE("LLS %s: %d", __FUNCTION__, __LINE__);
            return false;
        }
        uint32_t* num_radios_ptr = (uint32_t*)(((u8*)ml_stat) + confirmed_size);
        ALOGV("LLS %s: num_radios_ptr=%p", __FUNCTION__, num_radios_ptr);
        num_radios = *num_radios_ptr;
        ALOGV("LLS %s: num_radios=%" PRIu32, __FUNCTION__, num_radios);
        confirmed_size += sizeof(uint32_t);

        // wifi_radio_stat(s)
        wifi_radio_stat* current_radio_stat = (wifi_radio_stat*)(((u8*)ml_stat) + confirmed_size);
        radio_stats_ptr = current_radio_stat;
        uint32_t*** tx_time_per_levels_pointer_list = new uint32_t**[num_radios];
        uint32_t* tx_levels_offsets = new uint32_t[num_radios];
        uint32_t total_num_tx_levels = 0;
        for (int i = 0; i < num_radios; ++i) {
            if (confirmed_size + offsetof(wifi_radio_stat, channels) > len) {
                delete[] tx_time_per_levels_pointer_list;
                delete[] tx_levels_offsets;
                ALOGE("LLS %s: %d: [FAILED] check wifi_radio_stat(s), confirmed_size=%" PRIu32
                      ", offsetof(wifi_radio_stat, channels)=%zu, len=%d",
                      __FUNCTION__, __LINE__, confirmed_size, offsetof(wifi_radio_stat, channels),
                      len);
                return false;
            }
            ALOGI("LLS %s: %d: current_radio_stat->num_channels=%" PRIu32, __FUNCTION__, __LINE__,
                  current_radio_stat->num_channels);
            int expected_size = offsetof(wifi_radio_stat, channels) +
                                current_radio_stat->num_channels * sizeof(wifi_channel_stat);
            if (confirmed_size + expected_size > len) {
                delete[] tx_time_per_levels_pointer_list;
                delete[] tx_levels_offsets;
                ALOGE("LLS %s: %d: [FAILED] check wifi_channel_stat(s), confirmed_size=%" PRIu32
                      ", expected_size=%d, sizeof(wifi_channel_stat)=%zu, len=%d",
                      __FUNCTION__, __LINE__, confirmed_size, expected_size,
                      sizeof(wifi_channel_stat), len);
                return false;
            }
            tx_time_per_levels_pointer_list[i] = &(current_radio_stat->tx_time_per_levels);
            tx_levels_offsets[i] = total_num_tx_levels;
            total_num_tx_levels += current_radio_stat->num_tx_levels;
            confirmed_size += expected_size;
            current_radio_stat = (wifi_radio_stat*)(((u8*)current_radio_stat) + expected_size);
        }

        uint32_t* tx_levels = (uint32_t*)current_radio_stat;

        if (confirmed_size + total_num_tx_levels * sizeof(uint32_t) > len) {
            delete[] tx_time_per_levels_pointer_list;
            delete[] tx_levels_offsets;
            ALOGE("LLS %s: %d", __FUNCTION__, __LINE__);
            return false;
        }

        // back patch pointers
        for (int i = 0; i < num_radios; ++i) {
            *(tx_time_per_levels_pointer_list[i]) = tx_levels + tx_levels_offsets[i];
        }
        delete[] tx_time_per_levels_pointer_list;
        delete[] tx_levels_offsets;

        confirmed_size += total_num_tx_levels * sizeof(uint32_t);
        ALOGI("LLS %s: confirmed_size=%d, len=%d", __FUNCTION__, (int)confirmed_size, len);

        return true;
    }

// Macros for getting offsets
#define ML_STAT_(f) ((unsigned long)(offsetof(wifi_iface_ml_stat, f)))
#define LINK_STAT_(f) ((unsigned long)(offsetof(wifi_link_stat, f)))
#define IFACE_INFO_STAT_(f) ((unsigned long)(offsetof(wifi_interface_link_layer_info, f)))
#define WMM_STAT_(f) ((unsigned long)(offsetof(wifi_wmm_ac_stat, f)))
#define PEER_INFO_(f) ((unsigned long)(offsetof(wifi_peer_info, f)))
#define BSSLOAD_INFO_(f) ((unsigned long)(offsetof(bssload_info_t, f)))
#define RATE_STAT_(f) ((unsigned long)(offsetof(wifi_rate_stat, f)))
#define RATE_(f) ((unsigned long)(offsetof(wifi_rate, f)))
#define RADIO_STAT_(f) ((unsigned long)(offsetof(wifi_radio_stat, f)))
#define CHANNEL_STAT_(f) ((unsigned long)(offsetof(wifi_channel_stat, f)))
#define CHANNEL_INFO_(f) ((unsigned long)(offsetof(wifi_channel_info, f)))

    void dump_ml_stat(wifi_iface_ml_stat* ml_stat_ptr) {
        // Keep an eye on this offset as it's used for pointer arithmetic
        unsigned long current_offset = 0;
        ALOGI("Offset|Field Name|Value");
        ALOGI("--|--|--");
        ALOGI("%04lx|iface|%p", ML_STAT_(iface), (void*)(ml_stat_ptr->iface));

        // info
        {
            ALOGI("%04lx|info.mode|%d", ML_STAT_(info) + IFACE_INFO_STAT_(mode),
                  ml_stat_ptr->info.mode);
            ALOGI("%04lx|info.mac_addr|" MACSTR, ML_STAT_(info) + IFACE_INFO_STAT_(mac_addr),
                  MAC2STR(ml_stat_ptr->info.mac_addr));
            ALOGI("%04lx|info.state|%d", ML_STAT_(info) + IFACE_INFO_STAT_(state),
                  ml_stat_ptr->info.state);
            ALOGI("%04lx|info.roaming|%d", ML_STAT_(info) + IFACE_INFO_STAT_(roaming),
                  ml_stat_ptr->info.roaming);
            ALOGI("%04lx|info.capabilities|%" PRIu32,
                  ML_STAT_(info) + IFACE_INFO_STAT_(capabilities), ml_stat_ptr->info.capabilities);
            ALOGI("%04lx|info.ssid|%s", ML_STAT_(info) + IFACE_INFO_STAT_(ssid),
                  ml_stat_ptr->info.ssid);
            ALOGI("%04lx|info.bssid|" MACSTR, ML_STAT_(info) + IFACE_INFO_STAT_(bssid),
                  MAC2STR(ml_stat_ptr->info.bssid));
            ALOGI("%04lx|info.ap_country_str|%c%c%c",
                  ML_STAT_(info) + IFACE_INFO_STAT_(ap_country_str),
                  ml_stat_ptr->info.ap_country_str[0], ml_stat_ptr->info.ap_country_str[1],
                  ml_stat_ptr->info.ap_country_str[2]);
            ALOGI("%04lx|info.country_str|%c%c%c", ML_STAT_(info) + IFACE_INFO_STAT_(country_str),
                  ml_stat_ptr->info.country_str[0], ml_stat_ptr->info.country_str[1],
                  ml_stat_ptr->info.country_str[2]);
            ALOGI("%04lx|info.time_slicing_duty_cycle_percent|%" PRIu8,
                  ML_STAT_(info) + IFACE_INFO_STAT_(time_slicing_duty_cycle_percent),
                  ml_stat_ptr->info.time_slicing_duty_cycle_percent);
        }  // info

        ALOGI("%04lx|num_links|%d", ML_STAT_(num_links), ml_stat_ptr->num_links);

        // Count ml header
        current_offset += offsetof(wifi_iface_ml_stat, links);

        // links
        wifi_link_stat* link_ptr = ml_stat_ptr->links;
        for (int i = 0; i < ml_stat_ptr->num_links; ++i) {
            ALOGI("%04lx|links[%d].link_id|%" PRIu8, current_offset + LINK_STAT_(link_id), i,
                  link_ptr->link_id);
            ALOGI("%04lx|links[%d].state|%d", current_offset + LINK_STAT_(state), i,
                  (int)link_ptr->state);
            ALOGI("%04lx|links[%d].radio|%d", current_offset + LINK_STAT_(radio), i,
                  (int)link_ptr->radio);
            ALOGI("%04lx|links[%d].frequency|%" PRIu32, current_offset + LINK_STAT_(frequency), i,
                  link_ptr->frequency);
            ALOGI("%04lx|links[%d].beacon_rx|%" PRIu32, current_offset + LINK_STAT_(beacon_rx), i,
                  link_ptr->beacon_rx);
            ALOGI("%04lx|links[%d].average_tsf_offset|%" PRIu64,
                  current_offset + LINK_STAT_(average_tsf_offset), i, link_ptr->average_tsf_offset);
            ALOGI("%04lx|links[%d].leaky_ap_detected|%" PRIu32,
                  current_offset + LINK_STAT_(leaky_ap_detected), i, link_ptr->leaky_ap_detected);
            ALOGI("%04lx|links[%d].leaky_ap_avg_num_frames_leaked|%" PRIu32,
                  current_offset + LINK_STAT_(leaky_ap_avg_num_frames_leaked), i,
                  link_ptr->leaky_ap_avg_num_frames_leaked);
            ALOGI("%04lx|links[%d].leaky_ap_guard_time|%" PRIu32,
                  current_offset + LINK_STAT_(leaky_ap_guard_time), i,
                  link_ptr->leaky_ap_guard_time);
            ALOGI("%04lx|links[%d].mgmt_rx|%" PRIu32, current_offset + LINK_STAT_(mgmt_rx), i,
                  link_ptr->mgmt_rx);
            ALOGI("%04lx|links[%d].mgmt_action_rx|%" PRIu32,
                  current_offset + LINK_STAT_(mgmt_action_rx), i, link_ptr->mgmt_action_rx);
            ALOGI("%04lx|links[%d].mgmt_action_tx|%" PRIu32,
                  current_offset + LINK_STAT_(mgmt_action_tx), i, link_ptr->mgmt_action_tx);
            ALOGI("%04lx|links[%d].rssi_mgmt|%d", current_offset + LINK_STAT_(rssi_mgmt), i,
                  link_ptr->rssi_mgmt);
            ALOGI("%04lx|links[%d].rssi_data|%d", current_offset + LINK_STAT_(rssi_data), i,
                  link_ptr->rssi_data);
            ALOGI("%04lx|links[%d].rssi_ack|%d", current_offset + LINK_STAT_(rssi_ack), i,
                  link_ptr->rssi_ack);

            // wifi_wmm_ac_stat(s)
            for (int j = 0; j < WIFI_AC_MAX; ++j) {
                int ac_offset = current_offset + LINK_STAT_(ac) + sizeof(wifi_wmm_ac_stat) * j;
                ALOGI("%04lx|links[%d].ac[%d].ac|%d", ac_offset + WMM_STAT_(ac), i, j,
                      link_ptr->ac[j].ac);
                ALOGI("%04lx|links[%d].ac[%d].tx_mpdu|%" PRIu32, ac_offset + WMM_STAT_(tx_mpdu), i,
                      j, link_ptr->ac[j].tx_mpdu);
                ALOGI("%04lx|links[%d].ac[%d].rx_mpdu|%" PRIu32, ac_offset + WMM_STAT_(rx_mpdu), i,
                      j, link_ptr->ac[j].rx_mpdu);
                ALOGI("%04lx|links[%d].ac[%d].tx_mcast|%" PRIu32, ac_offset + WMM_STAT_(tx_mcast),
                      i, j, link_ptr->ac[j].tx_mcast);
                ALOGI("%04lx|links[%d].ac[%d].rx_mcast|%" PRIu32, ac_offset + WMM_STAT_(rx_mcast),
                      i, j, link_ptr->ac[j].rx_mcast);
                ALOGI("%04lx|links[%d].ac[%d].rx_ampdu|%" PRIu32, ac_offset + WMM_STAT_(rx_ampdu),
                      i, j, link_ptr->ac[j].rx_ampdu);
                ALOGI("%04lx|links[%d].ac[%d].tx_ampdu|%" PRIu32, ac_offset + WMM_STAT_(tx_ampdu),
                      i, j, link_ptr->ac[j].tx_ampdu);
                ALOGI("%04lx|links[%d].ac[%d].mpdu_lost|%" PRIu32, ac_offset + WMM_STAT_(mpdu_lost),
                      i, j, link_ptr->ac[j].mpdu_lost);
                ALOGI("%04lx|links[%d].ac[%d].retries|%" PRIu32, ac_offset + WMM_STAT_(retries), i,
                      j, link_ptr->ac[j].retries);
                ALOGI("%04lx|links[%d].ac[%d].retries_short|%" PRIu32,
                      ac_offset + WMM_STAT_(retries_short), i, j, link_ptr->ac[j].retries_short);
                ALOGI("%04lx|links[%d].ac[%d].retries_long|%" PRIu32,
                      ac_offset + WMM_STAT_(retries_long), i, j, link_ptr->ac[j].retries_long);
                ALOGI("%04lx|links[%d].ac[%d].contention_time_min|%" PRIu32,
                      ac_offset + WMM_STAT_(contention_time_min), i, j,
                      link_ptr->ac[j].contention_time_min);
                ALOGI("%04lx|links[%d].ac[%d].contention_time_max|%" PRIu32,
                      ac_offset + WMM_STAT_(contention_time_max), i, j,
                      link_ptr->ac[j].contention_time_max);
                ALOGI("%04lx|links[%d].ac[%d].contention_time_avg|%" PRIu32,
                      ac_offset + WMM_STAT_(contention_time_avg), i, j,
                      link_ptr->ac[j].contention_time_avg);
                ALOGI("%04lx|links[%d].ac[%d].contention_num_samples|%" PRIu32,
                      ac_offset + WMM_STAT_(contention_num_samples), i, j,
                      link_ptr->ac[j].contention_num_samples);
            }  // wifi_wmm_ac_stat(s)

            ALOGI("%04lx|links[%d].time_slicing_duty_cycle_percent|%" PRIu8,
                  current_offset + LINK_STAT_(time_slicing_duty_cycle_percent), i,
                  link_ptr->time_slicing_duty_cycle_percent);

            ALOGI("%04lx|links[%d].num_peers|%" PRIu32, current_offset + LINK_STAT_(num_peers), i,
                  link_ptr->num_peers);

            // Count link header
            current_offset += offsetof(wifi_link_stat, peer_info);

            // peers
            wifi_peer_info* peer_info_ptr = link_ptr->peer_info;
            for (int j = 0; j < link_ptr->num_peers; ++j) {
                ALOGI("%04lx|links[%d].peer_info[%d].type|%d", current_offset + PEER_INFO_(type), i,
                      j, peer_info_ptr->type);
                ALOGI("%04lx|links[%d].peer_info[%d].peer_mac_address|" MACSTR,
                      current_offset + PEER_INFO_(peer_mac_address), i, j,
                      MAC2STR(peer_info_ptr->peer_mac_address));
                ALOGI("%04lx|links[%d].peer_info[%d].capabilities|%" PRIu32,
                      current_offset + PEER_INFO_(capabilities), i, j, peer_info_ptr->capabilities);
                // peer_info[%d].bssload
                {
                    ALOGI("%04lx|links[%d].peer_info[%d].bssload.sta_count|%" PRIu16,
                          current_offset + PEER_INFO_(bssload) + BSSLOAD_INFO_(sta_count), i, j,
                          peer_info_ptr->bssload.sta_count);
                    ALOGI("%04lx|links[%d].peer_info[%d].bssload.chan_util|%" PRIu16,
                          current_offset + PEER_INFO_(bssload) + BSSLOAD_INFO_(chan_util), i, j,
                          peer_info_ptr->bssload.chan_util);
                }  // peer_info[%d].bssload
                ALOGI("%04lx|links[%d].peer_info[%d].num_rate|%" PRIu32,
                      current_offset + PEER_INFO_(num_rate), i, j, peer_info_ptr->num_rate);

                // Count peer header
                current_offset += offsetof(wifi_peer_info, rate_stats);

                // rates
                for (int k = 0; k < peer_info_ptr->num_rate; ++k) {
                    wifi_rate_stat* rate_stat_ptr = &(peer_info_ptr->rate_stats[k]);
                    // peer_info[%d].rate_stats[%d].rate
                    {
                        ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rate.preamble(3)|%01x",
                              current_offset + RATE_STAT_(rate), i, j, k,
                              rate_stat_ptr->rate.preamble);
                        ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rate.nss(2)|%01x",
                              current_offset + RATE_STAT_(rate), i, j, k, rate_stat_ptr->rate.nss);
                        ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rate.bw(3)|%01x",
                              current_offset + RATE_STAT_(rate), i, j, k, rate_stat_ptr->rate.bw);
                        ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rate.rateMcsIdx(8)|%"
                              "01x",
                              current_offset + RATE_STAT_(rate), i, j, k,
                              rate_stat_ptr->rate.rateMcsIdx);
                        ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rate.bitrate|%" PRIu32,
                              current_offset + RATE_STAT_(rate) + RATE_(bitrate), i, j, k,
                              rate_stat_ptr->rate.bitrate);
                    }  // peer_info[%d].rate_stats[%d].rate
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].tx_mpdu|%" PRIu32,
                          current_offset + RATE_STAT_(tx_mpdu), i, j, k, rate_stat_ptr->tx_mpdu);
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].rx_mpdu|%" PRIu32,
                          current_offset + RATE_STAT_(rx_mpdu), i, j, k, rate_stat_ptr->rx_mpdu);
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].mpdu_lost|%" PRIu32,
                          current_offset + RATE_STAT_(mpdu_lost), i, j, k,
                          rate_stat_ptr->mpdu_lost);
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].retries|%" PRIu32,
                          current_offset + RATE_STAT_(retries), i, j, k, rate_stat_ptr->retries);
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].retries_short|%" PRIu32,
                          current_offset + RATE_STAT_(retries_short), i, j, k,
                          rate_stat_ptr->retries_short);
                    ALOGI("%04lx|links[%d].peer_info[%d].rate_stats[%d].retries_long|%" PRIu32,
                          current_offset + RATE_STAT_(retries_long), i, j, k,
                          rate_stat_ptr->retries_long);

                    current_offset += sizeof(wifi_rate_stat);
                }  // rates

                peer_info_ptr = (wifi_peer_info*)((u8*)(ml_stat_ptr) + current_offset);
            }  // peers
            link_ptr = (wifi_link_stat*)((u8*)(ml_stat_ptr) + current_offset);
        }  // links
    }

    void dump_radio_stats(uint32_t num_radios, wifi_radio_stat* radio_stats_ptr,
                          wifi_iface_ml_stat* ml_stat_ptr) {
        wifi_radio_stat* current = radio_stats_ptr;
        unsigned long current_offset = (u8*)radio_stats_ptr - (u8*)ml_stat_ptr;
        for (int i = 0; i < num_radios; ++i) {
            ALOGI("%04lx|wifi_radio_stat[%d].radio|%d", current_offset + RADIO_STAT_(radio), i,
                  current->radio);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time), i, current->on_time);
            ALOGI("%04lx|wifi_radio_stat[%d].tx_time|%" PRIu32,
                  current_offset + RADIO_STAT_(tx_time), i, current->tx_time);
            ALOGI("%04lx|wifi_radio_stat[%d].num_tx_levels|%" PRIu32,
                  current_offset + RADIO_STAT_(num_tx_levels), i, current->num_tx_levels);
            ALOGI("%04lx|wifi_radio_stat[%d].tx_time_per_levels|%p",
                  current_offset + RADIO_STAT_(tx_time_per_levels), i, current->tx_time_per_levels);
            for (int j = 0; j < current->num_tx_levels; ++j) {
                ALOGI("....|wifi_radio_stat[%d].tx_time_per_levels[%d]|%" PRIu32, i, j,
                      current->tx_time_per_levels[j]);
            }
            ALOGI("%04lx|wifi_radio_stat[%d].rx_time|%" PRIu32,
                  current_offset + RADIO_STAT_(rx_time), i, current->rx_time);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_scan|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_scan), i, current->on_time_scan);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_nbd|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_nbd), i, current->on_time_nbd);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_gscan|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_gscan), i, current->on_time_gscan);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_roam_scan|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_roam_scan), i, current->on_time_roam_scan);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_pno_scan|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_pno_scan), i, current->on_time_pno_scan);
            ALOGI("%04lx|wifi_radio_stat[%d].on_time_hs20|%" PRIu32,
                  current_offset + RADIO_STAT_(on_time_hs20), i, current->on_time_hs20);
            ALOGI("%04lx|wifi_radio_stat[%d].num_channels|%" PRIu32,
                  current_offset + RADIO_STAT_(num_channels), i, current->num_channels);
            current_offset += offsetof(wifi_radio_stat, channels);
            for (int j = 0; j < current->num_channels; ++j) {
                // wifi_radio_stat[%d].channels[%d].channel
                {
                    ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].channel.width|%d",
                          current_offset + CHANNEL_STAT_(channel) + CHANNEL_INFO_(width), i, j,
                          current->channels[j].channel.width);
                    ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].channel.center_freq|%d",
                          current_offset + CHANNEL_STAT_(channel) + CHANNEL_INFO_(center_freq), i,
                          j, current->channels[j].channel.center_freq);
                    ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].channel.center_freq0|%d",
                          current_offset + CHANNEL_STAT_(channel) + CHANNEL_INFO_(center_freq0), i,
                          j, current->channels[j].channel.center_freq0);
                    ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].channel.center_freq1|%d",
                          current_offset + CHANNEL_STAT_(channel) + CHANNEL_INFO_(center_freq1), i,
                          j, current->channels[j].channel.center_freq1);
                }  // wifi_radio_stat[%d].channels[%d].channel
                ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].on_time|%" PRIu32,
                      current_offset + CHANNEL_STAT_(on_time), i, j, current->channels[j].on_time);
                ALOGI("%04lx|wifi_radio_stat[%d].channels[%d].cca_busy_time|%" PRIu32,
                      current_offset + CHANNEL_STAT_(cca_busy_time), i, j,
                      current->channels[j].cca_busy_time);
                current_offset += sizeof(wifi_channel_stat);
            }
            current = (wifi_radio_stat*)(((u8*)ml_stat_ptr) + current_offset);
        }
    }

// Clear to avoid macro polution
#undef ML_STAT_
#undef LINK_STAT_
#undef IFACE_INFO_STAT_
#undef WMM_STAT_
#undef PEER_INFO_
#undef BSSLOAD_INFO_
#undef RATE_STAT_
#undef RATE_
#undef RADIO_STAT_
#undef CHANNEL_STAT_
#undef CHANNEL_INFO_
};

// NOTE: Sync these with driver data structures.
const static size_t COMB_MATRIX_LEN = 6;
struct ANDROID_T_COMB_UNIT {
    uint8_t band_0;
    uint8_t ant_0;
    uint8_t band_1;
    uint8_t ant_1;
};

struct ANDROID_T_COMB_MATRIX {
    struct ANDROID_T_COMB_UNIT comb_mtx[COMB_MATRIX_LEN];
};

class GetSupportedRadioCombinationsMatrixCommand : public WifiCommand {
  private:
    int ctrl_iface;
    wifi_radio_combination_matrix* matrix;
    u32* rc_size;
    u32 set_size_max;

  public:
    GetSupportedRadioCombinationsMatrixCommand(
            wifi_handle handle, int ctrl_iface, u32 max_size, u32* size,
            wifi_radio_combination_matrix* radio_combination_matrix)
        : WifiCommand("GetSupportedRadioCombinationsMatrixCommand", handle, 0),
          ctrl_iface(ctrl_iface),
          matrix(radio_combination_matrix),
          rc_size(size),
          set_size_max(max_size) {}

    virtual int create() {
        int ret = mMsg.create(OUI_MTK, MTK_SUBCMD_GET_RADIO_COMBO_MATRIX);
        if (ret < 0) {
            ALOGE("Can't create message %d to send to driver - %d",
                  MTK_SUBCMD_GET_RADIO_COMBO_MATRIX, ret);
        }
        mMsg.put_u32(NL80211_ATTR_IFINDEX, ctrl_iface);
        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }
        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("GetSupportedRadioCombinationsMatrixCommand::handleResponse");
        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        // id & subcmd are only for debugging, remove before submit
        int id = reply.get_vendor_id();
        int subcmd = reply.get_vendor_subcmd();

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();

        ALOGV("Id = %0x, subcmd = %d, len = %d", id, subcmd, len);
        if (vendor_data == NULL || len == 0) {
            ALOGE("no vendor data in GetSupportedRadioCombinationsMatrixCommand response; ignore");
            return NL_SKIP;
        }

        for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
            if (it.get_type() == WIFI_ATTRIBUTE_RADIO_COMBINATIONS_MATRIX_MATRIX) {
                ALOGV("Got WIFI_ATTRIBUTE_RADIO_COMBINATIONS_MATRIX_MATRIX");
                void* data = it.get_data();
                int data_len = it.get_len();

                if (data == nullptr || data_len < sizeof(ANDROID_T_COMB_MATRIX)) {
                    ALOGE("Invalid buffer: data = %p, data_len=%d", data, data_len);
                    return NL_SKIP;
                }

                struct ANDROID_T_COMB_MATRIX* driver_matrix = (struct ANDROID_T_COMB_MATRIX*)data;
                int valid_combo_count = 0;
                if (NL_OK != check_sanity(driver_matrix, rc_size, &valid_combo_count)) {
                    ALOGE("Invalid buffer content");
                    return NL_SKIP;
                }
                if (set_size_max < *rc_size) {
                    ALOGE("Invalid buffer size");
                    return NL_SKIP;
                }

                // Fill buffer
                memset(matrix, 0, set_size_max);
                matrix->num_radio_combinations = valid_combo_count;
                wifi_radio_combination* p_combo = &(matrix->radio_combinations[0]);
                for (int i = 0; i < COMB_MATRIX_LEN; ++i) {
                    if (driver_matrix->comb_mtx[i].band_0 != 0) {
                        p_combo->num_radio_configurations = 1;
                        if (NL_OK != convert_band(driver_matrix->comb_mtx[i].band_0,
                                                  &(p_combo->radio_configurations[0].band))) {
                            return NL_SKIP;
                        }
                        if (NL_OK !=
                            convert_antenna(driver_matrix->comb_mtx[i].ant_0,
                                            &(p_combo->radio_configurations[0].antenna_cfg))) {
                            return NL_SKIP;
                        }
                        if (driver_matrix->comb_mtx[i].band_1 != 0) {
                            p_combo->num_radio_configurations += 1;
                            if (NL_OK != convert_band(driver_matrix->comb_mtx[i].band_1,
                                                      &(p_combo->radio_configurations[1].band))) {
                                return NL_SKIP;
                            }
                            if (NL_OK !=
                                convert_antenna(driver_matrix->comb_mtx[i].ant_1,
                                                &(p_combo->radio_configurations[1].antenna_cfg))) {
                                return NL_SKIP;
                            }
                        }
                        p_combo =
                                (wifi_radio_combination*)((uint8_t*)(p_combo) +
                                                          sizeof(wifi_radio_combination) +
                                                          sizeof(wifi_radio_configuration) *
                                                                  p_combo->num_radio_configurations);
                    }
                }
            } else {
                ALOGW("Ignoring invalid attribute type = %d, size = %d", it.get_type(),
                      it.get_len());
            }
        }

        ALOGV("GetSupportedRadioCombinationsMatrixCommand::Success");
        return NL_OK;
    }

    int convert_band(uint8_t band, wlan_mac_band* out) {
        switch (band) {
            case 2:
                *out = WLAN_MAC_2_4_BAND;
                return NL_OK;
            case 5:
                *out = WLAN_MAC_5_0_BAND;
                return NL_OK;
            case 6:
                *out = WLAN_MAC_6_0_BAND;
                return NL_OK;
            case 60:
                *out = WLAN_MAC_60_0_BAND;
                return NL_OK;
            default:
                ALOGE("Unsupported band %d", band);
                return NL_SKIP;
        }
    }

    int convert_antenna(uint8_t antenna, wifi_antenna_configuration* out) {
        switch (antenna) {
            case 1:
                *out = WIFI_ANTENNA_1X1;
                return NL_OK;
            case 2:
                *out = WIFI_ANTENNA_2X2;
                return NL_OK;
            case 3:
                *out = WIFI_ANTENNA_3X3;
                return NL_OK;
            case 5:
                *out = WIFI_ANTENNA_4X4;
                return NL_OK;
            default:
                ALOGE("Unsupported antenna %d", antenna);
                return NL_SKIP;
        }
    }

    int check_sanity(struct ANDROID_T_COMB_MATRIX* driver_matrix, u32* expected_size, int* count) {
        *expected_size = sizeof(wifi_radio_combination_matrix);
        *count = 0;
        for (int i = 0; i < COMB_MATRIX_LEN; ++i) {
            if (driver_matrix->comb_mtx[i].band_0 != 0) {
                *count += 1;
                *expected_size += sizeof(wifi_radio_combination);
                *expected_size += sizeof(wifi_radio_configuration);
                if (driver_matrix->comb_mtx[i].band_1 != 0) {
                    *expected_size += sizeof(wifi_radio_configuration);
                }
            }
        }
        return NL_OK;
    }

    void dump_matrix(wifi_radio_combination_matrix* matrix) {
        ALOGI("Dump wifi_radio_combination_matrix");
        ALOGI("num_radio_combinations=%d", matrix->num_radio_combinations);
        wifi_radio_combination* p_combo = &(matrix->radio_combinations[0]);
        for (int i = 0; i < matrix->num_radio_combinations; ++i) {
            ALOGI("wifi_radio_combination {");
            for (int j = 0; j < p_combo->num_radio_configurations; ++j) {
                ALOGI("  band=%d, antenna=%d", p_combo->radio_configurations[j].band,
                      p_combo->radio_configurations[j].antenna_cfg);
            }
            ALOGI("}");
            p_combo =
                    (wifi_radio_combination*)((uint8_t*)(p_combo) + sizeof(wifi_radio_combination) +
                                              sizeof(wifi_radio_configuration) *
                                                      p_combo->num_radio_configurations);
        }
        ALOGI("Dump wifi_radio_combination_matrix done");
    }
};

class GetUsableChannelCommand : public WifiCommand {
  private:
    int mCtrlIface;
    u32 mBandMask;
    u32 mIfaceModeMask;
    u32 mFilterMask;
    u32 mMaxSize;
    u32* mSize;
    wifi_usable_channel* mChannels;

  public:
    GetUsableChannelCommand(wifi_handle handle, int ctrl_iface, u32 band_mask, u32 iface_mode_mask,
                            u32 filter_mask, u32 max_size, u32* size, wifi_usable_channel* channels)
        : WifiCommand("GetUsableChannelCommand", handle, 0),
          mCtrlIface(ctrl_iface),
          mBandMask(band_mask),
          mIfaceModeMask(iface_mode_mask),
          mFilterMask(filter_mask),
          mMaxSize(max_size),
          mSize(size),
          mChannels(channels) {}

    int createRequest(WifiRequest& request) {
        ALOGV("GetUsableChannelCommand::createRequest");
        int result = request.create(OUI_MTK, MTK_SUBCMD_GET_USABLE_CHANNEL);
        if (result < 0) {
            ALOGE("Failed to create UsableChannel request; result = %d", result);
            return result;
        }

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);

        result = request.put_u32(WIFI_ATTRIBUTE_USABLE_CHANNEL_BAND, mBandMask);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to put log level; result = %d", result);
            return result;
        }
        result = request.put_u32(WIFI_ATTRIBUTE_USABLE_CHANNEL_IFACE, mIfaceModeMask);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to put ring flags; result = %d", result);
            return result;
        }
        result = request.put_u32(WIFI_ATTRIBUTE_USABLE_CHANNEL_FILTER, mFilterMask);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to put usablechan filter; result = %d", result);
            return result;
        }
        result = request.put_u32(WIFI_ATTRIBUTE_USABLE_CHANNEL_MAX_SIZE, mMaxSize);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to put usablechan max_size; result = %d", result);
            return result;
        }
        request.attr_end(data);

        return WIFI_SUCCESS;
    }

    int start() {
        ALOGV("GetUsableChannelCommand::start");
        WifiRequest request(familyId(), mCtrlIface);
        int result = createRequest(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("failed to create request, result = %d", result);
            return result;
        }

        result = requestResponse(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("failed to get usable channel; result = %d", result);
        }

        return result;
    }

    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("GetUsableChannelCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();
        wifi_usable_channel* channels(mChannels);

        if (vendor_data == NULL || len == 0) {
            ALOGE("No data found");
            return NL_SKIP;
        }

        nl_iterator it(vendor_data);
        if (it.get_type() == WIFI_ATTRIBUTE_USABLE_CHANNEL_RESP_ARRAY) {
            if (it.get_len() < sizeof(((wifi_usable_channel_response*)0)->array_size)) {
                ALOGE("Invalid response size: %d", it.get_len());
                return NL_SKIP;
            }
            wifi_usable_channel_response* resp_data = (wifi_usable_channel_response*)it.get_data();
            ALOGV("resp_data->array_size=%" PRIu16, resp_data->array_size);
            if (it.get_len() < sizeof(wifi_usable_channel) * resp_data->array_size +
                                       sizeof(wifi_usable_channel_response)) {
                ALOGE("Invalid response size: %d, array size: %" PRIu16, it.get_len(),
                      resp_data->array_size);
                return NL_SKIP;
            }
            *mSize = resp_data->array_size;
            memcpy(channels, resp_data->channel_array,
                   sizeof(wifi_usable_channel) * resp_data->array_size);
        } else {
            ALOGE("Unknown attribute: %d expecting %d", it.get_type(),
                  WIFI_ATTRIBUTE_USABLE_CHANNEL_RESP_ARRAY);
            return NL_SKIP;
        }

        return NL_OK;
    }
};

class GetChipCapabilitiesCommand : public WifiCommand {
  private:
    int mCtrlIface;
    wifi_chip_capabilities* mCapabilities;

  public:
    GetChipCapabilitiesCommand(wifi_handle handle, int ctrl_iface,
                               wifi_chip_capabilities* capabilities)
        : WifiCommand("GetChipCapabilitiesCommand", handle, 0),
          mCtrlIface(ctrl_iface),
          mCapabilities(capabilities) {}

    virtual int create() {
        ALOGV("GetChipCapabilitiesCommand::create");
        int ret = mMsg.create(OUI_MTK, MTK_SUBCMD_GET_CHIP_CAPABILITIES);
        if (ret < 0) {
            ALOGE("Can't create message %d to send to driver - %d",
                  MTK_SUBCMD_GET_CHIP_CAPABILITIES, ret);
        }
        mMsg.put_u32(NL80211_ATTR_IFINDEX, mCtrlIface);
        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }
        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }

    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("GetChipCapabilitiesCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();

        if (vendor_data == NULL || len == 0) {
            ALOGE("No data found");
            return NL_SKIP;
        }
        // UINT32_MAX will be mapped to -1 after convertLegacyWifiChipCapabilitiesToAidl
        // assigns uint32_t to int. We expect this to work with framework properly (unsupported
        // value should be -1).
        mCapabilities->max_mlo_association_link_count = UINT32_MAX;
        mCapabilities->max_mlo_str_link_count = UINT32_MAX;
        mCapabilities->max_concurrent_tdls_session_count = UINT32_MAX;

        nl_iterator it(vendor_data);
        for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
            if (it.get_type() ==
                WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_MLO_ASSOCIATION_LINK_COUNT) {
                mCapabilities->max_mlo_association_link_count = it.get_u32();
            } else if (it.get_type() ==
                       WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_MLO_STR_LINK_COUNT) {
                mCapabilities->max_mlo_str_link_count = it.get_u32();
            } else if (it.get_type() ==
                       WIFI_ATTRIBUTE_CHIP_CAPABILITIES_RESP_MAX_CONCURRENT_TDLS_SESSION_COUNT) {
                mCapabilities->max_concurrent_tdls_session_count = it.get_u32();
            } else {
                ALOGW("Ignore invalid attribute type = %d", it.get_type());
            }
        }

        return NL_OK;
    }
};

class GetChipConcurrencyMatrixCommand : public WifiCommand {
  private:
    int mCtrlIface;
    wifi_iface_concurrency_matrix* mMatrix;

  public:
    GetChipConcurrencyMatrixCommand(wifi_handle handle, int ctrl_iface,
                                    wifi_iface_concurrency_matrix* matrix)
        : WifiCommand("GetChipConcurrencyMatrixCommand", handle, 0),
          mCtrlIface(ctrl_iface),
          mMatrix(matrix) {}

    int createRequest(WifiRequest& request) {
        ALOGV("GetChipConcurrencyMatrixCommand::createRequest");
        int result = request.create(OUI_MTK, MTK_SUBCMD_GET_CHIP_CONCURRENCY_MATRIX);
        if (result < 0) {
            ALOGE("Failed to create GetChipConcurrencyMatrix request; result = %d", result);
            return result;
        }

        request.put_u32(NL80211_ATTR_IFINDEX, mCtrlIface);

        return WIFI_SUCCESS;
    }

    int start() {
        ALOGV("GetChipConcurrencyMatrixCommand::start");
        WifiRequest request(familyId(), mCtrlIface);
        int result = createRequest(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("failed to create request, result = %d", result);
            return result;
        }

        result = requestResponse(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("failed to get usable channel; result = %d", result);
        }

        return result;
    }

    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("GetChipConcurrencyMatrixCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        nlattr* vendor_data = reply.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = reply.get_vendor_data_len();

        if (vendor_data == NULL || len == 0) {
            ALOGE("No data found");
            return NL_SKIP;
        }

        memset(mMatrix, 0, sizeof(*mMatrix));

        for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
            if (it.get_type() == WIFI_ATTRIBUTE_CONCURRENCY_MATRIX) {
                if (it.get_len() <= sizeof(*mMatrix)) {
                    memcpy(mMatrix, it.get_data(), it.get_len());
                } else {
                    ALOGE("Buffer size mismatch!");
                    return NL_SKIP;
                }
            } else {
                ALOGW("Ignore invalid attribute type = %d, size = %d", it.get_type(), it.get_len());
            }
        }

        return NL_OK;
    }
};

/******************************************************************************/
/* HAL functions.                                                             */
/******************************************************************************/

static int wifi_get_multicast_id(wifi_handle handle, const char* name, const char* group) {
    GetMulticastIdCommand cmd(handle, name, group);
    int res = cmd.requestResponse();
    if (res < 0)
        return res;
    else
        return cmd.getId();
}

static bool is_wifi_interface(const char* name) {
    if (strncmp(name, "wlan", 4) != 0 && strncmp(name, "p2p", 3) != 0 &&
        strncmp(name, "ap", 2) != 0) {
        /* Not a wifi interface; ignore it */
        return false;
    } else {
        return true;
    }
}

int get_interface(const char* name, interface_info* info) {
    strncpy(info->name, name, sizeof(info->name));
    info->name[sizeof(info->name) - 1] = '\0';
    info->id = if_nametoindex(name);
    ALOGV("found an interface : %s, id = %d", name, info->id);
    return WIFI_SUCCESS;
}

wifi_error wifi_init_interfaces(wifi_handle handle) {
    ALOGI("init wifi interfaces");
    hal_info* info = (hal_info*)handle;
    struct dirent* de;

    DIR* d = opendir("/sys/class/net");
    if (d == 0) return WIFI_ERROR_UNKNOWN;

    int n = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (is_wifi_interface(de->d_name)) {
            n++;
        }
    }

    closedir(d);

    if (n == 0) return WIFI_ERROR_NOT_AVAILABLE;

    d = opendir("/sys/class/net");
    if (d == 0) return WIFI_ERROR_UNKNOWN;

    info->interfaces = (interface_info**)malloc(sizeof(interface_info*) * n);
    if (!info->interfaces) {
        closedir(d);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }
    memset(info->interfaces, 0, sizeof(interface_info*) * n);

    int i = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (is_wifi_interface(de->d_name)) {
            interface_info* ifinfo = (interface_info*)malloc(sizeof(interface_info));
            if (ifinfo == NULL) {
                ALOGE("%s: Error ifinfo NULL", __FUNCTION__);
                while (i > 0) {
                    free(info->interfaces[i - 1]);
                    i--;
                }
                free(info->interfaces);
                closedir(d);
                return WIFI_ERROR_OUT_OF_MEMORY;
            }
            if (get_interface(de->d_name, ifinfo) != WIFI_SUCCESS) {
                free(ifinfo);
                continue;
            }
            ifinfo->handle = handle;
            info->interfaces[i] = ifinfo;
            i++;
        }
    }

    closedir(d);

    info->num_interfaces = n;
    return WIFI_SUCCESS;
}

wifi_error wifi_get_ifaces(wifi_handle handle, int* num, wifi_interface_handle** interfaces) {
    hal_info* info = (hal_info*)handle;

    ALOGI("%s: reinit to sync dynamically created/removed ifaces", __FUNCTION__);
    // reinit to sync dynamically created/removed ifaces
    if (info->num_interfaces > 0) {
        for (int i = 0; i < info->num_interfaces; i++) free(info->interfaces[i]);
        free(info->interfaces);
        info->interfaces = NULL;
        info->num_interfaces = 0;
    }

    wifi_error ret = wifi_init_interfaces(handle);
    if (ret != WIFI_SUCCESS) {
        ALOGE("Failed to reinit interfaces");
        return ret;
    }

    *interfaces = (wifi_interface_handle*)info->interfaces;
    *num = info->num_interfaces;

    return WIFI_SUCCESS;
}

wifi_error wifi_get_iface_name(wifi_interface_handle iface, char* name, size_t size) {
    if (iface == nullptr || name == nullptr) {
        ALOGE("%s: invalid args: iface=%p, name=%p", __FUNCTION__, (void*)iface, name);
        return WIFI_ERROR_INVALID_ARGS;
    }
    interface_info* info = (interface_info*)iface;
    strncpy(name, info->name, size);
    name[size - 1] = '\0';
    return WIFI_SUCCESS;
}

wifi_error wifi_get_supported_feature_set(wifi_interface_handle iface, feature_set* pset) {
    if (iface == nullptr || pset == nullptr) {
        ALOGE("%s: invalid args: iface=%p, pset=%p", __FUNCTION__, (void*)iface, pset);
        return WIFI_ERROR_INVALID_ARGS;
    }
    GetFeatureSetCommand command(iface, WIFI_ATTRIBUTE_FEATURE_SET, pset, NULL, NULL, 1);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_set_country_code(wifi_interface_handle iface, const char* country_code) {
    if (iface == nullptr || country_code == nullptr) {
        ALOGE("%s: invalid args: iface=%p, country_code=%p", __FUNCTION__, (void*)iface,
              country_code);
        return WIFI_ERROR_INVALID_ARGS;
    }
    SetCountryCodeCommand command(iface, country_code);
    return (wifi_error)command.requestResponse();
}

static wifi_error wifi_start_rssi_monitoring(wifi_request_id id, wifi_interface_handle iface,
                                             s8 max_rssi, s8 min_rssi, wifi_rssi_event_handler eh) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    wifi_handle handle = getWifiHandle(iface);
    SetRSSIMonitorCommand* cmd = new SetRSSIMonitorCommand(id, iface, max_rssi, min_rssi, eh);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = wifi_register_cmd(handle, id, cmd);
    if (result != WIFI_SUCCESS) {
        cmd->releaseRef();
        return result;
    }
    result = (wifi_error)cmd->start();
    if (result != WIFI_SUCCESS) {
        wifi_unregister_cmd(handle, id);
        cmd->releaseRef();
        return result;
    }
    return result;
}

static wifi_error wifi_stop_rssi_monitoring(wifi_request_id id, wifi_interface_handle iface) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }

    if (id == -1) {
        wifi_rssi_event_handler handler;
        s8 max_rssi = 0, min_rssi = 0;
        memset(&handler, 0, sizeof(handler));
        SetRSSIMonitorCommand* cmd =
                new SetRSSIMonitorCommand(id, iface, max_rssi, min_rssi, handler);
        NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
        cmd->cancel();
        cmd->releaseRef();
        return WIFI_SUCCESS;
    }
    return wifi_cancel_cmd(id, iface);
}

wifi_error wifi_get_roaming_capabilities(wifi_interface_handle iface,
                                         wifi_roaming_capabilities* caps) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }

    wifi_handle handle = getWifiHandle(iface);
    hal_info* info = getHalInfo(handle);

    if (!info) {
        ALOGE("%s: hal_info is NULL", __FUNCTION__);
        return WIFI_ERROR_INVALID_ARGS;
    }

    // first time to get roaming cap
    if (info->roaming_capa.max_blacklist_size == 0 && info->roaming_capa.max_whitelist_size == 0) {
        int size = 0;
        GetFeatureSetCommand command(iface, WIFI_ATTRIBUTE_ROAMING_CAPABILITIES, NULL,
                                     (feature_set*)caps, &size, 2);
        wifi_error ret = (wifi_error)command.requestResponse();
        if (ret == WIFI_SUCCESS) {
            info->roaming_capa.max_blacklist_size = caps->max_blacklist_size;
            info->roaming_capa.max_whitelist_size = caps->max_whitelist_size;
        }
        return ret;
    } else {
        memcpy(caps, &info->roaming_capa, sizeof(wifi_roaming_capabilities));
    }

    return WIFI_SUCCESS;
}

wifi_error wifi_configure_roaming(wifi_interface_handle iface,
                                  wifi_roaming_config* roaming_config) {
    if (iface == nullptr || roaming_config == nullptr) {
        ALOGE("%s: invalid args: iface=%p, roaming_config=%p", __FUNCTION__, (void*)iface,
              roaming_config);
        return WIFI_ERROR_INVALID_ARGS;
    }

    wifi_handle handle = getWifiHandle(iface);
    hal_info* info = getHalInfo(handle);

    /* Set bssid blacklist */
    if (roaming_config->num_blacklist_bssid > info->roaming_capa.max_blacklist_size) {
        ALOGE("%s: Number of blacklist bssids(%d) provided is more than maximum blacklist "
              "bssids(%d)"
              " supported",
              __FUNCTION__, roaming_config->num_blacklist_bssid,
              info->roaming_capa.max_blacklist_size);
        return WIFI_ERROR_NOT_SUPPORTED;
    }

    /* Set ssid whitelist */
    if (roaming_config->num_whitelist_ssid > info->roaming_capa.max_whitelist_size) {
        ALOGE("%s: Number of whitelist ssid(%d) provided is more than maximum whitelist ssids(%d) "
              "supported",
              __FUNCTION__, roaming_config->num_whitelist_ssid,
              info->roaming_capa.max_whitelist_size);
        return WIFI_ERROR_NOT_SUPPORTED;
    }

    ConfigRoamingCommand command(iface, roaming_config);
    return (wifi_error)command.start();
}

wifi_error wifi_enable_firmware_roaming(wifi_interface_handle iface, fw_roaming_state_t state) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    EnableRoamingCommand command(iface, state);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_select_tx_power_scenario(wifi_interface_handle iface,
                                         wifi_power_scenario scenario) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    SelectTxPowerCommand command(iface, scenario);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_reset_tx_power_scenario(wifi_interface_handle iface) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    SelectTxPowerCommand command(iface, -1);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_set_scanning_mac_oui(wifi_interface_handle iface, oui scan_oui) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    SetScanMacOuiCommand command(iface, scan_oui);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_get_valid_channels(wifi_interface_handle iface, int band, int max_channels,
                                   wifi_channel* channels, int* num_channels) {
    if (iface == nullptr || channels == nullptr || num_channels == nullptr) {
        ALOGE("%s: invalid args: iface=%p, channels=%p, num_channels=%p", __FUNCTION__,
              (void*)iface, channels, num_channels);
        return WIFI_ERROR_INVALID_ARGS;
    }
    GetChannelListCommand command(iface, band, max_channels, channels, num_channels);
    return (wifi_error)command.requestResponse();
}

/////////////////////////////////////////////////////////////////////////////

static wifi_error wifi_get_wake_reason_stats_dummy(
        wifi_interface_handle iface, WLAN_DRIVER_WAKE_REASON_CNT* wifi_wake_reason_cnt) {
    if (iface == nullptr || wifi_wake_reason_cnt == nullptr) {
        ALOGE("%s: invalid args: iface=%p, wifi_wake_reason_cnt=%p", __FUNCTION__, (void*)iface,
              wifi_wake_reason_cnt);
        return WIFI_ERROR_INVALID_ARGS;
    }

    memset(wifi_wake_reason_cnt, 0, sizeof(*wifi_wake_reason_cnt));
    return WIFI_SUCCESS;
}

static wifi_error wifi_get_packet_filter_capabilities(wifi_interface_handle iface, u32* version,
                                                      u32* max_len) {
    if (iface == nullptr || version == nullptr || max_len == nullptr) {
        ALOGE("%s: invalid args: iface=%p, version=%p, max_len=%p", __FUNCTION__, (void*)iface,
              version, max_len);
        return WIFI_ERROR_INVALID_ARGS;
    }

    AndroidPacketFilterCommand* cmd = new AndroidPacketFilterCommand(iface, version, max_len);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = (wifi_error)cmd->start();
    if (result == WIFI_SUCCESS) {
        ALOGV("Getting APF capability, version = %d, max_len = %d\n", *version, *max_len);
    }
    if (result == WIFI_ERROR_NOT_SUPPORTED) {
        result = WIFI_SUCCESS;
        *version = 0;
        *max_len = 0;
    }
    cmd->releaseRef();
    return result;
}

static wifi_error wifi_set_packet_filter(wifi_interface_handle iface, const u8* program, u32 len) {
    if (iface == nullptr || program == nullptr || len == 0) {
        ALOGE("%s: invalid args: iface=%p, program=%p, len=%d", __FUNCTION__, (void*)iface, program,
              (int)len);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGV("Setting APF program, iface = %p\n", iface);
    AndroidPacketFilterCommand* cmd = new AndroidPacketFilterCommand(iface, program, len);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = (wifi_error)cmd->start();
    cmd->releaseRef();
    return result;
}

static wifi_error wifi_read_packet_filter(wifi_interface_handle iface, u32 src_offset, u8* host_dst,
                                          u32 length) {
    if (iface == nullptr || host_dst == nullptr || length == 0) {
        ALOGE("%s: invalid args: iface=%p, host_dst=%p, length=%d", __FUNCTION__, (void*)iface,
              host_dst, (int)length);
        return WIFI_ERROR_INVALID_ARGS;
    }

    AndroidPacketFilterCommand* cmd = new AndroidPacketFilterCommand(iface, host_dst, length);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = (wifi_error)cmd->start();
    if (result == WIFI_SUCCESS) {
        ALOGI("Read APF program success\n");
    }
    cmd->releaseRef();
    return result;
}

static wifi_error wifi_get_tx_pkt_fates_dummy(wifi_interface_handle iface,
                                              wifi_tx_report* tx_report_bufs,
                                              size_t n_requested_fates, size_t* n_provided_fates) {
    if (iface == nullptr || tx_report_bufs == nullptr || n_provided_fates == nullptr) {
        ALOGE("%s: invalid args: iface=%p, tx_report_bufs=%p, n_provided_fates=%p", __FUNCTION__,
              (void*)iface, tx_report_bufs, n_provided_fates);
        return WIFI_ERROR_INVALID_ARGS;
    }

    *n_provided_fates = 0;
    return WIFI_SUCCESS;
}

static wifi_error wifi_get_rx_pkt_fates_dummy(wifi_interface_handle iface,
                                              wifi_rx_report* rx_report_bufs,
                                              size_t n_requested_fates, size_t* n_provided_fates) {
    if (iface == nullptr || rx_report_bufs == nullptr || n_provided_fates == nullptr) {
        ALOGE("%s: invalid args: iface=%p, rx_report_bufs=%p, n_provided_fates=%p", __FUNCTION__,
              (void*)iface, rx_report_bufs, n_provided_fates);
        return WIFI_ERROR_INVALID_ARGS;
    }

    *n_provided_fates = 0;
    return WIFI_SUCCESS;
}

static wifi_error wifi_get_ring_buffers_status_dummy(wifi_interface_handle iface, u32* num_rings,
                                                     wifi_ring_buffer_status* status) {
    if (iface == nullptr || num_rings == nullptr || status == nullptr) {
        ALOGE("%s: invalid args: iface=%p, num_rings=%p, status=%p", __FUNCTION__, (void*)iface,
              num_rings, status);
        return WIFI_ERROR_INVALID_ARGS;
    }

    *num_rings = 1;
    memset(status, 0, sizeof(*status));
    strncpy((char*)status->name, "dummy", sizeof(status->name));
    ((char*)status->name)[sizeof(status->name) - 1] = 0;
    return WIFI_SUCCESS;
}

static wifi_error wifi_get_logger_supported_feature_set_dummy(wifi_interface_handle iface,
                                                              unsigned int* support) {
    if (iface == nullptr || support == nullptr) {
        ALOGE("%s: invalid args: iface=%p, support=%p", __FUNCTION__, (void*)iface, support);
        return WIFI_ERROR_INVALID_ARGS;
    }

    *support = 0;
    return WIFI_SUCCESS;
}

wifi_error wifi_trigger_subsystem_restart(wifi_handle handle) {
    /*
     * Disable SSR to avoid massive EE during MTBF test casued by DHCP failure.
     */
    return WIFI_ERROR_NOT_SUPPORTED;
#if 0
    if (handle == nullptr) {
        ALOGE("%s: invalid args: handle=%p", __FUNCTION__, (void *) handle);
        return WIFI_ERROR_INVALID_ARGS;
    }

    WifiVendorCommand command("TriggerSubsystemRestartCommand", handle, 0, OUI_MTK, MTK_SUBCMD_TRIGGER_RESET);
    wifi_error result = (wifi_error) command.requestResponse();
    if (result != WIFI_SUCCESS) {
        ALOGE("%s: Failed to trigger subsystem reset: result=%d", __FUNCTION__, (int) result);
    }

    // Do not invoke info->restart_handler.on_subsystem_restart as
    // driver will send reset event.

    return result;
#endif
}

#define HAL_SUBSYSTEM_RESET_ID 0

wifi_error wifi_set_subsystem_restart_handler(wifi_handle handle,
                                              wifi_subsystem_restart_handler handler) {
    if (handle == nullptr) {
        ALOGE("%s: invalid args: handle=%p", __FUNCTION__, (void*)handle);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("Set subsystem restart handler %p", handler.on_subsystem_restart);

    SetSubsystemRestartHandlerCommand* cmd = new SetSubsystemRestartHandlerCommand(handle, handler);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = wifi_register_cmd(handle, HAL_SUBSYSTEM_RESET_ID, cmd);
    if (result != WIFI_SUCCESS) {
        cmd->releaseRef();
        return result;
    }
    result = (wifi_error)cmd->start();
    if (result != WIFI_SUCCESS) {
        wifi_unregister_cmd(handle, HAL_SUBSYSTEM_RESET_ID);
        cmd->releaseRef();
        return result;
    }

    /* Cache the handler to use it for trigger subsystem restart */
    ((hal_info*)handle)->restart_handler = handler;
    return result;
}

static std::vector<std::string> hal_created_ifaces;

wifi_error wifi_virtual_interface_create(wifi_handle handle, const char* ifname,
                                         wifi_interface_type iface_type) {
    if (!handle || ifname == nullptr) {
        ALOGE("%s: invalid args: handle=%p, ifname=%p", __FUNCTION__, (void*)handle, ifname);
        return WIFI_ERROR_INVALID_ARGS;
    }

    nl80211_iftype type;
    switch (iface_type) {
        case WIFI_INTERFACE_TYPE_STA:
            type = NL80211_IFTYPE_STATION;
            break;
        case WIFI_INTERFACE_TYPE_AP:
            type = NL80211_IFTYPE_AP;
            break;
        case WIFI_INTERFACE_TYPE_P2P:
            type = NL80211_IFTYPE_P2P_DEVICE;
            break;
        case WIFI_INTERFACE_TYPE_NAN:
            type = NL80211_IFTYPE_NAN;
            break;
        default:
            ALOGE("%s: Unknown interface type %u", __FUNCTION__, iface_type);
            return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    if (if_nametoindex(ifname)) {
        ALOGV("%s: %s already exists, ignore", __FUNCTION__, ifname);
        return WIFI_SUCCESS;
    }

    ALOGI("%s: create iface %s", __FUNCTION__, ifname);

    AddInterfaceCommand command(handle, ctrl_iface_index, ifname, type);
    wifi_error result = (wifi_error)command.requestResponse();
    ALOGI("%s: result=%d", __FUNCTION__, (int)result);
    if (result == WIFI_SUCCESS) {
        hal_created_ifaces.push_back(ifname);
        ALOGI("%s: existing HAL created ifaces:", __FUNCTION__);
        for (const auto& iface : hal_created_ifaces) {
            ALOGI("\t%s", iface.c_str());
        }
    }
    return result;
}

wifi_error wifi_virtual_interface_delete(wifi_handle handle, const char* ifname) {
    if (!handle || ifname == nullptr) {
        ALOGE("%s: invalid args: handle=%p, ifname=%p", __FUNCTION__, (void*)handle, ifname);
        return WIFI_ERROR_INVALID_ARGS;
    }

    if (!is_hal_created_iface(ifname)) {
        ALOGI("%s: ignore iface not created by HAL: %s", __FUNCTION__, ifname);
        return WIFI_SUCCESS;
    }

    ALOGI("%s: delete iface %s", __FUNCTION__, ifname);
    DeleteInterfaceCommand command(handle, ifname);

    wifi_error result = (wifi_error)command.requestResponse();
    ALOGI("%s: result=%d", __FUNCTION__, (int)result);
    hal_created_ifaces.erase(
            std::remove(hal_created_ifaces.begin(), hal_created_ifaces.end(), std::string(ifname)),
            hal_created_ifaces.end());
    ALOGI("%s: remaining HAL created ifaces:", __FUNCTION__);
    for (const auto& iface : hal_created_ifaces) {
        ALOGI("\t%s", iface.c_str());
    }
    return result;
}

static void remove_all_hal_created_ifaces(wifi_handle handle) {
    for (const auto& iface : hal_created_ifaces) {
        wifi_virtual_interface_delete(handle, iface.c_str());
    }
    hal_created_ifaces.clear();
    ALOGV("Done removing HAL created ifaces");
}

static bool is_hal_created_iface(const char* ifname) {
    for (const auto& iface : hal_created_ifaces) {
        if (iface == std::string(ifname)) return true;
    }
    return false;
}

wifi_error wifi_multi_sta_set_primary_connection(wifi_handle handle, wifi_interface_handle iface) {
    if (handle == nullptr || iface == nullptr) {
        ALOGE("%s: invalid args: handle=%p, iface=%p", __FUNCTION__, (void*)handle, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("%s: Set primary to %s", __FUNCTION__, getIfaceInfo(iface)->name);
    MultiStaSetPrimaryCommand command(iface);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_multi_sta_set_use_case(wifi_handle handle, wifi_multi_sta_use_case use_case) {
    if (handle == nullptr) {
        ALOGE("%s: handle is null", __FUNCTION__);
        return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    ALOGI("%s: use_case=%d", __FUNCTION__, (int)use_case);
    MultiStaSetUseCaseCommand command(handle, ctrl_iface_index, use_case);
    return (wifi_error)command.requestResponse();
}

wifi_interface_handle wifi_get_iface_handle(wifi_handle handle, char* name) {
    if (handle == nullptr || name == nullptr) {
        ALOGE("%s: invalid args: handle=%p, name=%p", __FUNCTION__, (void*)handle, name);
        return NULL;
    }
    hal_info* info = (hal_info*)handle;
    for (int i = 0; i < info->num_interfaces; i++) {
        if (!strcmp(info->interfaces[i]->name, name)) {
            return ((wifi_interface_handle)(info->interfaces)[i]);
        }
    }
    return NULL;
}

wifi_error wifi_get_link_stats(wifi_request_id id, wifi_interface_handle iface,
                               wifi_stats_result_handler handler) {
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }

    GetLinkStatsCommand command(iface, handler);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_set_link_stats(wifi_interface_handle iface, wifi_link_layer_params params) {
    ALOGV("%s: not supported", __FUNCTION__);
    if (iface == nullptr) {
        ALOGE("%s: invalid args: iface=%p", __FUNCTION__, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }
    return WIFI_SUCCESS;
}

wifi_error wifi_clear_link_stats(wifi_interface_handle iface, u32 stats_clear_req_mask,
                                 u32* stats_clear_rsp_mask, u8 stop_req, u8* stop_rsp) {
    ALOGV("%s: not supported", __FUNCTION__);
    if (iface == nullptr || stats_clear_rsp_mask == nullptr || stop_rsp == nullptr) {
        ALOGE("%s: invalid args: iface=%p, stats_clear_rsp_mask=%p, stop_rsp=%p", __FUNCTION__,
              (void*)iface, stats_clear_rsp_mask, stop_rsp);
        return WIFI_ERROR_INVALID_ARGS;
    }

    return WIFI_SUCCESS;
}

wifi_error wifi_get_supported_radio_combinations_matrix(
        wifi_handle handle, u32 max_size, u32* size,
        wifi_radio_combination_matrix* radio_combination_matrix) {
    if (handle == nullptr) {
        ALOGE("%s: handle is null", __FUNCTION__);
        return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    GetSupportedRadioCombinationsMatrixCommand command(handle, ctrl_iface_index, max_size, size,
                                                       radio_combination_matrix);
    return (wifi_error)command.requestResponse();
}

#define PRIV_CMD_SIZE 512

typedef struct android_wifi_priv_cmd {
    char buf[PRIV_CMD_SIZE];
    int used_len;
    int total_len;
} android_wifi_priv_cmd;

wifi_error wifi_set_scan_mode(wifi_interface_handle handle, bool enable) {
    if (handle == nullptr) {
        ALOGE("%s: invalid args: handle=%p", __FUNCTION__, handle);
        return WIFI_ERROR_INVALID_ARGS;
    }

    interface_info *info = (interface_info *)handle;
    const char* ifname = info->name;
    if (ifname == nullptr) {
        ALOGE("%s: invalid args: ifname=%p", __FUNCTION__, ifname);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("%s: ifname=%s, enable=%s", __FUNCTION__, ifname, enable ? "true" : "false");

    int ioctl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ioctl_sock < 0) {
        ALOGE("%s: failed to create socket: ret=%d, errno=%s", __FUNCTION__, ioctl_sock,
              strerror(errno));
        return WIFI_ERROR_UNKNOWN;
    }

    struct ifreq ifr;
    android_wifi_priv_cmd priv_cmd;

    memset(&ifr, 0, sizeof(struct ifreq));
    memset(&priv_cmd, 0, sizeof(priv_cmd));

    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    int ret = snprintf(priv_cmd.buf, sizeof(priv_cmd.buf), "set_fw_param alwaysscanen %d",
                       (enable ? 1 : 0));
    if (ret < 0 || ret >= PRIV_CMD_SIZE) {
        ALOGE("%s: snprintf failed", __FUNCTION__);
        close(ioctl_sock);
        return WIFI_ERROR_UNKNOWN;
    }
    priv_cmd.used_len = strlen(priv_cmd.buf) + 1;
    priv_cmd.total_len = PRIV_CMD_SIZE;

    ifr.ifr_data = &priv_cmd;

    ret = ioctl(ioctl_sock, SIOCDEVPRIVATE + 1, &ifr);
    close(ioctl_sock);
    if (ret < 0) {
        ALOGE("%s: ioctl: cmd=%s, ret=%d, error=%s", __FUNCTION__, priv_cmd.buf, ret,
              strerror(errno));
        return WIFI_ERROR_UNKNOWN;
    }
    ALOGV("%s: set scan mode to %s successfully", __FUNCTION__, enable ? "true" : "false");

    return WIFI_SUCCESS;
}

wifi_error wifi_get_usable_channels(wifi_handle handle, u32 band_mask, u32 iface_mode_mask,
                                    u32 filter_mask, u32 max_size, u32* size,
                                    wifi_usable_channel* channels) {
    if (handle == nullptr || size == nullptr || channels == nullptr) {
        ALOGE("%s: invalid args: handle=%p, size=%p, channels=%p", __FUNCTION__, handle, size,
              channels);
        return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    GetUsableChannelCommand command(handle, ctrl_iface_index, band_mask, iface_mode_mask,
                                    filter_mask, max_size, size, channels);
    return (wifi_error)command.start();
}

wifi_error wifi_get_chip_capabilities(wifi_handle handle,
                                      wifi_chip_capabilities* chip_capabilities) {
    if (handle == nullptr || chip_capabilities == nullptr) {
        ALOGE("%s: invalid args: handle=%p, chip_capabilities=%p", __FUNCTION__, handle,
              chip_capabilities);
        return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    GetChipCapabilitiesCommand command(handle, ctrl_iface_index, chip_capabilities);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_get_supported_iface_concurrency_matrix(wifi_handle handle,
                                                       wifi_iface_concurrency_matrix* matrix) {
    if (handle == nullptr || matrix == nullptr) {
        ALOGE("%s: invalid args: handle=%p, matrix=%p", __FUNCTION__, handle, matrix);
        return WIFI_ERROR_INVALID_ARGS;
    }

    int ctrl_iface_index = if_nametoindex("wlan0");
    if (!ctrl_iface_index) {
        ALOGE("%s: failed to find wlan0", __FUNCTION__);
        return WIFI_ERROR_UNKNOWN;
    }

    GetChipConcurrencyMatrixCommand command(handle, ctrl_iface_index, matrix);
    return (wifi_error)command.start();
}
