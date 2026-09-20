# Dcam Node Runtime

This APK is the unprivileged execution host for Dcam Node ABI.
It intentionally declares no internet, camera, audio, media-read, or storage permission.

The Runtime APK is mandatory. Dcam completes the ABI, UID, process-isolation,
and Vulkan handshake before opening `CameraDevice`. A missing service, rejected
binding, Runtime crash, or stock-node load failure closes Camera2 and produces
an explicit camera error. Preview and photo processing never fall back to the
Dcam process.

The canonical ABI, AIDL, schemas, samples, compatibility tests, and normative
Markdown specifications live in the sibling Dcam repository:

- `../Dcam/node-sdk`
- `../Dcam/docs/README.md`

Runtime compiles AIDL and the C header directly from that SDK. Its loader accepts
module/dependency FDs, resolves only `dcam_node_get_api`, serializes calls per
session, passes `AHardwareBuffer*` without pixel copies, presents preview through
NDK SurfaceControl, and reports release fences/dispositions back to Dcam.

Build the arm64 debug APK with:

```sh
./gradlew :app:assembleDebug
```
