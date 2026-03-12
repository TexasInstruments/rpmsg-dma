/*
 *  Copyright (C) 2026 Texas Instruments Incorporated
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file signal_chain_biquad_linux_example.c
 * @brief AM62D2-EVM Linux application for control & monitoring of Cascade Biquad Parametric EQ Example running on C7x
 *
 * This application runs on AM62D2-EVM and provides:use
 * - Firmware switching to signal chain biquad cascade
 * - Two operation modes Client Mode (network servers) and Command-Line Mode
 * - Signal Chain control via shared memory with C7x DSP
 * - Audio codec I2C initialization/control (TAD5212 DAC + PCM6240 ADC)
 * - Real-time DSP performance monitoring
 *
 Client Mode - Network Interface:
 * - Port 8888: Log/status messages
 * - Port 8889: Command interface (START/STOP)
 * - Port 8890: DSP statistics streaming (JSON format)
 *
 * Command-Line Mode:
 * - Direct execution: start, stop, sleep:N
 * - Examples: ./app start sleep:10 stop
 * - Return codes: 0=PASSED, 1=FAILED
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <time.h>
#include <stdarg.h>
#include <sys/time.h>
#include <float.h>

#include "rpmsg.h"
#include "dmabuf.h"
#include "fw_loader.h"
#include "signal_chain_biquad_linux_example.h"
#include "audio_codecs.h"

// === GLOBAL STATE ===

volatile bool application_running = true;
volatile bool c7x_connected = false;

// Dynamic remoteproc paths (determined at runtime)
static char c7x_firmware_state_path[256] = {0};
static char remoteproc_device_name[256] = {0};
static int c7x_remoteproc_id = -1;

// Network server state
static int log_server_fd = -1, log_client_fd = -1;
static int command_server_fd = -1, command_client_fd = -1;
static int stats_server_fd = -1, stats_client_fd = -1;

// C7x communication
static int c7x_rpmsg_fd = -1;

// === MODULE STATE ===

// C7x Communication state
static c7x_comm_status c7x_comm_state = {
    .initialized = false,
    .connected = false,
    .shared_memory_ready = false,
    .rpmsg_fd = -1,
    .last_command_sent = 0,
    .last_command_time = 0,
    .remoteproc_device = {0}
};

static c7x_shared_memory c7x_shared_mem = {
    .memory_ptr = NULL,
    .dma_buffer = {0},
    .buffer_size = 0,
    .physical_address = 0
};

// Threading
static pthread_t network_server_threads[3];
static pthread_t c7x_monitor_thread;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;


// C7x communication functions
static bool c7x_comm_init(void);
static bool c7x_comm_init_communication(void);
bool c7x_comm_send_control_command(uint32_t command);

// === UTILITY FUNCTIONS ===

/**
 * @brief Thread-safe logging function
 */
void log_message(const char *format, ...)
{
    pthread_mutex_lock(&log_mutex);

    char timestamp[32];
    time_t now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "[%H:%M:%S]", localtime(&now));

    printf("%s ", timestamp);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);

    // Send to network log client if connected
    if (log_client_fd >= 0) {
        char log_buf[LOG_BUFFER_SIZE];
        int len = snprintf(log_buf, sizeof(log_buf), "%s ", timestamp);
        va_start(args, format);
        len += vsnprintf(log_buf + len, sizeof(log_buf) - len, format, args);
        va_end(args);

        if (len < sizeof(log_buf) - 1) {
            log_buf[len] = '\n';
            log_buf[len + 1] = '\0';
            send(log_client_fd, log_buf, len + 1, MSG_NOSIGNAL);
        }
    }

    pthread_mutex_unlock(&log_mutex);
}

/**
 * @brief Find the C7x DSP remoteproc device dynamically
 * @return 0 on success, -1 on failure
 */
int find_c7x_remoteproc(void)
{
    FILE *name_file;
    char name_path[256];
    char device_name[256];
    int i;

    log_message("Scanning for C7x DSP remoteproc device...");

    for (i = 0; i < MAX_REMOTEPROC_DEVICES; i++) {
        // Check if remoteproc device exists
        snprintf(name_path, sizeof(name_path), "/sys/class/remoteproc/remoteproc%d/name", i);

        name_file = fopen(name_path, "r");
        if (!name_file) {
            continue; // Device doesn't exist
        }

        // Read device name
        if (fgets(device_name, sizeof(device_name), name_file) == NULL) {
            fclose(name_file);
            continue;
        }
        fclose(name_file);

        // Remove newline
        device_name[strcspn(device_name, "\n")] = 0;

        // Check if this is the C7x DSP (look for c7x or dsp in the name)
        if (strstr(device_name, "c7x") != NULL || strstr(device_name, "C7x") != NULL ||
            strstr(device_name, "dsp") != NULL || strstr(device_name, "DSP") != NULL) {
            c7x_remoteproc_id = i;
            snprintf(c7x_firmware_state_path, sizeof(c7x_firmware_state_path), "/sys/class/remoteproc/remoteproc%d/state", i);
            snprintf(remoteproc_device_name, sizeof(remoteproc_device_name), "/dev/remoteproc%d", i);

            log_message("C7x DSP found at remoteproc%d", i);
            return 0;
        }
    }

    log_message("ERROR: C7x DSP not found");

    return -1;
}

// === C7x COMMUNICATION ===

/**
 * @brief Initialize C7x communication and shared memory
 */
bool init_c7x_communication(void)
{
    log_message("Initializing C7x communication...");

    // Initialize shared memory
    if (dmabuf_heap_init(DMA_HEAP_NAME, sizeof(real_time_info), remoteproc_device_name, &c7x_shared_mem.dma_buffer) < 0) {
        log_message("ERROR: Failed to initialize shared memory");
        return false;
    }

    log_message("Shared memory: phys=0x%08lX, size=%d", c7x_shared_mem.dma_buffer.phys_addr, (int)sizeof(real_time_info));

    // Map shared memory
    c7x_shared_mem.memory_ptr = (volatile real_time_info*)c7x_shared_mem.dma_buffer.kern_addr;
    if (!c7x_shared_mem.memory_ptr) {
        log_message("ERROR: Failed to map shared memory");
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        return false;
    }

    // Initialize shared memory state
    memset((void*)c7x_shared_mem.memory_ptr, 0, sizeof(real_time_info));
    c7x_shared_mem.memory_ptr->demoCommand = DEMO_CMD_NO_CHANGE;
    c7x_shared_mem.memory_ptr->demoRunning = DEMO_STATE_STOPPED;
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_END);

    // Initialize RPMessage
    c7x_rpmsg_fd = init_rpmsg(C7_PROC_ID, RMT_EP);
    if (c7x_rpmsg_fd < 0) {
        log_message("ERROR: Failed to initialize RPMessage");
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        return false;
    }

    // Send setup message to C7x
    ipc_msg_buffer setup_msg = {
        .dataBuffer = 0,
        .paramsBuffer = c7x_shared_mem.dma_buffer.phys_addr,
        .dataSize = 0,
        .paramsSize = sizeof(real_time_info)
    };

    log_message("Sending setup message: size=%d, paramsBuffer=0x%08X, paramsSize=%d",
                sizeof(setup_msg), setup_msg.paramsBuffer, setup_msg.paramsSize);

    if (send_msg(c7x_rpmsg_fd, (char*)&setup_msg, sizeof(setup_msg)) < 0) {
        log_message("ERROR: Failed to send setup message to C7x");
        cleanup_rpmsg(c7x_rpmsg_fd);
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        return false;
    }

    log_message("Setup message sent successfully, waiting for C7x response...");

    // Wait for C7x response
    char response[256];
    int response_len;
    if (recv_msg(c7x_rpmsg_fd, sizeof(response), response, &response_len) < 0) {
        log_message("ERROR: No response from C7x");
        cleanup_rpmsg(c7x_rpmsg_fd);
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        return false;
    }

    log_message("Received C7x response: length=%d", response_len);

    c7x_connected = true;
    log_message("C7x communication initialized successfully");
    return true;
}

/**
 * @brief Send Control command to C7x via shared memory
 */
bool send_control_command(uint32_t command)
{
    if (!c7x_connected || !c7x_shared_mem.memory_ptr) {
        log_message("ERROR: C7x not connected");
        return false;
    }

    const char *cmd_name = (command == DEMO_CMD_START) ? "START" : "STOP";
    log_message("Sending Control %s command...", cmd_name);

    // Write command to shared memory
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START);
    c7x_shared_mem.memory_ptr->demoCommand = command;
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);

    // Wait for C7x to process command
    uint32_t expected_status = (command == DEMO_CMD_START) ? DEMO_STATE_RUNNING : DEMO_STATE_STOPPED;
    int timeout = 100; // 1 second (100 * 10ms)

    while (timeout > 0) {
        usleep(10000); // 10ms

        dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START);
        if (c7x_shared_mem.memory_ptr->demoRunning == expected_status) {
            dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);
            break;
        }
        dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);
        timeout--;
    }

    if (timeout <= 0) {
        log_message("WARNING: Timeout waiting for Demo %s confirmation", cmd_name);
        return false;
    }

    // Clear command after successful processing
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START);
    c7x_shared_mem.memory_ptr->demoCommand = DEMO_CMD_NO_CHANGE;
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);

    log_message("Command %s completed successfully", cmd_name);
    return true;
}

// === NETWORK SERVERS ===

/**
 * @brief Create and bind server socket
 */
int create_server_socket(int port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_message("ERROR: Failed to create socket for port %d: %s", port, strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message("ERROR: Failed to set socket options for port %d: %s", port, strerror(errno));
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("ERROR: Failed to bind socket to port %d: %s", port, strerror(errno));
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        log_message("ERROR: Failed to listen on port %d: %s", port, strerror(errno));
        close(server_fd);
        return -1;
    }

    // Server listening silently
    return server_fd;
}

/**
 * @brief Log server thread - handles log message streaming
 */
static void* log_server_thread(void *arg)
{
    log_server_fd = create_server_socket(LOG_PORT);
    if (log_server_fd < 0) {
        return NULL;
    }

    while (application_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        log_client_fd = accept(log_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (log_client_fd < 0) {
            if (application_running) {
                log_message("ERROR: Failed to accept log client: %s", strerror(errno));
            }
            break;
        }

        log_message("Client connected: %s", inet_ntoa(client_addr.sin_addr));

        // Keep connection alive until client disconnects
        char dummy;
        while (application_running && recv(log_client_fd, &dummy, 1, MSG_PEEK) > 0) {
            sleep(1);
        }

        close(log_client_fd);
        log_client_fd = -1;
    }

    return NULL;
}

/**
 * @brief Command server thread - handleClient commands
 */
static void* cmd_server_thread(void *arg)
{
    command_server_fd = create_server_socket(CMD_PORT);
    if (command_server_fd < 0) {
        return NULL;
    }

    while (application_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        command_client_fd = accept(command_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (command_client_fd < 0) {
            if (application_running) {
                log_message("ERROR: Failed to accept command client: %s", strerror(errno));
            }
            break;
        }

        // Process commands
        char cmd_buffer[256];
        while (application_running) {
            int bytes = recv(command_client_fd, cmd_buffer, sizeof(cmd_buffer) - 1, 0);
            if (bytes <= 0) {
                break;
            }

            cmd_buffer[bytes] = '\0';
            char *cmd = strtok(cmd_buffer, " \n\r");

            if (cmd == NULL) continue;

            if (strcmp(cmd, "START") == 0) {
                bool status = c7x_comm_send_control_command(DEMO_CMD_START);
                send(command_client_fd, status ? "OK\n" : "ERROR\n", status ? 3 : 6, MSG_NOSIGNAL);
            }
            else if (strcmp(cmd, "STOP") == 0) {
                bool status = c7x_comm_send_control_command(DEMO_CMD_STOP);
                send(command_client_fd, status ? "OK\n" : "ERROR\n", status ? 3 : 6, MSG_NOSIGNAL);
            }
            else if (strcmp(cmd, "CODEC_INIT") == 0) {
                bool status = audio_codecs_init(NULL);
                send(command_client_fd, status ? "OK\n" : "ERROR\n", status ? 3 : 6, MSG_NOSIGNAL);
            }
            else if (strcmp(cmd, "CODEC_SHUTDOWN") == 0) {
                bool status = audio_codecs_shutdown();
                send(command_client_fd, status ? "OK\n" : "ERROR\n", status ? 3 : 6, MSG_NOSIGNAL);
            }
            else {
                send(command_client_fd, "UNKNOWN_COMMAND\n", 16, MSG_NOSIGNAL);
            }
        }

        close(command_client_fd);
        command_client_fd = -1;
    }

    return NULL;
}

/**
 * @brief Statistics server thread - streams C7x performance data
 */
static void* stats_server_thread(void *arg)
{
    stats_server_fd = create_server_socket(STATS_PORT);
    if (stats_server_fd < 0) {
        return NULL;
    }

    while (application_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        stats_client_fd = accept(stats_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (stats_client_fd < 0) {
            if (application_running) {
                log_message("ERROR: Failed to accept stats client: %s", strerror(errno));
            }
            break;
        }

        // Stream C7x statistics
        while (application_running && c7x_connected && c7x_shared_mem.memory_ptr) {
            // Read current C7x stats
            dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START);
            real_time_info current_stats = *c7x_shared_mem.memory_ptr;  // Atomic copy
            dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);

            // Send stats as JSON
            char stats_json[512];
            int len = snprintf(stats_json, sizeof(stats_json),
                "{"
                "\"c7xLoad\":%.2f,"
                "\"cycleCount\":%u,"
                "\"throughput\":%.3f,"
                "\"demoRunning\":%u,"
                "\"timestamp\":%ld"
                "}\n",
                current_stats.c7xLoad / 100.0,
                current_stats.cycleCount,
                current_stats.throughput,
                current_stats.demoRunning,
                time(NULL)
            );

            if (send(stats_client_fd, stats_json, len, MSG_NOSIGNAL) < 0) {
                break;
            }

            sleep(1); // Send stats every second
        }

        close(stats_client_fd);
        stats_client_fd = -1;
    }

    return NULL;
}

/**
 * @brief C7x monitoring thread - minimal console logging
 */
static void* c7x_monitor_thread_func(void *arg)
{
    // Reduced logging - detailed stats available iClient
    while (application_running) {
        sleep(10);
    }

    return NULL;
}

// === CLEANUP AND SIGNAL HANDLING ===

/**
 * @brief Cleanup function - restores system state
 */
void cleanup(void)
{
    log_message("Cleaning up...");

    application_running = false;

    // Stop Demo if running
    if (c7x_connected && c7x_shared_mem.memory_ptr && c7x_shared_mem.memory_ptr->demoRunning == DEMO_STATE_RUNNING) {
        log_message("Stopping Demo...");
        c7x_comm_send_control_command(DEMO_CMD_STOP);
    }

    // Shutdown codecs
    audio_codec_status codec_status;
    if (audio_codecs_get_status(&codec_status) &&
        (codec_status.tad5212_initialized || codec_status.pcm6240_initialized)) {
        audio_codecs_shutdown();
    }

    // Close network connections
    if (log_client_fd >= 0) close(log_client_fd);
    if (log_server_fd >= 0) close(log_server_fd);
    if (command_client_fd >= 0) close(command_client_fd);
    if (command_server_fd >= 0) close(command_server_fd);
    if (stats_client_fd >= 0) close(stats_client_fd);
    if (stats_server_fd >= 0) close(stats_server_fd);

    // Cleanup C7x communication
    if (c7x_rpmsg_fd >= 0) {
        cleanup_rpmsg(c7x_rpmsg_fd);
    }
    if (c7x_shared_mem.dma_buffer.dma_buf_fd >= 0) {
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
    }

    // Restore base firmware
    log_message("Restoring base firmware...");
    switch_firmware(C7_BASE_FW, C7_FW_LINK, c7x_firmware_state_path);

    log_message("Cleanup completed");
}

/**
 * @brief Signal handler for clean shutdown
 */
static void signal_handler(int sig)
{
    log_message("Caught signal %d - shutting down...", sig);
    cleanup();
    exit(0);
}

// === COMMAND LINE MODE FUNCTIONS ===

/**
 * @brief Display current C7x performance metrics
 */
void display_performance_metrics(void)
{
    if (!c7x_connected || !c7x_shared_mem.memory_ptr) {
        return;
    }

    // Read current metrics from shared memory
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START);
    real_time_info current_stats = *c7x_shared_mem.memory_ptr;
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END);

    // Display metrics in a compact format on separate lines
    printf("[C7x Metrics] Load: %5.1f%% | Cycles: %6u | Throughput: %6.2f MB/s | Status: %s\n",
           current_stats.c7xLoad / 100.0,
           current_stats.cycleCount,
           current_stats.throughput,
           current_stats.demoRunning ? "RUNNING" : "STOPPED");
}

/**
 * @brief Print usage information for command-line mode
 */
void print_usage(const char *program_name)
{
    printf("TI Signal Chain Biquad Cascade Linux Example\n\n");
    printf("Usage:\n");
    printf("  %s                    Client mode (network server)\n", program_name);
    printf("  %s [commands...]      # Command-line mode\n\n", program_name);
    printf("Available commands:\n");
    printf("  start                 # Start Cascade Biquad Parametric EQ Processing \n");
    printf("  stop                  # Stop Cascade Biquad Parametric EQ Processing \n");
    printf("  sleep:N               # Sleep for N seconds\n");
    printf("  help                  # Show this usage information\n\n");
    printf("Examples:\n");
    printf("  %s start sleep:10 stop\n", program_name);
    printf("  %s start sleep:5 stop\n", program_name);
    printf("  %s help\n\n", program_name);
    printf("Return codes:\n");
    printf("  0 = All commands executed successfully (PASSED)\n");
    printf("  1 = Command sequence failed (FAILED)\n\n");
    printf("GUI mode):\n");
    printf("  Port 8888: Log messages    Port 8889: Commands    Port 8890: Statistics\n");
}

/**
 * @brief Execute a single command in command-line mode
 * @param cmd Command string (e.g., "start", "stop", "sleep:10")
 * @return true on success, false on failure
 */
bool execute_command(const char *cmd)
{
    if (strcmp(cmd, "start") == 0) {
        log_message("CMD: Starting Demo...");
        if (!audio_codecs_init(NULL)) {
            log_message("CMD: FAILED - Codec initialization failed");
            return false;
        }
        if (!c7x_comm_send_control_command(DEMO_CMD_START)) {
            log_message("CMD: FAILED - Demo start command failed");
            return false;
        }
        log_message("CMD: SUCCESS - Demo started");

        // Display initial performance metrics
        printf("\nC7x Performance Metrics:\n");
        display_performance_metrics();
        printf("\n");

        return true;
    }
    else if (strcmp(cmd, "stop") == 0) {
        log_message("CMD: Stopping Demo...");

        // Display final performance metrics before stopping
        printf("\nFinal C7x Performance Metrics:\n");
        display_performance_metrics();
        printf("\n");

        if (!c7x_comm_send_control_command(DEMO_CMD_STOP)) {
            log_message("CMD: FAILED - Demo stop command failed");
            return false;
        }
        if (!audio_codecs_shutdown()) {
            log_message("CMD: FAILED - Codec shutdown failed");
            return false;
        }
        log_message("CMD: SUCCESS - Demo stopped");
        return true;
    }
    else if (strncmp(cmd, "sleep:", 6) == 0) {
        int sleep_duration = atoi(cmd + 6);
        if (sleep_duration <= 0) {
            log_message("CMD: FAILED - Invalid sleep duration: %s", cmd);
            return false;
        }
        log_message("CMD: Sleeping for %d seconds...", sleep_duration);

        // Display real-time performance metrics during sleep
        printf("\nMonitoring C7x Performance (updating every second for %d seconds):\n", sleep_duration);
        for (int i = 0; i < sleep_duration; i++) {
            display_performance_metrics();
            sleep(1);
        }
        printf("\n");

        log_message("CMD: SUCCESS - Sleep completed");
        return true;
    }
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        // Help command - this is handled at higher level
        return true;
    }
    else {
        log_message("CMD: FAILED - Unknown command: %s", cmd);
        log_message("Use 'help' to see available commands");
        return false;
    }
}

/**
 * @brief Run application in command-line mode
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success (all commands passed), 1 on failure
 */
int run_command_line_mode(int argc, char *argv[])
{
    // Handle help command
    if (argc == 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    log_message("=== Command Line Mode ===");
    log_message("Executing %d commands in sequence", argc - 1);

    // Install signal handlers for proper cleanup on Ctrl+C
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize system (C7x communication, firmware)
    if (find_c7x_remoteproc() < 0) {
        log_message("ERROR: Failed to find C7x remoteproc device");
        printf("FAILED\n");
        return 1;
    }

    // Switch to signal chain biquad firmware
    if (switch_firmware(C7_SIGCHAIN_FW, C7_FW_LINK, c7x_firmware_state_path) < 0) {
        log_message("ERROR: Failed to switch firmware");
        printf("FAILED\n");
        return 1;
    }

    // Initialize C7x communication
    if (!c7x_comm_init()) {
        log_message("ERROR: Failed to initialize C7x communication module");
        printf("FAILED\n");
        return 1;
    }
    if (!c7x_comm_init_communication()) {
        log_message("ERROR: Failed to initialize C7x communication");
        printf("FAILED\n");
        return 1;
    }

    log_message("System initialized successfully");

    bool all_commands_passed = true;

    // Execute each command in sequence
    for (int i = 1; i < argc; i++) {
        log_message("CMD[%d/%d]: %s", i, argc - 1, argv[i]);
        if (!execute_command(argv[i])) {
            all_commands_passed = false;
            break; // Stop on first failure
        }
    }

    // Cleanup
    cleanup();

    // Print final result
    if (all_commands_passed) {
        log_message("=== All commands executed successfully ===");
        printf("PASSED\n");
        return 0;
    } else {
        log_message("=== Command sequence failed ===");
        printf("FAILED\n");
        return 1;
    }
}

// === C7X COMMUNICATION FUNCTIONS ===

/**
 * @brief Get current timestamp in microseconds
 */
static uint64_t get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * @brief Clean up C7x communication resources
 */
static void cleanup_c7x_resources(void)
{
    // Close RPMsg connection
    if (c7x_comm_state.rpmsg_fd >= 0) {
        cleanup_rpmsg(c7x_comm_state.rpmsg_fd);
        c7x_comm_state.rpmsg_fd = -1;
    }

    // Clean up shared memory
    if (c7x_shared_mem.memory_ptr != NULL) {
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        c7x_shared_mem.memory_ptr = NULL;
        c7x_shared_mem.buffer_size = 0;
        c7x_shared_mem.physical_address = 0;
    }

    // Reset status
    c7x_comm_state.connected = false;
    c7x_comm_state.shared_memory_ready = false;
}

/**
 * @brief Initialize C7x communication module
 */
static bool c7x_comm_init(void)
{
    log_message("Initializing C7x communication module...");

    // Reset module state
    memset(&c7x_comm_state, 0, sizeof(c7x_comm_state));
    memset(&c7x_shared_mem, 0, sizeof(c7x_shared_mem));

    c7x_comm_state.rpmsg_fd = -1;
    c7x_remoteproc_id = -1;

    c7x_comm_state.initialized = true;
    log_message("C7x communication module initialized");
    return true;
}

/**
 * @brief Synchronize shared memory access (start)
 */
static bool c7x_comm_sync_memory_start(void)
{
    if (!c7x_comm_state.shared_memory_ready) {
        return false;
    }
    return (dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START) >= 0);
}

/**
 * @brief Synchronize shared memory access (end)
 */
static bool c7x_comm_sync_memory_end(void)
{
    if (!c7x_comm_state.shared_memory_ready) {
        return false;
    }
    return (dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_END) >= 0);
}

/**
 * @brief Initialize C7x communication (RPMsg + shared memory)
 */
static bool c7x_comm_init_communication(void)
{
    log_message("Initializing C7x communication...");

    // Initialize shared memory
    c7x_shared_mem.buffer_size = sizeof(real_time_info);
    if (dmabuf_heap_init(DMA_HEAP_NAME, c7x_shared_mem.buffer_size,
                         remoteproc_device_name, &c7x_shared_mem.dma_buffer) < 0) {
        log_message("ERROR: Failed to initialize shared memory");
        return false;
    }

    c7x_shared_mem.physical_address = c7x_shared_mem.dma_buffer.phys_addr;
    log_message("Shared memory: phys=0x%08lX, size=%d",
                c7x_shared_mem.physical_address, (int)c7x_shared_mem.buffer_size);

    // Map shared memory
    c7x_shared_mem.memory_ptr = (volatile real_time_info*)c7x_shared_mem.dma_buffer.kern_addr;
    if (!c7x_shared_mem.memory_ptr) {
        log_message("ERROR: Failed to map shared memory");
        dmabuf_heap_destroy(&c7x_shared_mem.dma_buffer);
        return false;
    }

    // Initialize shared memory state
    memset((void*)c7x_shared_mem.memory_ptr, 0, sizeof(real_time_info));
    c7x_shared_mem.memory_ptr->demoCommand = DEMO_CMD_NO_CHANGE;
    c7x_shared_mem.memory_ptr->demoRunning = DEMO_STATE_STOPPED;
    dmabuf_sync(c7x_shared_mem.dma_buffer.dma_buf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_END);

    c7x_comm_state.shared_memory_ready = true;

    // Initialize RPMessage
    c7x_comm_state.rpmsg_fd = init_rpmsg(C7_PROC_ID, RMT_EP);
    if (c7x_comm_state.rpmsg_fd < 0) {
        log_message("ERROR: Failed to initialize RPMessage");
        cleanup_c7x_resources();
        return false;
    }

    // Send setup message to C7x - USING ORIGINAL FORMAT that C7x expects
    ipc_msg_buffer setup_msg = {
        .dataBuffer = 0,
        .paramsBuffer = c7x_shared_mem.physical_address,
        .dataSize = 0,
        .paramsSize = sizeof(real_time_info)
    };

    log_message("Sending setup message: size=%d, paramsBuffer=0x%08X, paramsSize=%d",
                (int)sizeof(setup_msg), setup_msg.paramsBuffer, setup_msg.paramsSize);

    if (send_msg(c7x_comm_state.rpmsg_fd, (char*)&setup_msg, sizeof(setup_msg)) < 0) {
        log_message("ERROR: Failed to send setup message to C7x");
        cleanup_c7x_resources();
        return false;
    }

    log_message("Setup message sent successfully, waiting for C7x response...");

    // Wait for C7x response
    char response[C7X_MAX_RESPONSE_SIZE];
    int response_len;
    if (recv_msg(c7x_comm_state.rpmsg_fd, sizeof(response), response, &response_len) < 0) {
        log_message("ERROR: No response from C7x");
        cleanup_c7x_resources();
        return false;
    }

    log_message("Received C7x response: length=%d", response_len);

    c7x_comm_state.connected = true;
    log_message("C7x communication initialized successfully");

    // Update global variables for compatibility
    c7x_connected = true;
    c7x_rpmsg_fd = c7x_comm_state.rpmsg_fd;

    return true;
}

/**
 * @brief Send Control command to C7x via shared memory
 */
bool c7x_comm_send_control_command(uint32_t command)
{
    if (!c7x_comm_state.connected || !c7x_shared_mem.memory_ptr) {
        log_message("ERROR: C7x not connected");
        return false;
    }

    const char *cmd_name = (command == DEMO_CMD_START) ? "START" : "STOP";
    log_message("Sending %s command...", cmd_name);

    // Write command to shared memory
    if (!c7x_comm_sync_memory_start()) {
        log_message("ERROR: Failed to sync memory for command write");
        return false;
    }

    c7x_shared_mem.memory_ptr->demoCommand = command;
    c7x_comm_state.last_command_sent = command;
    c7x_comm_state.last_command_time = get_timestamp_us();

    if (!c7x_comm_sync_memory_end()) {
        log_message("ERROR: Failed to sync memory after command write");
        return false;
    }

    // Wait for C7x to process command
    uint32_t expected_status = (command == DEMO_CMD_START) ? DEMO_STATE_RUNNING : DEMO_STATE_STOPPED;
    int timeout = C7X_COMMAND_TIMEOUT_MS / C7X_STATUS_CHECK_DELAY_MS;

    while (timeout > 0) {
        usleep(C7X_STATUS_CHECK_DELAY_MS * 1000);

        if (!c7x_comm_sync_memory_start()) {
            break;
        }

        if (c7x_shared_mem.memory_ptr->demoRunning == expected_status) {
            c7x_comm_sync_memory_end();
            break;
        }

        c7x_comm_sync_memory_end();
        timeout--;
    }

    if (timeout <= 0) {
        log_message("WARNING: Timeout waiting for Demo %s confirmation", cmd_name);
        return false;
    }

    // Clear command after successful processing
    if (!c7x_comm_sync_memory_start()) {
        log_message("WARNING: Failed to sync memory for command clear");
        return true; // Command succeeded, but cleanup failed
    }

    c7x_shared_mem.memory_ptr->demoCommand = DEMO_CMD_NO_CHANGE;
    c7x_comm_sync_memory_end();

    log_message("Command %s completed successfully", cmd_name);
    return true;
}

// === MAIN FUNCTION ===

int main(int argc, char *argv[])
{
    log_message("=== Cascade Biquad Parametric EQ Signal Chain Example Application ===");
    // Check if running in command-line mode (has arguments) oClient mode (no arguments)
    if (argc > 1) {
        // Command-line mode: execute commands and exit
        return run_command_line_mode(argc, argv);
    }

    // Client mode: start network servers and run indefinitely
    log_message("Network API: Log=%d, Cmd=%d, Stats=%d", LOG_PORT, CMD_PORT, STATS_PORT);

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Find C7x DSP remoteproc device dynamically
    if (find_c7x_remoteproc() < 0) {
        log_message("ERROR: Failed to find C7x remoteproc device");
        return 1;
    }

    // Switch to signal chain biquad firmware
    log_message("Switching to signal chain biquad cascade firmware...");
    if (switch_firmware(C7_SIGCHAIN_FW, C7_FW_LINK, c7x_firmware_state_path) < 0) {
        log_message("ERROR: Failed to switch firmware");
        return 1;
    }
    log_message("Firmware switched successfully");

    // Initialize C7x communication
    if (!c7x_comm_init()) {
        log_message("ERROR: Failed to initialize C7x communication module");
        cleanup();
        return 1;
    }
    if (!c7x_comm_init_communication()) {
        log_message("ERROR: Failed to initialize C7x communication");
        cleanup();
        return 1;
    }

    // Start network server threads
    if (pthread_create(&network_server_threads[0], NULL, log_server_thread, NULL) != 0 ||
        pthread_create(&network_server_threads[1], NULL, cmd_server_thread, NULL) != 0 ||
        pthread_create(&network_server_threads[2], NULL, stats_server_thread, NULL) != 0) {
        log_message("ERROR: Failed to create network threads");
        cleanup();
        return 1;
    }

    // Start C7x monitoring thread
    if (pthread_create(&c7x_monitor_thread, NULL, c7x_monitor_thread_func, NULL) != 0) {
        log_message("ERROR: Failed to create monitoring thread");
        cleanup();
        return 1;
    }

    log_message("All services started successfully");
    log_message("Ready for host connections - Press Ctrl+C to exit");

    // Main loop - just wait for signals
    while (application_running) {
        sleep(1);
    }

    cleanup();
    return 0;
}
