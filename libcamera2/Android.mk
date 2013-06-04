
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

# HAL module implemenation stored in
# hw/<COPYPIX_HARDWARE_MODULE_ID>.<ro.product.board>.so
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

LOCAL_C_INCLUDES += \
	hardware/samsung_slsi/exynos4/include \
	frameworks/native/include \
	system/media/camera/include \
        vendor/samsung_slsi/exynos4412/gralloc

LOCAL_SRC_FILES:= \
	ExynosCameraHWInterface2.cpp \
        ExynosCamera2.cpp

LOCAL_SHARED_LIBRARIES:= libutils libcutils libbinder liblog libcamera_client libhardware

#LOCAL_CFLAGS += -DGAIA_FW_BETA

LOCAL_SHARED_LIBRARIES += libexynosutils libhwjpeg libexynosv4l2 libcamera_metadata

LOCAL_MODULE := camera.$(TARGET_DEVICE)

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
