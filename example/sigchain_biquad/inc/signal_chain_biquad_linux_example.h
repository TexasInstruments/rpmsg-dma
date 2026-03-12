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
 * @file signal_chain_biquad_linux_example.h
 * @brief TI Cascade Biquad Parametric EQ Signal Chain Example
 *
 */

#ifndef SIGNAL_CHAIN_BIQUAD_LINUX_EXAMPLE_H
#define SIGNAL_CHAIN_BIQUAD_LINUX_EXAMPLE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// === FORWARD DECLARATIONS ===

struct dma_buf_params;

// === FIRMWARE CONFIGURATION ===

/** @brief Path to the base C7x firmware (to restore on exit) */
#define C7_BASE_FW      "/lib/firmware/ti-ipc/am62dxx/ipc_echo_test_c7x_1_release_strip.xe71"

/** @brief Path to the signal chain biquad cascade firmware */
#define C7_SIGCHAIN_FW  "/lib/firmware/sigchain_biquad_cascade.c75ss0-0.release.strip.out"

/** @brief Symbolic link to the active C7x firmware */
#define C7_FW_LINK      "/lib/firmware/am62d-c71_0-fw"

// === REMOTEPROC CONFIGURATION ===

/** @brief Maximum remoteproc devices to scan */
#define MAX_REMOTEPROC_DEVICES  3

/** @brief DMA heap name for shared memory */
#define DMA_HEAP_NAME   "linux,cma"

/** @brief C7x processor ID for RPMessage */
#define C7_PROC_ID      8

/** @brief Remote endpoint for signal chain communication */
#define RMT_EP          13

// === NETWORK CONFIGURATION ===

#define LOG_PORT        8888    /**< Log/status messages port */
#define CMD_PORT        8889    /**< Command interface port */
#define STATS_PORT      8890    /**< C7x statistics streaming port */

#define MAX_CLIENTS     1       /**< Maximum concurrent client connections */
#define LOG_BUFFER_SIZE 1024    /**< Log message buffer size */

// === C7x SHARED MEMORY STRUCTURE ===

/**
 * @brief Real-time info structure shared between Linux and C7x
 * Must match RtInfo structure in C7x firmware exactly
 */
typedef struct __attribute__((__packed__)) {
    uint32_t c7xLoad;       /**< C7x load percentage * 100 (e.g., 2534 = 25.34%) */
    uint32_t cycleCount;    /**< Processing cycles per audio frame */
    float throughput;       /**< Audio throughput in MB/s */
    uint32_t demoCommand;  /**< demo command: 0=no_change, 1=start, 2=stop */
    uint32_t demoRunning;  /**< demo status: 0=stopped, 1=running */
} real_time_info;

// === IPC MESSAGE STRUCTURES ===

/**
 * @brief IPC message buffer structure for C7x communication (ORIGINAL FORMAT)
 * Must match ipc_msg_buffer definition in C7x firmware exactly
 * This is the 16-byte format that C7x expects for proper communication
 */
typedef struct __attribute__((__packed__)) {
    uint32_t dataBuffer;    /**< Physical address of data buffer */
    uint32_t paramsBuffer;  /**< Physical address of params buffer */
    int32_t dataSize;       /**< Size of data buffer in bytes */
    int32_t paramsSize;     /**< Size of params buffer in bytes */
} ipc_msg_buffer;


// === demo COMMANDS ===

#define DEMO_CMD_NO_CHANGE  0
#define DEMO_CMD_START      1
#define DEMO_CMD_STOP       2

// === demo RUNNING STATES ===

#define DEMO_STATE_STOPPED  0
#define DEMO_STATE_RUNNING  1

// === C7X COMMUNICATION CONSTANTS ===

#define C7X_COMMAND_TIMEOUT_MS      5000    /**< Command acknowledgment timeout */
#define C7X_STATUS_CHECK_DELAY_MS   100     /**< Status check polling interval */
#define C7X_MAX_RESPONSE_SIZE       64      /**< Maximum response message size */

// === DATA STRUCTURES ===

/**
 * @brief C7x communication status
 */
typedef struct {
    bool initialized;               /**< Communication initialized */
    bool connected;                 /**< C7x connection active */
    bool shared_memory_ready;       /**< Shared memory accessible */
    int rpmsg_fd;                   /**< RPMsg file descriptor */
    uint32_t last_command_sent;     /**< Last command sent to C7x */
    uint64_t last_command_time;     /**< Timestamp of last command */
    char remoteproc_device[256];    /**< Remoteproc device path */
} c7x_comm_status;

/**
 * @brief C7x shared memory information
 */
typedef struct {
    volatile real_time_info *memory_ptr;     /**< Pointer to shared memory */
    struct dma_buf_params dma_buffer;        /**< DMA buffer parameters */
    uint32_t buffer_size;                    /**< Buffer size in bytes */
    uint64_t physical_address;               /**< Physical memory address */
} c7x_shared_memory;


// === FUNCTION DECLARATIONS ===

/**
 * @brief Find the C7x C7x remoteproc device dynamically
 * @return 0 on success, -1 on failure
 */
int find_c7x_remoteproc(void);

/**
 * @brief Initialize C7x communication and shared memory
 * @return true on success, false on failure
 */
bool init_c7x_communication(void);

/**
 * @brief Send control command to C7x via shared memory
 * @param command control command (START/STOP)
 * @return true on success, false on failure
 */
bool c7x_comm_send_control_command(uint32_t command);


/**
 * @brief Thread-safe logging function
 * @param format Printf-style format string
 * @param ... Variable arguments
 */
void log_message(const char *format, ...);

/**
 * @brief Cleanup function - restores system state
 */
void cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_CHAIN_BIQUAD_LINUX_EXAMPLE_H */
