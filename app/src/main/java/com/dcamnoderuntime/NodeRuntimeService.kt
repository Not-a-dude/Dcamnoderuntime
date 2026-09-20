package com.dcamnoderuntime

import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Binder
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log
import android.view.Surface
import com.dcam.node.CaptureEvent
import com.dcam.node.DependencyParcel
import com.dcam.node.FrameParcel
import com.dcam.node.FrameRelease
import com.dcam.node.IDcamNodeHost
import com.dcam.node.IDcamNodeRuntime
import com.dcam.node.OutputResult
import com.dcam.node.OutputTargetParcel
import com.dcam.node.ArtifactRequestParcel
import com.dcam.node.ArtifactResult
import com.dcam.node.ArtifactTargetParcel
import com.dcam.node.RequestStateResult
import com.dcam.node.RequestStateTransaction
import com.dcam.node.RuntimeInfo
import com.dcam.node.SessionConfigParcel
import com.dcam.node.SessionOpenResult
import com.dcam.node.SingleCaptureRequest
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.ExecutionException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicLong

class NodeRuntimeService : Service() {
    private data class NativeNodeSpec(
        val typeId: String,
        val instanceId: String,
        val configJson: String,
    )

    private data class NativeEdgeSpec(
        val source: String,
        val target: String,
    )

    private data class NativeGraphSpec(
        val nodes: List<NativeNodeSpec>,
        val edges: List<NativeEdgeSpec>,
    )

    private data class RuntimeSession(
        val sessionId: Long,
        val nativeHandle: Long,
        val sensorOrientation: Int,
        val host: IDcamNodeHost,
        val callbacks: NativeHostCallbacks,
        val executor: ExecutorService,
        val deathRecipient: IBinder.DeathRecipient,
        val processingFrame: AtomicLong = AtomicLong(0),
    )

    private inner class NativeHostCallbacks(
        private val sessionId: Long,
        private val host: IDcamNodeHost,
    ) {
        private val outputTargets = ConcurrentHashMap<Long, OutputTargetParcel>()
        private val artifactTargets = ConcurrentHashMap<Long, ArtifactTargetParcel>()
        private val lastCaptureId = AtomicLong(0)

        @Suppress("unused")
        fun releaseFrameFromNative(frameId: Long, bufferId: Long, status: Int, flags: Int) {
            runCatching {
                host.onFrameReleased(FrameRelease().apply {
                    sessionId = this@NativeHostCallbacks.sessionId
                    this.frameId = frameId
                    disposition = FRAME_RELEASE
                    this.status = status
                    this.flags = flags
                    hardwareBufferId = bufferId
                    message = if (status == STATUS_OK) "OK" else "Error"
                })
            }.onFailure { error ->
                Log.w(TAG, "Unable to release deferred frame $frameId", error)
            }
        }

        @Suppress("unused")
        fun submitRequestStateFromNative(transaction: RequestStateTransaction): Int = try {
            transaction.sessionId = sessionId
            host.onRequestStateTransaction(transaction)
            STATUS_OK
        } catch (error: Throwable) {
            STATUS_IO
        }

        @Suppress("unused")
        fun requestSingleCaptureFromNative(source: Int, zslFrameId: Long): Int {
            val captureId = lastCaptureId.getAndSet(0)
            if (captureId == 0L) return STATUS_BAD_STATE
            return try {
                host.onSingleCaptureRequest(SingleCaptureRequest().apply {
                    this.sessionId = this@NativeHostCallbacks.sessionId
                    this.captureId = captureId
                    this.source = source
                    this.zslFrameId = zslFrameId
                })
                STATUS_OK
            } catch (error: Throwable) {
                STATUS_IO
            }
        }

        @Suppress("unused")
        fun duplicateOutputFdFromNative(captureId: Long): Int {
            val target = outputTargets[captureId]?.outputFd ?: return -1
            return try {
                ParcelFileDescriptor.dup(target.fileDescriptor).detachFd()
            } catch (error: IOException) {
                -1
            }
        }

        @Suppress("unused")
        fun completeOutputFromNative(captureId: Long, mimeType: String, bytes: Long, status: Int): Int {
            val target = outputTargets.remove(captureId) ?: return STATUS_NOT_FOUND
            val actualBytes = target.outputFd?.statSize ?: -1L
            val valid = status == STATUS_OK &&
                mimeType == target.expectedMimeType &&
                bytes in 1..target.maximumBytes &&
                actualBytes == bytes
            val reportedStatus = if (valid) STATUS_OK else if (status == STATUS_OK) STATUS_IO else status
            target.outputFd?.closeQuietly()
            return try {
                host.onOutputResult(OutputResult().apply {
                    this.sessionId = this@NativeHostCallbacks.sessionId
                    this.captureId = captureId
                    this.status = reportedStatus
                    this.mimeType = mimeType
                    bytesWritten = bytes
                    message = if (valid) "OK" else "Node output did not match its target contract"
                })
                if (valid) STATUS_OK else STATUS_IO
            } catch (error: Throwable) {
                STATUS_IO
            }
        }

        @Suppress("unused")
        fun openArtifactFromNative(
            captureId: Long,
            suggestedName: String,
            mimeType: String,
            maximumBytes: Long,
        ): LongArray? {
            val target = try {
                host.openArtifact(ArtifactRequestParcel().apply {
                    this.sessionId = this@NativeHostCallbacks.sessionId
                    this.captureId = captureId
                    this.suggestedName = suggestedName
                    this.mimeType = mimeType
                    this.maximumBytes = maximumBytes
                })
            } catch (error: Throwable) {
                return null
            }
            val descriptor = target.outputFd ?: run {
                rejectArtifact(target, "Host returned an artifact without an FD")
                return null
            }
            if (target.sessionId != sessionId || target.captureId != captureId ||
                target.artifactId <= 0 || target.maximumBytes <= 0) {
                descriptor.closeQuietly()
                rejectArtifact(target, "Host returned an invalid artifact target")
                return null
            }
            val duplicate = try {
                ParcelFileDescriptor.dup(descriptor.fileDescriptor).detachFd()
            } catch (error: IOException) {
                descriptor.closeQuietly()
                rejectArtifact(target, "Unable to duplicate artifact FD")
                return null
            }
            artifactTargets.put(target.artifactId, target)?.outputFd?.closeQuietly()
            return longArrayOf(target.artifactId, duplicate.toLong())
        }

        @Suppress("unused")
        fun completeArtifactFromNative(artifactId: Long, bytes: Long, status: Int): Int {
            val target = artifactTargets.remove(artifactId) ?: return STATUS_NOT_FOUND
            val actualBytes = target.outputFd?.statSize ?: -1L
            val valid = status == STATUS_OK && bytes in 1..target.maximumBytes && actualBytes == bytes
            val reportedStatus = if (valid) STATUS_OK else if (status == STATUS_OK) STATUS_IO else status
            target.outputFd?.closeQuietly()
            return try {
                host.onArtifactResult(ArtifactResult().apply {
                    this.sessionId = this@NativeHostCallbacks.sessionId
                    this.captureId = target.captureId
                    this.artifactId = artifactId
                    this.status = reportedStatus
                    displayName = target.displayName
                    mimeType = target.mimeType
                    bytesWritten = bytes
                    message = if (valid) "OK" else "Artifact did not match its size contract"
                })
                if (valid) STATUS_OK else STATUS_IO
            } catch (error: Throwable) {
                STATUS_IO
            }
        }

        fun noteShutter(captureId: Long) {
            lastCaptureId.set(captureId)
        }

        private fun rejectArtifact(target: ArtifactTargetParcel, message: String) {
            runCatching {
                host.onArtifactResult(ArtifactResult().apply {
                    sessionId = this@NativeHostCallbacks.sessionId
                    captureId = target.captureId
                    artifactId = target.artifactId
                    status = STATUS_IO
                    displayName = target.displayName
                    mimeType = target.mimeType
                    bytesWritten = 0
                    this.message = message
                })
            }.onFailure { error ->
                Log.w(TAG, "Unable to reject artifact ${target.artifactId}", error)
            }
        }

        fun setOutput(target: OutputTargetParcel) {
            outputTargets.put(target.captureId, target)?.outputFd?.closeQuietly()
        }

        fun close() {
            outputTargets.values.forEach { it.outputFd?.closeQuietly() }
            outputTargets.clear()
            artifactTargets.values.forEach { it.outputFd?.closeQuietly() }
            artifactTargets.clear()
        }
    }

    private val sessions = ConcurrentHashMap<Long, RuntimeSession>()
    private val sessionOpenLock = Any()
    private val vulkanAvailable by lazy(LazyThreadSafetyMode.SYNCHRONIZED) { probeVulkanNative() }
    private val watchdog: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task, "DcamNodeWatchdog").apply { isDaemon = true }
    }

    private val binder = object : IDcamNodeRuntime.Stub() {
        override fun getRuntimeInfo(): RuntimeInfo {
            enforceDcamCaller()
            return RuntimeInfo().apply {
                runtimeAbi = RUNTIME_ABI
                pid = Process.myPid()
                uid = Process.myUid()
                isolated = Process.isIsolated()
                vulkanAvailable = this@NodeRuntimeService.vulkanAvailable
                runtimeVersion = RUNTIME_VERSION
                diagnostics = "Runtime ABI $runtimeAbi, pid=$pid, uid=$uid, isolated=$isolated, vulkan=$vulkanAvailable"
            }
        }

        override fun openSession(
            config: SessionConfigParcel?,
            nodeLibrary: ParcelFileDescriptor?,
            host: IDcamNodeHost?,
        ): SessionOpenResult = openSessionInternal(config, nodeLibrary, emptyArray(), host)

        override fun openSessionWithDependencies(
            config: SessionConfigParcel?,
            nodeLibrary: ParcelFileDescriptor?,
            dependencies: Array<out DependencyParcel>?,
            host: IDcamNodeHost?,
        ): SessionOpenResult = openSessionInternal(config, nodeLibrary, dependencies, host)

        private fun openSessionInternal(
            config: SessionConfigParcel?,
            nodeLibrary: ParcelFileDescriptor?,
            dependencies: Array<out DependencyParcel>?,
            host: IDcamNodeHost?,
        ): SessionOpenResult {
            enforceDcamCaller()
            val response = newSessionResult()
            if (config == null || nodeLibrary == null || host == null) {
                nodeLibrary?.closeQuietly()
                dependencies.closeDescriptors()
                return response.fail(STATUS_INVALID_ARGUMENT, "Missing session config, node FD, or host callback")
            }
            if (config.runtimeAbi != RUNTIME_ABI) {
                nodeLibrary.closeQuietly()
                dependencies.closeDescriptors()
                return response.fail(
                    STATUS_ABI_MISMATCH,
                    "Dcam runtime ABI ${config.runtimeAbi} is unsupported; expected $RUNTIME_ABI",
                )
            }
            val sessionId = config.requestedSessionId
            if (sessionId <= 0 || sessions.containsKey(sessionId)) {
                nodeLibrary.closeQuietly()
                dependencies.closeDescriptors()
                return response.fail(STATUS_BAD_STATE, "Session ID is invalid or already active")
            }
            val dependencyList = dependencies.orEmpty()
            val dependencyError = validateDependencies(dependencyList)
            if (dependencyError != null) {
                nodeLibrary.closeQuietly()
                dependencyList.closeDescriptors()
                return response.fail(STATUS_DEPENDENCY, dependencyError)
            }
            val graph = runCatching { buildNativeGraph(config) }.getOrElse { error ->
                nodeLibrary.closeQuietly()
                dependencyList.closeDescriptors()
                return response.fail(
                    STATUS_INVALID_ARGUMENT,
                    "Invalid session JSON: ${error.message ?: error.javaClass.simpleName}",
                )
            }
            val sensorOrientation = JSONObject(config.characteristicsJson)
                .optInt("sensorOrientation", 0)
                .let { ((it % 360) + 360) % 360 }
            val grantedZslDepth = config.requestedZslDepth.coerceIn(0, MAX_ZSL_DEPTH)
            val callbacks = NativeHostCallbacks(sessionId, host)
            val handle = try {
                synchronized(sessionOpenLock) {
                    openSessionNative(
                        sessionId,
                        nodeLibrary.fd,
                        dependencyList.map { it.libraryFd!!.fd }.toIntArray(),
                        dependencyList.map { it.soname }.toTypedArray(),
                        graph.nodes.map { it.typeId }.toTypedArray(),
                        graph.nodes.map { it.instanceId }.toTypedArray(),
                        graph.nodes.map { it.configJson }.toTypedArray(),
                        graph.edges.map { it.source }.toTypedArray(),
                        graph.edges.map { it.target }.toTypedArray(),
                        grantedZslDepth,
                        callbacks,
                    )
                }
            } finally {
                nodeLibrary.closeQuietly()
                dependencyList.closeDescriptors()
            }
            if (handle == 0L) {
                callbacks.close()
                return response.fail(STATUS_PROCESSING, "Native session failed to open")
            }
            val descriptor = getDescriptorNative(handle)
            if (descriptor == null || descriptor.size < 3) {
                closeSessionNative(handle)
                callbacks.close()
                return response.fail(STATUS_PROCESSING, "Node returned an invalid descriptor")
            }
            lateinit var session: RuntimeSession
            val deathRecipient = IBinder.DeathRecipient {
                watchdog.execute {
                    sessions.remove(sessionId)?.let {
                        Log.w(TAG, "Host Binder died; closing session $sessionId")
                        closeRuntimeSession(it)
                    }
                }
            }
            session = RuntimeSession(
                sessionId = sessionId,
                nativeHandle = handle,
                sensorOrientation = sensorOrientation,
                host = host,
                callbacks = callbacks,
                executor = Executors.newSingleThreadExecutor { task ->
                    Thread(task, "DcamNode-$sessionId")
                },
                deathRecipient = deathRecipient,
            )
            try {
                host.asBinder().linkToDeath(deathRecipient, 0)
            } catch (error: Throwable) {
                closeRuntimeSession(session, unlinkDeath = false)
                return response.fail(STATUS_BAD_STATE, "Host callback Binder is already dead")
            }
            if (sessions.putIfAbsent(sessionId, session) != null) {
                closeRuntimeSession(session)
                return response.fail(STATUS_BAD_STATE, "Session ID raced with another open")
            }
            response.status = STATUS_OK
            response.sessionId = sessionId
            response.grantedZslDepth = grantedZslDepth
            response.moduleId = descriptor[0]
            response.moduleVersion = descriptor[1]
            response.nodeTypeId = descriptor[2]
            response.message = "Session opened"
            Log.i(TAG, "Session opened: $sessionId")
            return response
        }

        override fun setPreviewSurface(sessionId: Long, surface: Surface?): Int {
            enforceDcamCaller()
            val session = sessions[sessionId] ?: return STATUS_BAD_STATE
            if (surface == null) return STATUS_INVALID_ARGUMENT
            return try {
                session.executor.submit<Int> {
                    setPreviewSurfaceNative(
                        session.nativeHandle,
                        surface,
                        session.sensorOrientation,
                    )
                }.get(CONTROL_TIMEOUT_MS, TimeUnit.MILLISECONDS).also { status ->
                    if (status != STATUS_OK) {
                        Log.e(TAG, "Native preview Surface registration failed for $sessionId: $status")
                    }
                }
            } catch (error: TimeoutException) {
                Log.e(TAG, "Timed out setting preview surface for $sessionId", error)
                STATUS_BUSY
            } catch (error: ExecutionException) {
                Log.e(TAG, "Unable to set preview surface for $sessionId", error.cause ?: error)
                STATUS_PROCESSING
            }
        }

        override fun processFrame(frame: FrameParcel?) {
            enforceDcamCaller()
            if (frame == null) return
            val session = sessions[frame.sessionId]
            if (session == null || frame.hardwareBuffer == null) {
                frame.hardwareBuffer?.close()
                frame.acquireFence?.closeQuietly()
                return
            }
            runCatching { session.executor.execute {
                val acquireFenceFd = runCatching { frame.acquireFence?.detachFd() ?: -1 }.getOrDefault(-1)
                session.processingFrame.set(frame.frameId)
                val processTimeout = if (frame.streamId == STILL_RAW_STREAM_ID) {
                    STILL_PROCESS_TIMEOUT_MS
                } else {
                    PROCESS_TIMEOUT_MS
                }
                val kill = watchdog.schedule({
                    if (session.processingFrame.get() == frame.frameId) {
                        Log.e(TAG, "Node hung on frame ${frame.frameId}; terminating runtime process")
                        Process.killProcess(Process.myPid())
                    }
                }, processTimeout, TimeUnit.MILLISECONDS)
                try {
                    val result = processFrameNative(session.nativeHandle, frame, acquireFenceFd)
                    val status = result?.getOrNull(0)?.toInt() ?: STATUS_PROCESSING
                    val disposition = result?.getOrNull(1)?.toInt() ?: FRAME_RELEASE
                    val releaseFenceFd = result?.getOrNull(2)?.toInt() ?: -1
                    val bufferId = result?.getOrNull(3) ?: 0L
                    val flags = result?.getOrNull(4)?.toInt() ?: 0
                    val deferred = result?.getOrNull(5) == 1L
                    val release = FrameRelease().apply {
                        this.sessionId = frame.sessionId
                        frameId = frame.frameId
                        this.disposition = disposition
                        this.status = status
                        this.flags = flags
                        hardwareBufferId = bufferId
                        releaseFence = releaseFenceFd.takeIf { it >= 0 }?.let(ParcelFileDescriptor::adoptFd)
                        message = if (status == STATUS_OK) "OK" else "Error"
                    }
                    if (deferred) {
                        release.releaseFence?.closeQuietly()
                    } else {
                        try {
                            session.host.onFrameReleased(release)
                        } finally {
                            release.releaseFence?.closeQuietly()
                        }
                    }
                } catch (error: Throwable) {
                    Log.e(TAG, "Node processing failed", error)
                    runCatching {
                        session.host.onFrameReleased(FrameRelease().apply {
                            this.sessionId = frame.sessionId
                            frameId = frame.frameId
                            disposition = FRAME_RELEASE
                            status = STATUS_PROCESSING
                            message = error.message ?: "Native processing failed"
                        })
                    }
                } finally {
                    kill.cancel(false)
                    session.processingFrame.compareAndSet(frame.frameId, 0)
                    frame.hardwareBuffer?.close()
                    frame.acquireFence?.closeQuietly()
                }
            } }.onFailure {
                frame.hardwareBuffer?.close()
                frame.acquireFence?.closeQuietly()
                Log.w(TAG, "Session ${frame.sessionId} rejected frame ${frame.frameId}", it)
            }
        }

        override fun updateParameters(sessionId: Long, revision: Long, jsonPatch: String?) {
            enforceDcamCaller()
            val session = sessions[sessionId] ?: return
            session.executor.execute {
                updateParametersNative(session.nativeHandle, revision, jsonPatch.orEmpty())
            }
        }

        override fun shutterPressed(sessionId: Long, captureId: Long) {
            enforceDcamCaller()
            val session = sessions[sessionId] ?: return
            session.callbacks.noteShutter(captureId)
            session.executor.execute {
                captureEventNative(session.nativeHandle, CaptureEvent().apply {
                    this.sessionId = sessionId
                    this.captureId = captureId
                    eventType = CAPTURE_SHUTTER
                    status = STATUS_OK
                    message = "Shutter pressed"
                })
            }
        }

        override fun deliverCaptureEvent(event: CaptureEvent?) {
            enforceDcamCaller()
            if (event == null) return
            sessions[event.sessionId]?.let { session ->
                session.executor.execute { captureEventNative(session.nativeHandle, event) }
            }
        }

        override fun deliverRequestStateResult(result: RequestStateResult?) {
            enforceDcamCaller()
            if (result == null) return
            sessions[result.sessionId]?.let { session ->
                session.executor.execute { requestStateResultNative(session.nativeHandle, result) }
            }
        }

        override fun setOutputTarget(target: OutputTargetParcel?) {
            enforceDcamCaller()
            if (target == null) return
            val session = sessions[target.sessionId]
            if (session == null || target.outputFd == null) {
                target.outputFd?.closeQuietly()
                return
            }
            session.callbacks.setOutput(target)
        }

        override fun evictZslFrame(sessionId: Long, frameId: Long) {
            enforceDcamCaller()
            sessions[sessionId]?.let { session ->
                session.executor.execute {
                    if (evictZslFrameNative(session.nativeHandle, frameId) == STATUS_OK) {
                        captureEventNative(session.nativeHandle, CaptureEvent().apply {
                            this.sessionId = sessionId
                            eventType = CAPTURE_ZSL_EVICTED
                            status = STATUS_BUSY
                            this.frameId = frameId
                            message = "Host evicted a Runtime-owned ZSL frame"
                        })
                    }
                }
            }
        }

        override fun flushSession(sessionId: Long) {
            enforceDcamCaller()
            val session = sessions[sessionId] ?: return
            runCatching {
                session.executor.submit<Int> { flushSessionNative(session.nativeHandle) }
                    .get(SESSION_CLOSE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            }.onFailure { Log.w(TAG, "Flush failed for $sessionId", it) }
        }

        override fun closeSession(sessionId: Long) {
            enforceDcamCaller()
            sessions.remove(sessionId)?.let(::closeRuntimeSession)
        }
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Runtime created: pid=${Process.myPid()} uid=${Process.myUid()} isolated=${Process.isIsolated()}")
    }

    override fun onBind(intent: Intent): IBinder {
        Log.i(TAG, "Runtime bound: ${intent.action}")
        return binder
    }

    override fun onDestroy() {
        sessions.keys.toList().forEach { id -> sessions.remove(id)?.let(::closeRuntimeSession) }
        watchdog.shutdownNow()
        super.onDestroy()
    }

    private fun closeRuntimeSession(session: RuntimeSession, unlinkDeath: Boolean = true) {
        if (unlinkDeath) runCatching { session.host.asBinder().unlinkToDeath(session.deathRecipient, 0) }
        val closed = runCatching {
            session.executor.submit {
                flushSessionNative(session.nativeHandle)
                closeSessionNative(session.nativeHandle)
            }.get(SESSION_CLOSE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            true
        }.getOrElse {
            Log.e(TAG, "Native session ${session.sessionId} did not close cleanly", it)
            false
        }
        session.callbacks.close()
        session.executor.shutdownNow()
        if (!closed) Process.killProcess(Process.myPid())
    }

    private fun newSessionResult() = SessionOpenResult().apply { runtimeAbi = RUNTIME_ABI }

    private fun validateDependencies(dependencies: Array<out DependencyParcel>): String? {
        if (dependencies.size > MAX_DEPENDENCIES) return "Too many node dependencies"
        val sonames = HashSet<String>()
        dependencies.forEach { dependency ->
            if (dependency.libraryFd == null) return "Dependency ${dependency.moduleId} has no library FD"
            if (!dependency.soname.matches(SONAME_PATTERN)) return "Invalid dependency SONAME ${dependency.soname}"
            if (!dependency.sha256.matches(SHA256_PATTERN)) return "Invalid dependency SHA-256 for ${dependency.soname}"
            if (!sonames.add(dependency.soname)) return "Duplicate dependency SONAME ${dependency.soname}"
        }
        return null
    }

    private fun buildNativeGraph(config: SessionConfigParcel): NativeGraphSpec {
        fun parse(label: String, value: String): JSONObject {
            require(value.length <= MAX_CONFIG_SECTION_CHARS) { "$label is too large" }
            return JSONObject(value)
        }
        val preset = parse("preset", config.presetJson)
        val nodes = preset.getJSONArray("nodes")
        require(nodes.length() in 1..MAX_NODES_PER_PRESET) { "Preset node count is outside limits" }
        val initialParameters = parse("initial parameters", config.initialParametersJson)
        val streams = parse("stream catalog", config.streamCatalogJson)
        val requestKeys = parse("request key catalog", config.requestKeyCatalogJson)
        val characteristics = parse("characteristics", config.characteristicsJson)
        val requestState = parse("initial request state", config.initialRequestStateJson)
        val instanceIds = HashSet<String>()
        val nodeSpecs = (0 until nodes.length()).map { index ->
            val node = nodes.getJSONObject(index)
            require(node.keys().asSequence().toSet() == setOf("id", "type", "parameters")) {
                "Preset node[$index] has an invalid field set"
            }
            val instanceId = node.getString("id")
            val typeId = node.getString("type")
            require(INSTANCE_ID_PATTERN.matches(instanceId) && instanceIds.add(instanceId)) {
                "Preset contains an invalid or duplicate node instance ID"
            }
            require(NODE_TYPE_PATTERN.matches(typeId)) { "Preset node $instanceId has an invalid type ID" }
            if (index == 0) {
                require(typeId == config.nodeTypeId) { "Primary node type does not match the preset" }
            }
            val root = JSONObject()
                .put("wireVersion", 1)
                .put("preset", preset)
                .put("streams", streams)
                .put("requestKeys", requestKeys)
                .put("characteristics", characteristics)
                .put("requestState", requestState)
                .put("initialParameters", initialParameters)
                .put("nodeParameters", node.getJSONObject("parameters"))
            initialParameters.keys().forEach { key -> root.put(key, initialParameters.get(key)) }
            val result = root.toString()
            require(result.length <= MAX_COMBINED_CONFIG_CHARS) { "Combined node config is too large" }
            NativeNodeSpec(typeId, instanceId, result)
        }
        val edges = preset.getJSONArray("edges")
        require(edges.length() <= MAX_EDGES_PER_PRESET) { "Preset edge count is outside limits" }
        val connectedInputs = HashSet<String>()
        val edgePairs = HashSet<Pair<String, String>>()
        val edgeSpecs = (0 until edges.length()).map { index ->
            val edge = edges.getJSONObject(index)
            require(edge.keys().asSequence().toSet() == setOf("from", "to")) {
                "Preset edge[$index] has an invalid field set"
            }
            val source = edge.getString("from")
            val target = edge.getString("to")
            require(NODE_ENDPOINT_PATTERN.matches(source) && NODE_ENDPOINT_PATTERN.matches(target)) {
                "Preset edge[$index] has an invalid endpoint"
            }
            require(edgePairs.add(source to target)) { "Preset contains a duplicate edge" }
            require(connectedInputs.add(target)) { "Preset input $target has multiple producers" }
            NativeEdgeSpec(source, target)
        }
        return NativeGraphSpec(nodeSpecs, edgeSpecs)
    }

    private fun Array<out DependencyParcel>?.closeDescriptors() {
        this?.forEach { it.libraryFd?.closeQuietly() }
    }

    private fun enforceDcamCaller() {
        val callerUid = Binder.getCallingUid()
        val packages = packageManager.getPackagesForUid(callerUid)
        if (packages == null || CLIENT_PACKAGE !in packages) {
            throw SecurityException("Only $CLIENT_PACKAGE may use the node runtime")
        }
        if (checkPermission(PERMISSION_RUN_NODE, Binder.getCallingPid(), callerUid) != PackageManager.PERMISSION_GRANTED) {
            throw SecurityException("Caller $CLIENT_PACKAGE must have $PERMISSION_RUN_NODE permission")
        }
    }

    private fun SessionOpenResult.fail(code: Int, detail: String): SessionOpenResult = apply {
        status = code
        message = detail
    }

    private fun ParcelFileDescriptor.closeQuietly() {
        try {
            close()
        } catch (error: IOException) {
            Log.w(TAG, "Unable to close file descriptor", error)
        }
    }

    private external fun openSessionNative(
        sessionId: Long,
        libraryFd: Int,
        dependencyFds: IntArray,
        dependencySonames: Array<String>,
        nodeTypeIds: Array<String>,
        instanceIds: Array<String>,
        configJsons: Array<String>,
        edgeSources: Array<String>,
        edgeTargets: Array<String>,
        maximumZslDepth: Int,
        callbacks: NativeHostCallbacks,
    ): Long
    private external fun getDescriptorNative(handle: Long): Array<String>?
    private external fun processFrameNative(handle: Long, frame: FrameParcel, acquireFenceFd: Int): LongArray?
    private external fun updateParametersNative(handle: Long, revision: Long, jsonPatch: String): Int
    private external fun captureEventNative(handle: Long, event: CaptureEvent): Int
    private external fun requestStateResultNative(handle: Long, result: RequestStateResult): Int
    private external fun setPreviewSurfaceNative(
        handle: Long,
        surface: Surface,
        sensorOrientation: Int,
    ): Int
    private external fun evictZslFrameNative(handle: Long, frameId: Long): Int
    private external fun flushSessionNative(handle: Long): Int
    private external fun closeSessionNative(handle: Long)
    private external fun probeVulkanNative(): Boolean

    companion object {
        private const val TAG = "DcamNodeRuntime"
        private const val CLIENT_PACKAGE = "com.dcam"
        private const val PERMISSION_RUN_NODE = "com.dcamnoderuntime.permission.RUN_NODE"
        private const val RUNTIME_ABI = 1
        private const val RUNTIME_VERSION = "1.1.0"
        private const val STATUS_OK = 0
        private const val STATUS_INVALID_ARGUMENT = -1
        private const val STATUS_BAD_STATE = -4
        private const val STATUS_PROCESSING = -5
        private const val STATUS_ABI_MISMATCH = -6
        private const val STATUS_NOT_FOUND = -7
        private const val STATUS_BUSY = -8
        private const val STATUS_DEPENDENCY = -10
        private const val STATUS_IO = -11
        private const val FRAME_RELEASE = 0
        private const val CAPTURE_SHUTTER = 1
        private const val CAPTURE_ZSL_EVICTED = 5
        private const val MAX_ZSL_DEPTH = 16
        private const val PROCESS_TIMEOUT_MS = 2_500L
        private const val STILL_PROCESS_TIMEOUT_MS = 20_000L
        private const val STILL_RAW_STREAM_ID = 3
        private const val CONTROL_TIMEOUT_MS = 2_000L
        private const val SESSION_CLOSE_TIMEOUT_MS = STILL_PROCESS_TIMEOUT_MS + 5_000L
        private const val MAX_DEPENDENCIES = 32
        private const val MAX_NODES_PER_PRESET = 32
        private const val MAX_EDGES_PER_PRESET = 128
        private const val MAX_CONFIG_SECTION_CHARS = 512 * 1024
        private const val MAX_COMBINED_CONFIG_CHARS = 900 * 1024
        private val SONAME_PATTERN = Regex("lib[A-Za-z0-9_.+-]+\\.so")
        private val SHA256_PATTERN = Regex("[0-9a-f]{64}")
        private val NODE_TYPE_PATTERN = Regex("[a-z0-9]+(\\.[a-z0-9_-]+)+")
        private val INSTANCE_ID_PATTERN = Regex("[A-Za-z][A-Za-z0-9_-]*")
        private val NODE_ENDPOINT_PATTERN = Regex("[A-Za-z][A-Za-z0-9_-]*\\.[a-z][a-z0-9_-]*")

        init {
            System.loadLibrary("dcam_node_loader")
        }
    }
}
