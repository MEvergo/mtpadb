# Include from the target product makefile after adding the source checkout at
# external/mtpadb and applying one pinned ADB patch series.
PRODUCT_PACKAGES += \
    mtpadbd \
    mtpadbctl \
    mtprpcd
PRODUCT_COPY_FILES += \
    external/mtpadb/android/init/mtpadbd.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/mtpadbd.rc

PRODUCT_PRIVATE_SEPOLICY_DIRS += external/mtpadb/android/sepolicy
