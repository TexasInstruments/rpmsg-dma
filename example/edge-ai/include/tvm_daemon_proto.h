/*
 * tvm_daemon_proto.h — Wire protocol shared by tvm_model_daemon and TvmInferenceClient.
 *
 * Layout (all fields native-endian / little-endian on ARM):
 *
 *   Client → Daemon:  Header{MAGIC, PING,       0}
 *   Daemon → Client:  Header{MAGIC, PONG,       0}
 *
 *   Client → Daemon:  Header{MAGIC, INFER_REQ,  n_bytes} + n_bytes of float32 input
 *   Daemon → Client:  Header{MAGIC, INFER_RESP, n_bytes} + n_bytes of float32 output
 *   Daemon → Client:  Header{MAGIC, ERROR_RESP, n_bytes} + n_bytes of UTF-8 error string
 *
 *   Client → Daemon:  Header{MAGIC, LOAD_MODEL, n_bytes} + n_bytes of UTF-8 artifacts path
 *   Daemon → Client:  Header{MAGIC, LOAD_RESP,  0}        (success)
 *   Daemon → Client:  Header{MAGIC, ERROR_RESP, n_bytes} + n_bytes of UTF-8 error string (failure)
 *
 * LOAD_MODEL hot-reloads the TVM model inside the running daemon — no daemon restart or
 * root privileges required.  Any process that can reach the socket may issue LOAD_MODEL.
 */

#pragma once

#include <cstdint>

namespace TvmDaemon {

static constexpr const char* SOCKET_PATH = "/var/run/tvm-inference.sock";
static constexpr uint32_t    MAGIC       = 0x544D5644u; /* 'TMVD' */

enum class MsgType : uint32_t {
    PING       = 0,
    PONG       = 1,
    INFER_REQ  = 2,
    INFER_RESP = 3,
    ERROR_RESP = 4,
    LOAD_MODEL = 5,  /* client → daemon: request hot-reload of artifacts at given path */
    LOAD_RESP  = 6,  /* daemon → client: model loaded successfully (len=0) */
};

struct Header {
    uint32_t magic;
    uint32_t type;  /* MsgType cast to uint32_t */
    uint32_t len;   /* payload bytes following this header (0 for PING/PONG) */
};

} // namespace TvmDaemon
