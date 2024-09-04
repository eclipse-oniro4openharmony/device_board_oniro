# Copyright (c) 2022 Diemit <598757652@qq.com>
# Copyright (c) 2024 Institute of Software, Chinese Academy of Sciences. 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

KERNEL_CONFIG_PATH = $(PROJECT_ROOT)/device/board/${product_company}/common/kernel/configs
PREBUILTS_GCC_DIR := $(PROJECT_ROOT)/prebuilts/gcc
CLANG_HOST_TOOLCHAIN := $(PROJECT_ROOT)/prebuilts/clang/ohos/linux-x86_64/llvm/bin
CLANG_CC := $(CLANG_HOST_TOOLCHAIN)/clang

KERNEL_PREBUILT_MAKE := make
DTB := dtb
DTBS_ARCH := arm
DTBS := bcm2710-rpi-3-b.dtb bcm2711-rpi-4-b.dtb overlays/vc4-fkms-v3d.dtbo overlays/rpi-ft5406.dtbo overlays/rpi-backlight.dtbo overlays/dwc2.dtbo

KERNEL_TARGET_TOOLCHAIN_32 := $(PREBUILTS_GCC_DIR)/linux-x86/arm/gcc-linaro-7.5.0-arm-linux-gnueabi/bin/arm-linux-gnueabi-

KERNEL_CROSS_COMPILE_DTB :=
KERNEL_CROSS_COMPILE_DTB += CC="$(CLANG_CC)"
KERNEL_CROSS_COMPILE_DTB += CROSS_COMPILE="$(KERNEL_TARGET_TOOLCHAIN_32)"
KERNEL_MAKE := \
    $(KERNEL_PREBUILT_MAKE)

$(DTB):
	echo "build dtb..."
	cp -rf $(KERNEL_CONFIG_PATH)/bcm2711_oh_defconfig32 $(KERNEL_SRC_TMP_PATH)/arch/$(DTBS_ARCH)/configs/bcm2711_oh_defconfig
	$(KERNEL_MAKE) -C $(KERNEL_SRC_TMP_PATH) ARCH=$(DTBS_ARCH) $(KERNEL_CROSS_COMPILE_DTB) $(DEFCONFIG_FILE)
	$(KERNEL_MAKE) -C $(KERNEL_SRC_TMP_PATH) ARCH=$(DTBS_ARCH) $(KERNEL_CROSS_COMPILE_DTB) $(DTBS)

.PHONY: build-dtb
build-dtb: $(DTB)
