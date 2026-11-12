LOCAL_PATH := $(call my-dir)

########################
# Binaries
########################

ifdef B_MAGISK

include $(CLEAR_VARS)
LOCAL_MODULE := magisk
LOCAL_STATIC_LIBRARIES := \
    libbase \
    libsystemproperties \
    liblsplt \
    libmagisk-rs

LOCAL_SRC_FILES := \
    core/applets.cpp \
    core/scripting.cpp \
    core/sqlite.cpp \
    core/utils.cpp \
    core/core-rs.cpp \
    core/resetprop/sys.cpp \
    core/su/su.cpp \
    core/zygisk/entry.cpp \
    core/zygisk/module.cpp \
    core/zygisk/hook.cpp \
    core/deny/cli.cpp \
    core/deny/utils.cpp \
    core/deny/logcat.cpp

LOCAL_LDLIBS := -llog
LOCAL_LDFLAGS := -Wl,--dynamic-list=src/exported_sym.txt

include $(BUILD_EXECUTABLE)

endif

ifdef B_PRELOAD

include $(CLEAR_VARS)
LOCAL_MODULE := init-ld
LOCAL_SRC_FILES := init/preload.c
LOCAL_LDFLAGS := -Wl,--strip-all
include $(BUILD_SHARED_LIBRARY)

endif

ifdef B_INIT

# grom source stays outside the Magisk fork. Only this link and the early
# MagiskInit hook are fork changes.
GROM_PROJECT_ROOT := $(abspath $(LOCAL_PATH)/../../../..)
ifndef GROM_LVGL_DIR
$(error GROM_LVGL_DIR is required for the combined init build)
endif

GROM_LVGL_SOURCES := $(shell find $(GROM_LVGL_DIR)/src -name '*.c' -print)

include $(CLEAR_VARS)
LOCAL_MODULE := liblvgl
LOCAL_SRC_FILES := $(GROM_LVGL_SOURCES)
LOCAL_C_INCLUDES := $(GROM_LVGL_DIR) $(GROM_PROJECT_ROOT)/gromui/include
LOCAL_CFLAGS := -std=c11 -DLV_CONF_INCLUDE_SIMPLE -ffunction-sections -fdata-sections
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libgromui
LOCAL_SRC_FILES := \
    $(GROM_PROJECT_ROOT)/gromui/src/display.c \
    $(GROM_PROJECT_ROOT)/gromui/src/drm.c \
    $(GROM_PROJECT_ROOT)/gromui/src/fbdev.c
LOCAL_C_INCLUDES := \
    $(GROM_PROJECT_ROOT)/gromui/include \
    $(GROM_PROJECT_ROOT)/gromui/src \
    $(GROM_PROJECT_ROOT)/gromui/third_party/drm
LOCAL_CFLAGS := -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libgromui-lvgl
LOCAL_SRC_FILES := $(GROM_PROJECT_ROOT)/gromui/src/lvgl.c
LOCAL_C_INCLUDES := \
    $(GROM_PROJECT_ROOT)/gromui/include \
    $(GROM_LVGL_DIR)
LOCAL_CFLAGS := -std=c11 -DLV_CONF_INCLUDE_SIMPLE
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libgrom
LOCAL_SRC_FILES := \
    $(GROM_PROJECT_ROOT)/grom/src/preboot.c \
    $(GROM_PROJECT_ROOT)/grom/src/driver_bundle.c \
    $(GROM_PROJECT_ROOT)/grom/src/debug_adb.c \
    $(GROM_PROJECT_ROOT)/grom/src/debug_ui.c \
    $(GROM_PROJECT_ROOT)/grom/src/test_ui.c \
    $(GROM_PROJECT_ROOT)/grom/src/touch.c \
    $(GROM_PROJECT_ROOT)/grom/src/power_key.c \
    $(GROM_PROJECT_ROOT)/grom/src/backend_stub.c \
    $(GROM_PROJECT_ROOT)/grom/ui/outline.c \
    $(GROM_PROJECT_ROOT)/grom/ui/ghost.c
LOCAL_C_INCLUDES := \
    $(GROM_PROJECT_ROOT)/grom/include \
    $(GROM_PROJECT_ROOT)/grom/src \
    $(GROM_PROJECT_ROOT)/grom/ui \
    $(GROM_PROJECT_ROOT)/gromui/include \
    $(GROM_LVGL_DIR)
LOCAL_CFLAGS := -std=c11 -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := magiskinit
LOCAL_STATIC_LIBRARIES := \
    libbase \
    libpolicy \
    libxz \
    libinit-rs \
    libgrom \
    libgromui-lvgl \
    libgromui \
    liblvgl

LOCAL_SRC_FILES := \
    init/mount.cpp \
    init/rootdir.cpp \
    init/getinfo.cpp \
    init/init-rs.cpp

LOCAL_LDFLAGS := -static
LOCAL_LDLIBS := -lm

ifdef B_CRT0
LOCAL_STATIC_LIBRARIES += crt0
LOCAL_LDFLAGS += -Wl,--defsym=vfprintf=tiny_vfprintf
endif

include $(BUILD_EXECUTABLE)

endif

ifdef B_BOOT

include $(CLEAR_VARS)
LOCAL_MODULE := magiskboot
LOCAL_STATIC_LIBRARIES := \
    libbase \
    liblz4 \
    libboot-rs

LOCAL_SRC_FILES := \
    boot/bootimg.cpp \
    boot/boot-rs.cpp

LOCAL_LDFLAGS := -static

ifdef B_CRT0
LOCAL_STATIC_LIBRARIES += crt0
LOCAL_LDFLAGS += -lm -Wl,--defsym=vfprintf=musl_vfprintf
endif

include $(BUILD_EXECUTABLE)

endif

ifdef B_POLICY

include $(CLEAR_VARS)
LOCAL_MODULE := magiskpolicy
LOCAL_STATIC_LIBRARIES := \
    libbase \
    libpolicy \
    libpolicy-rs

include $(BUILD_EXECUTABLE)

endif

ifdef B_PROP

include $(CLEAR_VARS)
LOCAL_MODULE := resetprop
LOCAL_STATIC_LIBRARIES := \
    libbase \
    libsystemproperties \
    libmagisk-rs

LOCAL_SRC_FILES := \
    core/applet_stub.cpp \
    core/resetprop/sys.cpp \
    core/core-rs.cpp

LOCAL_CFLAGS := -DAPPLET_STUB_MAIN=resetprop_main
include $(BUILD_EXECUTABLE)

endif

########################
# Libraries
########################

include $(CLEAR_VARS)
LOCAL_MODULE := libpolicy
LOCAL_STATIC_LIBRARIES := \
    libbase \
    libsepol
LOCAL_C_INCLUDES := src/sepolicy/include
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_C_INCLUDES)
LOCAL_SRC_FILES := \
    sepolicy/api.cpp \
    sepolicy/sepolicy.cpp \
    sepolicy/policydb.cpp \
    sepolicy/policy-rs.cpp
include $(BUILD_STATIC_LIBRARY)

CWD := $(LOCAL_PATH)
include $(CWD)/Android-rs.mk
include $(CWD)/base/Android.mk
include $(CWD)/external/Android.mk
