/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.mediatek.hardware.mtkpower;

@VintfStability
interface IMtkPerf {
    int perfCusLockHint(in int hint, in int duration);
    int perfLockAcquire(in int pl_handle, in int duration, in int[] boostsList, in int reserved);
    oneway void perfLockRelease(in int pl_handle, in int reserved);
    int perfLockReleaseSync(in int pl_handle, in int reserved);
}
