ifeq ($(TARGET_SUPPORT), sa510m)
ccflags-y += -DTELEMATICS_TARGET
endif

ifeq ($(TARGET_SUPPORT), sa510m)
ccflags-y += -DTELEMATICS_TARGET
endif

qti-can-y += driver/qti-can.o
obj-m += qti-can.o
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/qti-can.ko
