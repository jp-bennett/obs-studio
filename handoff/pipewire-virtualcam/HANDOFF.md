# Handoff: PipeWire virtual camera

Branch: `claude/exciting-tesla-jnni69` on `jp-bennett/obs-studio`.

**Do not open a pull request against `obsproject/obs-studio` and do not
comment on or link upstream issues/PRs.** OBS does not accept AI-written
contributions. This work lives on the fork only. Avoid `#1234` style
references in commits, because a push to GitHub turns those into
"mentioned this" events on upstream.

## Goal

On a Linux desktop (Fedora laptop), OBS should publish its output as a
PipeWire video source that browsers can use as a webcam in a web call,
without the v4l2loopback kernel module.

## What was done

Two commits on top of upstream master `f1c0a1164`:

1. `linux-pipewire: Add PipeWire virtual camera output`
   - New `plugins/linux-pipewire/virtualcam.{c,h}` registers the output
     `pipewire_virtualcam_output` (display name "Virtual Camera (PipeWire)").
   - On start it connects straight to the PipeWire daemon (not a portal)
     and creates a stream node:
     `node.name=obs_virtual_camera`, `node.description="OBS Virtual Camera"`,
     `media.class=Video/Source`, `media.role=Camera`. It is a driver node
     with no autoconnect, so it waits for consumers.
   - It offers I420, NV12 and YUY2 at the canvas size plus 1080/720/480/360
     heights below it (same aspect ratio, even widths). Each one is a
     separate EnumFormat with a fixed size.
   - OBS delivers frames at canvas size in NV12 or I420. If the consumer
     picks something else, frames are converted with the libobs
     `video_scaler`. Combinations the scaler can't create are not offered.
   - Output is always limited range, BT.709 (BT.601 if the canvas is 601),
     matching what the v4l2 virtual camera does.
   - `pipewire.c` / `pipewire.h`: `obs_pipewire_connect()` (no portal fd),
     `obs_pipewire_get_core()`, `obs_pipewire_get_thread_loop()`, and
     cleanup when connecting fails.
   - Built only with PipeWire >= 0.3.34 (`pw_stream_trigger_process`).
2. `frontend: Allow choosing the virtual camera backend`
   - The UI still has one "Start Virtual Camera" button. The backend is
     `virtualcam_output` (v4l2loopback) or `pipewire_virtualcam_output`,
     whichever is registered, with v4l2loopback preferred when both exist.
   - With both available, the virtual camera settings dialog (gear next to
     the button) shows a "Backend" dropdown. The choice is saved in the
     user config, `[VirtualCam] OutputId`. Changing it while running
     restarts the camera.
   - On a Fedora machine without v4l2loopback (the default), PipeWire is
     the only backend, so the dropdown stays hidden and it's used directly.

Prior art used: upstream PR 5377 by columbarius (draft "PipeWire Virtual
Camera", last reworked March 2024). Not reused from it:
- It advertised a size *range* (1x1 to 8192x4320), which libwebrtc
  ignores, so browsers would have seen no usable formats.
- It only offered RGBA.
- It copied private pipewire.c structs into a header.
- It had an `if (old = PW_STREAM_STATE_STREAMING)` assignment bug.
- It replaced the v4l2 virtual camera outright.

### Why the format choices (libwebrtc details)

Chrome and Firefox both use libwebrtc's
`modules/video_capture/linux/{pipewire_session,video_capture_pipewire}.cc`:

- It lists nodes whose `media.role` is `Camera` and that have a
  `node.description`.
- It skips EnumFormats whose size or pixel format isn't a single fixed
  value.
- `maxFPS` is the integer part of the highest framerate. It then requests
  exactly `maxFPS/1`. So for 29.97 fps (30000/1001) the node also offers
  29/1 and 30/1 in an Enum, otherwise negotiation fails.
- It reads only `datas[0]` with `chunk->size`, as one tightly packed
  frame: I420/NV12 stride = width, YUY2 stride = 2*width.
- It dereferences the `SPA_META_Header` without a NULL check, so the node
  always provides that meta.

## Already verified in the cloud container (Ubuntu 24.04, PipeWire 1.0.5)

- `linux-pipewire` builds with `-Werror`.
- Headless libobs harness (red color source) plus
  `handoff/pipewire-virtualcam/webrtc-like-consumer.c`, a test client that
  behaves like libwebrtc:
  - 1280x720@30 canvas: NV12/I420/YUY2 at 1280x720, 854x480 and 640x360
    all pass. Chunk sizes and strides are correct, and pixel values are
    correct for pure red in BT.709 limited range (Y=63 U=102 V=240).
  - 1920x1080@29.97 canvas: the client requests 30/1 as libwebrtc would,
    and it works.
  - Full-range canvas still produces limited-range output.
  - Two consumers on the same format at the same time work.
  - Stop, start again, and shutdown work with no crash and no added leaks.
  - With no PipeWire daemon, start fails with the message "Could not
    connect to PipeWire. Make sure the PipeWire service is running."
  - GStreamer works:
    `gst-launch-1.0 pipewiresrc target-object=obs_virtual_camera ! video/x-raw,format=YUY2,width=640,height=360 ! ...`
    Without a caps filter, `pipewiresrc` connects with no format and
    WirePlumber reports "target not found". That's GStreamer, not the node.
- **The UI was compiled but not run in the container.** See "UI compile
  status" at the end.

## Build on Fedora

Master currently requires FFmpeg >= 8.0, MbedTLS 3.x, Qt >= 6.9 (the
plugin manager uses `QString::arg(std::string_view)`), and
PipeWire >= 0.3.34. Check what the Fedora release provides; RPM Fusion
has newer FFmpeg if Fedora's `ffmpeg-free` is too old.

Suggested dependencies (adjust names to what dnf finds):

```sh
sudo dnf install cmake ninja-build gcc gcc-c++ git pkgconf \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtsvg-devel \
  pipewire-devel libdrm-devel glib2-devel mesa-libEGL-devel mesa-libGL-devel \
  libX11-devel libxcb-devel libXcomposite-devel libXinerama-devel libxkbcommon-devel \
  wayland-devel pulseaudio-libs-devel alsa-lib-devel jack-audio-connection-kit-devel \
  libv4l-devel systemd-devel libcurl-devel jansson-devel zlib-devel \
  freetype-devel fontconfig-devel speexdsp-devel x264-devel mbedtls-devel \
  luajit-devel swig nlohmann-json-devel websocketpp-devel asio-devel \
  qrcodegencpp-devel uthash-devel simde-devel libva-devel pciutils-devel \
  oneVPL-devel librist-devel srt-devel libdatachannel-devel \
  ffmpeg-devel   # or ffmpeg-free-devel; must be >= 8.0
```

If FFmpeg 8 isn't packaged, build it into a prefix (this is what the
cloud session did):

```sh
git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg && ./configure --prefix=$HOME/opt/ffmpeg8 --enable-shared --disable-static \
  --disable-programs --disable-doc --enable-gpl && make -j$(nproc) && make install
```

Then pass `-DCMAKE_PREFIX_PATH=$HOME/opt/ffmpeg8` and
`PKG_CONFIG_PATH=$HOME/opt/ffmpeg8/lib/pkgconfig` to CMake. Do the same
for MbedTLS 3.6 if needed (tag `v3.6.4`, `-DUSE_SHARED_MBEDTLS_LIBRARY=ON`).

Configure and build. The `ubuntu` preset works on any Linux; the browser
source needs CEF, so turn it off:

```sh
git submodule update --init --depth 1 plugins/obs-browser plugins/obs-websocket
cmake --preset ubuntu -DENABLE_BROWSER=OFF -DENABLE_AJA=OFF -DENABLE_WEBRTC=OFF \
  -DENABLE_VLC=OFF -DENABLE_SCRIPTING_PYTHON=OFF \
  -DCMAKE_INSTALL_PREFIX=$HOME/opt/obs-vcam
cmake --build build_ubuntu -j$(nproc)
cmake --install build_ubuntu
```

`ENABLE_SCRIPTING_PYTHON=OFF` is only needed if the system Python is
newer than master accepts. Look for
`PipeWire 0.3.34+ virtual camera support` in the configure feature
summary.

Run it with
`LD_LIBRARY_PATH=$HOME/opt/obs-vcam/lib64:$HOME/opt/obs-vcam/lib $HOME/opt/obs-vcam/bin/obs`
(check where the preset put the libs; it uses
`lib/<arch>-linux-gnu`). Using a separate profile/config is a good idea:
`--portable`, or `XDG_CONFIG_HOME=/tmp/obs-test-config`.

## Test plan on the laptop

1. **Node appears.** In OBS, add a source (color source, or a webcam via
   "Video Capture Device (PipeWire)"), then click "Start Virtual Camera".
   - `pw-cli ls Node | grep -A8 obs_virtual_camera` shows
     `media.class = "Video/Source"` and `media.role = "Camera"`.
   - OBS log: `[pipewire-virtualcam] Created stream ... offering N formats`
     and `Virtual camera started (WxH NV12 @ fps)`.
   - `pw-dump` / `pw-top` should list it. `pw-top` shows its frame rate
     while something is consuming it.
2. **Browser-like client.** Build and run the bundled test client:
   ```sh
   gcc -O1 -o /tmp/webrtc-like-consumer handoff/pipewire-virtualcam/webrtc-like-consumer.c $(pkg-config --cflags --libs libpipewire-0.3)
   /tmp/webrtc-like-consumer I420 1280 720 60   # FORMAT W H frames
   ```
   It prints the offered capabilities and must end in `RESULT OK`. Try a
   size from the printed list that isn't the canvas size to exercise
   conversion.
3. **Chromium / Chrome.** Open `chrome://flags/#enable-webrtc-pipewire-camera`,
   set it to Enabled and relaunch. Start the OBS virtual camera *before*
   opening the page (or reload after). Open
   `https://webrtc.github.io/samples/src/content/getusermedia/gum/`, allow
   the camera in the portal dialog, and pick "OBS Virtual Camera".
   `https://webrtc.github.io/samples/src/content/getusermedia/resolution/`
   exercises the different sizes.
4. **Firefox.** In `about:config`, `media.webrtc.camera.allow-pipewire`
   must be `true`, then restart Firefox. Same test pages.
5. **Real call.** Join a test meeting (Jitsi `meet.jit.si`, Google Meet,
   etc.) and pick "OBS Virtual Camera".
6. **Lifecycle.**
   - Stop the virtual camera while the browser is using it: the browser
     should lose the camera, and OBS must not hang or crash.
   - Start it again and re-select it in the browser.
   - Change the virtual camera output type (Program / Scene / Source) in
     the gear dialog while running.
   - Close OBS while the browser is consuming.
7. **Backend selector.** Only if v4l2loopback is also installed
   (`akmod-v4l2loopback` from RPM Fusion): the gear dialog shows
   "Backend". Switching works both while stopped and while running, and
   persists across restarts (`[VirtualCam] OutputId` in `user.ini`).
8. **Flatpak note.** Not a target for this test. A Flatpak OBS can only
   reach the daemon if the sandbox allows the PipeWire socket. Upstream
   discussion stalled on that point; a native build doesn't have the
   problem.

What to report back: the OBS log section with `[pipewire-virtualcam]`
lines, any `RESULT FAIL` from the test client, and which browsers and
versions listed and displayed the camera.

## Known limitations / ideas

- `node.name` is fixed (`obs_virtual_camera`). Two OBS instances running
  the camera at once would both use it, which can confuse libwebrtc, as it
  uses `node.name` as the device id.
- A PipeWire source node negotiates one format at a time. A second
  consumer asking for a *different* size or format than the first one
  fails while the first is connected (same as a real camera).
- No DMA-BUF / zero-copy path; frames are copied into shared memory. It
  is fine for a webcam-sized stream.
- Colour metadata (matrix/range) isn't advertised in the SPA format.
  Consumers assume limited range, which is what is sent.
- libobs bug found along the way and left alone:
  `VIDEO_SCALE_BILINEAR` in `libobs/media-io/video-scaler-ffmpeg.c` maps
  to `SWS_BILINEAR | SWS_AREA`. FFmpeg 8 rejects that ("Exactly one scaler
  algorithm must be chosen"), so the virtual camera uses
  `VIDEO_SCALE_BICUBIC`.

## UI compile status (filled in by the cloud session)

The frontend was compiled with `-Werror` against Qt 6.8.3 built from
source. These compile cleanly, along with every other frontend file that
includes `VCamConfig.hpp`:
- `OBSBasicVCamConfig.cpp`
- `BasicOutputHandler.cpp`
- `OBSBasic.cpp`
- `OBSBasic_VirtualCam.cpp`
- `OBSStudioAPI.cpp`

That pass found and fixed one bug, in the commit "frontend: Fix virtual
camera dialog build".

The `obs` binary could not be linked there, because two unrelated
upstream files need more than that Qt build had:
- `InstalledPluginRow.cpp` needs Qt >= 6.9.
- `OBSApp.cpp` needs Qt's Wayland support.

So the UI flow (button, gear dialog, backend dropdown) still needs its
first real run on the laptop.
