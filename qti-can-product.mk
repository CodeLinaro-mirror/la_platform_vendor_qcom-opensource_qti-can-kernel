ifneq ($(filter $(PLATFORM_VERSION), 16 Baklava),$(PLATFORM_VERSION))
PRODUCT_PACKAGES += qti-can.ko
endif
