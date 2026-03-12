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
 *    from this software without specific written permission.
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
 * @file audio_codecs.c
 * @brief TI Audio Codecs Control Implementation
 *
 * Hardware-specific implementation for TI audio codec control:
 * - TAD5212: 8-channel DAC (I2C addresses 0x50-0x53)
 * - PCM6240: 4-channel ADC (I2C addresses 0x48-0x49)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#include "audio_codecs.h"

// === PRIVATE CONSTANTS ===

/** @brief I2C bus device path constant */
static const char* I2C_BUS_PATH = "/dev/i2c-1";

// === GLOBAL STATE ===

/** @brief Current codec initialization status */
static audio_codec_status current_codec_status = {
    .tad5212_initialized = false,
    .pcm6240_initialized = false,
    .i2c_available = false
};

// === PRIVATE FUNCTIONS ===

/**
 * @brief Write single register via I2C
 */
static int i2c_write_register(int i2c_fd, uint8_t device_addr, uint8_t reg_addr, uint8_t value)
{
    // Set I2C slave address
    if (ioctl(i2c_fd, I2C_SLAVE, device_addr) < 0) {
        fprintf(stderr, "ERROR: Failed to set I2C slave address=0x%02x: %s\n", device_addr, strerror(errno));
        return -1;
    }

    // Write register and value
    uint8_t data[2] = {reg_addr, value};
    if (write(i2c_fd, data, 2) != 2) {
        fprintf(stderr, "ERROR: Failed to write value=0x%02x to register address=0x%02x: %s\n", value, reg_addr, strerror(errno));
        return -1;
    }

    // Small delay for register write
    usleep(1000); // 1ms
    return 0;
}

/**
 * @brief Write register table to I2C device
 */
static bool write_codec_register_table(int i2c_fd, uint8_t device_addr,
                                     const audio_codec_register *table, size_t table_size)
{
    for (size_t reg_index = 0; reg_index < table_size; reg_index++) {
        if (i2c_write_register(i2c_fd, device_addr, table[reg_index].reg, table[reg_index].val) < 0) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Get array size for register tables
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// === PUBLIC FUNCTIONS ===

bool audio_codecs_init(const char *i2c_bus_path)
{
    const char *bus_path = (i2c_bus_path != NULL) ? i2c_bus_path : I2C_BUS_PATH;

    // Reset status
    current_codec_status.tad5212_initialized = false;
    current_codec_status.pcm6240_initialized = false;
    current_codec_status.i2c_available = false;

    // Open I2C bus
    int i2c_fd = open(bus_path, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus %s: %s\n", bus_path, strerror(errno));
        return false;
    }

    current_codec_status.i2c_available = true;
    bool overall_status = true;

    // Initialize TAD5212 DACs
    printf("Configuring TAD5212 DACs...\n");
    for (int codec_index = 0; codec_index < TAD5212_DAC_COUNT; codec_index++) {
        if (!write_codec_register_table(i2c_fd, tad5212_i2c_addresses[codec_index],
                                      tad5212_power_on, ARRAY_SIZE(tad5212_power_on))) {
            fprintf(stderr, "ERROR: Failed to configure TAD5212 at 0x%02x\n", tad5212_i2c_addresses[codec_index]);
            overall_status = false;
        }
    }

    if (overall_status) {
        current_codec_status.tad5212_initialized = true;
    }

    // Initialize PCM6240 ADCs
    printf("Configuring PCM6240 ADCs...\n");

    // Array of routing configurations for each ADC
    const audio_codec_register* pcm6240_routing_configs[PCM6240_ADC_COUNT] = {
        pcm6240_adc0_routing,  // ADC0 routing
        pcm6240_adc1_routing   // ADC1 routing
    };

    const size_t pcm6240_routing_sizes[PCM6240_ADC_COUNT] = {
        ARRAY_SIZE(pcm6240_adc0_routing),  // ADC0 routing size
        ARRAY_SIZE(pcm6240_adc1_routing)   // ADC1 routing size
    };

    // Configure each PCM6240 ADC in a for loop
    for (int adc_index = 0; adc_index < PCM6240_ADC_COUNT; adc_index++) {
        // First apply common configuration
        if (!write_codec_register_table(i2c_fd, pcm6240_i2c_addresses[adc_index],
                                       pcm6240_common_config, ARRAY_SIZE(pcm6240_common_config))) {
            fprintf(stderr, "ERROR: Failed to configure PCM6240 common config at 0x%02x\n", pcm6240_i2c_addresses[adc_index]);
            overall_status = false;
            continue;
        }

        // Then apply specific ADC routing configuration
        if (!write_codec_register_table(i2c_fd, pcm6240_i2c_addresses[adc_index],
                                       pcm6240_routing_configs[adc_index], pcm6240_routing_sizes[adc_index])) {
            fprintf(stderr, "ERROR: Failed to configure PCM6240 routing at 0x%02x\n", pcm6240_i2c_addresses[adc_index]);
            overall_status = false;
        }
    }

    if (overall_status) {
        current_codec_status.pcm6240_initialized = true;
    }

    close(i2c_fd);

    if (overall_status) {
        printf("[OK] Audio codecs initialized successfully\n");
    } else {
        fprintf(stderr, "[ERROR] Some codec initialization failures occurred\n");
    }

    return overall_status;
}

bool audio_codecs_shutdown(void)
{
    if (!current_codec_status.tad5212_initialized && !current_codec_status.pcm6240_initialized) {
        return true; // Nothing to shutdown
    }

    // Open I2C bus
    int i2c_fd = open(I2C_BUS_PATH, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus for shutdown: %s\n", strerror(errno));
        return false;
    }

    bool overall_status = true;

    printf("Shutting down audio codecs...\n");

    // Power off PCM6240 ADCs first
    if (current_codec_status.pcm6240_initialized) {
        for (int codec_index = 0; codec_index < PCM6240_ADC_COUNT; codec_index++) {
            if (!write_codec_register_table(i2c_fd, pcm6240_i2c_addresses[codec_index],
                                          pcm6240_power_off, ARRAY_SIZE(pcm6240_power_off))) {
                overall_status = false;
            }
        }
        if (overall_status) {
            current_codec_status.pcm6240_initialized = false;
        }
    }

    // Power off TAD5212 DACs
    if (current_codec_status.tad5212_initialized) {
        for (int codec_index = 0; codec_index < TAD5212_DAC_COUNT; codec_index++) {
            if (!write_codec_register_table(i2c_fd, tad5212_i2c_addresses[codec_index],
                                          tad5212_power_off, ARRAY_SIZE(tad5212_power_off))) {
                overall_status = false;
            }
        }
        if (overall_status) {
            current_codec_status.tad5212_initialized = false;
        }
    }

    close(i2c_fd);

    if (overall_status) {
        printf("[OK] Audio codecs shutdown successfully\n");
    } else {
        fprintf(stderr, "[ERROR] Some codec shutdown failures occurred\n");
    }

    return overall_status;
}

bool audio_codecs_get_status(audio_codec_status *status)
{
    if (status == NULL) {
        return false;
    }

    *status = current_codec_status;
    return true;
}

