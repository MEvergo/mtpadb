$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, external/mtpadb/android/deploy/mtpadb_product.mk)

PRODUCT_NAME := mtpadb_arm64
PRODUCT_DEVICE := arm64
PRODUCT_BRAND := Android
PRODUCT_MODEL := MTPADB arm64 build target
