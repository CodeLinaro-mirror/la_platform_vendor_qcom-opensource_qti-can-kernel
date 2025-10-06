# Makefile for use with Android's kernel/build system
ifeq ($(TARGET_SUPPORT), sa510m)
M=$(PWD)
endif

ifeq ($(TARGET_SUPPORT), sa535m)
M=$(PWD)
endif

KBUILD_OPTIONS += QTI_CAN_KERNEL_ROOT=$(shell pwd)
KBUILD_OPTIONS += KERNEL_ROOT=$(ROOT_DIR)/$(KERNEL_DIR)
KBUILD_OPTIONS += MODNAME=qti-can
QTI_CAN_BLD_DIR := $(TOP)/vendor/qcom/opensource/qti-can-kernel/

all: modules

modules dtbs:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
