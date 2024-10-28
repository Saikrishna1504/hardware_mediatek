/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.mediatek.hardware.mtkpower;

@VintfStability
interface IMtkPowerCallback {
    oneway void mtkPowerHint(in int hint, in int duration);
    oneway void notifyAppState(in String pack, in String act, in int pid, in int state,
        in int uid);
    oneway void notifyScnUpdate(in int hint, in int data);
}
