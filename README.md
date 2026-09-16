# RPMsg DMA: DSP Offload and Edge AI on AM62D

`rpmsg-dma` provides a Linux user-space library and reference applications for
moving data between the Arm cores and the C7x DSP on TI AM62D. It combines
RPMsg control messages with DMA-BUF buffers allocated from Linux DMA Heaps, so
applications can build DSP compute, real-time audio, and Edge AI pipelines
without copying large payloads through RPMsg.

The repository contains:

- `libti_rpmsg_dma.so`: RPMsg, DMA-BUF, and remoteproc firmware-management APIs.
- DSP examples: audio filtering, 2D FFT, and a real-time biquad signal chain.
- Edge AI pipelines: DSP preprocessing/postprocessing combined with TVM model inference on C7x.
- Command-line, host-utility, and web-portal integration examples.

> This is example software for TI AM62D Linux SDK environments. The required
> C7x firmware, device-tree configuration, model artifacts, and device paths
> must match the target image.

## Examples

| Example | Executable | What it demonstrates | Interface |
| --- | --- | --- | --- |
| [Audio offload](example/audio_offload/) | `rpmsg_audio_offload_example` | 8-channel, 48-kHz audio processing with FFT band-pass filtering and Arm/C7x execution selection | CLI, Python monitor, or web portal |
| [2D FFT](example/2dfft/) | `rpmsg_2dfft_example` | C7x 2D FFT using reference input/output data, with pass/fail and performance reporting | CLI or web portal |
| [Signal-chain biquad](example/sigchain_biquad/) | `rpmsg_sigchain_biquad_example` | Real-time 3-stage parametric EQ, codec control, and C7x load/cycle/throughput metrics | CLI, Python GUI, or web portal |
| [Edge AI](example/edge-ai/) | `rpmsg_inference_example` | JSON-defined DSP + TVM pipelines for speech enhancement, audio classification, STFT/ISTFT, and direct inference | File, ALSA, raw stream, or web portal |

## Architecture

```text
                             Linux on Arm
+-------------------------------------------------------------------+
| Applications                                                      |
|  DSP examples                 Edge AI pipeline runner              |
|  - audio offload              - JSON pipeline configuration       |
|  - 2D FFT                     - WAV / ALSA / stdin input           |
|  - biquad signal chain        - model-daemon client               |
|             |                              |                      |
|             +---------------+--------------+                      |
|                             v                                     |
|                  libti_rpmsg_dma.so                               |
|             RPMsg control | DMA-BUF data | firmware switching     |
+---------------------------+---------------------------------------+
                            |
              /dev/rpmsg_char* | /dev/dma_heap/* | remoteproc
                            |
+---------------------------v---------------------------------------+
|                         C7x DSP                                   |
|  Generic DSP task (audio/STFT/FFT)  |  TVM inference task         |
+-------------------------------------------------------------------+
```

RPMsg carries commands and buffer metadata. Bulk input, output, and parameter
data resides in DMA-BUF allocations shared with the C7x firmware.

## Repository Layout

```text
library/
  include/                         Public headers
  src/                             RPMsg, DMA-BUF, and firmware-loader code
example/
  audio_offload/                   Multichannel audio filtering example
  2dfft/                           2D FFT validation example
  sigchain_biquad/                 Real-time parametric EQ example
  edge-ai/                         DSP + TVM Edge AI pipeline framework
```

## Library APIs

The public headers are installed from `library/include/`. The principal API groups are:

- RPMsg: `init_rpmsg()`, `send_msg()`, `recv_msg()`, and `cleanup_rpmsg()`.
- DMA-BUF: `dmabuf_heap_init()`, `dmabuf_sync()`, and `dmabuf_heap_destroy()`.
- Firmware control: `switch_firmware()` stops the remote processor, changes
  the active firmware link, and starts the processor again.

See the public headers for current parameter types and return-value details.

### RPMSG, DMABUF & FW LOADER API Reference

```
RPMSG API Endpoints

init_rpmsg
  Description: Initializes the RPMSG communication.
  Parameters:
    rproc_id: The ID of the remoteproc device.
    rmt_ep: The remote endpoint number.
  Returns: The file descriptor of the RPMSG channel.
  Example: int fd = init_rpmsg(1, 2);

send_msg
  Description: Sends a message over the RPMSG channel.
  Parameters:
    fd: The file descriptor of the RPMSG channel.
    msg: The message to be sent.
    len: The length of the message.
  Returns: The number of bytes sent.
  Example: int sent = send_msg(fd, "Hello, world!", 13);

recv_msg
  Description: Receives a message over the RPMSG channel.
  Parameters:
    fd: The file descriptor of the RPMSG channel.
    len: The length of the message to be received.
    reply_msg: A pointer to a buffer to store the received message.
    reply_len: A pointer to store the length of the received message.
  Returns: 0 on success, -1 on error.
  Example: int len = 0; char reply[1024]; int ret = recv_msg(fd, 1024, reply, &len);

cleanup_rpmsg
  Description: Cleans up the RPMSG channel and releases its resources.
  Parameters:
    fd: The file descriptor of the RPMSG channel.
  Example: cleanup_rpmsg(fd);

DMABUF API Endpoints

dmabuf_heap_init
  Description: Initializes a DMA heap and returns its file descriptor.
  Parameters:
    heap_name: The name of the DMA heap to initialize.
    buffer_size: The size of the buffer to allocate.
    rproc_dev: The path to the remoteproc device.
    params: A pointer to a struct dma_buf_params object that will hold the DMA buffer parameters.
  Returns: The file descriptor of the DMA heap.
  Example: int fd = dmabuf_heap_init("heap_name", 1024, "/dev/remoteproc", &params);

dmabuf_sync
  Description: Indicates the start or end of a map access session for a DMA buffer.
  Parameters:
    fd: The file descriptor of the DMA buffer.
    start_stop: A flag indicating whether to start (1) or stop (0) the map access session.
  Returns: The result of the ioctl system call.
  Example: int ret = dmabuf_sync(fd, 1);

dmabuf_heap_destroy
  Description: Destroys a DMA buffer and releases its resources.
  Parameters:
    params: A pointer to a struct dma_buf_params object that holds the DMA buffer parameters.
  Example: dmabuf_heap_destroy(&params);

FW Loader API

switch_firmware
  Description: Switches to a new firmware by stopping the current firmware, updating the symlink
               to the new firmware, and then starting the new firmware.
  Parameters:
    new_fw: Path to the new firmware file to load.
    fw_link: Path to the symlink that points to the current firmware.
    remote_proc_state_path: Path to the file that controls the state of the remote processor.
  Return Value:
    0: Success
   -1: Failure
```

## Prerequisites

### Target software

- TI AM62D Linux SDK with C7x remoteproc/RPMsg support.
- Linux `remoteproc`, `rpmsg_char`, and DMA Heap support.
- TI `ti-rpmsg-char` user-space library.
- C7x firmware matching the selected example or Edge AI pipeline.
- A reserved/shared DMA heap, such as `linux,cma`, configured for the target.

### Build dependencies

- CMake 3.10 or newer and a C/C++ cross-toolchain.
- `pkg-config`, ALSA, libsndfile, FFTW3, and json-c development packages.
- Neo-TVM headers and an Arm64 `libtvm_runtime.so` when building Edge AI.

The exact package names depend on the host distribution or Yocto SDK.

## Build

All targets are enabled by default. Because Edge AI requires Neo-TVM, provide
`TVM_ROOT` (or `NEO_TVM_PATH`) when building the complete repository:

```bash
cmake -S . -B build -DTVM_ROOT=/path/to/neo-tvm
cmake --build build
sudo cmake --install build
```

For a DSP-only build, disable the Edge AI target:

```bash
cmake -S . -B build -DBUILD_EDGE_AI_EXAMPLE=OFF
cmake --build build
sudo cmake --install build
```

| CMake option | Target |
| --- | --- |
| `BUILD_LIB` | Shared RPMsg-DMA library |
| `BUILD_AUDIO_OFFLOAD_EXAMPLE` | Audio offload example |
| `BUILD_2DFFT_OFFLOAD_EXAMPLE` | 2D FFT example |
| `BUILD_SIGNAL_CHAIN_BIQUAD_EXAMPLE` | Signal-chain biquad example |
| `BUILD_EDGE_AI_EXAMPLE` | Edge AI pipelines and TVM model daemon |

## Installed Components

Depending on the enabled targets, `cmake --install` installs:

- Library and headers under the configured GNU install directories.
- Example executables under `${CMAKE_INSTALL_PREFIX}/bin`.
- C7x example firmware under `${CMAKE_INSTALL_PREFIX}/lib/firmware`.
- DSP test/configuration assets under `${CMAKE_INSTALL_PREFIX}/share` and `${CMAKE_INSTALL_PREFIX}/etc`.
- Edge AI JSON files, labels, and inputs under `${CMAKE_INSTALL_PREFIX}/share/tvm_inference`.
- `tvm-model-daemon.service` and `tvm-model-preload.service` in the detected systemd system-unit directory.

Model artifacts referenced by the Edge AI JSON files must be deployed
separately under `/usr/share/tvm_inference/artifacts/<model>`.

## Quick Start

Verify that the expected C7x remote processor and RPMsg endpoints are present,
then follow the README for the chosen example. Typical commands are:

```bash
rpmsg_2dfft_example
rpmsg_audio_offload_example

rpmsg_inference_example \
  /usr/share/tvm_inference/json/pipeline_speech_enhancement.json

rpmsg_inference_example \
  /usr/share/tvm_inference/json/pipeline_audio_classification_yamnet.json \
  --device plughw:0,0
```

Only one demo that owns or reloads the C7x should run at a time.

## Web Portal Support

The AM62D integration in
[`TexasInstruments/webserver-oob-demo`](https://github.com/TexasInstruments/webserver-oob-demo)
provides browser pages for the audio-offload, 2D FFT, signal-chain biquad,
speech-enhancement, audio-classification, and TVM-inference demonstrations.

The webserver does not build this repository. Install the required binaries,
firmware, configurations, Edge AI pipeline files, model artifacts, and audio
assets on the EVM first. The portal then starts/stops the executables, bridges
their logs and TCP data to WebSockets, and presents controls and metrics in the
browser. See each example README for its specific integration details.

## Troubleshooting

- List remote processors: `grep . /sys/class/remoteproc/remoteproc*/name`
- Check remoteproc state: `grep . /sys/class/remoteproc/remoteproc*/state`
- Inspect DSP trace (debugfs required): `cat /sys/kernel/debug/remoteproc/remoteproc*/trace0`
- Confirm RPMsg devices: `ls /dev/rpmsg*`
- Confirm DMA heaps: `ls /dev/dma_heap/`
- Confirm runtime linking: `ldd /usr/bin/rpmsg_inference_example`
- Follow the model daemon: `journalctl -u tvm-model-daemon -f`

If an RPMsg endpoint cannot be opened, first confirm that the loaded C7x
firmware exports the endpoint expected by the application or pipeline JSON.

## License

BSD-3-Clause. See [LICENSE](LICENSE).
