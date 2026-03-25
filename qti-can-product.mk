ifneq ($(filter $(PLATFORM_VERSION), 16 Baklava CinnamonBun 17),$(PLATFORM_VERSION))
PRODUCT_PACKAGES += qti-can.ko
endif
