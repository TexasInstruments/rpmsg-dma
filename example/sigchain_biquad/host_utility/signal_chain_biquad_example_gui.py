#!/usr/bin/env python3
#
#  Copyright (C) 2026 Texas Instruments Incorporated
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
#    Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the
#    distribution.
#
#    Neither the name of Texas Instruments Incorporated nor the names of
#    its contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

"""
Cascade Biquad Parametric EQ Signal Chain Demo GUI (PC-side)
Real-time monitoring and control for TI Cascade Biquad Parametric EQ Signal Chain Example

This application runs on a PC and connects to the AM62D2-EVM over network.
The EVM runs signal_chain_biquad_linux_example which handles firmware switching
and DSP communication.

Network Protocol:
- Port 8888: Log messages from EVM
- Port 8889: Commands to EVM (start/stop with automatic codec control)
- Port 8890: DSP statistics streaming (JSON)

Usage: python3 sigchain_biquad_demo_gui.py <evm_ip_address>
"""

import threading
import tkinter as tk
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import time
import os
import sys
import datetime
import signal
from tkinter.scrolledtext import ScrolledText
from tkinter import ttk, messagebox
from collections import deque
import numpy as np
import socket
import json
import queue

# --- CONFIGURATION ---
LOG_PORT = 8888        # Log messages from EVM
CMD_PORT = 8889        # Commands to EVM
STATS_PORT = 8890      # DSP statistics stream

RETRY_INTERVAL = 3     # Reconnection interval (seconds)
STATS_UPDATE_INTERVAL = 500   # Graph update interval (ms)
GUI_UPDATE_INTERVAL = 200     # Status update interval (ms)
MAX_SAMPLES = 500      # Maximum graph data points

# --- GLOBAL STATE ---
evm_ip = None
connected_log = False
connected_cmd = False
connected_stats = False

# Network sockets
log_socket = None
cmd_socket = None
stats_socket = None

# Data buffers for graphing
frame_numbers = deque(maxlen=MAX_SAMPLES)
c7x_loads = deque(maxlen=MAX_SAMPLES)
cycle_counts = deque(maxlen=MAX_SAMPLES)
throughputs = deque(maxlen=MAX_SAMPLES)
demo_status_history = deque(maxlen=MAX_SAMPLES)

# Current DSP state
current_c7x_stats = {
    "c7xLoad": 0.0,
    "cycleCount": 0,
    "throughput": 0.0,
    "demoRunning": 0,
    "timestamp": 0
}

# Threading control
monitoring_active = False
frame_counter = 0
log_queue = queue.Queue()
stats_queue = queue.Queue()  # Thread-safe stats data transfer
stats_lock = threading.Lock()  # Protect shared data access

# GUI widgets (will be set during GUI creation)
log_console = None
c7x_load_label = None
cycles_label = None
throughput_label = None
demo_label = None
start_stop_btn = None
connect_btn = None
disconnect_btn = None
animation_obj = None

# --- NETWORK COMMUNICATION ---

def connect_to_evm():
    """Connect to EVM application"""
    global log_socket, cmd_socket, stats_socket
    global connected_log, connected_cmd, connected_stats

    if not evm_ip:
        log_message("ERROR: No EVM IP address specified")
        return False

    log_message(f"Connecting to EVM at {evm_ip}...")

    try:
        # Connect to log stream
        log_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        log_socket.settimeout(5.0)  # 5 second connection timeout
        log_socket.connect((evm_ip, LOG_PORT))
        connected_log = True
        log_message("Connected to log stream")

        # Connect to command interface
        cmd_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cmd_socket.settimeout(5.0)  # 5 second connection timeout
        cmd_socket.connect((evm_ip, CMD_PORT))
        connected_cmd = True
        log_message("Connected to command interface")

        # Connect to stats stream
        stats_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        stats_socket.settimeout(1.0)  # 1 second timeout for stats reading
        stats_socket.connect((evm_ip, STATS_PORT))
        connected_stats = True
        log_message("Connected to stats stream")

        # Start background threads
        start_network_threads()

        log_message("Successfully connected to EVM!")
        return True

    except Exception as e:
        log_message(f"Connection failed: {e}")
        disconnect_from_evm()
        return False

def disconnect_from_evm():
    """Disconnect from EVM"""
    global log_socket, cmd_socket, stats_socket
    global connected_log, connected_cmd, connected_stats
    global monitoring_active

    log_message("Disconnecting from EVM...")

    # Stop demo if it's running before disconnecting
    if current_c7x_stats["demoRunning"]:
        log_message("Stopping demo before disconnect...")
        send_command("STOP")
        send_command("CODEC_SHUTDOWN")
        time.sleep(1)  # Give time for commands to process

    monitoring_active = False

    # Close sockets
    if log_socket:
        try:
            log_socket.close()
        except:
            pass
        log_socket = None
        connected_log = False

    if cmd_socket:
        try:
            cmd_socket.close()
        except:
            pass
        cmd_socket = None
        connected_cmd = False

    if stats_socket:
        try:
            stats_socket.close()
        except:
            pass
        stats_socket = None
        connected_stats = False

    log_message("Disconnected from EVM")

def send_command(command):
    """Send command to EVM"""
    if not connected_cmd or not cmd_socket:
        log_message("ERROR: Not connected to command interface")
        return False

    try:
        cmd_socket.send((command + '\n').encode())

        # Wait for response
        response = cmd_socket.recv(1024).decode().strip()

        if response == "OK":
            log_message(f"Command '{command}' successful")
            return True
        else:
            log_message(f"Command '{command}' failed: {response}")
            return False

    except Exception as e:
        log_message(f"Command send failed: {e}")
        return False

def log_reader_thread():
    """Background thread to read log messages from EVM"""
    global log_socket, connected_log

    while monitoring_active and connected_log and log_socket:
        try:
            data = log_socket.recv(1024)
            if not data:
                break

            message = data.decode('utf-8', errors='ignore').strip()
            if message:
                log_queue.put(message)

        except socket.timeout:
            # Timeout is normal - continue to check monitoring_active
            continue
        except Exception as e:
            if monitoring_active:
                log_queue.put(f"Log reader error: {e}")
            break

def stats_reader_thread():
    """Background thread to read DSP statistics from EVM with auto-reconnection"""
    global stats_socket, connected_stats

    reconnect_attempts = 0
    max_reconnect_attempts = 5

    while monitoring_active:
        try:
            # Check if we need to reconnect
            if not connected_stats or not stats_socket:
                if reconnect_attempts < max_reconnect_attempts:
                    log_queue.put(f"Attempting to reconnect to stats stream (attempt {reconnect_attempts + 1}/{max_reconnect_attempts})...")
                    try:
                        # Close existing socket if any
                        if stats_socket:
                            stats_socket.close()

                        # Reconnect to stats stream
                        stats_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        stats_socket.settimeout(1.0)
                        stats_socket.connect((evm_ip, STATS_PORT))
                        connected_stats = True
                        reconnect_attempts = 0
                        log_queue.put("Successfully reconnected to stats stream")
                    except Exception as e:
                        reconnect_attempts += 1
                        connected_stats = False
                        stats_socket = None
                        log_queue.put(f"Stats reconnection failed: {e}")
                        time.sleep(RETRY_INTERVAL)
                        continue
                else:
                    # Max reconnection attempts reached
                    log_queue.put("Max stats reconnection attempts reached. Stopping stats reader.")
                    break

            # Read data if connected
            if connected_stats and stats_socket:
                data = stats_socket.recv(1024)
                if not data:
                    log_queue.put("Stats connection closed by EVM")
                    connected_stats = False
                    continue

                # Parse JSON statistics
                json_str = data.decode('utf-8', errors='ignore').strip()
                if json_str:
                    try:
                        stats = json.loads(json_str)
                        # Use thread-safe queue instead of direct update
                        stats_queue.put(stats)
                        # Reset reconnect attempts on successful data
                        reconnect_attempts = 0
                    except json.JSONDecodeError as e:
                        log_queue.put(f"Invalid JSON from stats stream: {json_str[:50]}...")

        except socket.timeout:
            # Timeout is normal - continue to check monitoring_active
            continue
        except Exception as e:
            if monitoring_active:
                log_queue.put(f"Stats reader error: {e}")
                connected_stats = False
                stats_socket = None
            time.sleep(1)  # Brief pause before retry

def start_network_threads():
    """Start background networking threads"""
    global monitoring_active

    monitoring_active = True

    # Start log reader thread
    log_thread = threading.Thread(target=log_reader_thread, daemon=True)
    log_thread.start()

    # Start stats reader thread
    stats_thread = threading.Thread(target=stats_reader_thread, daemon=True)
    stats_thread.start()

    log_message("Network monitoring threads started")

# --- GUI FUNCTIONS ---

def log_message(msg):
    """Log message to GUI console"""
    timestamp = datetime.datetime.now().strftime("%H:%M:%S")
    full_msg = f"[{timestamp}] {msg}"
    print(full_msg)

    if log_console:
        try:
            log_console.insert(tk.END, full_msg + "\n")
            log_console.see(tk.END)
        except:
            pass

def update_status_display():
    """Update GUI status labels with current state"""
    try:
        # Always update connection status and button states
        if connected_log and connected_cmd and connected_stats:
            connect_btn.config(state="disabled")
            disconnect_btn.config(state="normal")
            connection_status = "CONNECTED"
        else:
            connect_btn.config(state="normal")
            disconnect_btn.config(state="disabled")
            connection_status = "DISCONNECTED"

        # Update demo controls based on connection status
        if not connected_stats:
            demo_label.config(text=f"Demo: {connection_status}", fg="red" if connection_status == "DISCONNECTED" else "gray")
            c7x_load_label.config(text="DSP Load: --")
            cycles_label.config(text="Cycles: --")
            throughput_label.config(text="Throughput: --")
            start_stop_btn.config(state="disabled" if connection_status == "DISCONNECTED" else "normal")
            return

        # Enable demo controls when connected
        start_stop_btn.config(state="normal")

        # Thread-safe access to DSP stats
        with stats_lock:
            # demo status
            if current_c7x_stats["demoRunning"]:
                demo_label.config(text="Demo: RUNNING", fg="green")
                start_stop_btn.config(text="Stop Demo", bg="red")
            else:
                demo_label.config(text="Demo: STOPPED", fg="orange")
                start_stop_btn.config(text="Start Demo", bg="green")

            # DSP stats
            c7x_load_label.config(text=f"DSP Load: {current_c7x_stats['c7xLoad']:.1f}%")
            cycles_label.config(text=f"Cycles: {current_c7x_stats['cycleCount']}")
            throughput_label.config(text=f"Throughput: {current_c7x_stats['throughput']:.2f} MB/s")

    except tk.TclError:
        # Widget has been destroyed during shutdown
        return
    except Exception as e:
        # Log other errors but don't crash GUI updates
        print(f"Status display error: {e}")

def toggle_demo():
    """Toggle demo start/stop with automatic codec control"""
    if not (connected_cmd and connected_stats):
        messagebox.showerror("Error", "Not connected to EVM")
        return

    # Thread-safe access to demo status
    with stats_lock:
        demo_running = current_c7x_stats["demoRunning"]

    if demo_running:
        # Stop demo: stop demo first, then shutdown codecs with proper timing
        log_message("Stopping demo...")
        if send_command("STOP"):
            time.sleep(0.5)  # Wait for demo to stop before shutting down codecs
            log_message("Shutting down codecs...")
            if send_command("CODEC_SHUTDOWN"):
                log_message("Demo stopped successfully")
            else:
                log_message("WARNING: Codec shutdown failed")
        else:
            log_message("ERROR: Failed to stop demo")
    else:
        # Start demo: initialize codecs first, then start demo with proper timing
        log_message("Initializing codecs...")
        if send_command("CODEC_INIT"):
            time.sleep(0.5)  # Wait for codec initialization before starting demo
            log_message("Starting demo...")
            if send_command("START"):
                log_message("Demo started successfully")
            else:
                log_message("ERROR: Failed to start demo")
        else:
            log_message("ERROR: Codec initialization failed")

# Note: Codec control is now automatic in toggle_demo() function

def connect_gui():
    """Connect GUI to EVM"""
    if connect_to_evm():
        messagebox.showinfo("Success", f"Connected to EVM at {evm_ip}")
    else:
        messagebox.showerror("Error", f"Failed to connect to EVM at {evm_ip}")

def disconnect_gui():
    """Disconnect GUI from EVM"""
    disconnect_from_evm()
    messagebox.showinfo("Disconnected", "Disconnected from EVM")

def clear_data():
    """Clear graph data"""
    global frame_counter

    # Thread-safe data clearing
    with stats_lock:
        frame_numbers.clear()
        c7x_loads.clear()
        cycle_counts.clear()
        throughputs.clear()
        demo_status_history.clear()
        frame_counter = 0

    log_message("Graph data cleared")

def save_data():
    """Save graphs and logs"""
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    save_dir = f"signal_chain_logs_{timestamp}"
    os.makedirs(save_dir, exist_ok=True)

    # Force canvas redraw
    canvas.draw()

    # Save graphs
    fig_path = os.path.join(save_dir, f"signal_chain_graphs_{timestamp}.png")
    fig.savefig(fig_path, dpi=150, bbox_inches='tight')

    # Save data summary
    summary_path = os.path.join(save_dir, f"signal_chain_summary_{timestamp}.txt")
    with open(summary_path, "w") as f:
        f.write(f"Signal Chain Biquad Cascade Data - {timestamp}\n")
        f.write(f"Board IP: {evm_ip}\n")
        f.write("=" * 50 + "\n")
        f.write(f"Frames: {len(frame_numbers)}\n")
        if c7x_loads:
            f.write(f"DSP Load (%): Min={min(c7x_loads):.1f}, Max={max(c7x_loads):.1f}, Avg={sum(c7x_loads)/len(c7x_loads):.1f}\n")
        if cycle_counts:
            f.write(f"Cycle Count: Min={min(cycle_counts)}, Max={max(cycle_counts)}, Avg={sum(cycle_counts)//len(cycle_counts)}\n")
        if throughputs:
            f.write(f"Throughput (MB/s): Min={min(throughputs):.2f}, Max={max(throughputs):.2f}, Avg={sum(throughputs)/len(throughputs):.2f}\n")
        f.write("\n--- Console Log ---\n")
        if log_console:
            f.write(log_console.get("1.0", tk.END))

    log_message(f"Data saved to {save_dir}/")
    messagebox.showinfo("Saved", f"Data saved to {save_dir}/")

def animate_graphs(frame):
    """Update real-time graphs with improved error handling"""
    try:
        # Clear axes
        for ax in [ax1, ax2, ax3]:
            ax.clear()

        # Thread-safe data access
        with stats_lock:
            if not frame_numbers:
                # Show connection status when no data
                status_msg = "Waiting for DSP data..."
                if not connected_stats:
                    status_msg = "Not connected to stats stream"
                elif not monitoring_active:
                    status_msg = "Monitoring inactive"

                ax1.text(0.5, 0.5, status_msg, ha='center', va='center', transform=ax1.transAxes)
                ax2.text(0.5, 0.5, status_msg, ha='center', va='center', transform=ax2.transAxes)
                ax3.text(0.5, 0.5, status_msg, ha='center', va='center', transform=ax3.transAxes)
                return

            frames = list(frame_numbers)
            loads = list(c7x_loads)
            cycles = list(cycle_counts)
            throughput_data = list(throughputs)

            # Add debug info to title
            data_points = len(frames)
            latest_timestamp = current_c7x_stats.get("timestamp", 0)

        # DSP Load graph
        if frames and loads and len(frames) == len(loads):
            ax1.plot(frames, loads, 'b-', linewidth=2, label='DSP Load')
            ax1.set_ylabel("Load (%)")
            ax1.set_title(f"C7x DSP Load ({data_points} samples)", fontweight='bold')
            ax1.grid(True, alpha=0.3)
            ax1.set_ylim(0, 100)

        # Cycle Count graph
        if frames and cycles and len(frames) == len(cycles):
            ax2.plot(frames, cycles, 'r-', linewidth=2, label='Cycle Count')
            ax2.set_ylabel("Cycles")
            ax2.set_title(f"DSP Processing Cycles (Latest: {cycles[-1] if cycles else 0})", fontweight='bold')
            ax2.grid(True, alpha=0.3)

        # Throughput graph
        if frames and throughput_data and len(frames) == len(throughput_data):
            ax3.plot(frames, throughput_data, 'g-', linewidth=2, label='Throughput')
            ax3.set_xlabel("Sample")
            ax3.set_ylabel("MB/s")
            ax3.set_title(f"Demo Throughput (Avg: {sum(throughput_data)/len(throughput_data):.2f} MB/s)", fontweight='bold')
            ax3.grid(True, alpha=0.3)

    except Exception as e:
        print(f"Graph animation error: {e}")
        # Show error on graphs
        for ax in [ax1, ax2, ax3]:
            ax.clear()
            ax.text(0.5, 0.5, f'Graph Error: {str(e)[:50]}...', ha='center', va='center', transform=ax.transAxes)

def process_log_queue():
    """Process incoming log messages"""
    try:
        while True:
            message = log_queue.get_nowait()
            log_message(message)
    except queue.Empty:
        pass

def process_stats_queue():
    """Process incoming stats data thread-safely"""
    global frame_counter

    try:
        while True:
            stats = stats_queue.get_nowait()

            # Validate stats data
            if not isinstance(stats, dict):
                continue

            # Thread-safe update of shared data
            with stats_lock:
                current_c7x_stats.update(stats)

                # Add to graph buffers
                frame_counter += 1
                frame_numbers.append(frame_counter)
                c7x_loads.append(stats.get("c7xLoad", 0.0))
                cycle_counts.append(stats.get("cycleCount", 0))
                throughputs.append(stats.get("throughput", 0.0))
                demo_status_history.append(stats.get("demoRunning", 0))

    except queue.Empty:
        pass
    except Exception as e:
        # Only log actual errors, not routine stats data
        print(f"Stats processing error: {e}")

def signal_handler(sig, frame):
    """Handle Ctrl+C signal"""
    print("Received interrupt signal (Ctrl+C)...")
    try:
        on_closing()
    except:
        # If GUI isn't initialized yet, just exit
        sys.exit(0)

def on_closing():
    """Handle application closing"""
    global monitoring_active

    log_message("Shutting down Demo...")

    # Disconnect from EVM
    disconnect_from_evm()

    # Destroy GUI
    if root:
        root.quit()  # Exit mainloop
        root.destroy()

# --- MAIN GUI SETUP ---

def create_gui():
    """Create main GUI window"""
    global root, canvas, fig, ax1, ax2, ax3, animation_obj
    global log_console, c7x_load_label, cycles_label, throughput_label, demo_label
    global start_stop_btn, connect_btn, disconnect_btn

    # Main window
    root = tk.Tk()
    root.title(f"Signal Chain Biquad Cascade Demo GUI")
    root.geometry("1400x800")
    root.protocol("WM_DELETE_WINDOW", on_closing)

    # Left control panel
    left_frame = tk.Frame(root, bg="#f0f0f0", width=400)
    left_frame.pack(side=tk.LEFT, fill=tk.Y)
    left_frame.pack_propagate(False)

    # Title
    title_label = tk.Label(left_frame, text="Signal Chain Biquad Cascade\nDemo",
                          font=("Helvetica", 16, "bold"), bg="#f0f0f0", fg="#333")
    title_label.pack(pady=20)

    # Connection controls
    conn_frame = tk.LabelFrame(left_frame, text="Board Connection", font=("Helvetica", 12, "bold"),
                              bg="#f0f0f0", padx=10, pady=10)
    conn_frame.pack(pady=10, padx=10, fill=tk.X)

    tk.Label(conn_frame, text=f"Board IP: {evm_ip}", font=("Helvetica", 11),
             bg="#f0f0f0", fg="blue").pack(anchor="w")

    btn_frame = tk.Frame(conn_frame, bg="#f0f0f0")
    btn_frame.pack(fill=tk.X, pady=5)

    connect_btn = tk.Button(btn_frame, text="Connect", command=connect_gui,
                           bg="green", fg="white", font=("Helvetica", 11, "bold"))
    connect_btn.pack(side=tk.LEFT, padx=5)

    disconnect_btn = tk.Button(btn_frame, text="Disconnect", command=disconnect_gui,
                              bg="gray", fg="white", font=("Helvetica", 11, "bold"), state="disabled")
    disconnect_btn.pack(side=tk.LEFT, padx=5)

    # Status display
    status_frame = tk.LabelFrame(left_frame, text="System Status", font=("Helvetica", 12, "bold"),
                                bg="#f0f0f0", padx=10, pady=10)
    status_frame.pack(pady=10, padx=10, fill=tk.X)

    demo_label = tk.Label(status_frame, text="Demo: --", font=("Helvetica", 11),
                          bg="#f0f0f0", fg="gray")
    demo_label.pack(anchor="w")

    c7x_load_label = tk.Label(status_frame, text="DSP Load: --", font=("Helvetica", 11),
                             bg="#f0f0f0", fg="blue")
    c7x_load_label.pack(anchor="w")

    cycles_label = tk.Label(status_frame, text="Cycles: --", font=("Helvetica", 11),
                           bg="#f0f0f0", fg="purple")
    cycles_label.pack(anchor="w")

    throughput_label = tk.Label(status_frame, text="Throughput: --", font=("Helvetica", 11),
                               bg="#f0f0f0", fg="green")
    throughput_label.pack(anchor="w")

    # Demo control
    demo_frame = tk.LabelFrame(left_frame, text="Demo Control", font=("Helvetica", 12, "bold"),
                               bg="#f0f0f0", padx=10, pady=10)
    demo_frame.pack(pady=10, padx=10, fill=tk.X)

    start_stop_btn = tk.Button(demo_frame, text="Start Demo", command=toggle_demo,
                              bg="green", fg="white", font=("Helvetica", 12, "bold"), height=2)
    start_stop_btn.pack(fill=tk.X, pady=5)

    # Note: Codec control is now automatic with demo start/stop

    # Data control
    data_frame = tk.LabelFrame(left_frame, text="Data Control", font=("Helvetica", 12, "bold"),
                              bg="#f0f0f0", padx=10, pady=10)
    data_frame.pack(pady=10, padx=10, fill=tk.X)

    clear_btn = tk.Button(data_frame, text="Clear Graphs", command=clear_data,
                         bg="orange", fg="white", font=("Helvetica", 11))
    clear_btn.pack(side=tk.LEFT, padx=5)

    save_btn = tk.Button(data_frame, text="Save Data", command=save_data,
                        bg="darkblue", fg="white", font=("Helvetica", 11))
    save_btn.pack(side=tk.LEFT, padx=5)

    # Log console
    log_frame = tk.LabelFrame(left_frame, text="Console Log", font=("Helvetica", 11, "bold"),
                             bg="#f0f0f0")
    log_frame.pack(pady=10, padx=10, fill=tk.BOTH, expand=True)

    log_console = ScrolledText(log_frame, height=12, font=("Courier", 9),
                              bg="black", fg="lime")
    log_console.pack(fill=tk.BOTH, expand=True)

    # Right graph panel
    right_frame = tk.Frame(root, bg="white")
    right_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)

    # Create matplotlib figure
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8))
    fig.tight_layout(pad=3.0)

    canvas = FigureCanvasTkAgg(fig, master=right_frame)
    canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # Use FuncAnimation for smooth graph updates
    animation_obj = animation.FuncAnimation(fig, animate_graphs, interval=STATS_UPDATE_INTERVAL, cache_frame_data=False, blit=False)

    # Force initial canvas draw
    canvas.draw()

    # Fast status update timer (separate from graph animation)
    def update_gui():
        try:
            # Always process incoming data and update display
            process_stats_queue()
            process_log_queue()
            update_status_display()

            # Schedule next update if GUI still exists
            if root and root.winfo_exists():
                root.after(GUI_UPDATE_INTERVAL, update_gui)
        except tk.TclError:
            # GUI has been destroyed, stop updates
            return
        except Exception as e:
            # Log any unexpected errors but continue GUI updates
            print(f"GUI update error: {e}")
            if root and root.winfo_exists():
                root.after(GUI_UPDATE_INTERVAL, update_gui)

    # Start GUI updates
    root.after(1000, update_gui)  # Start after 1 second

    # Initial log message
    log_message("Signal Chain Biquad Network GUI started")
    log_message(f"Ready to connect to EVM at {evm_ip}")
    log_message("Click 'Connect' to begin")

def main():
    """Main application entry point"""
    global evm_ip

    if len(sys.argv) != 2:
        print("Usage: python3 signal_chain_biquad_example_gui.py <evm_ip_address>")
        print("Example: python3 signal_chain_biquad_example_gui.py 192.168.1.100")
        sys.exit(1)

    evm_ip = sys.argv[1]

    # Validate IP address format
    try:
        socket.inet_aton(evm_ip)
    except socket.error:
        print(f"ERROR: Invalid IP address format: {evm_ip}")
        sys.exit(1)

    # Register signal handler for Ctrl+C
    signal.signal(signal.SIGINT, signal_handler)

    # Starting GUI
    create_gui()
    root.mainloop()

if __name__ == "__main__":
    main()