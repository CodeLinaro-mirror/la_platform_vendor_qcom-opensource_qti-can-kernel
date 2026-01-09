# Build QTI-CAN kernel driver
ifeq ($(TARGET_BOARD_AUTO),true)
ifneq ($(filter $(PLATFORM_VERSION), 16 Baklava CinnamonBun 17),$(PLATFORM_VERSION))
	BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/qti-can.ko
endif
endif
