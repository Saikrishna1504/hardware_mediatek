# Thermal HAL
PRODUCT_PACKAGES += \
    android.hardware.thermal@2.0-service.mediatek

# Thermal utils
PRODUCT_PACKAGES += \
    thermal_symlinks_mediatek

ifneq (,$(filter userdebug eng, $(TARGET_BUILD_VARIANT)))
PRODUCT_PACKAGES += \
    thermal_logd_mediatek
endif

BOARD_SEPOLICY_DIRS += hardware/google/pixel-sepolicy/thermal
