LOCAL_PATH := $(call my-dir)
DLKM_DIR := $(TOP)/device/qcom/common/dlkm

#KBUILD_OPTIONS
KBUILD_OPTIONS += KERNEL_ROOT=$(shell pwd)/kernel/msm-$(TARGET_KERNEL_VERSION)/
KBUILD_OPTIONS += MODNAME=qti-can
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTIONS += CONFIG_QTI_CAN=y
$(info value of TARGET_USES_KERNEL_PLATFORM IS '$(TARGET_USES_KERNEL_PLATFORM)')

#Clear Environment Variables
include $(CLEAR_VARS)
#Defining the local options
LOCAL_SRC_FILES             :=  \
                                $(shell find $(LOCAL_PATH)/driver/ -L -type f) \
                                $(LOCAL_PATH)/Android.mk   \
                                $(LOCAL_PATH)/Kbuild	\
                                $(LOCAL_PATH)/qti-can-board.mk   \
                                $(LOCAL_PATH)/qti-can-product.mk
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
LOCAL_MODULE              := qti-can.ko
LOCAL_MODULE_TAGS         := optional

include $(DLKM_DIR)/Build_external_kernelmodule.mk
