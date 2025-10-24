OHOS_ROOT = /home/diemit/OpenHarmony
CC = $(OHOS_ROOT)/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang --sysroot=$(OHOS_ROOT)/out/sagit/obj/third_party/musl
CXX = $(OHOS_ROOT)/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang++ --sysroot=$(OHOS_ROOT)/out/sagit/obj/third_party/musl
AR = $(OHOS_ROOT)/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-ar
LD = $(OHOS_ROOT)/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-link
ARFLAG = -rcs

TARGET = gl_test
PROJECT_PATH = $(shell pwd)

CFLAGS := --target=aarch64-linux-ohosmusl \
	--sysroot=$(OHOS_ROOT)/out/sagit/obj/third_party/musl

# Warning
CFLAGS += -Wno-c++11-narrowing

# Lib
CLIBS = -lm -ldl -lrt
CLIBS += -L$(OHOS_ROOT)/device/soc/qualcomm/msm8998/hardware/gpu/lib64 -lgallium_dri
CLIBS += -L$(OHOS_ROOT)/out/sagit/packages/phone/system/lib64 -lhilog -lsurface.z -lutils.z

CFLAGS += -DOHOS_USE_DRM -DOHOS_USE_GLESv2 -DOHOS_USE_EGL
INCLUDE_DIRS += \
    -I$(OHOS_ROOT)/third_party/EGL/api \
    -I$(OHOS_ROOT)/third_party/openGLES/api \
    -I$(OHOS_ROOT)/utils/native/base/include \
    -I$(OHOS_ROOT)/drivers/peripheral/base \
    -I$(OHOS_ROOT)/foundation/graphic/standard/interfaces/innerkits/common \
    -I$(OHOS_ROOT)/foundation/graphic/standard/interfaces/innerkits/surface \
    -I$(OHOS_ROOT)/foundation/graphic/standard/utils/buffer_handle/export \
    -I$(OHOS_ROOT)/foundation/communication/ipc/interfaces/innerkits/ipc_core/include \
    -I$(OHOS_ROOT)/foundation/aafwk/standard/frameworks/kits/ability/ability_runtime/include \
    -I$(OHOS_ROOT)/foundation/aafwk/standard/interfaces/innerkits/ability_manager/include  \
    -I$(OHOS_ROOT)/foundation/aafwk/standard/interfaces/innerkits/app_manager/include/appmgr  \
    -I$(OHOS_ROOT)/third_party/jsoncpp/include  \
    -I$(OHOS_ROOT)/third_party/json/include  \
    -I$(OHOS_ROOT)/foundation/windowmanager/interfaces/innerkits/wm \
    -I$(OHOS_ROOT)/foundation/graphic/standard/frameworks/surface \
    -I$(OHOS_ROOT)/foundation/graphic/standard/rosen/modules/render_service_base/include \
    -I$(OHOS_ROOT)/foundation/graphic/standard/rosen/modules/render_service_client/core \
    -I$(OHOS_ROOT)/foundation/graphic/standard/rosen/modules/render_service_client \
    -I$(OHOS_ROOT)/third_party/flutter/skia \
    -I$(OHOS_ROOT)/third_party/flutter/skia/include/core \
    -I$(OHOS_ROOT) \
    -I$(OHOS_ROOT)/third_party/skia

INCLUDE_DIRS += -I$(PROJECT_PATH)/include \
    -I$(PROJECT_PATH)/src/glad/include \
    -I$(PROJECT_PATH)/src/libmatrix \
    -I$(PROJECT_PATH)/src 

export CC CXX CFLAGS AR LD ARFLAG MODULE_SELECT

CPP_SOURCES = $(wildcard ./src/*.cpp)
CPP_OBJECTS = $(patsubst %.cpp,%.o,$(CPP_SOURCES))

CC_SOURCES = $(wildcard ./src/libmatrix/*.cc)
CC_OBJECTS = $(patsubst %.cc,%.o,$(CC_SOURCES))

C_SOURCES = $(wildcard ./src/*.c)
C_SOURCES += $(wildcard ./src/glad/src/*.c)
C_OBJECTS = $(patsubst %.c,%.o,$(C_SOURCES))

OBJECTS := $(CPP_OBJECTS)
OBJECTS += $(CC_OBJECTS)
OBJECTS += $(C_OBJECTS)

.PHONY : all clean

all: $(CPP_OBJECTS) $(CC_OBJECTS) $(C_OBJECTS) $(TARGET)
	@echo $(CPP_OBJECTS)
	@echo $(C_OBJECTS)

$(CPP_OBJECTS) : %.o : %.cpp
	$(CXX) -c -fPIC $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

$(CC_OBJECTS) : %.o : %.cc
	$(CXX) -c -fPIC $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

$(C_OBJECTS) : %.o : %.c
	$(CC) -c -fPIC $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

main.o : main.cpp
	$(CC) -c -fPIC $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

$(TARGET) : $(OBJECTS) main.o
	$(CXX) $(CFLAGS) $^ -o $@ $(CLIBS)

native_window:
	$(CXX) $(CFLAGS) -shared -fPIC $(OBJECTS) -o libnative_window_wrapper.z.so $(INCLUDE_DIRS) --verbose

clean:
	@rm -rf main.o
	for dir in $(SUBDIRS);\
	do $(MAKE) -C $$dir clean||exit 1;\
	done
	rm -rf $(TARGET) $(OBJECTS)
