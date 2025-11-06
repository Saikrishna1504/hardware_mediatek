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
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <linux/pkt_sched.h>
#include <netlink/handlers.h>
#include <netlink/netlink.h>
#include <netlink/object-api.h>
#include <netlink/socket.h>

#include <errno.h>

#include "common.h"
#include "cpp_bindings.h"

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "WifiHAL"
#endif

interface_info* getIfaceInfo(wifi_interface_handle handle) {
    return (interface_info*)handle;
}

wifi_handle getWifiHandle(wifi_interface_handle handle) {
    return getIfaceInfo(handle)->handle;
}

hal_info* getHalInfo(wifi_handle handle) {
    return (hal_info*)handle;
}

hal_info* getHalInfo(wifi_interface_handle handle) {
    return getHalInfo(getWifiHandle(handle));
}

wifi_handle getWifiHandle(hal_info* info) {
    return (wifi_handle)info;
}

wifi_interface_handle getIfaceHandle(interface_info* info) {
    return (wifi_interface_handle)info;
}

wifi_error wifi_register_handler(wifi_handle handle, int cmd, nl_recvmsg_msg_cb_t func, void* arg) {
    hal_info* info = (hal_info*)handle;

    /* TODO: check for multiple handlers? */
    pthread_mutex_lock(&info->cb_lock);

    wifi_error result = WIFI_ERROR_OUT_OF_MEMORY;

    if (info->num_event_cb < info->alloc_event_cb) {
        info->event_cb[info->num_event_cb].nl_cmd = cmd;
        info->event_cb[info->num_event_cb].vendor_id = 0;
        info->event_cb[info->num_event_cb].vendor_subcmd = 0;
        info->event_cb[info->num_event_cb].cb_func = func;
        info->event_cb[info->num_event_cb].cb_arg = arg;
        info->num_event_cb++;
        ALOGI("Successfully added event handler %p:%p for command %d at %d", func, arg, cmd,
              info->num_event_cb);
        result = WIFI_SUCCESS;
    }

    pthread_mutex_unlock(&info->cb_lock);
    return result;
}

wifi_error wifi_register_vendor_handler(wifi_handle handle, uint32_t id, int subcmd,
                                        nl_recvmsg_msg_cb_t func, void* arg, char* ifname) {
    ALOGI("%s: id=%d, subcmd=%d, ifname=%p [%s]", __FUNCTION__, id, subcmd, ifname,
          ifname == nullptr ? "<nullptr>" : ifname);
    hal_info* info = (hal_info*)handle;

    /* TODO: check for multiple handlers? */
    pthread_mutex_lock(&info->cb_lock);

    wifi_error result = WIFI_ERROR_OUT_OF_MEMORY;

    // update existing handler
    for (int i = 0; i < info->num_event_cb; i++) {
        cb_info* cbi = &info->event_cb[i];
        if (cbi->vendor_id == id && cbi->vendor_subcmd == subcmd) {
            // check ifname if specified
            if (ifname != nullptr && 0 != strncmp(cbi->ifname, ifname, sizeof(cbi->ifname) - 1)) {
                ALOGI("[%d] ifname mismatch, ignore: cbi->ifname [%s], ifname [%s]", i, cbi->ifname,
                      ifname);
                continue;
            }
            cbi->cb_func = func;
            cbi->cb_arg = arg;
            ALOGI("Update vendor handler %p:%p for vendor %d, subcmd %d and iface [%s] at %d", func,
                  arg, id, subcmd, cbi->ifname, i);
            pthread_mutex_unlock(&info->cb_lock);
            return WIFI_SUCCESS;
        }
    }

    // add new handler
    if (info->num_event_cb < info->alloc_event_cb) {
        cb_info* cbi = &info->event_cb[info->num_event_cb];
        cbi->nl_cmd = NL80211_CMD_VENDOR;
        cbi->vendor_id = id;
        cbi->vendor_subcmd = subcmd;
        cbi->cb_func = func;
        cbi->cb_arg = arg;
        if (ifname == nullptr) {
            cbi->ifname[0] = '\0';
        } else {
            strncpy(cbi->ifname, ifname, sizeof(cbi->ifname));
            cbi->ifname[sizeof(cbi->ifname) - 1] = '\0';
        }
        info->num_event_cb++;
        ALOGI("Register vendor handler %p:%p for vendor %d, subcmd %d and iface [%s] at %d", arg,
              func, id, subcmd, ifname == nullptr ? "<nullptr>" : ifname, info->num_event_cb);
        result = WIFI_SUCCESS;
    }

    pthread_mutex_unlock(&info->cb_lock);
    return result;
}

void wifi_unregister_handler(wifi_handle handle, int cmd) {
    ALOGI("%s: handle=%p, cmd=%d", __FUNCTION__, handle, cmd);
    hal_info* info = (hal_info*)handle;

    if (cmd == NL80211_CMD_VENDOR) {
        ALOGE("Must use wifi_unregister_vendor_handler to remove vendor handlers");
        return;
    }

    pthread_mutex_lock(&info->cb_lock);

    for (int i = 0; i < info->num_event_cb; i++) {
        if (info->event_cb[i].nl_cmd == cmd) {
            ALOGI("Deregister wifi handler %p:%p for cmd = %d from %d", info->event_cb[i].cb_func,
                  info->event_cb[i].cb_arg, cmd, i);

            memmove(&info->event_cb[i], &info->event_cb[i + 1],
                    (info->num_event_cb - i - 1) * sizeof(cb_info));
            info->num_event_cb--;
            break;
        }
    }

    pthread_mutex_unlock(&info->cb_lock);
}

/**
 * @param ifname For handlers which use same id for all ifaces (e.g. RSSI monitoring). Can be
 *                  safely ignored if `nullptr` is specified.
 */
void wifi_unregister_vendor_handler(wifi_handle handle, uint32_t id, int subcmd, char* ifname) {
    ALOGI("%s: handle=%p, id=%d, subcmd=%d, ifname=%p [%s]", __FUNCTION__, handle, id, subcmd,
          ifname, ifname == nullptr ? "<nullptr>" : ifname);
    hal_info* info = (hal_info*)handle;

    pthread_mutex_lock(&info->cb_lock);

    for (int i = 0; i < info->num_event_cb; i++) {
        cb_info* cbi = &info->event_cb[i];
        ALOGI("%s: nl_cmd=%d, id=%d, subcmd=%d", __FUNCTION__, cbi->nl_cmd, cbi->vendor_id,
              cbi->vendor_subcmd);
        if (cbi->nl_cmd == NL80211_CMD_VENDOR && cbi->vendor_id == id &&
            cbi->vendor_subcmd == subcmd) {
            // check ifname if specified
            if (ifname != nullptr && 0 != strncmp(cbi->ifname, ifname, sizeof(cbi->ifname) - 1)) {
                ALOGI("[%d] ifname mismatch, ignore: cbi->ifname [%s], ifname [%s]", i, cbi->ifname,
                      ifname);
                continue;
            }
            ALOGI("Deregister vendor handler %p:%p for vendor %d, subcmd %d from %d", cbi->cb_arg,
                  cbi->cb_func, id, subcmd, i);
            memmove(&info->event_cb[i], &info->event_cb[i + 1],
                    (info->num_event_cb - i - 1) * sizeof(cb_info));
            info->num_event_cb--;
            break;
        }
    }

    pthread_mutex_unlock(&info->cb_lock);
}

wifi_error wifi_register_cmd(wifi_handle handle, int id, WifiCommand* cmd) {
    hal_info* info = (hal_info*)handle;

    ALOGI("registering command %d", id);

    wifi_error result = WIFI_ERROR_OUT_OF_MEMORY;

    if (info->num_cmd < info->alloc_cmd) {
        info->cmd[info->num_cmd].id = id;
        info->cmd[info->num_cmd].cmd = cmd;
        ALOGI("Successfully added command %d: %p at %d", id, cmd, info->num_cmd);
        info->num_cmd++;
        result = WIFI_SUCCESS;
    } else {
        ALOGE("Failed to add command %d: %p at %d, reached max limit %d", id, cmd, info->num_cmd,
              info->alloc_cmd);
    }

    return result;
}

WifiCommand* wifi_unregister_cmd(wifi_handle handle, int id) {
    hal_info* info = (hal_info*)handle;

    ALOGI("un-registering command %d", id);

    WifiCommand* cmd = NULL;

    for (int i = 0; i < info->num_cmd; i++) {
        if (info->cmd[i].id == id) {
            cmd = info->cmd[i].cmd;
            memmove(&info->cmd[i], &info->cmd[i + 1], (info->num_cmd - i - 1) * sizeof(cmd_info));
            info->num_cmd--;
            ALOGI("Successfully removed command %d: %p from %d", id, cmd, i);
            break;
        }
    }

    if (!cmd) {
        ALOGI("Failed to remove command %d: %p", id, cmd);
    }

    return cmd;
}

WifiCommand* wifi_get_cmd(wifi_handle handle, int id) {
    hal_info* info = (hal_info*)handle;

    WifiCommand* cmd = NULL;

    for (int i = 0; i < info->num_cmd; i++) {
        if (info->cmd[i].id == id) {
            cmd = info->cmd[i].cmd;
            break;
        }
    }

    return cmd;
}

void wifi_unregister_cmd(wifi_handle handle, WifiCommand* cmd) {
    hal_info* info = (hal_info*)handle;

    for (int i = 0; i < info->num_cmd; i++) {
        if (info->cmd[i].cmd == cmd) {
            int id = info->cmd[i].id;
            memmove(&info->cmd[i], &info->cmd[i + 1], (info->num_cmd - i - 1) * sizeof(cmd_info));
            info->num_cmd--;
            ALOGI("Successfully removed command %d: %p from %d", id, cmd, i);
            break;
        }
    }
}

wifi_error wifi_cancel_cmd(wifi_request_id id, wifi_interface_handle iface) {
    wifi_handle handle = getWifiHandle(iface);

    WifiCommand* cmd = wifi_unregister_cmd(handle, id);
    ALOGI("Cancel WifiCommand = %p", cmd);
    if (cmd) {
        cmd->cancel();
        cmd->releaseRef();
        return WIFI_SUCCESS;
    }

    return WIFI_ERROR_INVALID_ARGS;
}

void hexdump(void* buf, u16 len) {
    int i = 0;
    char* bytes = (char*)buf;

    if (len) {
        ALOGE("HEXDUMP: buf=%p, len=%d, len=%d * 8 + %d,", buf, len, len / 8, len % 8);
        ALOGE("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++");
        uint64_t addr = (uint64_t)buf;
        for (i = 0; ((i + 7) < len); i += 8) {
            ALOGE("%016" PRIx64 " | %02x %02x %02x %02x   %02x %02x %02x %02x", addr + i,
                  (int)bytes[i], (int)bytes[i + 1], (int)bytes[i + 2], (int)bytes[i + 3],
                  (int)bytes[i + 4], (int)bytes[i + 5], (int)bytes[i + 6], (int)bytes[i + 7]);
        }
        int remain = len - i;
        if (remain > 0) {
            char remain_buf[16 + 2 + 3 * 4 + 2 + 3 * 3 + 1];
            memset(remain_buf, 0, sizeof(remain_buf));
            int result =
                    snprintf(remain_buf, sizeof(remain_buf),
                             "%016" PRIx64 " | %02x %02x %02x %02x   %02x %02x %02x", addr + i,
                             (int)bytes[i], remain > 1 ? (int)bytes[i + 1] : 0,
                             remain > 2 ? (int)bytes[i + 2] : 0, remain > 3 ? (int)bytes[i + 3] : 0,
                             remain > 4 ? (int)bytes[i + 4] : 0, remain > 5 ? (int)bytes[i + 5] : 0,
                             remain > 6 ? (int)bytes[i + 6] : 0);
            remain_buf[16 + 2 + 3 * remain] = 0;
            if (result >= 0) {
                ALOGE("%s", remain_buf);
            }
        }
        ALOGE("------------------------------------------------------------");
    } else {
        return;
    }
}
