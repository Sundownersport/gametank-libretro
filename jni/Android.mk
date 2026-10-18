LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/..

include $(CORE_DIR)/Makefile.common

include $(CLEAR_VARS)
LOCAL_MODULE := retro
LOCAL_SRC_FILES := $(SOURCES_CXX)
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -Wall $(INCFLAGS)
LOCAL_LDFLAGS := -Wl,--version-script=$(CORE_DIR)/link.T
include $(BUILD_SHARED_LIBRARY)
