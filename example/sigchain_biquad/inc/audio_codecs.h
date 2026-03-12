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
 * @file audio_codecs.h
 * @brief TI Audio Codecs Control Module - TAD5212 DAC + PCM6240 ADC
 *
 * This module provides hardware abstraction for following codecs on AM62D2-EVM
 * - 4 TAD5212 DAC: each 2-channel
 * - 2 PCM6240 ADC: each 4-channel
 *
 * Features:
 * - I2C register-based configuration
 * - Power management (init/shutdown)
 * - Multiple codec addressing support
 * - Thread-safe operations
 */

#ifndef AUDIO_CODECS_H
#define AUDIO_CODECS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// === DATA STRUCTURES ===

/**
 * @brief I2C register-value pair for codec configuration
 */
typedef struct {
    uint8_t reg;    /**< Register address */
    uint8_t val;    /**< Register value */
} audio_codec_register;

// === PUBLIC CONSTANTS ===

/** @brief Codec count constants */
#define PCM6240_ADC_COUNT 2
#define TAD5212_DAC_COUNT 4

// === PUBLIC VARIABLES ===

/** @brief PCM6240 ADC I2C addresses */
static const uint8_t pcm6240_i2c_addresses[PCM6240_ADC_COUNT] = {0x48, 0x49};

/** @brief TAD5212 DAC I2C addresses (4 DACs) */
static const uint8_t tad5212_i2c_addresses[TAD5212_DAC_COUNT] = {0x50, 0x51, 0x52, 0x53};

// === CODEC REGISTER TABLES ===

/** @brief TAD5212 DAC power-on configuration sequence */
static const audio_codec_register tad5212_power_on[] = {
    {0x00,0x00},{0x02,0x01},{0x1A,0x70},
    {0x64,0x28},{0x65,0x60},{0x67,0xC9},
    {0x6B,0x28},{0x6C,0x60},{0x6E,0xC9},
    {0x29,0x30},{0x76,0x0C},
    {0x00,0x01},{0x0a,0x10},{0x1a,0x40},
    {0x24,0x06},{0x2d,0x05},{0x2f,0x07},
    {0x30,0x07},
    {0x47,0x00},{0x48,0x00},{0x4a,0xb0},
    {0x53,0x80},
    {0x00,0x03},
    {0x38,0x24},{0x39,0x28},{0x3a,0x26},{0x3b,0x20},
    {0x3c,0x00},{0x3d,0x00},{0x3e,0x09},
    {0x48,0x01},{0x49,0x01},
    {0x00,0x00},{0x78,0x40}
};

/** @brief TAD5212 DAC power-off configuration sequence */
static const audio_codec_register tad5212_power_off[] = {
    {0x00,0x00}, {0x02,0x00}
};

/** @brief PCM6240 ADC common configuration */
static const audio_codec_register pcm6240_common_config[] = {
    {0x00,0x00}, {0x01,0x01}, {0x00,0x00}, {0x02,0x09},
    {0x07,0x31}, {0x08,0x01}, {0x3B,0x70}, {0x3C,0x10},
    {0x41,0x10}, {0x46,0x10}, {0x4B,0x10},
    {0x74,0xF0}, {0x75,0xE0}
};

/** @brief PCM6240 ADC0 routing configuration */
static const audio_codec_register pcm6240_adc0_routing[] = {
    {0x0B,0x00},{0x0C,0x04},{0x0D,0x01},{0x0E,0x05}
};

/** @brief PCM6240 ADC1 routing configuration */
static const audio_codec_register pcm6240_adc1_routing[] = {
    {0x0B,0x02},{0x0C,0x06},{0x0D,0x03},{0x0E,0x07}
};

/** @brief PCM6240 ADC power-off configuration */
static const audio_codec_register pcm6240_power_off[] = {
    {0x01,0x00}
};

// === ADDITIONAL DATA STRUCTURES ===

/**
 * @brief Audio codec initialization status
 */
typedef struct {
    bool tad5212_initialized;       /**< TAD5212 DACs initialized */
    bool pcm6240_initialized;       /**< PCM6240 ADCs initialized */
    bool i2c_available;             /**< I2C bus accessible */
} audio_codec_status;

// === FUNCTION DECLARATIONS ===

/**
 * @brief Initialize all audio codecs (TAD5212 DACs + PCM6240 ADCs)
 *
 * This function performs complete audio codec initialization:
 * - Powers on all TAD5212 DAC channels (addresses 0x50-0x53)
 * - Configures PCM6240 ADCs (addresses 0x48-0x49)
 * - Sets up I2S routing and sample rates
 * - Enables audio signal path
 *
 * @param i2c_bus_path Path to I2C device (e.g., "/dev/i2c-1"), NULL for default
 * @return true on success, false on failure
 */
bool audio_codecs_init(const char *i2c_bus_path);

/**
 * @brief Shutdown all audio codecs
 *
 * This function safely powers down all audio codecs:
 * - Disables audio signal paths
 * - Powers down TAD5212 DACs
 * - Powers down PCM6240 ADCs
 * - Releases I2C resources
 *
 * @return true on success, false on failure
 */
bool audio_codecs_shutdown(void);

/**
 * @brief Check current audio codec initialization status
 *
 * @param status Pointer to status structure to fill
 * @return true if status retrieved successfully, false otherwise
 */
bool audio_codecs_get_status(audio_codec_status *status);


#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODECS_H */