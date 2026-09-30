#include "session.h"
#include <dlfcn.h>

JNIEnv* getEnv(NativeSession* session, bool* attached) {
    *attached = false;
    JNIEnv* env = nullptr;
    if (session->vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if (session->vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        *attached = true;
        return env;
    }
    return nullptr;
}

void finishEnv(NativeSession* session, bool attached) {
    if (attached) session->vm->DetachCurrentThread();
}

bool NativeSession::cacheJavaBindings(JNIEnv* env, jobject callbacks_obj) {
    env->GetJavaVM(&vm);
    callbacks = env->NewGlobalRef(callbacks_obj);
    if (!callbacks) return false;
    jclass cls = env->GetObjectClass(callbacks);
    release_frame_method = env->GetMethodID(cls, "releaseFrameFromNative", "(JJII)V");
    request_state_method = env->GetMethodID(cls, "submitRequestStateFromNative", "(Lcom/dcam/node/RequestStateTransaction;)I");
    single_capture_method = env->GetMethodID(cls, "requestSingleCaptureFromNative", "(IJ)I");
    duplicate_output_method = env->GetMethodID(cls, "duplicateOutputFdFromNative", "(J)I");
    complete_output_method = env->GetMethodID(cls, "completeOutputFromNative", "(JLjava/lang/String;JI)I");
    open_artifact_method = env->GetMethodID(
            cls, "openArtifactFromNative", "(JLjava/lang/String;Ljava/lang/String;J)[J");
    complete_artifact_method = env->GetMethodID(cls, "completeArtifactFromNative", "(JJI)I");

    jclass tx_local = env->FindClass("com/dcam/node/RequestStateTransaction");
    jclass entry_local = env->FindClass("com/dcam/node/RequestStateEntry");
    if (tx_local && entry_local) {
        transaction_class = (jclass)env->NewGlobalRef(tx_local);
        request_entry_class = (jclass)env->NewGlobalRef(entry_local);
        transaction_ctor = env->GetMethodID(transaction_class, "<init>", "()V");
        request_entry_ctor = env->GetMethodID(request_entry_class, "<init>", "()V");

        tx_session_id = env->GetFieldID(transaction_class, "sessionId", "J");
        tx_scope = env->GetFieldID(transaction_class, "scope", "I");
        tx_template_id = env->GetFieldID(transaction_class, "templateId", "I");
        tx_revision = env->GetFieldID(transaction_class, "revision", "J");
        tx_base_revision = env->GetFieldID(transaction_class, "baseRevision", "J");
        tx_target_streams = env->GetFieldID(transaction_class, "targetStreamIds", "[I");
        tx_entries = env->GetFieldID(transaction_class, "entries", "[Lcom/dcam/node/RequestStateEntry;");
        tx_lifetime = env->GetFieldID(transaction_class, "lifetime", "I");

        re_key_id = env->GetFieldID(request_entry_class, "keyId", "I");
        re_wire_type = env->GetFieldID(request_entry_class, "wireType", "I");
        re_operation = env->GetFieldID(request_entry_class, "operation", "I");
        re_element_count = env->GetFieldID(request_entry_class, "elementCount", "I");
        re_name = env->GetFieldID(request_entry_class, "name", "Ljava/lang/String;");
        re_value = env->GetFieldID(request_entry_class, "value", "[B");

        jclass md_local = env->FindClass("com/dcam/node/MetadataParcel");
        jclass me_local = env->FindClass("com/dcam/node/MetadataEntry");
        if (md_local && me_local) {
            metadata_class = (jclass)env->NewGlobalRef(md_local);
            metadata_entry_class = (jclass)env->NewGlobalRef(me_local);
            md_wire_version = env->GetFieldID(metadata_class, "wireVersion", "I");
            md_entries = env->GetFieldID(metadata_class, "entries", "[Lcom/dcam/node/MetadataEntry;");
            me_key_id = env->GetFieldID(metadata_entry_class, "keyId", "I");
            me_wire_type = env->GetFieldID(metadata_entry_class, "wireType", "I");
            me_element_count = env->GetFieldID(metadata_entry_class, "elementCount", "I");
            me_flags = env->GetFieldID(metadata_entry_class, "flags", "I");
            me_name = env->GetFieldID(metadata_entry_class, "name", "Ljava/lang/String;");
            me_value = env->GetFieldID(metadata_entry_class, "value", "[B");
            env->DeleteLocalRef(md_local);
            env->DeleteLocalRef(me_local);
        }

        env->DeleteLocalRef(tx_local);
        env->DeleteLocalRef(entry_local);
    }
    env->DeleteLocalRef(cls);
    return release_frame_method && request_state_method && single_capture_method &&
           duplicate_output_method && complete_output_method &&
           open_artifact_method && complete_artifact_method &&
           transaction_ctor && request_entry_ctor && tx_session_id && tx_scope &&
           tx_template_id && tx_revision && tx_base_revision && tx_target_streams &&
           tx_entries && tx_lifetime && re_key_id && re_wire_type && re_operation &&
           re_element_count && re_name && re_value && metadata_class && md_wire_version &&
           md_entries && metadata_entry_class && me_key_id && me_wire_type &&
           me_element_count && me_flags && me_name && me_value;
}

void NativeSession::releaseJavaBindings(JNIEnv* env) const {
    if (callbacks) env->DeleteGlobalRef(callbacks);
    if (transaction_class) env->DeleteGlobalRef(transaction_class);
    if (request_entry_class) env->DeleteGlobalRef(request_entry_class);
    if (metadata_class) env->DeleteGlobalRef(metadata_class);
    if (metadata_entry_class) env->DeleteGlobalRef(metadata_entry_class);
}

void destroySession(JNIEnv* env, NativeSession* session) {
    if (!session) return;
    for (auto it = session->nodes.rbegin(); it != session->nodes.rend(); ++it) {
        if (it->handle && it->api.destroy) {
            it->api.destroy(it->handle);
            it->handle = 0;
        }
    }
    releasePresentedPreview(session);
    releaseGraphBuffers(session);
    std::unordered_map<DcamBufferHandle, NativeBufferHold> buffers;
    {
        std::lock_guard<std::mutex> lock(session->buffer_mutex);
        buffers.swap(session->buffers);
    }
    for (const auto& pair : buffers) {
        AHardwareBuffer_release(pair.second.buffer);
        releaseExternalFrame(session, pair.second.external_frame_id);
    }
    if (session->preview_control) ASurfaceControl_release(session->preview_control);
    if (session->preview_window) ANativeWindow_release(session->preview_window);
    if (session->library) dlclose(session->library);
    for (void* dep : session->dependencies) dlclose(dep);
    session->releaseJavaBindings(env);
    delete session;
}
