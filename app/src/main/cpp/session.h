#pragma once

#include "dcam_node_abi.h"
#include <jni.h>
#include <android/native_window.h>
#include <android/surface_control.h>
#include <android/hardware_buffer.h>
#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

struct NativePort {
    uint32_t id = 0;
    uint32_t type = 0;
    std::string name;
};

struct NativeNodeInstance {
    DcamNodeHandle handle = 0;
    std::string type_id;
    std::string instance_id;
    std::vector<NativePort> inputs;
    std::vector<NativePort> outputs;
};

struct NativeGraphEdge {
    size_t source_node = 0;
    uint32_t source_port = 0;
    size_t target_node = 0;
    uint32_t target_port = 0;
};

struct NativeZslHold {
    DcamNodeHandle owner = 0;
    uint64_t frame_id = 0;
    AHardwareBuffer* buffer = nullptr;
    uint64_t external_frame_id = 0;
};

struct NativeExternalFrame {
    uint64_t buffer_id = 0;
    uint32_t hold_count = 0;
    DcamStatus status = DCAM_OK;
    uint32_t flags = 0;
    bool processing = true;
};

struct NativeBufferHold {
    AHardwareBuffer* buffer = nullptr;
    uint64_t external_frame_id = 0;
};

struct NativeSession {
    JavaVM* vm = nullptr;
    uint64_t session_id = 0;
    void* library = nullptr;
    std::vector<void*> dependencies;
    DcamHostApi host_api{};
    DcamNodeApi node_api{};
    std::vector<NativeNodeInstance> nodes;
    std::vector<NativeGraphEdge> graph_edges;
    std::vector<size_t> graph_roots;
    std::string module_id;
    std::string module_version;
    std::string node_type_id;
    uint32_t maximum_zsl_depth = 0;

    jobject callbacks = nullptr;
    jmethodID release_frame_method = nullptr;
    jmethodID request_state_method = nullptr;
    jmethodID single_capture_method = nullptr;
    jmethodID duplicate_output_method = nullptr;
    jmethodID complete_output_method = nullptr;
    jmethodID open_artifact_method = nullptr;
    jmethodID complete_artifact_method = nullptr;

    jclass transaction_class = nullptr;
    jmethodID transaction_ctor = nullptr;
    jfieldID tx_session_id = nullptr;
    jfieldID tx_scope = nullptr;
    jfieldID tx_template_id = nullptr;
    jfieldID tx_revision = nullptr;
    jfieldID tx_base_revision = nullptr;
    jfieldID tx_target_streams = nullptr;
    jfieldID tx_entries = nullptr;
    jfieldID tx_lifetime = nullptr;

    jclass metadata_class = nullptr;
    jfieldID md_wire_version = nullptr;
    jfieldID md_entries = nullptr;

    jclass metadata_entry_class = nullptr;
    jfieldID me_key_id = nullptr;
    jfieldID me_wire_type = nullptr;
    jfieldID me_element_count = nullptr;
    jfieldID me_flags = nullptr;
    jfieldID me_name = nullptr;
    jfieldID me_value = nullptr;

    jclass request_entry_class = nullptr;
    jmethodID request_entry_ctor = nullptr;
    jfieldID re_key_id = nullptr;
    jfieldID re_wire_type = nullptr;
    jfieldID re_operation = nullptr;
    jfieldID re_element_count = nullptr;
    jfieldID re_name = nullptr;
    jfieldID re_value = nullptr;

    ANativeWindow* preview_window = nullptr;
    ASurfaceControl* preview_control = nullptr;
    int32_t preview_transform = ANATIVEWINDOW_TRANSFORM_IDENTITY;
    std::mutex preview_mutex;
    AHardwareBuffer* presented_buffer = nullptr;
    uint64_t presented_frame_id = 0;
    std::atomic<uint64_t> processing_frame_id{0};
    std::atomic<AHardwareBuffer*> processing_frame_buffer{nullptr};
    std::atomic<bool> processing_frame_presented{false};

    std::recursive_mutex graph_mutex;
    std::mutex zsl_mutex;
    std::vector<NativeZslHold> zsl_holds;
    std::mutex external_frame_mutex;
    std::unordered_map<uint64_t, NativeExternalFrame> external_frames;

    std::mutex buffer_mutex;
    std::unordered_map<DcamBufferHandle, NativeBufferHold> buffers;
    std::atomic<uint64_t> next_buffer_handle{1};

    bool cacheJavaBindings(JNIEnv* env, jobject callbacks);
    void releaseJavaBindings(JNIEnv* env) const;
};

JNIEnv* getEnv(NativeSession* session, bool* attached);
void finishEnv(NativeSession* session, bool attached);
void destroySession(JNIEnv* env, NativeSession* session);
bool configureGraph(NativeSession* session,
                    const std::vector<std::string>& edge_sources,
                    const std::vector<std::string>& edge_targets);
DcamStatus processGraphRoots(NativeSession* session, const DcamFrame* frame,
                             uint32_t* out_flags);
DcamStatus emitGraphFrame(NativeSession* session, DcamNodeHandle source_node,
                          uint32_t output_port_id, const DcamFrame* frame);
DcamStatus releaseGraphZslFrame(NativeSession* session, DcamNodeHandle owner,
                                uint64_t frame_id, int32_t release_fence_fd);
void retainExternalFrame(NativeSession* session, uint64_t frame_id,
                         AHardwareBuffer* buffer);
uint64_t retainExternalFrameForBuffer(NativeSession* session,
                                      AHardwareBuffer* buffer);
void releaseExternalFrame(NativeSession* session, uint64_t frame_id);
bool completeExternalFrame(NativeSession* session, uint64_t frame_id,
                           DcamStatus status, uint32_t flags);
void releaseGraphBuffers(NativeSession* session);
void releasePresentedPreview(NativeSession* session);
extern "C" void initHostApi(DcamHostApi* api, void* ctx);
