/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <linux/errqueue.h>
#include <linux/filter.h>
#include <linux/rtnetlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netpacket/packet.h>
#include <stdint.h>
#include <sys/socket.h>

#include <linux/pkt_sched.h>
#include <netlink/netlink.h>
#include <netlink/object-api.h>
#include <netlink/socket.h>

#include "nl80211_copy.h"
#include "sync.h"

#define LOG_TAG "WifiHAL"

#include <utils/Log.h>

#include "common.h"
#include "cpp_bindings.h"

typedef enum {
    WIFI_OFFLOAD_START_MKEEP_ALIVE = ANDROID_NL80211_SUBCMD_WIFI_OFFLOAD_RANGE_START,
    WIFI_OFFLOAD_STOP_MKEEP_ALIVE,
} WIFI_OFFLOAD_SUB_COMMAND;

typedef enum {
    MKEEP_ALIVE_ATTRIBUTE_ID = 1,
    MKEEP_ALIVE_ATTRIBUTE_IP_PKT_LEN,
    MKEEP_ALIVE_ATTRIBUTE_IP_PKT,
    MKEEP_ALIVE_ATTRIBUTE_SRC_MAC_ADDR,
    MKEEP_ALIVE_ATTRIBUTE_DST_MAC_ADDR,
    MKEEP_ALIVE_ATTRIBUTE_PERIOD_MSEC
} WIFI_MKEEP_ALIVE_ATTRIBUTE;

typedef enum {
    START_MKEEP_ALIVE,
    STOP_MKEEP_ALIVE,
} GetCmdType;

///////////////////////////////////////////////////////////////////////////////
class MKeepAliveCommand : public WifiCommand {
    u8 mIndex;
    u8* mIpPkt;
    u16 mIpPktLen;
    u8* mSrcMacAddr;
    u8* mDstMacAddr;
    u32 mPeriodMsec;
    GetCmdType mType;

  public:
    // constructor for start sending
    MKeepAliveCommand(wifi_interface_handle iface, u8 id, u8* ip_packet, u16 ip_packet_len,
                      u8* src_mac_addr, u8* dst_mac_addr, u32 period_msec, GetCmdType cmdType)
        : WifiCommand("MKeepAliveCommand", iface, 0),
          mIndex(id),
          mIpPkt(ip_packet),
          mIpPktLen(ip_packet_len),
          mSrcMacAddr(src_mac_addr),
          mDstMacAddr(dst_mac_addr),
          mPeriodMsec(period_msec),
          mType(cmdType) {}

    // constructor for stop sending
    MKeepAliveCommand(wifi_interface_handle iface, u8 id, GetCmdType cmdType)
        : WifiCommand("MKeepAliveCommand", iface, 0), mIndex(id), mType(cmdType) {}

    int createRequest(WifiRequest& request) {
        int result;

        switch (mType) {
            case START_MKEEP_ALIVE: {
                result = request.create(GOOGLE_OUI, WIFI_OFFLOAD_START_MKEEP_ALIVE);
                if (result != WIFI_SUCCESS) {
                    ALOGE("Failed to create start keep alive request; result = %d", result);
                    return result;
                }

                nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
                if (!data) {
                    ALOGE("Failed attr_start for VENDOR_DATA");
                    return WIFI_ERROR_UNKNOWN;
                }

                result = request.put_u8(MKEEP_ALIVE_ATTRIBUTE_ID, mIndex);
                if (result < 0) {
                    ALOGE("Failed to put id request; result = %d", result);
                    return result;
                }

                result = request.put_u16(MKEEP_ALIVE_ATTRIBUTE_IP_PKT_LEN, mIpPktLen);
                if (result < 0) {
                    ALOGE("Failed to put ip pkt len request; result = %d", result);
                    return result;
                }

                result = request.put(MKEEP_ALIVE_ATTRIBUTE_IP_PKT, (u8*)mIpPkt, mIpPktLen);
                if (result < 0) {
                    ALOGE("Failed to put ip pkt request; result = %d", result);
                    return result;
                }

                result = request.put_addr(MKEEP_ALIVE_ATTRIBUTE_SRC_MAC_ADDR, mSrcMacAddr);
                if (result < 0) {
                    ALOGE("Failed to put src mac address request; result = %d", result);
                    return result;
                }

                result = request.put_addr(MKEEP_ALIVE_ATTRIBUTE_DST_MAC_ADDR, mDstMacAddr);
                if (result < 0) {
                    ALOGE("Failed to put dst mac address request; result = %d", result);
                    return result;
                }

                result = request.put_u32(MKEEP_ALIVE_ATTRIBUTE_PERIOD_MSEC, mPeriodMsec);
                if (result < 0) {
                    ALOGE("Failed to put period request; result = %d", result);
                    return result;
                }

                request.attr_end(data);
                break;
            }

            case STOP_MKEEP_ALIVE: {
                result = request.create(GOOGLE_OUI, WIFI_OFFLOAD_STOP_MKEEP_ALIVE);
                if (result != WIFI_SUCCESS) {
                    ALOGE("Failed to create stop keep alive request; result = %d", result);
                    return result;
                }

                nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
                if (!data) {
                    ALOGE("Failed attr_start for VENDOR_DATA");
                    return WIFI_ERROR_UNKNOWN;
                }

                result = request.put_u8(MKEEP_ALIVE_ATTRIBUTE_ID, mIndex);
                if (result < 0) {
                    ALOGE("Failed to put id request; result = %d", result);
                    return result;
                }

                request.attr_end(data);
                break;
            }

            default:
                ALOGE("Unknown wifi keep alive command");
                result = WIFI_ERROR_UNKNOWN;
        }
        return result;
    }

    int start() {
        ALOGV("Start mkeep_alive command");
        WifiRequest request(familyId(), ifaceId());
        int result = createRequest(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to create keep alive request; result = %d", result);
            return result;
        }

        result = requestResponse(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("Failed to register keep alive response; result = %d", result);
        }
        return result;
    }

    virtual int handleResponse(WifiEvent& reply) {
        ALOGV("In MKeepAliveCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        switch (mType) {
            case START_MKEEP_ALIVE:
            case STOP_MKEEP_ALIVE:
                break;

            default:
                ALOGW("Unknown mkeep_alive command");
        }
        return NL_OK;
    }

    virtual int handleEvent(WifiEvent& event) {
        /* NO events! */
        return NL_SKIP;
    }
};

/* API to send specified mkeep_alive packet periodically. */
wifi_error wifi_start_sending_offloaded_packet(wifi_request_id id, wifi_interface_handle iface,
                                               u16 /*ether_type*/, u8* ip_packet, u16 ip_packet_len,
                                               u8* src_mac_addr, u8* dst_mac_addr,
                                               u32 period_msec) {
    if ((id <= 0 || id > N_AVAIL_ID) || iface == nullptr || ip_packet == nullptr ||
        ip_packet_len > MKEEP_ALIVE_IP_PKT_MAX || src_mac_addr == nullptr ||
        dst_mac_addr == nullptr || period_msec <= 0) {
        ALOGE("%s: invalid args: id=%d, iface=%p, ip_packet=%p, ip_packet_len=%d, src_mac_addr=%p, "
              "dst_mac_addr=%p,"
              "period_msec=%" PRIu32,
              __FUNCTION__, id, (void*)iface, ip_packet, ip_packet_len, src_mac_addr, dst_mac_addr,
              period_msec);
        return WIFI_ERROR_INVALID_ARGS;
    }

    MKeepAliveCommand* cmd =
            new MKeepAliveCommand(iface, id, ip_packet, ip_packet_len, src_mac_addr, dst_mac_addr,
                                  period_msec, START_MKEEP_ALIVE);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = (wifi_error)cmd->start();
    cmd->releaseRef();
    return result;
}

/* API to stop sending mkeep_alive packet. */
wifi_error wifi_stop_sending_offloaded_packet(wifi_request_id id, wifi_interface_handle iface) {
    if ((id <= 0 || id > N_AVAIL_ID) || iface == nullptr) {
        ALOGE("%s: invalid args: id=%d, iface=%p", __FUNCTION__, id, (void*)iface);
        return WIFI_ERROR_INVALID_ARGS;
    }

    MKeepAliveCommand* cmd = new MKeepAliveCommand(iface, id, STOP_MKEEP_ALIVE);
    NULL_CHECK_RETURN(cmd, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = (wifi_error)cmd->start();
    cmd->releaseRef();
    return result;
}
