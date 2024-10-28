/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.mediatek.hardware.mtkpower;

import vendor.mediatek.hardware.mtkpower.IMtkPowerCallback;

@VintfStability
interface IMtkPowerService {
    oneway void mtkCusPowerHint(in int hint, in int data);
    oneway void mtkPowerHint(in int hint, in int data);
    oneway void notifyAppState(in String pack, in String act, in int pid, in int state,
        in int uid);
    int querySysInfo(in int cmd, in int param);
    int setMtkPowerCallback(in IMtkPowerCallback callback);
    int setMtkScnUpdateCallback(in int hint, in IMtkPowerCallback callback);
    int setSysInfo(in int type, in String data);
    void setSysInfoAsync(in int type, in String data);
}
