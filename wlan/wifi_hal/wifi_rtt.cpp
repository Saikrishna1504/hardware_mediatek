/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "WifiHAL"

#include <utils/Log.h>
#include <utils/String8.h>
#include <array>
#include <vector>

#include "common.h"
#include "cpp_bindings.h"
#include "cutils/properties.h"

#include "wifi_rtt.h"

using android::String8;

typedef struct strmap_entry {
    int id;
    String8 text;
} strmap_entry_t;

// TODO: check spec
static const strmap_entry_t err_info[] = {
        {RTT_STATUS_SUCCESS, String8("Success")},
        {RTT_STATUS_FAILURE, String8("Failure")},
        {RTT_STATUS_FAIL_NO_RSP, String8("No reponse")},
        {RTT_STATUS_FAIL_INVALID_TS, String8("Invalid Timestamp")},
        {RTT_STATUS_FAIL_PROTOCOL, String8("Protocol error")},
        {RTT_STATUS_FAIL_REJECTED, String8("Rejected")},
        {RTT_STATUS_FAIL_NOT_SCHEDULED_YET, String8("not scheduled")},
        {RTT_STATUS_FAIL_SCHEDULE, String8("schedule failed")},
        {RTT_STATUS_FAIL_TM_TIMEOUT, String8("timeout")},
        {RTT_STATUS_FAIL_AP_ON_DIFF_CHANNEL, String8("AP is on difference channel")},
        {RTT_STATUS_FAIL_NO_CAPABILITY, String8("no capability")},
        {RTT_STATUS_FAIL_BUSY_TRY_LATER, String8("busy and try later")},
        {RTT_STATUS_ABORTED, String8("aborted")}};

static const char* get_err_info(int status) {
    int i;
    const strmap_entry_t* p_entry;
    int num_entries = sizeof(err_info) / sizeof(err_info[0]);
    p_entry = err_info;
    for (i = 0; i < (int)num_entries; i++) {
        if (p_entry->id == status) return p_entry->text;
        p_entry++;
    }
    return "unknown error";
}

class RttRequestCommand : public WifiCommand {
  private:
    std::vector<wifi_rtt_config> mRttConfigs;
    wifi_rtt_event_handler mHandler;

    int totalCount;
    static const int MAX_RESULTS = 1024;
    wifi_rtt_result* rttResults[MAX_RESULTS];

  public:
    RttRequestCommand(wifi_interface_handle iface, wifi_request_id id, unsigned num_rtt_config,
                      wifi_rtt_config rtt_config[], wifi_rtt_event_handler handler)
        : WifiCommand("RttRequestCommand", iface, id), mHandler(handler), totalCount(0) {
        for (int i = 0; i < num_rtt_config; ++i) {
            mRttConfigs.push_back(rtt_config[i]);
        }
        for (int i = 0; i < MAX_RESULTS; ++i) {
            rttResults[i] = nullptr;
        }
    }

    virtual int create() {
        int result = mMsg.create(GOOGLE_OUI, RTT_SUBCMD_SET_CONFIG);
        if (result < 0) {
            ALOGE("Can't create message to send to driver - %d", result);
            return result;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_CNT, mRttConfigs.size());
        if (result < 0) {
            return result;
        }
        nlattr* rtt_config = mMsg.attr_start(RTT_ATTRIBUTE_TARGET_INFO);
        for (unsigned i = 0; i < mRttConfigs.size(); ++i) {
            auto config = mRttConfigs[i];
            nlattr* attr2 = mMsg.attr_start(i);
            if (attr2 == nullptr) {
                return WIFI_ERROR_OUT_OF_MEMORY;
            }

            result = mMsg.put_addr(RTT_ATTRIBUTE_TARGET_MAC, config.addr);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_TYPE, config.type);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_PEER, config.peer);
            if (result < 0) {
                return result;
            }

            result = mMsg.put(RTT_ATTRIBUTE_TARGET_CHAN, &(config.channel),
                              sizeof(wifi_channel_info));
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_NUM_BURST, config.num_burst);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_NUM_FTM_BURST, config.num_frames_per_burst);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTM,
                                  config.num_retries_per_rtt_frame);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTMR, config.num_retries_per_ftmr);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_PERIOD, config.burst_period);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u32(RTT_ATTRIBUTE_TARGET_BURST_DURATION, config.burst_duration);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_LCI, config.LCI_request);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_LCR, config.LCR_request);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_BW, config.bw);
            if (result < 0) {
                return result;
            }

            result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_PREAMBLE, config.preamble);
            if (result < 0) {
                return result;
            }
            mMsg.attr_end(attr2);
        }
        mMsg.attr_end(rtt_config);
        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }

    int start() {
        ALOGI("Start RTT request");

        registerVendorHandler(GOOGLE_OUI, RTT_EVENT_COMPLETE);
        int result = requestResponse();
        if (result != WIFI_SUCCESS) {
            ALOGE("failed to configure RTT setup; result = %d", result);
            return result;
        }

        ALOGI("Successfully started RTT operation");
        return WIFI_SUCCESS;
    }

    virtual int cancel() {
        ALOGI("Cancel RTT request");
        WifiRequest request(familyId(), ifaceId());
        int result = request.create(GOOGLE_OUI, RTT_SUBCMD_CANCEL_CONFIG);
        if (result < 0) {
            ALOGE("Can't create message to send to driver - %d", result);
            return result;
        }

        nlattr* data = request.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        result = request.put_u8(RTT_ATTRIBUTE_TARGET_CNT, 0);
        if (result < 0) {
            return result;
        }

        request.attr_end(data);

        result = requestResponse(request);
        if (result != WIFI_SUCCESS) {
            ALOGE("%s: failed to stop ranging, result = %d", __FUNCTION__, result);
        }

        unregisterVendorHandler(GOOGLE_OUI, RTT_EVENT_COMPLETE);
        return WIFI_SUCCESS;
    }

    virtual int handleEvent(WifiEvent& event) {
        ALOGI("Handle RTT event");
        nlattr* vendor_data = event.get_attribute(NL80211_ATTR_VENDOR_DATA);
        int len = event.get_vendor_data_len();
        if (vendor_data == nullptr || len == 0) {
            ALOGE("Empty RTT vendor data");
            return NL_STOP;
        }

        int result_count = 0;
        totalCount = 0;
        for (nl_iterator it(vendor_data); it.has_next(); it.next()) {
            if (it.get_type() == RTT_ATTRIBUTE_RESULT_CNT) {
                result_count = it.get_u32();
            } else if (it.get_type() == RTT_ATTRIBUTE_RESULT) {
                int result_len = it.get_len();
                void* result_data = it.get_data();
                if (result_data == nullptr || result_len <= 0) {
                    ALOGE("Invalid RTT result");
                    break;
                }
                rttResults[totalCount] = (wifi_rtt_result*)malloc(result_len);
                wifi_rtt_result* rtt_result = rttResults[totalCount];
                if (rtt_result == nullptr) {
                    ALOGE("Failed to allocate wifi_rtt_result");
                    break;
                }
                memcpy(rtt_result, result_data, it.get_len());

                // Search for trailing LCI and/or LCR and patch pointers
                result_len -= sizeof(wifi_rtt_result);
                if (result_len > 0) {
                    dot11_rm_ie_t* e1;
                    dot11_rm_ie_t* e2;

                    e1 = (dot11_rm_ie_t*)(rtt_result + 1);
                    if (e1->id == DOT11_MNG_MEASURE_REQUEST_ID) {
                        if (e1->type == DOT11_MEASURE_TYPE_LCI) {
                            rtt_result->LCI = (wifi_information_element*)e1;
                            result_len -= (e1->len + DOT11_HDR_LEN);
                            /* get a next rm ie */
                            if (result_len > 0) {
                                e2 = (dot11_rm_ie_t*)((char*)e1 + (e1->len + DOT11_HDR_LEN));
                                if ((e2->id == DOT11_MNG_MEASURE_REQUEST_ID) &&
                                    (e2->type == DOT11_MEASURE_TYPE_CIVICLOC)) {
                                    rtt_result->LCR = (wifi_information_element*)e2;
                                }
                            }
                        } else if (e1->type == DOT11_MEASURE_TYPE_CIVICLOC) {
                            rtt_result->LCR = (wifi_information_element*)e1;
                            result_len -= (e1->len + DOT11_HDR_LEN);
                            /* get a next rm ie */
                            if (result_len > 0) {
                                e2 = (dot11_rm_ie_t*)((char*)e1 + (e1->len + DOT11_HDR_LEN));
                                if ((e2->id == DOT11_MNG_MEASURE_REQUEST_ID) &&
                                    (e2->type == DOT11_MEASURE_TYPE_LCI)) {
                                    rtt_result->LCI = (wifi_information_element*)e2;
                                }
                            }
                        }
                    }
                }
                totalCount++;
            }
        }
        if (totalCount != result_count) {
            ALOGW("result count and results do not match");
        }

        unregisterVendorHandler(GOOGLE_OUI, RTT_EVENT_COMPLETE);
        (*mHandler.on_rtt_results)(id(), totalCount, rttResults);
        for (int i = 0; i < totalCount; i++) {
            free(rttResults[i]);
            rttResults[i] = nullptr;
        }
        WifiCommand* cmd = wifi_unregister_cmd(wifiHandle(), id());
        if (cmd) cmd->releaseRef();
        return NL_SKIP;
    }

    virtual ~RttRequestCommand() { mRttConfigs.clear(); }
};

class RttCancelCommand : public WifiCommand {
  private:
    unsigned int mNumDevices;
    mac_addr* mAddrs;

  public:
    RttCancelCommand(wifi_interface_handle iface, wifi_request_id id, unsigned int num_devices,
                     mac_addr addr[])
        : WifiCommand("RttCancelCommand", iface, id), mNumDevices(num_devices), mAddrs(addr) {}

    virtual int create() {
        int result = mMsg.create(GOOGLE_OUI, RTT_SUBCMD_CANCEL_CONFIG);
        if (result < 0) {
            ALOGE("Can't create message to send to driver - %d", result);
            return result;
        }

        nlattr* data = mMsg.attr_start(NL80211_ATTR_VENDOR_DATA);
        if (!data) {
            ALOGE("Failed attr_start for VENDOR_DATA");
            return WIFI_ERROR_UNKNOWN;
        }

        result = mMsg.put_u8(RTT_ATTRIBUTE_TARGET_CNT, mNumDevices);
        if (result < 0) {
            return result;
        }

        for (int i = 0; i < mNumDevices; ++i) {
            result = mMsg.put_addr(RTT_ATTRIBUTE_TARGET_MAC, mAddrs[i]);
            if (result < 0) {
                return result;
            }
        }

        mMsg.attr_end(data);
        return WIFI_SUCCESS;
    }

    virtual ~RttCancelCommand() {}
};

class RttGetCapabilitiesCommand : public WifiCommand {
  private:
    wifi_rtt_capabilities* mCapabilities;

  public:
    RttGetCapabilitiesCommand(wifi_interface_handle iface, wifi_rtt_capabilities* capabitlites)
        : WifiCommand("GetRttCapabilitiesCommand", iface, 0), mCapabilities(capabitlites) {
        memset(mCapabilities, 0, sizeof(*mCapabilities));
    }

    virtual int create() {
        int ret = mMsg.create(GOOGLE_OUI, RTT_SUBCMD_GETCAPABILITY);
        if (ret < 0) {
            ALOGE("Can't create message to send to driver - %d", ret);
        }
        return ret;
    }

  protected:
    virtual int handleResponse(WifiEvent& reply) {
        ALOGI("RttGetCapabilitiesCommand::handleResponse");

        if (reply.get_cmd() != NL80211_CMD_VENDOR) {
            ALOGV("Ignoring reply with cmd = %d", reply.get_cmd());
            return NL_SKIP;
        }

        int id = reply.get_vendor_id();
        int subcmd = reply.get_vendor_subcmd();
        void* data = reply.get_vendor_data();
        int len = reply.get_vendor_data_len();

        if (data == nullptr || len <= 0) {
            ALOGE("Empty response, ignore.");
            return NL_SKIP;
        }

        memcpy(mCapabilities, data, min(len, (int)sizeof(*mCapabilities)));
        return NL_OK;
    }
};

wifi_error wifi_rtt_range_request(wifi_request_id id, wifi_interface_handle iface,
                                  unsigned int num_rtt_config, wifi_rtt_config rtt_config[],
                                  wifi_rtt_event_handler handler) {
    if (iface == nullptr || num_rtt_config == 0) {
        ALOGE("%s: invalid args: iface=%p, num_rtt_config=%u", __FUNCTION__, (void*)iface,
              num_rtt_config);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("%s: iface=%s", __FUNCTION__, getIfaceInfo(iface)->name);

    wifi_handle handle = getWifiHandle(iface);
    if (handle == nullptr) {
        ALOGE("%s: wifi handle is null", __FUNCTION__);
        return WIFI_ERROR_INVALID_ARGS;
    }
    RttRequestCommand* command =
            new RttRequestCommand(iface, id, num_rtt_config, rtt_config, handler);
    NULL_CHECK_RETURN(command, "memory allocation failure", WIFI_ERROR_OUT_OF_MEMORY);
    wifi_error result = wifi_register_cmd(handle, id, command);
    if (result != WIFI_SUCCESS) {
        ALOGE("%s: failed to register command", __FUNCTION__);
        command->releaseRef();
        return result;
    }
    result = (wifi_error)command->start();
    if (result != WIFI_SUCCESS) {
        ALOGE("%s: failed to start command", __FUNCTION__);
        wifi_unregister_cmd(handle, id);
        command->releaseRef();
        return result;
    }
    return result;
}

wifi_error wifi_rtt_range_cancel(wifi_request_id id, wifi_interface_handle iface,
                                 unsigned int num_devices, mac_addr addr[]) {
    if (iface == nullptr || num_devices == 0) {
        ALOGE("%s: invalid args: iface=%p, num_devices=%u", __FUNCTION__, (void*)iface,
              num_devices);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("%s: iface=%s", __FUNCTION__, getIfaceInfo(iface)->name);

    RttCancelCommand command(iface, id, num_devices, addr);
    return (wifi_error)command.requestResponse();
}

wifi_error wifi_get_rtt_capabilities(wifi_interface_handle iface,
                                     wifi_rtt_capabilities* capabilities) {
    if (iface == nullptr || capabilities == nullptr) {
        ALOGE("%s: invalid args: iface=%p, capabilities=%p", __FUNCTION__, (void*)iface,
              capabilities);
        return WIFI_ERROR_INVALID_ARGS;
    }

    ALOGI("%s: iface=%s", __FUNCTION__, getIfaceInfo(iface)->name);

    RttGetCapabilitiesCommand command(iface, capabilities);
    return (wifi_error)command.requestResponse();
}
