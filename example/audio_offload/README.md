# Audio Offload Example
```
This example demonstrates how to offload 8-channel audio processing from Linux user-space
to the C7x DSP on TI AM62D using TI’s RPMsg-char framework and Linux DMA Heaps.
```
## Features
```
- Real-time audio processing on the C7x DSP
- FFT-based filtering (Filtering ON/OFF control)
- Dynamic firmware switching between echo test and audio filter firmware
- UART & Ethernet based monitoring & control for enabling/disabling Filter (Band pass, range 2k-8k)
```
## Prerequisites
```
- Linux kernel with:
  - remoteproc & rpmsg_char drivers enabled
  - DMA Heap support (e.g. linux,cma heap)
- TI’s ti-rpmsg-char user-space library (installed or in your SDK)
- ALSA user-space tools (aplay, arecord)
- Python 3 for the host utility (requires socket, struct, threading modules)
- A built C7x firmware image for both echo and filter modes
```
## Building
```
Run the following commands from the root:
cmake -S . -B build
cmake --build build
- This will build:
  - The shared library (`libti_rpmsg_dma.so`)
  - The example application (`rpmsg_audio_offload_example`)
To install the built files (requires root privileges):
sudo cmake --install build
This install:
- The library to `/usr/lib` (by default)
- The example binary to `/usr/bin`
- The configuration file (`dsp_offload.cfg`) to `/etc`
- The sample audio file (`sample_audio.wav`) to `/usr/share/`
- The DSP Test firmware file (dsp_audio_filter_offload.c75ss0-0.release.strip.out) to `/usr/lib/firmware`

To build only the example, use:
cmake -S . -B build \
  -DBUILD_EDGE_AI_EXAMPLE=OFF \
  -DBUILD_2DFFT_OFFLOAD_EXAMPLE=OFF \
  -DBUILD_SIGNAL_CHAIN_BIQUAD_EXAMPLE=OFF
```
## Configuration (dsp_offload.cfg)
```
All parameters are provided via the config/dsp_offload.cfg file. 
### Example content:

#Linux DSP Offload Example Application Configuration
PCM_DEVICE=default
UART_DEVICE=/dev/ttyS2
RPROC_DEV_NAME=/dev/remoteproc0
DMA_HEAP_RESERVED=linux,cma
DATA_SIZE=4096
PARAM_SIZE=256
FW_LINK_PATH=/lib/firmware/am62d-c71_0-fw
C7_OLD_FW_PATH=/lib/firmware/ti-ipc/am62dxx/ipc_echo_test_c7x_1_release_strip.xe71
C7_NEW_FW_PATH=/lib/firmware/dsp_audio_filter_offload.c75ss0-0.release.strip.out
C7_STATE_PATH=/sys/class/remoteproc/remoteproc0/state
C7_PROC_ID=8
REMOTE_ENDPT=14
SAMPLE_AUDIO_FILE=/usr/share/sample_audio.wav (8ch audio wav file)
DSP_EXEC_MODE=1
HOST_ETH_INTERFACE=1
FILTER_ENABLE=1
AUDIO_LOGGING_ENABLE=0

PCM_DEVICE: ALSA device for audio capture/playback
UART_DEVICE: UART for host communication
RPROC_DEV_NAME: Remoteproc control device
DMA_HEAP_RESERVED: DMA heap name (e.g. linux,cma)
DATA_SIZE / PARAM_SIZE: Buffer sizes for audio & control parameters
FW_LINK_PATH: Symlink to the “active” firmware for DSP
C7_OLD_FW_PATH / C7_NEW_FW_PATH: Paths to the echo test and filter firmware images
C7_STATE_PATH: Remoteproc state file (state)
C7_PROC_ID / REMOTE_ENDPT: RPMsg remoteproc ID & endpoint
SAMPLE_AUDIO_FILE: Path to the test WAV file
DSP_EXEC_MODE: 0 = processing on ARM, 1 = processing on C7
HOST_ETH_INTERFACE: 1 to enable Ethernet control utility
FILTER_ENABLE: 1 to enable filtering, 0 to bypass
AUDIO_LOGGING_ENABLE: 1 to save raw audio data to file(/tmp/wave_xx_ch0.txt)
```
## Running the Example
```
1. Ensure your DSP firmware images are installed under /lib/firmware/ as referenced in the config.
2. Launch the example:
	rpmsg_audio_offload_example
3. Monitor logs via UART console/dmesg.
```
## Host-Side Utility
```
Refer: https://github.com/TexasInstruments/rpmsg-dma/blob/REL.11.01/example/audio_offload/host%20utility/README
```

## Web Portal Integration

The AM62D portal in
[`webserver-oob-demo`](https://github.com/TexasInstruments/webserver-oob-demo)
can launch and stop `rpmsg_audio_offload_example` and display input/output
spectra plus Arm/C7x performance metrics in a browser. It replaces the Python
host monitor for portal-driven demonstrations.

The application acts as a local TCP server. The webserver plugin connects to:

| Port | Purpose |
| --- | --- |
| 8888 | Logs and performance metrics |
| 8889 | Commands, including start/stop and filter control |
| 8890 | Tagged input PCM frames |
| 8891 | Tagged output PCM frames |

Install this example, its configuration, sample audio, and matching firmware
on the EVM before installing or starting the webserver. With the default
webserver configuration, the binary must be available at
`/usr/bin/rpmsg_audio_offload_example`. The webserver manages the process and
connects to it over `127.0.0.1`; do not run a second instance manually.

Open the portal on port 3000 of the EVM and select **Audio DSP Offload**. See
the webserver repository for portal build and deployment instructions.
