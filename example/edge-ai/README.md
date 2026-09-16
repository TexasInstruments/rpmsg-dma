# Edge AI DSP + TVM Pipeline Example

This example runs configurable audio and tensor pipelines across Linux and the
C7x DSP on TI AM62D. DSP stages use the generic C7x RPMsg service; inference
stages use the C7x TVM service. JSON files define the stage sequence, endpoint
configuration, tensor shapes, model artifacts, and input data.

## Capabilities

- Auto-detects the C7x remote processor named `7e000000.dsp`.
- Uses endpoint 13 for generic DSP processing and endpoint 20 for TVM inference.
- Shares bulk data with C7x through the RPMsg-DMA library and DMA-BUF.
- Accepts WAV files, a live ALSA capture device, raw S16_LE PCM on stdin, or a binary tensor.
- Supports a persistent TVM model daemon to avoid reloading artifacts for every run.
- Provides installable systemd units and web-portal-compatible output.

## Included Pipelines

| JSON file | Pipeline | Stages | Default input |
| --- | --- | --- | --- |
| `pipeline_speech_enhancement.json` | GCRN speech enhancement | STFT, deinterleave, TVM, interleave, ISTFT | 16-kHz WAV |
| `pipeline_audio_classification_yamnet.json` | YAMNet, 521 classes | Log-mel/STFT preprocessing, TVM | 16-kHz WAV, ALSA, or stdin |
| `pipeline_audio_classification_vggish.json` | VGGish/UrbanSound8K, 10 classes | Log-mel/STFT preprocessing, TVM | 16-kHz WAV, ALSA, or stdin |
| `pipeline_stft_istft.json` | DSP round trip | STFT, deinterleave, interleave, ISTFT | 16-kHz WAV |
| `pipeline_tvm_inference.json` | Direct tensor inference | TVM | Binary tensor |

The supplied JSON files use C7x processor ID 8 and DSP endpoint 13. Deploy
firmware that also exposes the TVM task expected by the runtime. Adjust the JSON
when the target image uses different IDs, paths, shapes, or stage parameters.

## Prerequisites

- AM62D Linux image with remoteproc, RPMsg-char, and DMA Heap support.
- C7x dual-task firmware providing the generic DSP and TVM services.
- `libti_rpmsg_dma.so` and TI `ti-rpmsg-char`.
- Neo-TVM headers and an Arm64 `libtvm_runtime.so`.
- ALSA, libsndfile, and json-c libraries.
- Model artifacts deployed at the paths named by each pipeline, by default:

```text
/usr/share/tvm_inference/artifacts/gcrn/
/usr/share/tvm_inference/artifacts/yamnet/
/usr/share/tvm_inference/artifacts/vggish/
```

Model artifacts are not installed by this example's CMake rules.

## Build and Install

From the repository root:

```bash
cmake -S . -B build -DTVM_ROOT=/path/to/neo-tvm
cmake --build build
sudo cmake --install build
```

Alternatively, set `NEO_TVM_PATH`. Use `TVM_RUNTIME_LIB` if the runtime library
is not located at `${TVM_ROOT}/libtvm_runtime.so`:

```bash
export NEO_TVM_PATH=/path/to/neo-tvm
cmake -S . -B build \
  -DTVM_RUNTIME_LIB=/path/to/aarch64/libtvm_runtime.so
cmake --build build
```

This target installs:

- `rpmsg_inference_example` and `tvm_model_daemon` under `/usr/bin` with the default prefix.
- Pipeline JSON, labels, and sample inputs under `/usr/share/tvm_inference/`.
- `tvm-model-daemon.service` and `tvm-model-preload.service` in the systemd system-unit directory.

## Run

### File input

```bash
rpmsg_inference_example \
  /usr/share/tvm_inference/json/pipeline_speech_enhancement.json

rpmsg_inference_example \
  /usr/share/tvm_inference/json/pipeline_audio_classification_yamnet.json \
  --input-file /tmp/test.wav
```

### Live ALSA classification

`--device` is supported by audio-classification pipelines:

```bash
rpmsg_inference_example \
  /usr/share/tvm_inference/json/pipeline_audio_classification_yamnet.json \
  --device plughw:0,0
```

### Raw PCM stream

`--stream` is supported by audio-classification pipelines and expects mono,
16-kHz, S16_LE PCM:

```bash
arecord -t raw -f S16_LE -c 1 -r 16000 | \
  rpmsg_inference_example \
    /usr/share/tvm_inference/json/pipeline_audio_classification_yamnet.json \
    --stream
```

Use `--debug` for per-batch logs, `--version` for build information, and
`--help` for the current command-line syntax.

## Model Daemon

`tvm_model_daemon` keeps a TVM module loaded and serves requests through a Unix
domain socket, reducing repeated artifact-loading overhead. To start it at boot:

```bash
systemctl enable --now tvm-model-daemon.service
journalctl -u tvm-model-daemon -f
```

The separate preload service invokes `rpmsg_inference_example --preload`. Use
the service that matches the integration strategy of the target image; do not
enable competing model owners without validating their lifecycle.

## Web Portal Integration

The AM62D portal in
[`webserver-oob-demo`](https://github.com/TexasInstruments/webserver-oob-demo)
uses this executable for speech enhancement, audio classification, and direct
TVM inference. The portal captures or selects audio, launches the appropriate
pipeline JSON, collects structured output, and displays results in the browser.

Before starting the webserver, verify at least:

```bash
test -x /usr/bin/rpmsg_inference_example
test -f /usr/share/tvm_inference/json/pipeline_speech_enhancement.json
test -f /usr/share/tvm_inference/json/pipeline_audio_classification_yamnet.json
test -d /usr/share/tvm_inference/artifacts
```

The webserver and its demo plugins are built and installed from the separate
webserver repository; they are not installed by `rpmsg-dma`.

## JSON Pipeline Format

The common top-level fields are:

- `pipeline_type`: `speech_enhancement`, `audio_classification`, `stft_istft`, or `tvm_only`.
- `input_file`: default WAV or binary input path.
- `artifacts_path`: model directory for TVM stages.
- `labels_path` and `num_classes`: classification metadata.
- `dsp_config`: C7x processor ID and generic-task endpoint.
- `stages`: ordered DSP or TVM operations with message-specific parameters.

Copy the nearest supplied JSON file when adding a pipeline and keep tensor
shape, frame count, hop size, model elements, and C7x firmware configuration consistent.

## Troubleshooting

```bash
grep . /sys/class/remoteproc/remoteproc*/name
grep . /sys/class/remoteproc/remoteproc*/state
ls -l /dev/rpmsg* /dev/dma_heap/*
cat /sys/kernel/debug/remoteproc/remoteproc*/trace0
ldd /usr/bin/rpmsg_inference_example
find /usr/share/tvm_inference -maxdepth 3 -type f
```

If connection to endpoint 13 or the TVM service fails, confirm that the active
C7x firmware contains both tasks. If inference fails after connection, check
the model artifact path and the input shape in the selected JSON.
