#include "session.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <poll.h>
#include <unordered_set>
#include <unistd.h>

namespace {

void waitAndCloseFence(int fd) {
    if (fd < 0) return;
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    while (poll(&descriptor, 1, -1) < 0 && errno == EINTR) {}
    close(fd);
}

bool splitEndpoint(const std::string& endpoint, std::string* instance, std::string* port) {
    const size_t separator = endpoint.find('.');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= endpoint.size() || separator != endpoint.rfind('.')) {
        return false;
    }
    *instance = endpoint.substr(0, separator);
    *port = endpoint.substr(separator + 1);
    return true;
}

const NativePort* findPort(const std::vector<NativePort>& ports, const std::string& name) {
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const NativePort& port) {
        return port.name == name;
    });
    return found == ports.end() ? nullptr : &*found;
}

const NativePort* findPort(const std::vector<NativePort>& ports, uint32_t id) {
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const NativePort& port) {
        return port.id == id;
    });
    return found == ports.end() ? nullptr : &*found;
}

size_t findNode(const NativeSession* session, const std::string& instance_id) {
    for (size_t index = 0; index < session->nodes.size(); ++index) {
        if (session->nodes[index].instance_id == instance_id) return index;
    }
    return session->nodes.size();
}

size_t findNode(const NativeSession* session, DcamNodeHandle handle) {
    for (size_t index = 0; index < session->nodes.size(); ++index) {
        if (session->nodes[index].handle == handle) return index;
    }
    return session->nodes.size();
}

void notifyExternalFrameReleased(
        NativeSession* session, uint64_t frame_id, const NativeExternalFrame& frame) {
    bool attached = false;
    JNIEnv* env = getEnv(session, &attached);
    if (!env) return;
    env->CallVoidMethod(
            session->callbacks, session->release_frame_method,
            static_cast<jlong>(frame_id), static_cast<jlong>(frame.buffer_id),
            static_cast<jint>(frame.status), static_cast<jint>(frame.flags));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    finishEnv(session, attached);
}

bool retainZslFrame(NativeSession* session, const NativeNodeInstance& node,
                    const DcamFrame& frame) {
    std::lock_guard lock(session->zsl_mutex);
    const auto existing = std::find_if(
            session->zsl_holds.begin(), session->zsl_holds.end(),
            [&](const NativeZslHold& hold) {
                return hold.owner == node.handle && hold.frame_id == frame.frame_id;
            });
    if (existing != session->zsl_holds.end()) return true;
    if (session->zsl_holds.size() >= session->maximum_zsl_depth) return false;
    AHardwareBuffer_acquire(frame.hardware_buffer);
    const uint64_t external_frame_id =
            retainExternalFrameForBuffer(session, frame.hardware_buffer);
    session->zsl_holds.push_back(
            {node.handle, frame.frame_id, frame.hardware_buffer, external_frame_id});
    return true;
}

    void notifyZslEvicted(const NativeNodeInstance &node,
                      uint64_t frame_id) {
        if (!node.api.on_capture_event) return;
    static constexpr char message[] = "Runtime ZSL capacity is exhausted";
    const DcamCaptureEvent event{
        .struct_size = sizeof(DcamCaptureEvent),
        .event_type = DCAM_CAPTURE_ZSL_EVICTED,
        .status = DCAM_ERROR_BUSY,
        .capture_id = 0,
        .frame_id = frame_id,
        .message = {message, sizeof(message) - 1},
    };
        node.api.on_capture_event(node.handle, &event);
}

DcamStatus processNode(NativeSession* session, size_t node_index,
                       const DcamFrame& frame, uint32_t* out_flags) {
    if (!session || node_index >= session->nodes.size() || !frame.hardware_buffer) {
        waitAndCloseFence(frame.acquire_fence_fd);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    const NativeNodeInstance& node = session->nodes[node_index];
    DcamFrameResult result{
        .struct_size = sizeof(DcamFrameResult),
        .disposition = DCAM_FRAME_RELEASE,
        .release_fence_fd = -1,
    };
    DcamStatus status;
    {
        std::lock_guard lock(session->graph_mutex);
        status = node.api.process(node.handle, &frame, &result);
    }
    waitAndCloseFence(result.release_fence_fd);
    if (out_flags) *out_flags |= result.flags;
    if (result.disposition == DCAM_FRAME_RETAIN_ZSL && status == DCAM_OK) {
        if (!retainZslFrame(session, node, frame)) notifyZslEvicted(node, frame.frame_id);
    } else if (result.disposition != DCAM_FRAME_RELEASE && status == DCAM_OK) {
        status = DCAM_ERROR_PROCESSING;
    }
    return status;
}

}  // namespace

bool configureGraph(NativeSession* session,
                    const std::vector<std::string>& edge_sources,
                    const std::vector<std::string>& edge_targets) {
    if (!session || edge_sources.size() != edge_targets.size()) return false;
    std::vector<size_t> indegree(session->nodes.size(), 0);
    std::vector<std::vector<size_t>> adjacency(session->nodes.size());
    std::unordered_set<std::string> connected_inputs;
    session->graph_edges.clear();

    for (size_t index = 0; index < edge_sources.size(); ++index) {
        std::string source_instance;
        std::string source_port_name;
        std::string target_instance;
        std::string target_port_name;
        if (!splitEndpoint(edge_sources[index], &source_instance, &source_port_name) ||
            !splitEndpoint(edge_targets[index], &target_instance, &target_port_name) ||
            !connected_inputs.insert(edge_targets[index]).second) {
            return false;
        }
        const size_t source_node = findNode(session, source_instance);
        const size_t target_node = findNode(session, target_instance);
        if (source_node == session->nodes.size() || target_node == session->nodes.size()) return false;
        const NativePort* source_port = findPort(session->nodes[source_node].outputs, source_port_name);
        const NativePort* target_port = findPort(session->nodes[target_node].inputs, target_port_name);
        if (!source_port || !target_port || source_port->type != DCAM_PORT_FRAME ||
            target_port->type != DCAM_PORT_FRAME) {
            return false;
        }
        session->graph_edges.push_back(
                {source_node, source_port->id, target_node, target_port->id});
        adjacency[source_node].push_back(target_node);
        ++indegree[target_node];
    }

    std::deque<size_t> ready;
    for (size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) ready.push_back(index);
    }
    size_t visited = 0;
    while (!ready.empty()) {
        const size_t node = ready.front();
        ready.pop_front();
        ++visited;
        for (const size_t target : adjacency[node]) {
            if (--indegree[target] == 0) ready.push_back(target);
        }
    }
    if (visited != session->nodes.size()) return false;

    session->graph_roots.clear();
    for (size_t index = 0; index < session->nodes.size(); ++index) {
        const bool accepts_frames = std::any_of(
                session->nodes[index].inputs.begin(), session->nodes[index].inputs.end(),
                [](const NativePort& port) { return port.type == DCAM_PORT_FRAME; });
        const bool has_frame_input = std::any_of(
                session->graph_edges.begin(), session->graph_edges.end(),
                [index](const NativeGraphEdge& edge) { return edge.target_node == index; });
        if (accepts_frames && !has_frame_input) session->graph_roots.push_back(index);
    }
    return !session->graph_roots.empty();
}

DcamStatus processGraphRoots(NativeSession* session, const DcamFrame* frame,
                             uint32_t* out_flags) {
    if (!session || !frame || frame->struct_size < sizeof(DcamFrame) ||
        !frame->hardware_buffer || session->graph_roots.empty()) {
        if (frame) waitAndCloseFence(frame->acquire_fence_fd);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    DcamStatus aggregate = DCAM_OK;
    for (const size_t root : session->graph_roots) {
        const auto input = std::find_if(
                session->nodes[root].inputs.begin(), session->nodes[root].inputs.end(),
                [](const NativePort& port) { return port.type == DCAM_PORT_FRAME; });
        if (input == session->nodes[root].inputs.end()) continue;
        DcamFrame routed = *frame;
        routed.input_port_id = input->id;
        routed.acquire_fence_fd = frame->acquire_fence_fd >= 0
                ? dup(frame->acquire_fence_fd) : -1;
        if (frame->acquire_fence_fd >= 0 && routed.acquire_fence_fd < 0) {
            if (aggregate == DCAM_OK) aggregate = DCAM_ERROR_IO;
            continue;
        }
        const DcamStatus status = processNode(session, root, routed, out_flags);
        if (aggregate == DCAM_OK && status != DCAM_OK) aggregate = status;
    }
    waitAndCloseFence(frame->acquire_fence_fd);
    return aggregate;
}

DcamStatus emitGraphFrame(NativeSession* session, DcamNodeHandle source_node,
                          uint32_t output_port_id, const DcamFrame* frame) {
    if (!session || !frame || frame->struct_size < sizeof(DcamFrame) ||
        !frame->hardware_buffer) {
        if (frame) waitAndCloseFence(frame->acquire_fence_fd);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    const size_t source = findNode(session, source_node);
    if (source == session->nodes.size()) {
        waitAndCloseFence(frame->acquire_fence_fd);
        return DCAM_ERROR_NOT_FOUND;
    }
    const NativePort* output = findPort(session->nodes[source].outputs, output_port_id);
    if (!output || output->type != DCAM_PORT_FRAME) {
        waitAndCloseFence(frame->acquire_fence_fd);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    bool routed_any = false;
    DcamStatus aggregate = DCAM_OK;
    for (const NativeGraphEdge& edge : session->graph_edges) {
        if (edge.source_node != source || edge.source_port != output_port_id) continue;
        routed_any = true;
        DcamFrame routed = *frame;
        routed.input_port_id = edge.target_port;
        routed.acquire_fence_fd = frame->acquire_fence_fd >= 0
                ? dup(frame->acquire_fence_fd) : -1;
        if (frame->acquire_fence_fd >= 0 && routed.acquire_fence_fd < 0) {
            if (aggregate == DCAM_OK) aggregate = DCAM_ERROR_IO;
            continue;
        }
        const DcamStatus status = processNode(session, edge.target_node, routed, nullptr);
        if (aggregate == DCAM_OK && status != DCAM_OK) aggregate = status;
    }
    waitAndCloseFence(frame->acquire_fence_fd);
    return routed_any ? aggregate : DCAM_ERROR_NOT_FOUND;
}

DcamStatus releaseGraphZslFrame(NativeSession* session, DcamNodeHandle owner,
                                uint64_t frame_id, int32_t release_fence_fd) {
    if (!session || frame_id == 0) {
        waitAndCloseFence(release_fence_fd);
        return DCAM_ERROR_INVALID_ARGUMENT;
    }
    std::vector<NativeZslHold> released;
    {
        std::lock_guard lock(session->zsl_mutex);
        auto next = session->zsl_holds.begin();
        while (next != session->zsl_holds.end()) {
            if (next->frame_id == frame_id && (owner == 0 || next->owner == owner)) {
                released.push_back(*next);
                next = session->zsl_holds.erase(next);
                if (owner != 0) break;
            } else {
                ++next;
            }
        }
    }
    waitAndCloseFence(release_fence_fd);
    for (const NativeZslHold& hold : released) {
        AHardwareBuffer_release(hold.buffer);
        releaseExternalFrame(session, hold.external_frame_id);
    }
    return released.empty() ? DCAM_ERROR_NOT_FOUND : DCAM_OK;
}

void retainExternalFrame(NativeSession* session, uint64_t frame_id,
                         AHardwareBuffer* buffer) {
    if (!session || frame_id == 0 || !buffer) return;
    uint64_t buffer_id = 0;
    AHardwareBuffer_getId(buffer, &buffer_id);
    std::lock_guard lock(session->external_frame_mutex);
    auto [entry, inserted] = session->external_frames.try_emplace(frame_id);
    if (inserted) entry->second.buffer_id = buffer_id;
    ++entry->second.hold_count;
}

uint64_t retainExternalFrameForBuffer(NativeSession* session,
                                      AHardwareBuffer* buffer) {
    if (!session || !buffer) return 0;
    std::lock_guard lock(session->buffer_mutex);
    const auto retained = std::find_if(
            session->buffers.begin(), session->buffers.end(),
            [buffer](const auto& entry) {
                return entry.second.buffer == buffer && entry.second.external_frame_id != 0;
            });
    uint64_t frame_id = retained == session->buffers.end()
            ? 0 : retained->second.external_frame_id;
    if (frame_id == 0 && session->processing_frame_buffer.load() == buffer) {
        frame_id = session->processing_frame_id.load();
    }
    if (frame_id != 0) retainExternalFrame(session, frame_id, buffer);
    return frame_id;
}

void releaseExternalFrame(NativeSession* session, uint64_t frame_id) {
    if (!session || frame_id == 0) return;
    NativeExternalFrame released;
    bool notify = false;
    {
        std::lock_guard lock(session->external_frame_mutex);
        const auto entry = session->external_frames.find(frame_id);
        if (entry == session->external_frames.end() || entry->second.hold_count == 0) return;
        --entry->second.hold_count;
        if (entry->second.hold_count == 0 && !entry->second.processing) {
            released = entry->second;
            session->external_frames.erase(entry);
            notify = true;
        }
    }
    if (notify) notifyExternalFrameReleased(session, frame_id, released);
}

bool completeExternalFrame(NativeSession* session, uint64_t frame_id,
                           DcamStatus status, uint32_t flags) {
    if (!session || frame_id == 0) return false;
    std::lock_guard lock(session->external_frame_mutex);
    const auto entry = session->external_frames.find(frame_id);
    if (entry == session->external_frames.end()) return false;
    entry->second.status = status;
    entry->second.flags = flags;
    entry->second.processing = false;
    if (entry->second.hold_count != 0) return true;
    session->external_frames.erase(entry);
    return false;
}

void releaseGraphBuffers(NativeSession* session) {
    if (!session) return;
    std::vector<NativeZslHold> holds;
    {
        std::lock_guard lock(session->zsl_mutex);
        holds.swap(session->zsl_holds);
    }
    for (const NativeZslHold& hold : holds) {
        AHardwareBuffer_release(hold.buffer);
        releaseExternalFrame(session, hold.external_frame_id);
    }
}
