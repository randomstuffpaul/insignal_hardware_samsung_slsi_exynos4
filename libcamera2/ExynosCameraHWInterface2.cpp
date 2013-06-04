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
 * \file      ExynosCameraHWInterface2.cpp
 * \brief     source file for Android Camera API 2.0 HAL
 * \date      2013/03/05
 *
 * <b>Revision History: </b>
 * - 2012/03/05:
 *   Initial Release
 */

#define LOG_TAG "ExynosCameraHWInterface2"
//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0 /* per frame debugging */

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <utils/Log.h>

#include "ExynosCameraHWInterface2.h"
#include "gralloc_priv.h"

#define ENQUEUED    0
#define DEQUEUED    1

namespace android {

ExynosCamera2 *g_camera2 = NULL;
gralloc_module_t const*             m_grallocHal;
sem_t                               request_manager_sem;
sem_t                               preview_start_sem;
sem_t                               record_start_sem;
sem_t                               send_preview_frame_sem;
sem_t                               preview_frame_sent_sem;
sem_t                               send_record_frame_sem;
sem_t                               record_frame_sent_sem;
sem_t                               send_capture_frame_sem;
sem_t                               capture_frame_sent_sem;
camera2_request_queue_src_ops_t     *m_requestQueueOps;
camera2_frame_queue_dst_ops_t       *m_frameQueueOps;
camera2_notify_callback             m_notifyCb;
void                                *m_callbackCookie;
const camera2_stream_ops_t          *m_previewStreamOps;
const camera2_stream_ops_t          *m_zslStreamOps;
const camera2_stream_ops_t          *m_captureStreamOps;
const camera2_stream_ops_t          *m_recordStreamOps;
int                                 m_previewWidth;
int                                 m_previewHeight;
int                                 m_recordWidth;
int                                 m_recordHeight;
int                                 m_captureWidth;
int                                 m_captureHeight;
bool                                request_manager_exit;
volatile bool                       preview_thread_created;
volatile bool                       preview_thread_running;
volatile bool                       record_thread_running;
volatile bool                       capture_thread_running;
buffer_handle_t                     preview_buffers[16];
buffer_handle_t                     record_buffers[16];
int                                 record_dequeue_status[16];
pthread_t                           preview_thread_id;
pthread_t                           record_thread_id;
int                                 m_cameraId;
bool                                mUsesInternalISP;
char                                sensor_name[20];
int                                 num_preview_buffers;
int                                 num_record_buffers;
volatile uint64_t                   request_in_progress;
volatile bool                       record_running;
volatile bool                       do_not_dequeue;
volatile bool                       exit_record;
volatile bool                       copy_buffer;
char                                *snapshot_buffer;
sem_t                               buffer_copied;

int findBufferIndex(buffer_handle_t buffer, buffer_handle_t *bufferArray, int num_buffers)
{
    for (int i = 0; i < num_buffers; i++) {
        if (buffer == bufferArray[i])
            return i;
    }
    return -1;
}

void *request_manager(void *arg)
{
    ALOGV("DEBUG: %s called", __func__);
    camera_metadata_t *currentRequest = NULL;
    camera_metadata_entry_t streams;
    int ret = 0;

    while (1) {
        ALOGV("DEBUG: %s: Request manager waiting for request queue to fill ", __func__);
        sem_wait(&request_manager_sem);
        ALOGV("DEBUG: %s: Request queue NOT empty notification received, proceeding with dequeing", __func__);

        if (request_manager_exit)
            goto exit;

        m_requestQueueOps->dequeue_request(m_requestQueueOps, &currentRequest);
        while (currentRequest != NULL) {
            request_in_progress++;
            /* sort metadata buffer for fast searching */
            sort_camera_metadata(currentRequest);

            ret = find_camera_metadata_entry(currentRequest, ANDROID_REQUEST_OUTPUT_STREAMS, &streams);
            if (ret != -ENOENT) {
                /* TODO: This condition seems correct but verify in case it fails.
                   When recording is on and number of stream count requested becomes
                   1, we assume that recording has been stopped */
                if (record_running && streams.count == 1) {
                    ALOGV("DEBUG: %s: Recording stopped\n", __func__);
                    record_running = false;
                    sem_post(&send_record_frame_sem);
                }

                /* TODO: This condition seems correct but verify in case it fails.
                   When recording is on and number of stream count requested becomes
                   3, we assume that capture is also requested meaning VIDEO_SNAPSHOT
                   is requested */
                if (record_running && streams.count == 3)
                   copy_buffer = true;

                for (int i = 0; i < streams.count; i++) {
                    ALOGVV("DEBUG: %s: Stream ID: %d, stream count: %d\n", __func__, streams.data.u8[i], streams.count);
                    if (streams.data.u8[i] == STREAM_ID_JPEG && capture_thread_running) {
                        ALOGVV("DEBUG: %s: Notifying capture thread to start\n", __func__);
                        sem_post(&send_capture_frame_sem);
                        ALOGVV("DEBUG: %s: Waiting for capture thread to send JPEG image\n", __func__);
                        sem_wait(&capture_frame_sent_sem);
                        ALOGVV("DEBUG: %s: Done waiting, JPEG image sent\n", __func__);
                    } else if (streams.data.u8[i] == STREAM_ID_RECORD && record_thread_running) {
                        if (!record_running && do_not_dequeue) {
                            sem_post(&record_start_sem);
                        }
                        record_running = true;
                        sem_post(&send_record_frame_sem);
                        ALOGVV("DEBUG: %s: Waiting for record thread to send record frame\n", __func__);
                        sem_wait(&record_frame_sent_sem);
                        ALOGVV("DEBUG: %s: Done waiting, record frame sent\n", __func__);
                    } else if (streams.data.u8[i] == STREAM_ID_PREVIEW && preview_thread_running) {
                        sem_post(&send_preview_frame_sem);
                        ALOGVV("DEBUG: %s: Waiting for preview thread to send preview frame\n", __func__);
                        sem_wait(&preview_frame_sent_sem);
                        ALOGVV("DEBUG: %s: Done waiting, preview frame sent\n", __func__);
                    }
                }
            }

            /* Return the metadata buffer to the framework */
            m_requestQueueOps->free_request(m_requestQueueOps, currentRequest);
            request_in_progress--;

            if (request_manager_exit)
                goto exit;

            /* Dequeue new request from framework */
            m_requestQueueOps->dequeue_request(m_requestQueueOps, &currentRequest);
        }
    }
exit:
    ALOGV("DEBUG: %s: Request manager exiting", __func__);
    return 0;
}

void *preview_thread(void *arg)
{
    ALOGV("DEBUG: %s called", __func__);
    int ret = 0;
    int index = 0;
    int m_preview_fd;
    struct v4l2_buffer v4l2_buf;
    struct v4l2_format v4l2_fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_plane planes[2];
    buffer_handle_t *buf = NULL;
    void *virtAddr[3];

    memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
    memset(&v4l2_fmt, 0, sizeof(struct v4l2_format));
    memset(&req, 0, sizeof(struct v4l2_requestbuffers));
    memset(&planes, 0, sizeof(struct v4l2_plane));

    m_preview_fd = exynos_v4l2_open(PREVIEW_NODE, O_RDWR, 0);
    if (m_preview_fd < 0)
        ALOGE("ERR(%s):Cannot open %s (error : %s)", __func__, PREVIEW_NODE, strerror(errno));

    /* S_INPUT */
    exynos_v4l2_s_input(m_preview_fd, m_cameraId);

    v4l2_fmt.fmt.pix_mp.width = m_previewWidth;
    v4l2_fmt.fmt.pix_mp.height = m_previewHeight;
    v4l2_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

    if (mUsesInternalISP) {
        v4l2_fmt.type = V4L2_BUF_TYPE_PRIVATE;
        v4l2_fmt.fmt.pix_mp.field = (enum v4l2_field) IS_MODE_PREVIEW_STILL;

        /* Call S_FMT for FIMC-IS first */
        ret = exynos_v4l2_s_fmt(m_preview_fd, &v4l2_fmt);
        if (ret < 0)
            ALOGE("%s: exynos_v4l2_s_fmt fail for FIMC-IS (%d)",__func__, ret);
    }

    /* Call S_FMT for Capture */
    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v4l2_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    ret = exynos_v4l2_s_fmt(m_preview_fd, &v4l2_fmt);
    if (ret < 0)
        ALOGE("%s: exynos_v4l2_s_fmt fail for Capture (%d)",__func__, ret);

    /* Call S_CTRL for FIMC IS */
    if (mUsesInternalISP)
        exynos_v4l2_s_ctrl(m_preview_fd, V4L2_CID_IS_S_SCENARIO_MODE, IS_MODE_PREVIEW_STILL);

    ALOGV("DEBUG: %s: Waiting for notification to start preview", __func__);
    sem_wait(&preview_start_sem);
    ALOGV("DEBUG: %s: Notification to start preview received", __func__);

    /* Call REQBUF */
    req.count = num_preview_buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_USERPTR;
    ret = exynos_v4l2_reqbufs(m_preview_fd, &req);
    if (ret < 0)
        ALOGE("%s: VIDIOC_REQBUFS (fd:%d) failed (%d)",__func__,m_preview_fd, ret);

    for (int i = 0; i < MAX_BUFFERS; i++) {
        ret = m_previewStreamOps->dequeue_buffer(m_previewStreamOps, &buf);
        if (ret != NO_ERROR || buf == NULL) {
            ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
            return 0;
        }

        if (m_grallocHal->lock(m_grallocHal, *buf,
                           GRALLOC_USAGE_SW_WRITE_OFTEN,
                           0, 0, m_previewWidth, m_previewHeight, virtAddr) != 0) {
            ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
            return 0;
        }

        index = findBufferIndex(*buf, preview_buffers, num_preview_buffers);
        if (index == -1) {
            ALOGE("ERROR: %s: Could not find matching buffer index", __func__);
            return 0;
        }

        v4l2_buf.m.planes = planes;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_USERPTR;
        v4l2_buf.length = 2;
        v4l2_buf.flags = 0;
        v4l2_buf.index = index;
        v4l2_buf.m.planes[0].m.userptr = (unsigned long)(virtAddr[0]);
        v4l2_buf.m.planes[0].length = (m_previewWidth * m_previewHeight * 3) >> 1;

        if (exynos_v4l2_qbuf(m_preview_fd, &v4l2_buf) < 0) {
            ALOGE("ERR(%s):preview exynos_v4l2_qbuf() fail", __func__);
            return 0;
        }
    }

    /* Stream ON */
    exynos_v4l2_streamon(m_preview_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    /* Indicate request manager that preview thread is now up and running
       and ready to accept preview requests */
    preview_thread_running = true;

    while (preview_thread_running) {

        ALOGVV("DEBUG: %s: Preview thread waiting for notification from request manager\n", __func__);
        /* Wait for notification from request manager to send preview frame */
        sem_wait(&send_preview_frame_sem);
        ALOGVV("DEBUG: %s: Notification to send preview frame received\n", __func__);

        /* Exit preview thread if it was requested to exit */
        if (!preview_thread_running)
            break;

        exynos_v4l2_dqbuf(m_preview_fd, &v4l2_buf);
        index = v4l2_buf.index;

        /* Copy the buffer in VIDEO_SNAPSHOT case */
        if (copy_buffer) {
            if (m_grallocHal->lock(m_grallocHal, preview_buffers[index],
                           GRALLOC_USAGE_SW_WRITE_OFTEN,
                           0, 0, m_previewWidth, m_previewHeight, virtAddr) != 0) {
                ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
                break;
            }

            snapshot_buffer = (char *) malloc((m_previewWidth * m_previewHeight * 2));
            if (snapshot_buffer != NULL)
                memcpy(snapshot_buffer, virtAddr[0], (m_previewWidth * m_previewHeight * 3) >> 1);
            else
                ALOGE("ERROR: %s: Snapshot buffer allocation failed\n", __func__);
            sem_post(&buffer_copied);
            copy_buffer = false;
        }

        ret = m_previewStreamOps->enqueue_buffer(m_previewStreamOps, systemTime(), &preview_buffers[index]);
        if (ret != NO_ERROR) {
            preview_thread_running = false;
            sem_post(&preview_frame_sent_sem);
            ALOGE("ERROR: %s: Unable to enqueue buffer for index = %d", __func__, index);
            break;
        }

        /* Notify Request manager that preview frame is sent */
        sem_post(&preview_frame_sent_sem);
        ALOGVV("DEBUG: %s: Preview frame successfully sent to camera service\n", __func__);

        ret = m_previewStreamOps->dequeue_buffer(m_previewStreamOps, &buf);
        if (ret != NO_ERROR || buf == NULL) {
            ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
            break;
        }

        index = findBufferIndex(*buf, preview_buffers, num_preview_buffers);
        if (index == -1) {
            ALOGE("ERROR: %s: Could not find matching buffer index", __func__);
            return 0;
        }

        if (m_grallocHal->lock(m_grallocHal, *buf,
                           GRALLOC_USAGE_SW_WRITE_OFTEN,
                           0, 0, m_previewWidth, m_previewHeight, virtAddr) != 0) {
            ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
            break;
        }

        v4l2_buf.m.planes = planes;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_USERPTR;
        v4l2_buf.length = 2;
        v4l2_buf.flags = 0;
        v4l2_buf.index = index;
        v4l2_buf.m.planes[0].m.userptr = (unsigned long)(virtAddr[0]);
        v4l2_buf.m.planes[0].length = (m_previewWidth * m_previewHeight * 3) >> 1;

        if (exynos_v4l2_qbuf(m_preview_fd, &v4l2_buf) < 0) {
            ALOGE("ERR(%s):preview exynos_v4l2_qbuf() fail", __func__);
            break;
        }
    }

    preview_thread_running = false;

    ALOGV("DEBUG: %s: while loop ended", __func__);

    /* Stream OFF */
    exynos_v4l2_streamoff(m_preview_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    /* close preview node */
    exynos_v4l2_close(m_preview_fd);

    return 0;
}

void *record_thread(void *arg)
{
    ALOGV("DEBUG: %s called", __func__);
    int ret = 0;
    int index = 0;
    int m_rec_fd;
    struct v4l2_buffer v4l2_buf;
    struct v4l2_format v4l2_fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_plane planes[2];
    buffer_handle_t *buf = NULL;
    void *virtAddr[3];

    memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
    memset(&v4l2_fmt, 0, sizeof(struct v4l2_format));
    memset(&req, 0, sizeof(struct v4l2_requestbuffers));
    memset(&planes, 0, sizeof(struct v4l2_plane));

    v4l2_fmt.fmt.pix_mp.width = m_recordWidth;
    v4l2_fmt.fmt.pix_mp.height = m_recordHeight;
    v4l2_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

again:
    ALOGV("DEBUG: %s: Waiting for notification to start record", __func__);
    sem_wait(&record_start_sem);
    ALOGV("DEBUG: %s: Notification to start record received", __func__);

    if (exit_record)
        return 0;

    /* Open capture node */
    m_rec_fd = exynos_v4l2_open(CAPTURE_NODE, O_RDWR, 0);
    if (m_rec_fd < 0) {
        ALOGE("ERR(%s):Cannot open %s (error : %s)", __func__, CAPTURE_NODE, strerror(errno));
    } else {
        if (exynos_v4l2_querycap(m_rec_fd, V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {

            /* S_INPUT */
            exynos_v4l2_s_input(m_rec_fd, m_cameraId);
        } else {
            ALOGE("ERROR: %s: %s node does not support capture", __func__, CAPTURE_NODE);
        }
    }

    /* Call S_FMT for Capture */
    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v4l2_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    ret = exynos_v4l2_s_fmt(m_rec_fd, &v4l2_fmt);
    if (ret < 0)
        ALOGE("%s: exynos_v4l2_s_fmt fail for Capture (%d)",__func__, ret);

    /* Call REQBUF */
    req.count = num_record_buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_USERPTR;
    ret = exynos_v4l2_reqbufs(m_rec_fd, &req);
    if (ret < 0)
        ALOGE("%s: VIDIOC_REQBUFS (fd:%d) failed (%d)",__func__,m_rec_fd, ret);

    if (!do_not_dequeue) {
        for (int i = 0; i < MAX_BUFFERS; i++) {
            ret = m_recordStreamOps->dequeue_buffer(m_recordStreamOps, &buf);
            if (ret != NO_ERROR || buf == NULL) {
                ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
                return 0;
            }

            if (m_grallocHal->lock(m_grallocHal, *buf,
                               GRALLOC_USAGE_SW_WRITE_OFTEN,
                               0, 0, m_recordWidth, m_recordHeight, virtAddr) != 0) {
                ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
                return 0;
            }

            index = findBufferIndex(*buf, record_buffers, num_record_buffers);
            if (index == -1) {
                ALOGE("ERROR: %s: Could not find matching buffer index", __func__);
                return 0;
            }

            record_dequeue_status[index] = DEQUEUED;

            v4l2_buf.m.planes = planes;
            v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v4l2_buf.memory = V4L2_MEMORY_USERPTR;
            v4l2_buf.length = 1;
            v4l2_buf.flags = 0;
            v4l2_buf.index = index;
            v4l2_buf.m.planes[0].m.userptr = (unsigned long)(virtAddr[0]);
            v4l2_buf.m.planes[0].length = (m_recordWidth * m_recordHeight * 3) >> 1;

            if (exynos_v4l2_qbuf(m_rec_fd, &v4l2_buf) < 0) {
                ALOGE("ERR(%s):record exynos_v4l2_qbuf() fail", __func__);
                return 0;
            }
        }
    } else {
        for (int i = 0; i < num_record_buffers; i++) {

            if (record_dequeue_status[i] == DEQUEUED) {
                if (m_grallocHal->lock(m_grallocHal, record_buffers[i],
                                   GRALLOC_USAGE_SW_WRITE_OFTEN,
                                   0, 0, m_recordWidth, m_recordHeight, virtAddr) != 0) {
                    ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
                    return 0;
                }

                index = findBufferIndex(record_buffers[i], record_buffers, num_record_buffers);
                if (index == -1) {
                    ALOGE("ERROR: %s: Could not find matching buffer index", __func__);
                    return 0;
                }

                v4l2_buf.m.planes = planes;
                v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                v4l2_buf.memory = V4L2_MEMORY_USERPTR;
                v4l2_buf.length = 1;
                v4l2_buf.flags = 0;
                v4l2_buf.index = index;
                v4l2_buf.m.planes[0].m.userptr = (unsigned long)(virtAddr[0]);
                v4l2_buf.m.planes[0].length = (m_recordWidth * m_recordHeight * 3) >> 1;

                if (exynos_v4l2_qbuf(m_rec_fd, &v4l2_buf) < 0) {
                    ALOGE("ERR(%s):record exynos_v4l2_qbuf() fail", __func__);
                    return 0;
                }
            }
        }
        do_not_dequeue = false;
    }

    /* Stream ON */
    exynos_v4l2_streamon(m_rec_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    /* Indicate request manager that record thread is now up and running
       and ready to accept record requests */
    record_thread_running = true;
    record_running = true;

    while (record_thread_running) {

        ALOGVV("DEBUG: %s: Record thread waiting for notification from request manager\n", __func__);
        /* Wait for notification from request manager to send record frame */
        sem_wait(&send_record_frame_sem);
        ALOGVV("DEBUG: %s: Notification to send record frame received\n", __func__);

        /* Exit if record thread has been asked to exit */
        if (!record_thread_running)
            break;

        if (!record_running) {
            do_not_dequeue = true;
            /* Stream OFF */
            exynos_v4l2_streamoff(m_rec_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

            /* Close record fd */
            exynos_v4l2_close(m_rec_fd);
            ALOGV("DEBUG: %s: Record stop signalled, FIMC closed and waiting "
                    "for record start notification", __func__);
            goto again;
        }

        exynos_v4l2_dqbuf(m_rec_fd, &v4l2_buf);
        index = v4l2_buf.index;

        ret = m_recordStreamOps->enqueue_buffer(m_recordStreamOps, systemTime(), &record_buffers[index]);
        if (ret != NO_ERROR) {
            record_thread_running = false;
            sem_post(&record_frame_sent_sem);
            ALOGE("ERROR: %s: Unable to enqueue buffer", __func__);
            break;
        }

        record_dequeue_status[index] = ENQUEUED;

        /* Notify Request manager that record frame is sent */
        sem_post(&record_frame_sent_sem);
        ALOGVV("DEBUG: %s: Record frame successfully sent to camera service\n", __func__);

        ret = m_recordStreamOps->dequeue_buffer(m_recordStreamOps, &buf);
        if (ret != NO_ERROR || buf == NULL) {
            ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
            break;
        }

        if (m_grallocHal->lock(m_grallocHal, *buf,
                           GRALLOC_USAGE_SW_WRITE_OFTEN,
                           0, 0, m_recordWidth, m_recordHeight, virtAddr) != 0) {
            ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
            break;
        }

        index = findBufferIndex(*buf, record_buffers, num_record_buffers);
        if (index == -1) {
            ALOGE("ERROR: %s: Could not find matching buffer index", __func__);
            return 0;
        }

        record_dequeue_status[index] = DEQUEUED;

        v4l2_buf.m.planes = planes;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_USERPTR;
        v4l2_buf.length = 1;
        v4l2_buf.flags = 0;
        v4l2_buf.index = index;
        v4l2_buf.m.planes[0].m.userptr = (unsigned long)(virtAddr[0]);
        v4l2_buf.m.planes[0].length = (m_recordWidth * m_recordHeight * 3) >> 1;

        if (exynos_v4l2_qbuf(m_rec_fd, &v4l2_buf) < 0) {
            ALOGE("ERR(%s):record exynos_v4l2_qbuf() fail", __func__);
            break;
        }
    }

    record_thread_running = false;

    ALOGV("DEBUG: %s: while loop ended", __func__);

    /* Stream OFF */
    exynos_v4l2_streamoff(m_rec_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    /* Close record fd */
    exynos_v4l2_close(m_rec_fd);

    return 0;
}

void *capture_thread(void *arg)
{
    ALOGV("DEBUG: %s called", __func__);
    int ret;
    int jpeg_fd;
    int m_cap_fd;
    struct v4l2_buffer v4l2_buf;
    struct v4l2_format v4l2_fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_plane planes[1];
    camera2_jpeg_blob *jpegBlob = NULL;
    void *captureBuffer = NULL;

    buffer_handle_t *buf = NULL;
    void *virtAddr[3];

    memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
    memset(&v4l2_fmt, 0, sizeof(struct v4l2_format));
    memset(&req, 0, sizeof(struct v4l2_requestbuffers));
    memset(&planes, 0, sizeof(struct v4l2_plane));

    capture_thread_running = true;

    while (capture_thread_running) {

        ALOGV("DEBUG: %s: Waiting for notification to start capture\n", __func__);
        sem_wait(&send_capture_frame_sem);
        ALOGV("DEBUG: %s: Notification to start capture received\n", __func__);

        /* Exit while here if sem_post was signalled from release stream */
        if (!capture_thread_running)
            break;

        if (record_running) {
            ALOGV("DEBUG: %s: Snapshot while recording received\n", __func__);

            /* Wait for record thread to copy buffer before proceding */
            sem_wait(&buffer_copied);
            ALOGV("DEBUG: %s: Snapshot buffer copied\n", __func__);

            /* Get JPEG using JPEG IP now */
            jpeg_fd = jpeghal_enc_init();

            if (jpeg_fd <= 0) {
                if (jpeg_fd < 0) {
                    jpeg_fd = 0;
                    ALOGE("ERR(%s):Cannot open a jpeg device file", __func__);
                    return 0;
                }
                ALOGE("ERR(%s):JPEG device was closed", __func__);
                return 0;
            }

            struct jpeg_config    enc_config;

            enc_config.mode = JPEG_ENCODE;
            enc_config.enc_qual = QUALITY_LEVEL_2;
            enc_config.width = m_previewWidth;
            enc_config.height = m_previewHeight;
            enc_config.pix.enc_fmt.in_fmt = V4L2_PIX_FMT_NV12;
            enc_config.pix.enc_fmt.out_fmt = V4L2_PIX_FMT_JPEG_420;
            enc_config.num_planes = 1;

            jpeghal_enc_setconfig(jpeg_fd, &enc_config);

            struct jpeg_buf    m_jpeg_inbuf;
            m_jpeg_inbuf.memory = V4L2_MEMORY_USERPTR;
            m_jpeg_inbuf.num_planes = 1;
            m_jpeg_inbuf.start[0] = snapshot_buffer;
            m_jpeg_inbuf.length[0] = ((m_previewWidth * m_previewHeight) * 2);

            if (jpeghal_set_inbuf(jpeg_fd, &m_jpeg_inbuf) < 0) {
                ALOGE("ERR(%s):Fail to JPEG input buffer!!", __func__);
                return 0;
            }

            ret = m_captureStreamOps->dequeue_buffer(m_captureStreamOps, &buf);
            if (ret != NO_ERROR || buf == NULL) {
                ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
                return 0;
            }

            if (m_grallocHal->lock(m_grallocHal, *buf,
                               GRALLOC_USAGE_SW_WRITE_OFTEN,
                               0, 0, m_captureWidth, m_captureHeight, virtAddr) != 0) {
                ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
                return 0;
            }

            struct jpeg_buf    m_jpeg_outbuf;
            m_jpeg_outbuf.memory = V4L2_MEMORY_USERPTR;
            m_jpeg_outbuf.num_planes = 1;
            m_jpeg_outbuf.start[0] = (char *)virtAddr[0] + 2;
            m_jpeg_outbuf.length[0] = (m_captureWidth * m_captureHeight);

            if (jpeghal_set_outbuf(jpeg_fd, &m_jpeg_outbuf) < 0) {
                ALOGE("ERR(%s):Fail to JPEG output buffer!!", __func__);
                return 0;
            }

            if (jpeghal_enc_exe(jpeg_fd, &m_jpeg_inbuf, &m_jpeg_outbuf) < 0) {
                ALOGE("ERR(%s):encode failed", __func__);
                return 0;
            }

            if (jpeg_fd > 0) {
                if (jpeghal_deinit(jpeg_fd, &m_jpeg_inbuf, &m_jpeg_outbuf) < 0)
                    ALOGE("ERR(%s):Fail on api_jpeg_encode_deinit", __func__);
                jpeg_fd = 0;
            }

            jpegBlob = (camera2_jpeg_blob *) (virtAddr[0] + (JPEG_MAX_SIZE - sizeof(camera2_jpeg_blob)));

            jpegBlob->jpeg_size = m_jpeg_outbuf.length[0] + 2;
            jpegBlob->jpeg_blob_id = CAMERA2_JPEG_BLOB_ID;

            ret = m_captureStreamOps->enqueue_buffer(m_captureStreamOps, systemTime(), buf);
            if (ret != NO_ERROR) {
                ALOGE("ERROR: %s: Unable to enqueue buffer", __func__);
            }

            sem_post(&capture_frame_sent_sem);
            ALOGV("DEBUG: %s: Video snapshot frame sent\n", __func__);

            if (snapshot_buffer != NULL) {
                free(snapshot_buffer);
                snapshot_buffer = NULL;
            }

        } else {

            /* Open capture node */
            m_cap_fd = exynos_v4l2_open(CAPTURE_NODE, O_RDWR, 0);
            if (m_cap_fd < 0) {
                ALOGE("ERR(%s):Cannot open %s (error : %s)", __func__, CAPTURE_NODE, strerror(errno));
            } else {
                if (exynos_v4l2_querycap(m_cap_fd, V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {

                    /* S_INPUT */
                    exynos_v4l2_s_input(m_cap_fd, m_cameraId);
                } else {
                    ALOGE("ERROR: %s: %s node does not support capture", __func__, CAPTURE_NODE);
                }
            }

            v4l2_fmt.fmt.pix_mp.width = m_captureWidth;
            v4l2_fmt.fmt.pix_mp.height = m_captureHeight;
            v4l2_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV;

            /* Now call S_FMT for Capture */
            v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v4l2_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
            ret = exynos_v4l2_s_fmt(m_cap_fd, &v4l2_fmt);
            if (ret < 0)
                ALOGE("%s: exynos_v4l2_s_fmt fail for Capture (%d)",__func__, ret);

            /* Call REQBUF */
            req.count = 1;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            req.memory = V4L2_MEMORY_USERPTR;
            ret = exynos_v4l2_reqbufs(m_cap_fd, &req);
            if (ret < 0)
                ALOGE("%s: VIDIOC_REQBUFS (fd:%d) failed (%d)",__func__,m_cap_fd, ret);

            captureBuffer = (char *) malloc((m_captureWidth * m_captureHeight) * 2 + (m_captureWidth * m_captureHeight));
            if (captureBuffer == NULL) {
                ALOGE("ERROR: %s: Capture buffer allocation failed", __func__);
                break;
            }

            v4l2_buf.m.planes = planes;
            v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v4l2_buf.memory = V4L2_MEMORY_USERPTR;
            v4l2_buf.length = 1;
            v4l2_buf.flags = 0;
            v4l2_buf.index = 0;
            v4l2_buf.m.planes[0].m.userptr = (unsigned long)(captureBuffer);
            v4l2_buf.m.planes[0].length = (m_captureWidth * m_captureHeight ) * 2;

            if (exynos_v4l2_qbuf(m_cap_fd, &v4l2_buf) < 0) {
                ALOGE("ERR(%s):capture exynos_v4l2_qbuf() fail", __func__);
                return 0;
            }

            /* Stream ON */
            exynos_v4l2_streamon(m_cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

            /* Dequeue buffer from driver */
            exynos_v4l2_dqbuf(m_cap_fd, &v4l2_buf);

            /* Get JPEG using JPEG IP now */
            jpeg_fd = jpeghal_enc_init();

            if (jpeg_fd <= 0) {
                if (jpeg_fd < 0) {
                    jpeg_fd = 0;
                    ALOGE("ERR(%s):Cannot open a jpeg device file", __func__);
                    return 0;
                }
                ALOGE("ERR(%s):JPEG device was closed", __func__);
                return 0;
            }

            struct jpeg_config    enc_config;

            /*m_captureWidth = m_previewWidth;
            m_captureHeight = m_previewHeight;*/

            enc_config.mode = JPEG_ENCODE;
            enc_config.enc_qual = QUALITY_LEVEL_2;
            enc_config.width = m_captureWidth;
            enc_config.height = m_captureHeight;
            enc_config.pix.enc_fmt.in_fmt = V4L2_PIX_FMT_YUYV;
            enc_config.pix.enc_fmt.out_fmt = V4L2_PIX_FMT_JPEG_422;
            enc_config.num_planes = 1;

            jpeghal_enc_setconfig(jpeg_fd, &enc_config);

            struct jpeg_buf    m_jpeg_inbuf;
            m_jpeg_inbuf.memory = V4L2_MEMORY_USERPTR;
            m_jpeg_inbuf.num_planes = 1;
            m_jpeg_inbuf.start[0] = captureBuffer;
            m_jpeg_inbuf.length[0] = ((m_captureWidth * m_captureHeight) * 2) + (m_captureWidth * m_captureHeight);

            if (jpeghal_set_inbuf(jpeg_fd, &m_jpeg_inbuf) < 0) {
                ALOGE("ERR(%s):Fail to JPEG input buffer!!", __func__);
                return 0;
            }

            ret = m_captureStreamOps->dequeue_buffer(m_captureStreamOps, &buf);
            if (ret != NO_ERROR || buf == NULL) {
                ALOGE("ERROR: %s: Unable to dequeue buffer", __func__);
                return 0;
            }

            if (m_grallocHal->lock(m_grallocHal, *buf,
                               GRALLOC_USAGE_SW_WRITE_OFTEN,
                               0, 0, m_captureWidth, m_captureHeight, virtAddr) != 0) {
                ALOGE("ERR(%s):could not obtain gralloc buffer", __func__);
                return 0;
            }

            struct jpeg_buf    m_jpeg_outbuf;
            m_jpeg_outbuf.memory = V4L2_MEMORY_USERPTR;
            m_jpeg_outbuf.num_planes = 1;
            m_jpeg_outbuf.start[0] = (char *)virtAddr[0] + 2;
            m_jpeg_outbuf.length[0] = (m_captureWidth * m_captureHeight) << 1;

            if (jpeghal_set_outbuf(jpeg_fd, &m_jpeg_outbuf) < 0) {
                ALOGE("ERR(%s):Fail to JPEG output buffer!!", __func__);
                return 0;
            }

            if (jpeghal_enc_exe(jpeg_fd, &m_jpeg_inbuf, &m_jpeg_outbuf) < 0) {
                ALOGE("ERR(%s):encode failed", __func__);
                return 0;
            }

            if (jpeg_fd > 0) {
                if (jpeghal_deinit(jpeg_fd, &m_jpeg_inbuf, &m_jpeg_outbuf) < 0)
                    ALOGE("ERR(%s):Fail on api_jpeg_encode_deinit", __func__);
                jpeg_fd = 0;
            }

            jpegBlob = (camera2_jpeg_blob *) (virtAddr[0] + (JPEG_MAX_SIZE - sizeof(camera2_jpeg_blob)));

            jpegBlob->jpeg_size = m_jpeg_outbuf.length[0] + 2;
            jpegBlob->jpeg_blob_id = CAMERA2_JPEG_BLOB_ID;

            ret = m_captureStreamOps->enqueue_buffer(m_captureStreamOps, systemTime(), buf);
            if (ret != NO_ERROR) {
                ALOGE("ERROR: %s: Unable to enqueue buffer", __func__);
            }

            sem_post(&capture_frame_sent_sem);
            ALOGV("DEBUG: %s: Capture frame sent\n", __func__);

            /* Stream OFF */
            exynos_v4l2_streamoff(m_cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

            /* Close capture FIMC, as this might be used by record_thread if
               recording is requested */
            exynos_v4l2_close(m_cap_fd);

            if (captureBuffer != NULL) {
                free(captureBuffer);
                captureBuffer = NULL;
            }
        }
    }

    capture_thread_running = false;

    return 0;
}

ExynosCameraHWInterface2::ExynosCameraHWInterface2(int cameraId, camera2_device_t *dev):
    request_manager_id(0)
{
    ALOGV("DEBUG(%s):", __func__);
    int ret;
    int fd;

    m_cameraId = cameraId;

    if (!m_grallocHal) {
        ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&m_grallocHal);
        if (ret)
            ALOGE("ERR(%s):Fail on loading gralloc HAL", __func__);
    }

    sem_init(&request_manager_sem, 0, 0);
    sem_init(&preview_start_sem, 0, 0);
    sem_init(&record_start_sem, 0, 0);
    sem_init(&send_preview_frame_sem, 0, 0);
    sem_init(&preview_frame_sent_sem, 0, 0);
    sem_init(&send_capture_frame_sem, 0, 0);
    sem_init(&capture_frame_sent_sem, 0, 0);
    sem_init(&buffer_copied, 0, 0);
    request_manager_exit = false;
    preview_thread_created = false;
    preview_thread_running = false;
    record_thread_running = false;
    capture_thread_running = false;
    request_in_progress = 0;
    record_running = false;
    do_not_dequeue = false;
    exit_record = false;
    copy_buffer = false;
    snapshot_buffer = NULL;

    /* Open preview node */
    fd = exynos_v4l2_open(PREVIEW_NODE, O_RDWR, 0);
    if (fd < 0) {
        ALOGE("ERR(%s):Cannot open %s (error : %s)", __func__, PREVIEW_NODE, strerror(errno));
    } else {
        if (exynos_v4l2_querycap(fd, V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {

            exynos_v4l2_enuminput(fd, m_cameraId, sensor_name);

            if (!strncmp(sensor_name, "ISP Camera", 10) || !strncmp(sensor_name, "S5K6A3", 10))
                mUsesInternalISP = true;
            else
                mUsesInternalISP = false;
        }
    }

    /* close preview node */
    exynos_v4l2_close(fd);

    ALOGV("DEBUG: %s: Uses internal ISP: %s", __func__, (mUsesInternalISP == true ? "true" : "false"));

    /* Create Request Manager thread and wait for notification from Camera Service */
    ret = pthread_create(&request_manager_id, NULL, request_manager, NULL);
    if (ret != 0) {
        ALOGE("ERROR: Request Manager thread creation failed");
    }
}

ExynosCameraHWInterface2::~ExynosCameraHWInterface2()
{
    ALOGV("DEBUG(%s):", __func__);
    request_manager_exit = true;
    /* Post all semaphores on which request manager might wait
       to unblock it */
    sem_post(&request_manager_sem);
    sem_post(&preview_frame_sent_sem);
    sem_post(&capture_frame_sent_sem);
    sem_post(&record_frame_sent_sem);
    ALOGV("DEBUG: %s: Waiting for request manager to exit", __func__);
    pthread_join(request_manager_id, NULL);
    ALOGV("DEBUG: %s: Request manager thread exited", __func__);
    /* Destroy all the semaphores */
    sem_destroy(&request_manager_sem);
    sem_destroy(&preview_start_sem);
    sem_destroy(&record_start_sem);
    sem_destroy(&send_preview_frame_sem);
    sem_destroy(&preview_frame_sent_sem);
    sem_destroy(&send_capture_frame_sem);
    sem_destroy(&capture_frame_sent_sem);
    sem_destroy(&buffer_copied);
}

int ExynosCameraHWInterface2::getCameraId() const
{
    return m_cameraId;
}


int ExynosCameraHWInterface2::setRequestQueueSrcOps(const camera2_request_queue_src_ops_t *request_src_ops)
{
    ALOGV("DEBUG(%s):", __func__);
    if ((NULL != request_src_ops) && (NULL != request_src_ops->dequeue_request)
            && (NULL != request_src_ops->free_request) && (NULL != request_src_ops->request_count)) {
        m_requestQueueOps = (camera2_request_queue_src_ops_t*)request_src_ops;
        return 0;
    }
    else {
        ALOGE("DEBUG(%s):setRequestQueueSrcOps : NULL arguments", __func__);
        return 1;
    }
}

int ExynosCameraHWInterface2::notifyRequestQueueNotEmpty()
{
    ALOGV("DEBUG(%s):", __func__);
    /* Notify Request manager thread */
    sem_post(&request_manager_sem);
    return 0;
}

int ExynosCameraHWInterface2::setFrameQueueDstOps(const camera2_frame_queue_dst_ops_t *frame_dst_ops)
{
    ALOGV("DEBUG(%s):", __func__);
    if ((NULL != frame_dst_ops) && (NULL != frame_dst_ops->dequeue_frame)
            && (NULL != frame_dst_ops->cancel_frame) && (NULL !=frame_dst_ops->enqueue_frame)) {
        m_frameQueueOps = (camera2_frame_queue_dst_ops_t *)frame_dst_ops;
        return 0;
    }
    else {
        ALOGE("DEBUG(%s):setFrameQueueDstOps : NULL arguments", __func__);
        return 1;
    }
}

int ExynosCameraHWInterface2::getInProgressCount()
{
    ALOGV("DEBUG(%s): In progress request: %lu", __func__, request_in_progress);
    return request_in_progress;
}

int ExynosCameraHWInterface2::flushCapturesInProgress()
{
    ALOGV("DEBUG(%s):", __func__);
    return 0;
}

int ExynosCameraHWInterface2::constructDefaultRequest(int request_template, camera_metadata_t **request)
{
    ALOGV("DEBUG(%s): making template (%d) ", __func__, request_template);

    if (request == NULL) return BAD_VALUE;
    if (request_template < 0 || request_template >= CAMERA2_TEMPLATE_COUNT) {
        return BAD_VALUE;
    }
    status_t res;
    /* Pass 1, calculate size and allocate */
    res = g_camera2->constructDefaultRequest(request_template,
            request,
            true);
    if (res != OK) {
        return res;
    }
    /* Pass 2, build request */
    res = g_camera2->constructDefaultRequest(request_template,
            request,
            false);
    if (res != OK) {
        ALOGE("Unable to populate new request for template %d",
                request_template);
    }

    return res;
}

int ExynosCameraHWInterface2::allocateStream(uint32_t width, uint32_t height, int format, const camera2_stream_ops_t *stream_ops,
                                    uint32_t *stream_id, uint32_t *format_actual, uint32_t *usage, uint32_t *max_buffers)
{
    ALOGV("DEBUG(%s): allocate with width(%d) height(%d) format(%x)", __func__,  width, height, format);
    int ret = 0;

    switch (format) {
        case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
            if (!preview_thread_created) {
                ALOGV("Received allocate_stream for Preview");
                m_previewStreamOps = stream_ops;
                m_previewWidth = width;
                m_previewHeight = height;
                *stream_id = STREAM_ID_PREVIEW;
                *format_actual = HAL_PIXEL_FORMAT_YCbCr_420_SP;
                *usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
                *max_buffers = MAX_BUFFERS;
                ret = pthread_create(&preview_thread_id, NULL, preview_thread, NULL);
                if (ret != 0)
                    ALOGE("ERROR: Preview thread creation failed");
                else
                    preview_thread_created = true;
            } else {
                ALOGV("Received allocate_stream for Record");
                m_recordStreamOps = stream_ops;
                m_recordWidth = width;
                m_recordHeight = height;
                *stream_id = STREAM_ID_RECORD;
                *format_actual = HAL_PIXEL_FORMAT_YCbCr_420_SP;
                *usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
                *max_buffers = MAX_BUFFERS;
                ret = pthread_create(&record_thread_id, NULL, record_thread, NULL);
                if (ret != 0)
                    ALOGE("ERROR: Record thread creation failed");
            }
            break;
        case CAMERA2_HAL_PIXEL_FORMAT_ZSL:
            ALOGW("Received allocate_stream for ZSL which is not supported yet");
            m_zslStreamOps = stream_ops;
            *stream_id = STREAM_ID_ZSL;
            *format_actual = HAL_PIXEL_FORMAT_YCbCr_420_SP;
            *usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
            *max_buffers = 4;
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            ALOGV("Received allocate_stream for JPEG Capture");
            m_captureStreamOps = stream_ops;
            m_captureWidth = width;
            m_captureHeight = height;
            *stream_id = STREAM_ID_JPEG;
            *format_actual = HAL_PIXEL_FORMAT_BLOB;
            *usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
            *max_buffers = MAX_CAPTURE_BUFFERS;

            ret = pthread_create(&capture_thread_id, NULL, capture_thread, NULL);
            if (ret != 0) {
                    ALOGE("ERROR: Capture thread creation failed");
            }
            break;
        default:
            ALOGV("\n\nReceived allocate_stream for UNKNOWN\n\n");
    }

    return 0;
}

int ExynosCameraHWInterface2::registerStreamBuffers(uint32_t stream_id, int num_buffers, buffer_handle_t *buffers)
{
    int i;

    if (stream_id == STREAM_ID_PREVIEW) {

        num_preview_buffers = num_buffers;

        for (i = 0; i < num_buffers; i++) {
            ALOGV("DEBUG: Registering preview buffer 0x%x", buffers[i]);
            preview_buffers[i] = buffers[i];
        }

        /* Notify preview thread that buffers have been registered and that it may
           start preview */
        sem_post(&preview_start_sem);

    } else if (stream_id == STREAM_ID_RECORD) {

        num_record_buffers = num_buffers;

        for (i = 0; i < num_buffers; i++) {
            ALOGV("DEBUG: Registering recording buffer 0x%x", buffers[i]);
            record_buffers[i] = buffers[i];
            record_dequeue_status[i] = ENQUEUED;
        }

        /* Notify record thread that buffers have been registered and that it may
           start recording */
        sem_post(&record_start_sem);
    }

    return 0;
}

int ExynosCameraHWInterface2::releaseStream(uint32_t stream_id)
{
    ALOGV("DEBUG(%s):", __func__);
    if (stream_id == STREAM_ID_PREVIEW) {
        ALOGV("DEBUG: %s: Release preview stream", __func__);
        preview_thread_running = false;
        /* Post this semaphore to unblock preview thread */
        sem_post(&send_preview_frame_sem);
        ALOGV("DEBUG: %s: Waiting for preview thread to exit", __func__);
        pthread_join(preview_thread_id, NULL);
        preview_thread_created = false;
        ALOGV("DEBUG: %s: Preview thread exited", __func__);
    } else if (stream_id == STREAM_ID_JPEG) {
        capture_thread_running = false;
        /* Post this semaphore to unblock capture thread if it was waiting */
        sem_post(&send_capture_frame_sem);
        ALOGV("DEBUG: %s: Waiting for capture thread to exit", __func__);
        pthread_join(capture_thread_id, NULL);
        ALOGV("DEBUG: %s: Capture thread exited", __func__);
    } else if (stream_id == STREAM_ID_RECORD) {
        ALOGV("DEBUG: %s: Release record stream", __func__);
        record_thread_running = false;
        exit_record = true;
        do_not_dequeue = false;
        /* Post all semaphores to unblock record thread */
        sem_post(&send_record_frame_sem);
        sem_post(&record_start_sem);
        ALOGV("DEBUG: %s: Waiting for record thread to exit", __func__);
        pthread_join(record_thread_id, NULL);
        ALOGV("DEBUG: %s: Record thread exited", __func__);
    }
    return 0;
}

int ExynosCameraHWInterface2::allocateReprocessStream(
    uint32_t width, uint32_t height, uint32_t format, const camera2_stream_in_ops_t *reprocess_stream_ops,
    uint32_t *stream_id, uint32_t *consumer_usage, uint32_t *max_buffers)
{
    ALOGV("DEBUG(%s):", __func__);
    return 0;
}

int ExynosCameraHWInterface2::releaseReprocessStream(uint32_t stream_id)
{
    ALOGV("DEBUG(%s):", __func__);
    return 0;
}

int ExynosCameraHWInterface2::triggerAction(uint32_t trigger_id, int ext1, int ext2)
{
    ALOGV("DEBUG(%s):", __func__);

    switch (trigger_id) {
    case CAMERA2_TRIGGER_AUTOFOCUS:
        ALOGV("DEBUG(%s):TRIGGER_AUTOFOCUS id(%d)", __FUNCTION__, ext1);
        break;
    case CAMERA2_TRIGGER_CANCEL_AUTOFOCUS:
        ALOGV("DEBUG(%s):CANCEL_AUTOFOCUS id(%d)", __FUNCTION__, ext1);
        break;
    case CAMERA2_TRIGGER_PRECAPTURE_METERING:
        ALOGV("DEBUG(%s):CAMERA2_TRIGGER_PRECAPTURE_METERING id(%d)", __FUNCTION__, ext1);
        break;
    default:
        ALOGV("DEBUG(%s):UNKNOWN id(%d)", __FUNCTION__, ext1);
        break;
    }

    return 0;
}

int ExynosCameraHWInterface2::setNotifyCallback(camera2_notify_callback notify_cb, void *user)
{
    ALOGV("DEBUG(%s):", __func__);
    m_notifyCb = notify_cb;
    m_callbackCookie = user;
    return 0;
}

int ExynosCameraHWInterface2::getMetadataVendorTagOps(vendor_tag_query_ops_t **ops)
{
    ALOGV("DEBUG(%s):", __func__);
    *ops = NULL;
    return 0;
}

int ExynosCameraHWInterface2::dump(int fd)
{
    ALOGV("DEBUG(%s):", __func__);
    return 0;
}


static camera2_device_t *g_cam2_device;

static int HAL2_camera_device_close(struct hw_device_t* device)
{
    ALOGV("DEBUG(%s):", __func__);
    if (device) {
        camera2_device_t *cam_device = (camera2_device_t *)device;
        delete static_cast<ExynosCameraHWInterface2 *>(cam_device->priv);
        free(cam_device);
        g_cam2_device = 0;
    }
    return 0;
}

static inline ExynosCameraHWInterface2 *obj(const struct camera2_device *dev)
{
    return reinterpret_cast<ExynosCameraHWInterface2 *>(dev->priv);
}

static int HAL2_device_set_request_queue_src_ops(const struct camera2_device *dev,
            const camera2_request_queue_src_ops_t *request_src_ops)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->setRequestQueueSrcOps(request_src_ops);
}

static int HAL2_device_notify_request_queue_not_empty(const struct camera2_device *dev)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->notifyRequestQueueNotEmpty();
}

static int HAL2_device_set_frame_queue_dst_ops(const struct camera2_device *dev,
            const camera2_frame_queue_dst_ops_t *frame_dst_ops)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->setFrameQueueDstOps(frame_dst_ops);
}

static int HAL2_device_get_in_progress_count(const struct camera2_device *dev)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->getInProgressCount();
}

static int HAL2_device_flush_captures_in_progress(const struct camera2_device *dev)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->flushCapturesInProgress();
}

static int HAL2_device_construct_default_request(const struct camera2_device *dev,
            int request_template, camera_metadata_t **request)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->constructDefaultRequest(request_template, request);
}

static int HAL2_device_allocate_stream(
            const struct camera2_device *dev,
            /* inputs */
            uint32_t width,
            uint32_t height,
            int      format,
            const camera2_stream_ops_t *stream_ops,
            /* outputs */
            uint32_t *stream_id,
            uint32_t *format_actual,
            uint32_t *usage,
            uint32_t *max_buffers)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->allocateStream(width, height, format, stream_ops,
                                    stream_id, format_actual, usage, max_buffers);
}


static int HAL2_device_register_stream_buffers(const struct camera2_device *dev,
            uint32_t stream_id,
            int num_buffers,
            buffer_handle_t *buffers)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->registerStreamBuffers(stream_id, num_buffers, buffers);
}

static int HAL2_device_release_stream(
        const struct camera2_device *dev,
            uint32_t stream_id)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->releaseStream(stream_id);
}

static int HAL2_device_allocate_reprocess_stream(
           const struct camera2_device *dev,
            uint32_t width,
            uint32_t height,
            uint32_t format,
            const camera2_stream_in_ops_t *reprocess_stream_ops,
            /* outputs */
            uint32_t *stream_id,
            uint32_t *consumer_usage,
            uint32_t *max_buffers)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->allocateReprocessStream(width, height, format, reprocess_stream_ops,
                                    stream_id, consumer_usage, max_buffers);
}

static int HAL2_device_release_reprocess_stream(
        const struct camera2_device *dev,
            uint32_t stream_id)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->releaseReprocessStream(stream_id);
}

static int HAL2_device_trigger_action(const struct camera2_device *dev,
           uint32_t trigger_id,
            int ext1,
            int ext2)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->triggerAction(trigger_id, ext1, ext2);
}

static int HAL2_device_set_notify_callback(const struct camera2_device *dev,
            camera2_notify_callback notify_cb,
            void *user)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->setNotifyCallback(notify_cb, user);
}

static int HAL2_device_get_metadata_vendor_tag_ops(const struct camera2_device*dev,
            vendor_tag_query_ops_t **ops)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->getMetadataVendorTagOps(ops);
}

static int HAL2_device_dump(const struct camera2_device *dev, int fd)
{
    ALOGV("DEBUG(%s):", __func__);
    return obj(dev)->dump(fd);
}

static int HAL2_getNumberOfCameras()
{
    ALOGV("DEBUG(%s):", __func__);

    int cam_fd;
    int index = 0;
    char camera_name[20];

    cam_fd = exynos_v4l2_open(PREVIEW_NODE, O_RDWR, 0);
    if (cam_fd < 0) {
        ALOGE("ERR(%s):Cannot open %s (error : %s)", __func__, PREVIEW_NODE, strerror(errno));
        return -1;
    }

    while (exynos_v4l2_enuminput(cam_fd, index, camera_name)) {
        ALOGI("Name of input channel[%d] is %s", index, camera_name);
        index++;
    }

    exynos_v4l2_close(cam_fd);

    ALOGV("DEBUG: %s: Number of cameras: %d", __func__, index);

    return index;
}

static int HAL2_getCameraInfo(int cameraId, struct camera_info *info)
{
    ALOGV("DEBUG(%s):", __func__);
    static camera_metadata_t * mCameraInfo = NULL;
    status_t res;

    if (cameraId == 0)
        info->facing = CAMERA_FACING_BACK;
    else if (cameraId == 1)
        info->facing = CAMERA_FACING_FRONT;

    if (!strncmp(sensor_name, "M5M0 Camera", 10))
        info->orientation = 270;
    else
        info->orientation = 90;

    info->device_version = HARDWARE_DEVICE_API_VERSION(2, 0);

    if (!g_camera2)
        g_camera2 = new ExynosCamera2(cameraId);

    if (mCameraInfo==NULL) {
        res = g_camera2->constructStaticInfo(&mCameraInfo, 0, true);
        if (res != OK) {
            ALOGE("%s: Unable to allocate static info: %s (%d)",
                    __func__, strerror(-res), res);
            return res;
        }
        res = g_camera2->constructStaticInfo(&mCameraInfo, 0, false);
        if (res != OK) {
            ALOGE("%s: Unable to fill in static info: %s (%d)",
                    __func__, strerror(-res), res);
            return res;
        }
    }
    info->static_camera_characteristics = mCameraInfo;
    return NO_ERROR;
}

static int HAL2_device_allocate_reprocess_stream_from_stream(
           const struct camera2_device *dev,
            uint32_t output_stream_id,
            const camera2_stream_in_ops_t *reprocess_stream_ops,
            /* outputs */
            uint32_t *stream_id)
{
    ALOGV("DEBUG(%s):", __FUNCTION__);
    return 0;
}


#define SET_METHOD(m) m : HAL2_device_##m

static camera2_device_ops_t camera2_device_ops = {
        SET_METHOD(set_request_queue_src_ops),
        SET_METHOD(notify_request_queue_not_empty),
        SET_METHOD(set_frame_queue_dst_ops),
        SET_METHOD(get_in_progress_count),
        SET_METHOD(flush_captures_in_progress),
        SET_METHOD(construct_default_request),
        SET_METHOD(allocate_stream),
        SET_METHOD(register_stream_buffers),
        SET_METHOD(release_stream),
        SET_METHOD(allocate_reprocess_stream),
        SET_METHOD(allocate_reprocess_stream_from_stream),
        SET_METHOD(release_reprocess_stream),
        SET_METHOD(trigger_action),
        SET_METHOD(set_notify_callback),
        SET_METHOD(get_metadata_vendor_tag_ops),
        SET_METHOD(dump),
};

#undef SET_METHOD



static int HAL2_camera_device_open(const struct hw_module_t* module,
                                  const char *id,
                                  struct hw_device_t** device)
{
    ALOGD(">>> This is Samsung's CameraHAL API Version 2 <<<");

    int cameraId = atoi(id);
    if (cameraId < 0 || cameraId >= HAL2_getNumberOfCameras()) {
        ALOGE("ERR(%s):Invalid camera ID %s", __func__, id);
        return -EINVAL;
    }

    if (g_cam2_device) {
        if (obj(g_cam2_device)->getCameraId() == cameraId) {
            ALOGV("DEBUG(%s):returning existing camera ID %s", __func__, id);
            goto done;
        } else {
            ALOGE("ERR(%s):Cannot open camera %d. camera %d is already running!",
                    __func__, cameraId, obj(g_cam2_device)->getCameraId());
            return -ENOSYS;
        }
    }

    g_cam2_device = (camera2_device_t *)malloc(sizeof(camera2_device_t));
    if (!g_cam2_device)
        return -ENOMEM;

    g_cam2_device->common.tag     = HARDWARE_DEVICE_TAG;
    g_cam2_device->common.version = CAMERA_DEVICE_API_VERSION_2_0;
    g_cam2_device->common.module  = const_cast<hw_module_t *>(module);
    g_cam2_device->common.close   = HAL2_camera_device_close;

    g_cam2_device->ops = &camera2_device_ops;

    ALOGV("DEBUG(%s):open camera2 %s", __func__, id);

    g_cam2_device->priv = new ExynosCameraHWInterface2(cameraId, g_cam2_device);

done:
    *device = (hw_device_t *)g_cam2_device;
    ALOGV("DEBUG(%s):opened camera2 %s (%p)", __func__, id, *device);

    return 0;
}


static hw_module_methods_t camera_module_methods = {
            open : HAL2_camera_device_open
};

extern "C" {
    struct camera_module HAL_MODULE_INFO_SYM = {
      common : {
          tag                : HARDWARE_MODULE_TAG,
          module_api_version : CAMERA_MODULE_API_VERSION_2_0,
          hal_api_version    : HARDWARE_HAL_API_VERSION,
          id                 : CAMERA_HARDWARE_MODULE_ID,
          name               : "Exynos Camera HAL2",
          author             : "Samsung Corporation",
          methods            : &camera_module_methods,
          dso:                NULL,
          reserved:           {0},
      },
      get_number_of_cameras : HAL2_getNumberOfCameras,
      get_camera_info       : HAL2_getCameraInfo
    };
}

}; /* namespace android */
