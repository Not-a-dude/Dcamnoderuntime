#include "session.h"
#include <android/data_space.h>
#include <android/log.h>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <condition_variable>
#include <algorithm>
#include <string>

namespace {

void waitAndCloseFence(int fd) {
    if (fd < 0) return;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, 1000) < 0 && errno == EINTR) {}
    close(fd);
}

jstring newString(JNIEnv* env, DcamStringView value) {
    if (!value.data || value.size == 0) return env->NewStringUTF("");
    const std::string copy(value.data, value.size);
    return env->NewStringUTF(copy.c_str());
}

bool clearJavaException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

ARect centeredPreviewCrop(
        uint32_t source_width,
        uint32_t source_height,
        int32_t destination_width,
        int32_t destination_height,
        int32_t transform) {
    int32_t crop_width = static_cast<int32_t>(source_width);
    int32_t crop_height = static_cast<int32_t>(source_height);
    const bool swaps_axes = (transform & ANATIVEWINDOW_TRANSFORM_ROTATE_90) != 0;
    const int64_t display_width = swaps_axes ? crop_height : crop_width;
    const int64_t display_height = swaps_axes ? crop_width : crop_height;

    if (display_width * destination_height >
        static_cast<int64_t>(destination_width) * display_height) {
        const int32_t cropped_display_width = std::max<int32_t>(
                1, static_cast<int32_t>(display_height * destination_width / destination_height));
        if (swaps_axes) crop_height = cropped_display_width;
        else crop_width = cropped_display_width;
    } else if (display_width * destination_height <
               static_cast<int64_t>(destination_width) * display_height) {
        const int32_t cropped_display_height = std::max<int32_t>(
                1, static_cast<int32_t>(display_width * destination_height / destination_width));
        if (swaps_axes) crop_width = cropped_display_height;
        else crop_height = cropped_display_height;
    }

    const int32_t left = (static_cast<int32_t>(source_width) - crop_width) / 2;
    const int32_t top = (static_cast<int32_t>(source_height) - crop_height) / 2;
    return {left, top, left + crop_width, top + crop_height};
}

struct PresentCompletion {
    ASurfaceControl* control;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int release_fence = -1;
};

void onPresentComplete(void* ctx, ASurfaceTransactionStats* stats) {
    auto* comp = (PresentCompletion*)ctx;
    comp->release_fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, comp->control);
    {
        std::lock_guard<std::mutex> lock(comp->mutex);
        comp->done = true;
    }
    comp->cv.notify_one();
}

} // namespace

void releasePresentedPreview(NativeSession* session) {
    if (!session) return;
    std::lock_guard preview_lock(session->preview_mutex);
    AHardwareBuffer* buffer = session->presented_buffer;
    const uint64_t frame_id = session->presented_frame_id;
    session->presented_buffer = nullptr;
    session->presented_frame_id = 0;
    if (!buffer) return;
    if (session->preview_control) {
        ASurfaceTransaction* tx = ASurfaceTransaction_create();
        if (tx) {
            PresentCompletion completion{};
            completion.control = session->preview_control;
            ASurfaceTransaction_reparent(tx, session->preview_control, nullptr);
            ASurfaceTransaction_setOnComplete(tx, &completion, onPresentComplete);
            ASurfaceTransaction_apply(tx);
            ASurfaceTransaction_delete(tx);
            std::unique_lock lock(completion.mutex);
            completion.cv.wait(lock, [&completion] { return completion.done; });
            waitAndCloseFence(completion.release_fence);
        }
    }
    AHardwareBuffer_release(buffer);
    releaseExternalFrame(session, frame_id);
}

extern "C" {

DcamStatus hostRetainBuffer(void* ctx, AHardwareBuffer* buf, DcamBufferHandle* out) {
    if (!ctx || !buf || !out) return DCAM_ERROR_INVALID_ARGUMENT;
    auto* s = (NativeSession*)ctx;
    AHardwareBuffer_acquire(buf);
    *out = s->next_buffer_handle.fetch_add(1);
    const uint64_t external_frame_id = retainExternalFrameForBuffer(s, buf);
    std::lock_guard<std::mutex> lock(s->buffer_mutex);
    s->buffers[*out] = {buf, external_frame_id};
    return DCAM_OK;
}

DcamStatus hostAllocateBuffer(void* ctx, const DcamBufferDesc* desc,
                              DcamBufferHandle* out, AHardwareBuffer** out_buffer) {
    if (!ctx || !desc || desc->struct_size < sizeof(DcamBufferDesc) || !out || !out_buffer ||
        desc->width == 0 || desc->height == 0 || desc->layers == 0) {
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    AHardwareBuffer_Desc native_desc{};
    native_desc.width = desc->width;
    native_desc.height = desc->height;
    native_desc.layers = desc->layers;
    native_desc.format = desc->format;
    native_desc.usage = desc->usage;
    AHardwareBuffer* buffer = nullptr;
    if (AHardwareBuffer_allocate(&native_desc, &buffer) != 0 || !buffer) {
        return DCAM_ERROR_OUT_OF_MEMORY;
    }
    auto* s = (NativeSession*)ctx;
    *out = s->next_buffer_handle.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(s->buffer_mutex);
        s->buffers[*out] = {buffer, 0};
    }
    *out_buffer = buffer;
    return DCAM_OK;
}

void hostReleaseBuffer(void* ctx, DcamBufferHandle h, int32_t fence) {
    auto* s = (NativeSession*)ctx;
    if (!s) {
        waitAndCloseFence(fence);
        return;
    }
    AHardwareBuffer* buf = nullptr;
    uint64_t external_frame_id = 0;
    {
        std::lock_guard<std::mutex> lock(s->buffer_mutex);
        auto it = s->buffers.find(h);
        if (it != s->buffers.end()) {
            buf = it->second.buffer;
            external_frame_id = it->second.external_frame_id;
            s->buffers.erase(it);
        }
    }
    waitAndCloseFence(fence);
    if (buf) AHardwareBuffer_release(buf);
    releaseExternalFrame(s, external_frame_id);
}

DcamStatus hostPresentBuffer(void* ctx, DcamSurfaceHandle surf, AHardwareBuffer* buf,
                             const DcamBufferDesc* desc, int32_t fence) {
    auto* s = static_cast<NativeSession*>(ctx);
    if (!s || !buf || !desc || desc->struct_size < sizeof(DcamBufferDesc) ||
        surf != DCAM_PREVIEW_SURFACE) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard preview_lock(s->preview_mutex);
    if (!s->preview_control || !s->preview_window) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_BAD_STATE;
    }
    AHardwareBuffer_Desc buffer_desc{};
    AHardwareBuffer_describe(buf, &buffer_desc);
    if (desc->width != buffer_desc.width || desc->height != buffer_desc.height ||
        desc->layers != buffer_desc.layers || desc->format != buffer_desc.format) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    if (buffer_desc.format == AHARDWAREBUFFER_FORMAT_BLOB ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_D16_UNORM ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_D24_UNORM ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_D32_FLOAT ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT ||
        buffer_desc.format == AHARDWAREBUFFER_FORMAT_S8_UINT) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_UNSUPPORTED;
    }
    const int32_t destination_width = ANativeWindow_getWidth(s->preview_window);
    const int32_t destination_height = ANativeWindow_getHeight(s->preview_window);
    if (buffer_desc.width == 0 || buffer_desc.height == 0 ||
        destination_width <= 0 || destination_height <= 0) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_BAD_STATE;
    }
    const ARect source = centeredPreviewCrop(
            buffer_desc.width,
            buffer_desc.height,
            destination_width,
            destination_height,
            s->preview_transform);
    const ARect destination{0, 0, destination_width, destination_height};
    ASurfaceTransaction* tx = ASurfaceTransaction_create();
    if (!tx) {
        waitAndCloseFence(fence);
        return DCAM_ERROR_OUT_OF_MEMORY;
    }
    AHardwareBuffer_acquire(buf);
    const uint64_t frame_id = retainExternalFrameForBuffer(s, buf);
    ASurfaceTransaction_setGeometry(
            tx, s->preview_control, source, destination, s->preview_transform);
    ASurfaceTransaction_setBuffer(tx, s->preview_control, buf, fence);
    int32_t data_space = desc->data_space;
    if (data_space == ADATASPACE_UNKNOWN) {
        switch (buffer_desc.format) {
            case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
                data_space = ADATASPACE_JFIF;
                break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
            case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            case AHARDWAREBUFFER_FORMAT_R10G10B10A10_UNORM:
                data_space = ADATASPACE_SRGB;
                break;
            default:
                break;
        }
    }
    ASurfaceTransaction_setBufferDataSpace(
            tx, s->preview_control, static_cast<ADataSpace>(data_space));
    PresentCompletion completion{};
    completion.control = s->preview_control;
    ASurfaceTransaction_setOnComplete(tx, &completion, onPresentComplete);
    AHardwareBuffer* previous = s->presented_buffer;
    const uint64_t previous_frame_id = s->presented_frame_id;
    s->presented_buffer = buf;
    s->presented_frame_id = frame_id;
    ASurfaceTransaction_apply(tx);
    ASurfaceTransaction_delete(tx);
    std::unique_lock completion_lock(completion.mutex);
    completion.cv.wait(completion_lock, [&completion] { return completion.done; });
    waitAndCloseFence(completion.release_fence);
    if (previous) AHardwareBuffer_release(previous);
    releaseExternalFrame(s, previous_frame_id);
    if (s->processing_frame_id.load()) s->processing_frame_presented.store(true);
    return DCAM_OK;
}

DcamStatus hostSubmitRequestState(void* ctx, const DcamRequestStateTransaction* tx) {
    if (!ctx || !tx || tx->struct_size < sizeof(DcamRequestStateTransaction) ||
        (tx->target_count > 0 && !tx->target_stream_ids) ||
        (tx->entry_count > 0 && !tx->entries)) {
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    auto* s = (NativeSession*)ctx;
    bool attached; JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    jobject jtx = env->NewObject(s->transaction_class, s->transaction_ctor);
    env->SetLongField(jtx, s->tx_session_id, (jlong)s->session_id);
    env->SetIntField(jtx, s->tx_scope, (jint)tx->scope);
    env->SetIntField(jtx, s->tx_template_id, (jint)tx->template_id);
    env->SetLongField(jtx, s->tx_revision, (jlong)tx->revision);
    env->SetLongField(jtx, s->tx_base_revision, (jlong)tx->base_revision);
    env->SetIntField(jtx, s->tx_lifetime, (jint)tx->lifetime);

    if (tx->target_count > 0 && tx->target_stream_ids) {
        jintArray jtargets = env->NewIntArray(tx->target_count);
        env->SetIntArrayRegion(jtargets, 0, tx->target_count, (const jint*)tx->target_stream_ids);
        env->SetObjectField(jtx, s->tx_target_streams, jtargets);
        env->DeleteLocalRef(jtargets);
    }

    jobjectArray entries = env->NewObjectArray(tx->entry_count, s->request_entry_class, nullptr);
    for (uint32_t i = 0; i < tx->entry_count; ++i) {
        jobject e = env->NewObject(s->request_entry_class, s->request_entry_ctor);
        env->SetIntField(e, s->re_key_id, (jint)tx->entries[i].key_id);
        env->SetIntField(e, s->re_wire_type, (jint)tx->entries[i].wire_type);
        env->SetIntField(e, s->re_operation, (jint)tx->entries[i].operation);
        env->SetIntField(e, s->re_element_count, (jint)tx->entries[i].element_count);

        jstring jname = newString(env, tx->entries[i].name);
        env->SetObjectField(e, s->re_name, jname);
        env->DeleteLocalRef(jname);

        jbyteArray jbytes = env->NewByteArray(tx->entries[i].value.size);
        env->SetByteArrayRegion(jbytes, 0, tx->entries[i].value.size, (const jbyte*)tx->entries[i].value.data);
        env->SetObjectField(e, s->re_value, jbytes);
        env->DeleteLocalRef(jbytes);

        env->SetObjectArrayElement(entries, i, e);
        env->DeleteLocalRef(e);
    }
    env->SetObjectField(jtx, s->tx_entries, entries);
    jint res = env->CallIntMethod(s->callbacks, s->request_state_method, jtx);
    const bool failed = clearJavaException(env);
    env->DeleteLocalRef(entries); env->DeleteLocalRef(jtx);
    finishEnv(s, attached);
    return failed ? DCAM_ERROR_IO : (DcamStatus)res;
}

DcamStatus hostRequestSingleCapture(void* ctx, uint32_t src, uint64_t zsl) {
    auto* s = (NativeSession*)ctx;
    bool attached; JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    jint res = env->CallIntMethod(s->callbacks, s->single_capture_method, (jint)src, (jlong)zsl);
    const bool failed = clearJavaException(env);
    finishEnv(s, attached);
    return failed ? DCAM_ERROR_IO : (DcamStatus)res;
}

DcamStatus hostReleaseZslFrame(void* ctx, uint64_t fid, int32_t fence) {
    return releaseGraphZslFrame(static_cast<NativeSession*>(ctx), 0, fid, fence);
}

DcamStatus hostEmitFrame(void* ctx, DcamNodeHandle source_node,
                         uint32_t output_port_id, const DcamFrame* frame) {
    return emitGraphFrame(
            static_cast<NativeSession*>(ctx), source_node, output_port_id, frame);
}

DcamStatus hostReleaseZslFrameForNode(void* ctx, DcamNodeHandle source_node,
                                      uint64_t frame_id, int32_t fence) {
    return releaseGraphZslFrame(
            static_cast<NativeSession*>(ctx), source_node, frame_id, fence);
}

DcamStatus hostDuplicateOutputFd(void* ctx, uint64_t cid, int32_t* out) {
    auto* s = (NativeSession*)ctx;
    bool attached; JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    *out = env->CallIntMethod(s->callbacks, s->duplicate_output_method, (jlong)cid);
    const bool failed = clearJavaException(env);
    finishEnv(s, attached);
    if (failed) return DCAM_ERROR_IO;
    return *out >= 0 ? DCAM_OK : DCAM_ERROR_NOT_FOUND;
}

DcamStatus hostCompleteOutput(void* ctx, uint64_t cid, DcamStringView mime, uint64_t sz, DcamStatus st) {
    auto* s = (NativeSession*)ctx;
    bool attached; JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    jstring jmime = newString(env, mime);
    jint res = env->CallIntMethod(s->callbacks, s->complete_output_method, (jlong)cid, jmime, (jlong)sz, (jint)st);
    const bool failed = clearJavaException(env);
    env->DeleteLocalRef(jmime);
    finishEnv(s, attached);
    return failed ? DCAM_ERROR_IO : (DcamStatus)res;
}

DcamStatus hostOpenArtifact(void* ctx, uint64_t capture_id,
                            DcamStringView suggested_name,
                            DcamStringView mime_type,
                            uint64_t maximum_bytes,
                            uint64_t* out_artifact_id,
                            int32_t* out_fd) {
    if (!ctx || !out_artifact_id || !out_fd) return DCAM_ERROR_INVALID_ARGUMENT;
    auto* s = (NativeSession*)ctx;
    bool attached;
    JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    jstring jname = newString(env, suggested_name);
    jstring jmime = newString(env, mime_type);
    auto result = (jlongArray)env->CallObjectMethod(
            s->callbacks, s->open_artifact_method, (jlong)capture_id,
            jname, jmime, (jlong)maximum_bytes);
    const bool failed = clearJavaException(env);
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(jmime);
    DcamStatus status = DCAM_ERROR_IO;
    if (!failed && result && env->GetArrayLength(result) == 2) {
        jlong values[2]{};
        env->GetLongArrayRegion(result, 0, 2, values);
        if (values[0] > 0 && values[1] >= 0) {
            *out_artifact_id = (uint64_t)values[0];
            *out_fd = (int32_t)values[1];
            status = DCAM_OK;
        }
    }
    if (result) env->DeleteLocalRef(result);
    finishEnv(s, attached);
    return status;
}

DcamStatus hostCompleteArtifact(void* ctx, uint64_t artifact_id,
                                uint64_t size, DcamStatus status) {
    if (!ctx || artifact_id == 0) return DCAM_ERROR_INVALID_ARGUMENT;
    auto* s = (NativeSession*)ctx;
    bool attached;
    JNIEnv* env = getEnv(s, &attached);
    if (!env) return DCAM_ERROR_BAD_STATE;
    jint result = env->CallIntMethod(
            s->callbacks, s->complete_artifact_method,
            (jlong)artifact_id, (jlong)size, (jint)status);
    const bool failed = clearJavaException(env);
    finishEnv(s, attached);
    return failed ? DCAM_ERROR_IO : (DcamStatus)result;
}

void hostLog(void*, int32_t priority, DcamStringView tag, DcamStringView message) {
    const std::string safe_tag = tag.data ? std::string(tag.data, tag.size) : "DcamNode";
    const std::string safe_message = message.data ? std::string(message.data, message.size) : std::string();
    __android_log_write(priority, safe_tag.c_str(), safe_message.c_str());
}

void initHostApi(DcamHostApi* api, void* ctx) {
    *api = {};
    api->struct_size = sizeof(*api);
    api->abi_major = DCAM_NODE_ABI_MAJOR;
    api->abi_minor = DCAM_NODE_ABI_MINOR;
    api->context = ctx;
    api->log = hostLog;
    api->retain_buffer = hostRetainBuffer;
    api->release_buffer = hostReleaseBuffer;
    api->allocate_buffer = hostAllocateBuffer;
    api->present_buffer = hostPresentBuffer;
    api->submit_request_state = hostSubmitRequestState;
    api->request_single_capture = hostRequestSingleCapture;
    api->release_zsl_frame = hostReleaseZslFrame;
    api->duplicate_output_fd = hostDuplicateOutputFd;
    api->complete_output = hostCompleteOutput;
    api->open_artifact = hostOpenArtifact;
    api->complete_artifact = hostCompleteArtifact;
    api->emit_frame = hostEmitFrame;
    api->release_zsl_frame_for_node = hostReleaseZslFrameForNode;
}

} // extern "C"
