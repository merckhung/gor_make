LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libmath
LOCAL_SRC_FILES := src/math.c
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/src
LOCAL_CFLAGS := -Wall -O2
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := calculator
LOCAL_SRC_FILES := src/main.c
LOCAL_STATIC_LIBRARIES := libmath
LOCAL_CFLAGS := -Wall -O2
include $(BUILD_EXECUTABLE)
