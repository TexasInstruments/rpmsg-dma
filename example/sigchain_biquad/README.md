# Signal Chain Biquad Example
```
This example demonstrates real-time 3-stage parametric equalizer (biquad cascade) processing
on TI C7x DSP with dual operation modes: Network GUI control and Command-line execution.
```
## Features
```
- Real-time Cascade Biquad Parametric EQ Signal Chain Example on C7x DSP
- Dual operation modes: Network GUI and Command-line execution
- TAD5212 DAC and PCM6240 ADC configuration support
- Live performance monitoring (C7x load, cycles, throughput) in both modes
- Enhanced command-line mode with real-time metrics display
- Automatic codec management (integrated with start/stop commands)
- Dynamic C7x firmware switching
- Multi-port TCP server (logs, commands, statistics)
```
## Prerequisites
```
- Linux kernel with:
  - remoteproc & rpmsg_char drivers enabled
  - DMA Heap support (e.g. linux,cma heap)
- TI's ti-rpmsg-char user-space library
- Python 3 with tkinter, matplotlib, numpy for PC GUI utility
- Root privileges for I2C codec control and firmware switching
- C7x firmware: sigchain_biquad_cascade.c75ss0-0.release.strip.out
```
## Building
```
Run the following commands from the root:
cmake -S . -B build
cmake --build build
- This will build:
  - The shared library (`libti_rpmsg_dma.so`)
  - The board application (`signal_chain_biquad_linux_example`)

To install the built files (requires root privileges):
sudo cmake --install build
This installs:
- The library to `/usr/lib` (by default)
- The example binary to `/usr/bin`
- The C7x firmware file (`sigchain_biquad_cascade.c75ss0-0.release.strip.out`) to `/lib/firmware/`

To build only this example:
cmake -S . -B build -DBUILD_SIGCHAIN_BIQUAD_EXAMPLE=ON
```
## Network Protocol
```
The board application provides a multi-port TCP server:
- Port 8888: Log messages from board to PC
- Port 8889: Commands from PC to board (START/STOP)
- Port 8890: Real-time C7x statistics streaming (JSON format)
```
## Running the Example
```
1. Ensure your C7x firmware image is installed under /lib/firmware/.
2. Launch the example:
   GUI mode (network server):     sudo ./signal_chain_biquad_linux_example
   Command-line mode:             sudo ./signal_chain_biquad_linux_example [commands...]
3. Monitor logs via UART console/dmesg.

Available commands for command-line mode:
  start                 # Start audio (auto-init codecs + start demo + display metrics)
  stop                  # Stop audio (stop demo + auto-shutdown codecs + display final metrics)
  sleep:N               # Sleep for N seconds (with real-time performance monitoring)
  help                  # Show usage information

Command-line mode features:
- Auto codec initialization/shutdown with start/stop commands
- Real-time C7x performance metrics display (load, cycles, throughput, status)
- Continuous performance monitoring during sleep commands

Examples:
  sudo ./signal_chain_biquad_linux_example start sleep:10 stop
  sudo ./signal_chain_biquad_linux_example start sleep:5 stop

Return codes:
  0 = All commands executed successfully (PASSED)
  1 = Command sequence failed (FAILED)
```
## Host-Side Utility
```
PC-side Python GUI for network control and monitoring:
python3 host_utility/signal_chain_biquad_example_gui.py <board_ip>

The GUI provides:
- Real-time biquad coefficient control
- Live C7x DSP performance monitoring
- Demo start/stop control
```
