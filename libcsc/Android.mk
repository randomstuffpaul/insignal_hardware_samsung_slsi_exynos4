LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_COPY_HEADERS_TO := libsecmm
LOCAL_COPY_HEADERS := \
	csc.h

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := \
	csc.c

ifeq ($(BOARD_USE_EXYNOS_OMX), true)
OMX_NAME := exynos
else
OMX_NAME := sec
endif

LOCAL_C_INCLUDES := \
	$(TOP)/hardware/samsung_slsi/openmax/include/khronos \
	$(TOP)/hardware/samsung_slsi/openmax/include/sec

LOCAL_CFLAGS :=

LOCAL_MODULE := libcsc

LOCAL_PRELINK_MODULE := false

LOCAL_ARM_MODE := arm

LOCAL_STATIC_LIBRARIES := libswconverter
LOCAL_SHARED_LIBRARIES := liblog

ifeq ($(BOARD_USE_SAMSUNG_COLORFORMAT), true)
LOCAL_CFLAGS += -DUSE_SAMSUNG_COLORFORMAT
endif

ifeq ($(TARGET_BOARD_PLATFORM), exynos4)
LOCAL_SRC_FILES += hwconverter_wrapper.cpp
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../libhwconverter \
LOCAL_CFLAGS += -DUSE_FIMC
LOCAL_SHARED_LIBRARIES += libfimc libhwconverter
endif

ifeq ($(BOARD_USE_EXYNOS_OMX), true)
LOCAL_CFLAGS += -DEXYNOS_OMX
endif

include $(BUILD_SHARED_LIBRARY)
