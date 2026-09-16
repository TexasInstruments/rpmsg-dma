# 2DFFT Offload Example
```
This example demonstrates how to offload test data for 2D FFT processing from Linux user-space
to the C7x DSP on TI AM62D using TI’s RPMsg-char framework and Linux DMA Heaps.
```
## Features
```
- Test data (128x128x2) for 2DFFT processing on the C7x DSP
- Output on logging console
- Pass/fail comparison, C7x cycle count, load, and DDR-throughput reporting
```
## Prerequisites
```
- Linux kernel with:
  - remoteproc & rpmsg_char drivers enabled
  - DMA Heap support (e.g. linux,cma heap)
- TI’s ti-rpmsg-char user-space library (installed or in your SDK)
```
## Building
```
Run the following commands from the root:
cmake -S . -B build
cmake --build build
- This will build:
  - The shared library (`libti_rpmsg_dma.so`)
  - The example application (`rpmsg_2dfft_example`)
To install the built files (requires root privileges):
sudo cmake --install build
This install:
- The library to `/usr/lib` (by default)
- The example binary to `/usr/bin`
- Test input data `2dfft_input_data.bin` to `/usr/share/2dfft_test_data/`
- Test expected output data `2dfft_expected_output_data.bin` to `/usr/share/2dfft_test_data/`
- The DSP test firmware file (`fft2d_linux_dsp_offload_example.c75ss0-0.release.strip.out`) to `/usr/lib/firmware`

To build only the example, use:
cmake -S . -B build \
  -DBUILD_EDGE_AI_EXAMPLE=OFF \
  -DBUILD_AUDIO_OFFLOAD_EXAMPLE=OFF \
  -DBUILD_SIGNAL_CHAIN_BIQUAD_EXAMPLE=OFF
```
## Running the Example
```
1. Ensure your DSP firmware images are installed under /lib/firmware/ as referenced in the config.
2. Launch the example:
	rpmsg_2dfft_example
3. Monitor output logs via UART console/dmesg.
```

## Web Portal Integration

The AM62D portal in
[`webserver-oob-demo`](https://github.com/TexasInstruments/webserver-oob-demo)
can run this validation from a browser. Its 2D FFT plugin starts
`/usr/bin/rpmsg_2dfft_example`, streams stdout/stderr to the page, and displays
the final `PASSED` or `FAILED` result and elapsed time.

Before starting the webserver, install the executable, both reference-data
files, and the matching C7x firmware. Do not launch the command-line instance
while the portal owns the C7x. Open the portal on port 3000 of the EVM and
select **2D FFT Offload**; the portal manages run/stop and DSP ownership.
