/*
**
** Copyright 2008, The Android Open Source Project
** Copyright 2012, Samsung Electronics Co. LTD
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/*!
 * \file      ExynosCameraHWInterface2.h
 * \brief     header file for Android Camera API 2.0 HAL
 * \author    Sungjoong Kang(sj3.kang@samsung.com)
 * \date      2012/05/31
 *
 * <b>Revision History: </b>
 * - 2012/05/31 : Sungjoong Kang(sj3.kang@samsung.com) \n
 *   Initial Release
 */

#ifndef EXYNOS_CAMERA_HW_INTERFACE_2_H
#define EXYNOS_CAMERA_HW_INTERFACE_2_H

#include <hardware/camera2.h>
#include <camera/Camera.h>
#include <camera/CameraParameters.h>
#include <pthread.h>
#include <semaphore.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/poll.h>

#include "ExynosCamera2.h"
#include "exynos_v4l2.h"
#include "videodev2_samsung.h"
#include "exynos_format.h"
#include "jpeg_hal.h"

#define PREVIEW_NODE    "/dev/video1"
#define CAPTURE_NODE    "/dev/video3"

#define STREAM_ID_PREVIEW           (0)
#define STREAM_ID_RECORD            (1)
#define STREAM_ID_PRVCB             (2)
#define STREAM_ID_JPEG              (4)
#define STREAM_ID_ZSL               (5)

#define STREAM_ID_JPEG_REPROCESS    (8)
#define STREAM_ID_LAST              STREAM_ID_JPEG_REPROCESS

#define MAX_BUFFERS                 (4)
#define MAX_CAPTURE_BUFFERS         (1)

namespace android {

class ExynosCameraHWInterface2:public virtual RefBase {
public:
    ExynosCameraHWInterface2(int cameraId, camera2_device_t *dev);
    virtual             ~ExynosCameraHWInterface2();

    inline  int         getCameraId() const;

    virtual int         setRequestQueueSrcOps(const
                            camera2_request_queue_src_ops_t *request_src_ops);
    virtual int         notifyRequestQueueNotEmpty();
    virtual int         setFrameQueueDstOps(const
                            camera2_frame_queue_dst_ops_t *frame_dst_ops);
    virtual int         getInProgressCount();
    virtual int         flushCapturesInProgress();
    virtual int         constructDefaultRequest(int request_template,
                            camera_metadata_t **request);
    virtual int         allocateStream(uint32_t width, uint32_t height, 
                                    int format,
                                    const camera2_stream_ops_t *stream_ops,
                                    uint32_t *stream_id,
                                    uint32_t *format_actual, uint32_t *usage,
                                    uint32_t *max_buffers);
    virtual int         registerStreamBuffers(uint32_t stream_id,
                                    int num_buffers, buffer_handle_t *buffers);
    virtual int         releaseStream(uint32_t stream_id);
    virtual int         allocateReprocessStream(uint32_t width, uint32_t height,
                                    uint32_t format,
                                    const camera2_stream_in_ops_t
                                    *reprocess_stream_ops,
                                    uint32_t *stream_id,
                                    uint32_t *consumer_usage,
                                    uint32_t *max_buffers);
    virtual int         releaseReprocessStream(uint32_t stream_id);
    virtual int         triggerAction(uint32_t trigger_id, int ext1, int ext2);
    virtual int         setNotifyCallback(camera2_notify_callback notify_cb,
                                    void *user);
    virtual int         getMetadataVendorTagOps(vendor_tag_query_ops_t **ops);
    virtual int         dump(int fd);
private:
    pthread_t           request_manager_id;
    pthread_t           capture_thread_id;
};

}; /* namespace android */

#endif
