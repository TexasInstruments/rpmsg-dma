# Audio Monitoring and Debug Console (audmon.py)

## Overview
```
audmon.py is a Python GUI tool designed for real-time audio DSP offload monitoring and visualization.
It connects to a remote embedded device via UART or IP, processes audio signal metrics, and displays
key performance indicators such as average amplitude, latency, CPU/DSP load, and FFT-based input/output spectrum analysis.
It supports one channel audio visualiztion and monitoring with 256 samples/channel input/output audio frame.

Note: Input/output audio spectrum plotting is only supported in ip mode. 
      UART mode supports only metrics and command interface, not audio data streams.
```
## Features
```
• Real-time visualization of:
	o Frame-level average amplitude (dBFS)
	o CPU and DSP latency tracking
	o CPU and DSP load
	o Input/output FFT spectrum (only in IP mode)
• Command interface to toggle features (e.g., enabling/disabling FFT filter)
• Ctrl+S based save for graphs and log summaries
• Summary labels for min/max/avg stats per run
• Fullscreen Tkinter GUI with Matplotlib embedding
```
## Requirements
```
• Python 3.x
• Dependencies
	o pip install matplotlib numpy pyserial  scipy
	o Target device that sends audio metrics over TCP/serial in defined protocol format
```
## Usage
```
Command Line: python audmon.py <mode> <IP address | COM port>
	Modes:
	1. ip: Connect over TCP/IP (supports all features including audio input/output plots)
	2. uart: Connect over UART (no spectrum plots, only metrics)
	Examples:
		python audmon.py ip 192.168.1.100
		python audmon.py uart COM5

```
## GUI Controls
```
• Filter Toggle: Enables/disables FFT filtering in the device
• Live Summary: Continuously updated stats for frames, latency, amplitude, CPU/DSP load
• Audio Spectrum: Visual display of input and output frequency spectrum (only IP mode)
• Logs
	o Save Data (Ctrl+S): Saves: 
		Summary text file
		Input/output sample CSVs
		Combined graph snapshot (PNG)
	o Directory Structure After Save
		Each run saves logs in a timestamped folder like logs_20250630_140023/ with:
			audmon_graphs_<timestamp>.png : GUI screenshot
			audmon_summary_<timestamp>.txt : Metrics summary
			input_samples_<timestamp>.csv : Raw input audio buffer
			output_samples_<timestamp>.csv : Raw output audio buffer
```
## Protocol Summary
```
• Data Port (8888): Receives real-time logs and metrics
• Command Port (8889): Sends commands like SET FFT FILTER 1
• Input Data Port (8890): Receives audio data with header INPT
• Output Data Port (8891): Receives audio data with header OUTP
```
## Limitations
```
• UART mode is restricted to metrics only; no audio waveform visualization.
• Audio buffers are limited to 2048 samples.
• No error recovery if device sends malformed frames or disconnects abruptly.
```

## Known Issues
```
• In UART mode Filter Toggle button some time may not work due to log traffic in uart channel
```
