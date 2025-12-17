LOCAL_PATH := $(call my-dir)
DLKM_DIR := $(TOP)/device/qcom/common/dlkm

#KBUILD_OPTIONS
KBUILD_OPTIONS += MODNAME=qti-uart-can
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTIONS += CONFIG_QTI_UART_CAN=y
$(info value of TARGET_USES_KERNEL_PLATFORM IS '$(TARGET_USES_KERNEL_PLATFORM)')

#Clear Environment Variables
include $(CLEAR_VARS)
#Defining the local options
LOCAL_SRC_FILES             :=  \
                                $(LOCAL_PATH)/driver/qti-uart-can.c \
                                $(LOCAL_PATH)/Android.mk   \
                                $(LOCAL_PATH)/Kbuild	
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
LOCAL_MODULE              := qti-uart-can.ko
LOCAL_MODULE_TAGS         := optional

#include $(DLKM_DIR)/Build_external_kernelmodule.mk
include $(DLKM_DIR)/AndroidKernelModule.mk
