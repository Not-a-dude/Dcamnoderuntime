#include "session.h"
#include <algorithm>
#include <android/dlext.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <cstring>
#include <string>
#include <unordered_set>
#include <unistd.h>

#define LOG_TAG "DcamNodeRuntime"

namespace {

constexpr int32_t kMaximumZslDepth = 16;
    thread_local std::string g_last_open_error;

    void logOpenError(const std::string &error_msg) {
        g_last_open_error = error_msg;
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s", error_msg.c_str());
    }

std::string copyJString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

bool hasRequiredNodeApi(const DcamNodeApi& api) {
    constexpr size_t required_size =
            offsetof(DcamNodeApi, destroy) + sizeof(api.destroy);
    return api.struct_size >= required_size &&
           api.abi_major == DCAM_NODE_ABI_MAJOR &&
           api.abi_minor <= DCAM_NODE_ABI_MINOR &&
           api.get_node_count && api.get_node_descriptor && api.create &&
           api.configure && api.process && api.flush && api.destroy;
}

bool copyJStringArray(JNIEnv* env, jobjectArray values, jsize expected_count,
                      std::vector<std::string>* output) {
    if (!values || env->GetArrayLength(values) != expected_count) return false;
    output->clear();
    output->reserve(expected_count);
    for (jsize index = 0; index < expected_count; ++index) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        const std::string copy = copyJString(env, value);
        if (value) env->DeleteLocalRef(value);
        if (copy.empty()) return false;
        output->push_back(copy);
    }
    return true;
}

bool copyPorts(const DcamPortDescriptor* ports, uint32_t count,
               std::vector<NativePort>* output) {
    if (count > 0 && !ports) return false;
    std::unordered_set<uint32_t> ids;
    std::unordered_set<std::string> names;
    output->clear();
    output->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const DcamPortDescriptor& port = ports[index];
        if (port.struct_size < sizeof(DcamPortDescriptor) || port.port_id == 0 ||
            !port.name.data || port.name.size == 0) {
            return false;
        }
        const std::string name(port.name.data, port.name.size);
        if (!ids.insert(port.port_id).second || !names.insert(name).second) return false;
        output->push_back({port.port_id, port.port_type, name});
    }
    return true;
}

bool describeNode(NativeSession* session, const std::string& type_id,
                  NativeNodeInstance* output) {
    for (const auto &api: session->node_apis) {
        uint32_t descriptor_count = 0;
        if (api.get_node_count(&descriptor_count) != DCAM_OK) continue;
        for (uint32_t index = 0; index < descriptor_count; ++index) {
            DcamNodeDescriptor descriptor{.struct_size = sizeof(DcamNodeDescriptor)};
            if (api.get_node_descriptor(index, &descriptor) != DCAM_OK ||
                !descriptor.node_type_id.data || !descriptor.module_id.data ||
                !descriptor.module_version.data ||
                std::string(descriptor.node_type_id.data, descriptor.node_type_id.size) !=
                type_id) {
                continue;
            }
            const std::string module_id(descriptor.module_id.data, descriptor.module_id.size);
            const std::string module_version(
                    descriptor.module_version.data, descriptor.module_version.size);
            if (session->module_id.empty()) {
                session->module_id = module_id;
                session->module_version = module_version;
            }
            output->api = api;
            return copyPorts(descriptor.input_ports, descriptor.input_port_count,
                             &output->inputs) &&
                   copyPorts(descriptor.output_ports, descriptor.output_port_count,
                             &output->outputs);
        }
    }
    return false;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_getLastOpenErrorNative(JNIEnv *env, jobject) {
    if (g_last_open_error.empty()) return nullptr;
    return env->NewStringUTF(g_last_open_error.c_str());
}

JNIEXPORT jlong JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_openSessionNative(
    JNIEnv* env, jobject, jlong sid, jint lfd, jintArray dfds, jobjectArray dnames,
    jobjectArray ntids, jobjectArray iids, jobjectArray cfgs,
    jobjectArray edge_sources_array, jobjectArray edge_targets_array,
    jint maximum_zsl_depth, jobject cb) {

    g_last_open_error.clear();

    if (sid <= 0 || lfd < 0 || !ntids || !iids || !cfgs || !cb) {
        logOpenError("Invalid session arguments passed to openSessionNative");
        return 0;
    }
    const jsize node_count = env->GetArrayLength(ntids);
    if (node_count <= 0 || env->GetArrayLength(iids) != node_count ||
        env->GetArrayLength(cfgs) != node_count) {
        logOpenError("Node array parameters count mismatch in openSessionNative");
        return 0;
    }
    std::vector<std::string> node_type_ids;
    std::vector<std::string> instance_ids;
    std::vector<std::string> configs;
    if (!copyJStringArray(env, ntids, node_count, &node_type_ids) ||
        !copyJStringArray(env, iids, node_count, &instance_ids) ||
        !copyJStringArray(env, cfgs, node_count, &configs) ||
        !edge_sources_array || !edge_targets_array ||
        env->GetArrayLength(edge_sources_array) != env->GetArrayLength(edge_targets_array)) {
        logOpenError("Failed to parse node configuration strings in openSessionNative");
        return 0;
    }
    const jsize edge_count = env->GetArrayLength(edge_sources_array);
    std::vector<std::string> edge_sources;
    std::vector<std::string> edge_targets;
    if (!copyJStringArray(env, edge_sources_array, edge_count, &edge_sources) ||
        !copyJStringArray(env, edge_targets_array, edge_count, &edge_targets)) {
        logOpenError("Failed to parse edge configuration strings in openSessionNative");
        return 0;
    }
    auto* s = new NativeSession{};
    s->session_id = static_cast<uint64_t>(sid);
    s->maximum_zsl_depth = static_cast<uint32_t>(
            std::clamp(static_cast<int32_t>(maximum_zsl_depth), 0, kMaximumZslDepth));
    if (!s->cacheJavaBindings(env, cb)) {
        logOpenError("Failed to cache Java callbacks in openSessionNative");
        destroySession(env, s);
        return 0;
    }
    initHostApi(&s->host_api, s);
    s->node_api.struct_size = sizeof(DcamNodeApi);

    jsize dcnt = dfds ? env->GetArrayLength(dfds) : 0;
    if ((dcnt > 0 && !dnames) || (dnames && env->GetArrayLength(dnames) != dcnt)) {
        logOpenError("Dependency file descriptor count mismatch");
        destroySession(env, s);
        return 0;
    }
    std::vector<jint> fds(dcnt);
    if (dcnt) env->GetIntArrayRegion(dfds, 0, dcnt, fds.data());
    for (jsize i = 0; i < dcnt; ++i) {
        auto nm = (jstring)env->GetObjectArrayElement(dnames, i);
        const std::string name = copyJString(env, nm);
        if (nm) env->DeleteLocalRef(nm);
        if (name.empty() || fds[i] < 0) {
            logOpenError(
                    "Invalid dependency file descriptor or name at index " + std::to_string(i));
            destroySession(env, s);
            return 0;
        }
        android_dlextinfo info{};
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = dup(fds[i]);
        if (info.library_fd < 0) {
            logOpenError("Failed to dup FD for dependency library '" + name + "'");
            destroySession(env, s);
            return 0;
        }
        dlerror(); // Clear any existing error
        void* dep = android_dlopen_ext(name.c_str(), RTLD_NOW | RTLD_GLOBAL, &info);
        close(info.library_fd);
        if (!dep) {
            const char *dlerr = dlerror();
            std::string msg = "Failed to dlopen dependency library '" + name + "'";
            if (dlerr) {
                msg += ": ";
                msg += dlerr;
            }
            logOpenError(msg);
            destroySession(env, s);
            return 0;
        }
        s->dependencies.push_back(dep);
    }

    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = dup(lfd);
    if (info.library_fd < 0) {
        logOpenError("Failed to dup FD for main node library");
        destroySession(env, s);
        return 0;
    }
    dlerror(); // Clear any existing error
    s->library = android_dlopen_ext("libnode.so", RTLD_NOW | RTLD_GLOBAL, &info);
    close(info.library_fd);
    if (!s->library) {
        const char *dlerr = dlerror();
        std::string msg = "Failed to dlopen main node library (libnode.so)";
        if (dlerr) {
            msg += ": ";
            msg += dlerr;
        }
        logOpenError(msg);
        destroySession(env, s);
        return 0;
    }

    dlerror();
    auto get_api = (DcamNodeGetApiFn)dlsym(s->library, DCAM_NODE_ENTRYPOINT);
    if (!get_api) {
        const char *dlerr = dlerror();
        std::string msg = "Failed to dlsym entrypoint '"
        DCAM_NODE_ENTRYPOINT
        "' from main library";
        if (dlerr) {
            msg += ": ";
            msg += dlerr;
        }
        logOpenError(msg);
        destroySession(env, s);
        return 0;
    }
    DcamStatus api_status = get_api(DCAM_NODE_ABI_MAJOR, &s->host_api, &s->node_api);
    if (api_status != DCAM_OK) {
        logOpenError("Main library entrypoint returned error status " + std::to_string(api_status));
        destroySession(env, s);
        return 0;
    }
    if (!hasRequiredNodeApi(s->node_api)) {
        logOpenError("Main library node API is missing required function pointers or ABI mismatch");
        destroySession(env, s);
        return 0;
    }
    s->node_apis.push_back(s->node_api);

    for (void *dep: s->dependencies) {
        auto dep_get_api = (DcamNodeGetApiFn) dlsym(dep, DCAM_NODE_ENTRYPOINT);
        if (dep_get_api) {
            DcamNodeApi dep_api{.struct_size = sizeof(DcamNodeApi)};
            if (dep_get_api(DCAM_NODE_ABI_MAJOR, &s->host_api, &dep_api) == DCAM_OK &&
                hasRequiredNodeApi(dep_api)) {
                s->node_apis.push_back(dep_api);
            }
        }
    }

    for (jsize i = 0; i < node_count; ++i) {
        NativeNodeInstance instance{};
        instance.type_id = node_type_ids[i];
        instance.instance_id = instance_ids[i];
        if (!describeNode(s, instance.type_id, &instance)) {
            logOpenError("Failed to describe node type '" + instance.type_id + "' (instance '" +
                         instance.instance_id + "')");
            destroySession(env, s);
            return 0;
        }
        const DcamStringView type_view{
            node_type_ids[i].data(), static_cast<uint32_t>(node_type_ids[i].size())};
        const DcamStringView instance_view{
            instance_ids[i].data(), static_cast<uint32_t>(instance_ids[i].size())};
        const DcamStringView config_view{
            configs[i].data(), static_cast<uint32_t>(configs[i].size())};
        DcamNodeHandle handle = 0;
        DcamStatus create_st = instance.api.create(type_view, instance_view, config_view, &handle);
        if (create_st != DCAM_OK || !handle) {
            logOpenError("Failed to create node instance '" + instance.instance_id + "' (type '" +
                         instance.type_id + "'), error: " + std::to_string(create_st));
            destroySession(env, s);
            return 0;
        }
        instance.handle = handle;
        DcamStatus cfg_st = instance.api.configure(handle, config_view);
        if (cfg_st != DCAM_OK) {
            logOpenError(
                    "Failed to configure node instance '" + instance.instance_id + "' (type '" +
                    instance.type_id + "'), error: " + std::to_string(cfg_st));
            destroySession(env, s);
            return 0;
        }
        s->nodes.push_back(std::move(instance));
    }
    if (!configureGraph(s, edge_sources, edge_targets)) {
        logOpenError("Failed to configure node execution graph");
        destroySession(env, s);
        return 0;
    }
    s->node_type_id = node_type_ids.front();
    return (jlong)s;
}

JNIEXPORT jobjectArray JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_getDescriptorNative(JNIEnv* env, jobject, jlong h) {
    auto* s = (NativeSession*)h;
    if (!s) return nullptr;
    jclass cls = env->FindClass("java/lang/String");
    jobjectArray res = env->NewObjectArray(3, cls, nullptr);
    jstring module = env->NewStringUTF(s->module_id.c_str());
    jstring version = env->NewStringUTF(s->module_version.c_str());
    jstring type = env->NewStringUTF(s->node_type_id.c_str());
    env->SetObjectArrayElement(res, 0, module);
    env->SetObjectArrayElement(res, 1, version);
    env->SetObjectArrayElement(res, 2, type);
    env->DeleteLocalRef(module);
    env->DeleteLocalRef(version);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(cls);
    return res;
}

JNIEXPORT jlongArray JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_processFrameNative(JNIEnv* env, jobject, jlong h, jobject f, jint af) {
    auto* s = (NativeSession*)h;
    if (!s || s->nodes.empty() || !f) {
        if (af >= 0) close(af);
        return nullptr;
    }
    jclass cls = env->GetObjectClass(f);
    jobject jb = env->GetObjectField(f, env->GetFieldID(cls, "hardwareBuffer", "Landroid/hardware/HardwareBuffer;"));
    AHardwareBuffer* buf = jb ? AHardwareBuffer_fromHardwareBuffer(env, jb) : nullptr;

    jobject jmd = env->GetObjectField(f, env->GetFieldID(cls, "metadata", "Lcom/dcam/node/MetadataParcel;"));
    const bool has_metadata = jmd != nullptr;
    DcamMetadata md{.struct_size = sizeof(DcamMetadata)};
    std::vector<DcamMetadataEntry> entries;
    struct StoredEntry { std::string name; std::vector<uint8_t> val; };
    std::vector<StoredEntry> storage;

    if (jmd && s->metadata_class) {
        md.wire_version = (uint32_t)env->GetIntField(jmd, s->md_wire_version);
        jobjectArray jentries = (jobjectArray)env->GetObjectField(jmd, s->md_entries);
        if (jentries) {
            jsize cnt = env->GetArrayLength(jentries);
            entries.resize(cnt); storage.resize(cnt);
            for (jsize i = 0; i < cnt; ++i) {
                jobject e = env->GetObjectArrayElement(jentries, i);
                entries[i] = { .struct_size = sizeof(DcamMetadataEntry),
                    .key_id = (uint32_t)env->GetIntField(e, s->me_key_id),
                    .wire_type = (uint32_t)env->GetIntField(e, s->me_wire_type),
                    .element_count = (uint32_t)env->GetIntField(e, s->me_element_count),
                    .flags = (uint32_t)env->GetIntField(e, s->me_flags)
                };
                jstring nm = (jstring)env->GetObjectField(e, s->me_name);
                if (nm) {
                    const char* cnm = env->GetStringUTFChars(nm, nullptr);
                    storage[i].name = cnm;
                    env->ReleaseStringUTFChars(nm, cnm); env->DeleteLocalRef(nm);
                }
                jbyteArray vb = (jbyteArray)env->GetObjectField(e, s->me_value);
                if (vb) {
                    jsize vbsz = env->GetArrayLength(vb);
                    storage[i].val.resize(vbsz);
                    env->GetByteArrayRegion(vb, 0, vbsz, (jbyte*)storage[i].val.data());
                    env->DeleteLocalRef(vb);
                }
                entries[i].name = {storage[i].name.data(), (uint32_t)storage[i].name.size()};
                entries[i].value = {storage[i].val.data(), (uint32_t)storage[i].val.size()};
                env->DeleteLocalRef(e);
            }
            md.entry_count = (uint32_t)cnt;
            md.entries = entries.data();
            env->DeleteLocalRef(jentries);
        }
        env->DeleteLocalRef(jmd);
    }

    DcamFrame frame{ .struct_size = sizeof(DcamFrame), .hardware_buffer = buf,
        .acquire_fence_fd = af, .metadata = has_metadata ? &md : nullptr };
    frame.session_id = env->GetLongField(f, env->GetFieldID(cls, "sessionId", "J"));
    frame.frame_id = env->GetLongField(f, env->GetFieldID(cls, "frameId", "J"));
    frame.stream_id = env->GetIntField(f, env->GetFieldID(cls, "streamId", "I"));
    frame.timestamp_ns = env->GetLongField(f, env->GetFieldID(cls, "timestampNs", "J"));
    frame.request_revision = env->GetLongField(f, env->GetFieldID(cls, "requestRevision", "J"));
    frame.buffer_desc.struct_size = sizeof(DcamBufferDesc);
    frame.buffer_desc.width = env->GetIntField(f, env->GetFieldID(cls, "width", "I"));
    frame.buffer_desc.height = env->GetIntField(f, env->GetFieldID(cls, "height", "I"));
    frame.buffer_desc.layers = env->GetIntField(f, env->GetFieldID(cls, "layers", "I"));
    frame.buffer_desc.format = env->GetIntField(f, env->GetFieldID(cls, "format", "I"));
    frame.buffer_desc.usage = env->GetLongField(f, env->GetFieldID(cls, "usage", "J"));
    frame.buffer_desc.data_space = env->GetIntField(f, env->GetFieldID(cls, "dataSpace", "I"));
    frame.crop_left = env->GetIntField(f, env->GetFieldID(cls, "cropLeft", "I"));
    frame.crop_top = env->GetIntField(f, env->GetFieldID(cls, "cropTop", "I"));
    frame.crop_right = env->GetIntField(f, env->GetFieldID(cls, "cropRight", "I"));
    frame.crop_bottom = env->GetIntField(f, env->GetFieldID(cls, "cropBottom", "I"));
    jstring jparameters = (jstring)env->GetObjectField(
            f, env->GetFieldID(cls, "parametersJson", "Ljava/lang/String;"));
    const char* parameters = jparameters ? env->GetStringUTFChars(jparameters, nullptr) : nullptr;
    frame.parameters_json = {
        parameters,
        parameters ? static_cast<uint32_t>(env->GetStringUTFLength(jparameters)) : 0u,
    };

    jfieldID content_tag_field = env->GetFieldID(cls, "contentTag", "Ljava/lang/String;");
    jstring jcontent_tag = content_tag_field ? (jstring) env->GetObjectField(f, content_tag_field)
                                             : nullptr;
    const char *content_tag = jcontent_tag ? env->GetStringUTFChars(jcontent_tag, nullptr)
                                           : nullptr;
    frame.content_tag = {
            content_tag,
            content_tag ? static_cast<uint32_t>(env->GetStringUTFLength(jcontent_tag)) : 0u,
    };
    frame.attachment_count = 0;
    frame.attachments = nullptr;

    uint32_t flags = 0;
    s->processing_frame_id.store(frame.frame_id);
    s->processing_frame_buffer.store(frame.hardware_buffer);
    s->processing_frame_presented.store(false);
    const DcamStatus status = processGraphRoots(s, &frame, &flags);
    if (s->processing_frame_presented.load()) {
        flags |= DCAM_FRAME_FLAG_PREVIEW_PRESENTED;
    }
    const bool deferred = completeExternalFrame(s, frame.frame_id, status, flags);
    s->processing_frame_buffer.store(nullptr);
    s->processing_frame_id.store(0);

    if (content_tag) env->ReleaseStringUTFChars(jcontent_tag, content_tag);
    if (jcontent_tag) env->DeleteLocalRef(jcontent_tag);
    if (parameters) env->ReleaseStringUTFChars(jparameters, parameters);
    if (jparameters) env->DeleteLocalRef(jparameters);

    uint64_t bid = 0; if (buf) AHardwareBuffer_getId(buf, &bid);
    jlong vals[] = {
        (jlong)status,
        (jlong)DCAM_FRAME_RELEASE,
        -1,
        (jlong)bid,
        (jlong)flags,
        deferred ? 1 : 0,
    };
    jlongArray res = env->NewLongArray(6); env->SetLongArrayRegion(res, 0, 6, vals);
    if (jb) env->DeleteLocalRef(jb);
    env->DeleteLocalRef(cls);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_updateParametersNative(JNIEnv* env, jobject, jlong h, jlong rev, jstring patch) {
    auto* s = (NativeSession*)h;
    if (!s) return DCAM_ERROR_UNSUPPORTED;
    const char* cpatch = env->GetStringUTFChars(patch, nullptr);
    DcamStatus status = DCAM_OK;
    const DcamStringView patch_view{cpatch, (uint32_t)strlen(cpatch)};
    for (const NativeNodeInstance& node : s->nodes) {
        if (!node.api.update_parameters) continue;
        const DcamStatus node_status =
                node.api.update_parameters(node.handle, (uint64_t) rev, patch_view);
        if (status == DCAM_OK && node_status != DCAM_OK) status = node_status;
    }
    env->ReleaseStringUTFChars(patch, cpatch);
    return (jint)status;
}

JNIEXPORT jint JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_captureEventNative(JNIEnv* env, jobject, jlong h, jobject ev) {
    auto* s = (NativeSession*)h;
    if (!s) return DCAM_ERROR_UNSUPPORTED;
    jclass cls = env->GetObjectClass(ev);
    jstring msg = (jstring)env->GetObjectField(ev, env->GetFieldID(cls, "message", "Ljava/lang/String;"));
    const char* cmsg = msg ? env->GetStringUTFChars(msg, nullptr) : "";
    DcamCaptureEvent event{
        .struct_size = sizeof(DcamCaptureEvent),
        .event_type = (uint32_t)env->GetIntField(ev, env->GetFieldID(cls, "eventType", "I")),
        .status = env->GetIntField(ev, env->GetFieldID(cls, "status", "I")),
        .capture_id = (uint64_t)env->GetLongField(ev, env->GetFieldID(cls, "captureId", "J")),
        .frame_id = (uint64_t)env->GetLongField(ev, env->GetFieldID(cls, "frameId", "J")),
        .message = {cmsg, (uint32_t)strlen(cmsg)}
    };
    DcamStatus status = DCAM_OK;
    for (const NativeNodeInstance& node : s->nodes) {
        if (!node.api.on_capture_event) continue;
        const DcamStatus node_status = node.api.on_capture_event(node.handle, &event);
        if (status == DCAM_OK && node_status != DCAM_OK) status = node_status;
    }
    if (msg) env->ReleaseStringUTFChars(msg, cmsg);
    if (msg) env->DeleteLocalRef(msg);
    env->DeleteLocalRef(cls);
    return (jint)status;
}

JNIEXPORT jint JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_requestStateResultNative(JNIEnv* env, jobject, jlong h, jobject r) {
    auto* s = (NativeSession*)h;
    if (!s) return DCAM_ERROR_UNSUPPORTED;
    jclass cls = env->GetObjectClass(r);
    jstring name = (jstring)env->GetObjectField(r, env->GetFieldID(cls, "keyName", "Ljava/lang/String;"));
    jstring msg = (jstring)env->GetObjectField(r, env->GetFieldID(cls, "message", "Ljava/lang/String;"));
    const char* cname = name ? env->GetStringUTFChars(name, nullptr) : "";
    const char* cmsg = msg ? env->GetStringUTFChars(msg, nullptr) : "";
    DcamRequestStateResult res{
        .struct_size = sizeof(DcamRequestStateResult),
        .revision = (uint64_t)env->GetLongField(r, env->GetFieldID(cls, "revision", "J")),
        .applied = (uint32_t)(env->GetBooleanField(r, env->GetFieldID(cls, "applied", "Z")) ? 1 : 0),
        .error_code = env->GetIntField(r, env->GetFieldID(cls, "errorCode", "I")),
        .key_id = (uint32_t)env->GetIntField(r, env->GetFieldID(cls, "keyId", "I")),
        .key_name = {cname, (uint32_t)strlen(cname)},
        .message = {cmsg, (uint32_t)strlen(cmsg)}
    };
    DcamStatus status = DCAM_OK;
    for (const NativeNodeInstance& node : s->nodes) {
        if (!node.api.on_request_state_result) continue;
        const DcamStatus node_status = node.api.on_request_state_result(node.handle, &res);
        if (status == DCAM_OK && node_status != DCAM_OK) status = node_status;
    }
    if (name) env->ReleaseStringUTFChars(name, cname);
    if (msg) env->ReleaseStringUTFChars(msg, cmsg);
    if (name) env->DeleteLocalRef(name);
    if (msg) env->DeleteLocalRef(msg);
    env->DeleteLocalRef(cls);
    return (jint)status;
}

JNIEXPORT jint JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_setPreviewSurfaceNative(
        JNIEnv* env, jobject, jlong h, jobject surf, jint sensor_orientation) {
    auto* s = (NativeSession*)h;
    if (!s) return DCAM_ERROR_INVALID_ARGUMENT;
    ANativeWindow* win = ANativeWindow_fromSurface(env, surf);
    if (!win) return DCAM_ERROR_INVALID_ARGUMENT;
    ASurfaceControl* ctrl = ASurfaceControl_createFromWindow(win, "DcamPreview");
    if (!ctrl) {
        ANativeWindow_release(win);
        return DCAM_ERROR_PROCESSING;
    }
    releasePresentedPreview(s);
    std::lock_guard preview_lock(s->preview_mutex);
    if (s->preview_control) ASurfaceControl_release(s->preview_control);
    if (s->preview_window) ANativeWindow_release(s->preview_window);
    s->preview_window = win;
    s->preview_control = ctrl;
    switch (sensor_orientation) {
        case 90:
            s->preview_transform = ANATIVEWINDOW_TRANSFORM_ROTATE_90;
            break;
        case 180:
            s->preview_transform = ANATIVEWINDOW_TRANSFORM_ROTATE_180;
            break;
        case 270:
            s->preview_transform = ANATIVEWINDOW_TRANSFORM_ROTATE_270;
            break;
        default:
            s->preview_transform = ANATIVEWINDOW_TRANSFORM_IDENTITY;
            break;
    }
    return DCAM_OK;
}

JNIEXPORT jint JNICALL
Java_com_dcamnoderuntime_NodeRuntimeService_evictZslFrameNative(
        JNIEnv*, jobject, jlong h, jlong frame_id) {
    if (frame_id <= 0) return DCAM_ERROR_INVALID_ARGUMENT;
    return releaseGraphZslFrame(
            reinterpret_cast<NativeSession*>(h), 0,
            static_cast<uint64_t>(frame_id), -1);
}

JNIEXPORT jint JNICALL Java_com_dcamnoderuntime_NodeRuntimeService_flushSessionNative(JNIEnv*, jobject, jlong h) {
    auto* s = (NativeSession*)h;
    if (!s) return DCAM_OK;
    DcamStatus status = DCAM_OK;
    for (const NativeNodeInstance& node : s->nodes) {
        if (!node.api.flush) continue;
        const DcamStatus node_status = node.api.flush(node.handle);
        if (status == DCAM_OK && node_status != DCAM_OK) status = node_status;
    }
    return status;
}

JNIEXPORT void JNICALL Java_com_dcamnoderuntime_NodeRuntimeService_closeSessionNative(JNIEnv* env, jobject, jlong h) {
    destroySession(env, (NativeSession*)h);
}

} // extern "C"
