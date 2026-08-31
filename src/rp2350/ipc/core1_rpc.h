#pragma once
#include <stdint.h>
#include "shared_state.h"

// core1_rpc.h — channel 1 of the core boundary: command/reply RPC.
//
// Core 0 is always the initiator; Core 1 never opens a transaction, it only
// answers. This header is the contract: ipc/core1_rpc.cpp is the client side
// (Core 0), core1/rpc_server.cpp the server side (Core 1).
//
// Carried on two pico_util queue_t rings rather than the hardware inter-core
// FIFO. pico/multicore.h calls those FIFOs "a very precious resource" and says
// "the majority of cases for transferring data between cores can be equally well
// handled by using a queue". Three concrete reasons here:
//
//   - RP2350's FIFO is 4 entries deep (RP2040's was 8), and the old FIFO_HOME
//     sequence pushed exactly 4 words back to back — precisely at capacity.
//   - arduino-pico uses the same hardware FIFO. RP2040Support.h's _MFIFO pushes
//     a _GOTOSLEEP sentinel for rp2040.idleOtherCore(). Latent rather than live
//     (this project hand-rolls the flash_op_requested handshake instead), but it
//     is a standing constraint on ever using the FIFO here.
//   - multicore_fifo_pop_blocking() has no timeout. A wedged Core 1 hangs Core 0
//     forever, which is what forced the IDLE/PAUSED/ALARM gate onto every relay
//     command in control_plane.cpp.

// ─── Request ──────────────────────────────────────────────────────────────────
// One shape for every bus transaction. The old encoding packed cmd, node and a
// single argument byte into one 32-bit word, which is why CMD_HOME (11 bytes of
// payload) and the debug step (an int32) each needed their own multi-word opcode
// in the FIFO_* namespace. A struct has room, so those opcodes are gone: `cmd`
// is always a CMD_* from common.h, and the arguments are just bytes.

#define RPC_ARG_MAX      CMD_HOME_PAYLOAD_LEN   // 11 — the largest command payload
#define RPC_PAYLOAD_MAX  32                     // max node reply payload

// What Core 1 should DO with this request. The old encoding had no such field,
// so a Core-1-local action (the debug step burst) had to disguise itself as a
// node opcode: FIFO_STEP_DEBUG = 0xF0 sat in the same byte as CMD_*, chosen to
// be a value the node protocol did not use yet. A separate field ends the
// shadowing -- `cmd` is now always a real CMD_*, and never has to be checked
// against a private range first.
typedef enum {
    RPC_OP_NODE = 0,     // relay `cmd` to `node` over RS485 and report the reply
    RPC_OP_STEP_DEBUG,   // Core-1-local: emit a stream-byte step burst, no reply
} RpcOp;

typedef struct {
    uint8_t  op;                   // RpcOp
    uint8_t  cmd;                  // CMD_* (common.h) — RPC_OP_NODE only
    uint8_t  node;                 // bus id, or BUS_ADDR_BROADCAST
    uint16_t id;                   // echoed in the reply; 0 = fire-and-forget
    uint16_t session;              // node session token; 0 until §8.2 lands
    uint8_t  argLen;
    uint8_t  args[RPC_ARG_MAX];
} RpcRequest;

// How long rpcCall() waits before declaring Core 1 unresponsive.
//
// This is a WEDGE DETECTOR, not a latency budget. Core 1 services requests
// only between microsegments, so a request issued mid-stream legitimately
// waits out the queue -- measured at 4 s of it. The value is therefore well
// above any honest wait; what it buys over multicore_fifo_pop_blocking() is
// that it RETURNS, so a wedged Core 1 can no longer hang Core 0 forever.
//
// Callers that must stay responsive mid-job should use rpcPost/rpcPoll and
// keep running their loop, not shorten this.
#define RPC_CALL_TIMEOUT_MS 6000

// ─── Reply ────────────────────────────────────────────────────────────────────
// The result of a bus transaction is currently ONE BIT: Core 1 computes
// `rxLen != 0xFF ? 1u : 0u` at eleven sites and Core 0 reads `resp & 0xFFFF`.
// There is nowhere for a reason to live, which is why control_plane.cpp:405
// prints "nak_or_timeout" with a comment admitting the two cannot be told apart.
//
// An enum, so every call site is written against three cases from the start.
// RPC_NAK is unreachable until the node learns CMD_NAK (§8.1) — that is the
// point: adding it later is then a node-firmware change, not a 30-site rewrite.
typedef enum {
    RPC_OK = 0,      // node answered as expected
    RPC_TIMEOUT,     // no answer within RESPONSE_TIMEOUT_MS
    RPC_NAK,         // node refused — see nakReason (§8.1)
    RPC_BAD_REPLY,   // node answered, but not with something we can read:
                     // payload too short, or a shape this decoder does not know.
                     // Distinct from RPC_TIMEOUT on purpose — folding the two is
                     // exactly the nak_or_timeout conflation this module exists
                     // to remove, and "it is there but talking nonsense" wants a
                     // different response from "it is not there".
} RpcResult;

typedef struct {
    uint16_t  id;                        // echoes the request's
    uint8_t   cmd, node;                 // echoed so the caller can ASSERT, not
                                         // discard as `resp & 0xFFFF` did
    RpcResult result;
    uint8_t   nakReason;                 // valid only when result == RPC_NAK
    uint8_t   len;                       // payload bytes (0 for ack-only)
    uint8_t   payload[RPC_PAYLOAD_MAX];
} RpcReply;

// The word this result prints as: "ok", "timeout", "bad_reply", or
// "nak <reason>". One place, because eight call sites printed the literal
// "timeout" for every non-OK result and each would otherwise have to learn the
// new cases separately.
//
// Reads the reason from the LAST completed rpcCall, which is sound only because
// one transaction is in flight at a time (see rpcPost). Call it on the result
// you just received, before issuing another.
const char* rpcResultText(RpcResult r);

// ─── Decoded node status ──────────────────────────────────────────────────────
// Every command that reports node state answers with the same bytes, from one
// serializer on the node (buildNodeStatus): [type][flags][type tail…], stepper
// tail [pos int32 BE][slot]. Decode once, here; consumers read st.pos and never
// buf[NS_STEP_POS]. The NS_* offsets do not leave this module.
//
// KEEP the `len` parameter. It lets the decoder branch on payload length and
// zero-fill absent fields, which is the difference between a staged firmware
// rollout and having to flash every node at once.
typedef struct {
    uint8_t  type;
    uint8_t  flags;                // NODE_FLAG_* from common.h
    uint16_t session;              // 0 until §8.2
    uint8_t  resetCause;           // SET_SESSION ack only (§8.2)
    uint8_t  fwId;                 // SET_SESSION ack only (§8.2)
    int32_t  pos;                  // stepper tail
    uint8_t  slot;                 // stepper tail
    bool     hasStepperTail;       // false when the payload stopped at [flags]

    // The type-specific tail, verbatim, already offset past the generic head.
    // Decoding it means knowing what a vacuum node or a knife node puts there,
    // which is not this module's business -- so it hands the bytes on instead of
    // growing a case per node type.
    uint8_t  tail[RPC_PAYLOAD_MAX];
    uint8_t  tailLen;
} NodeStatus;

bool nodeStatusDecode(const uint8_t* buf, uint8_t len, NodeStatus* out);

// ─── Transport ────────────────────────────────────────────────────────────────
// Call once from setup(), before Core 1 launches.
void rpcInit(void);

// Discard every queued request and reply, and clear the in-flight claim.
// For the soft-reset path only, which parks Core 1 first -- there is no
// locking here, and none is needed while the far side cannot run.
void rpcReset(void);

// Post a request without waiting. Returns false if a transaction is already in
// flight or the queue is full; *idOut receives the request id.
//
// ONE TRANSACTION IN FLIGHT AT A TIME. The RS485 bus is serial, so serialising
// requests costs no throughput, and it buys two things: replies cannot arrive
// out of order, and the existing alarmAtEntry compare stays sufficient (Core 0
// cannot process the command that would clear an alarm while another command is
// outstanding). Do not relax this without revisiting both.
bool rpcPost(const RpcRequest* req, uint16_t* idOut);

// Collect a reply if one is ready. Never blocks. false = nothing yet.
bool rpcPoll(RpcReply* out);

// True while a posted request has not been collected.
bool rpcBusy(void);

// Post and wait. Convenience over post/poll for callers that have nothing else
// to do; every one of them can be converted to the async pair without touching
// this module. Returns RPC_TIMEOUT if Core 1 does not answer within timeoutMs
// — which, unlike multicore_fifo_pop_blocking(), actually returns.
RpcResult rpcCall(const RpcRequest* req, RpcReply* out, uint32_t timeoutMs);

// ─── Convenience wrappers ─────────────────────────────────────────────────────
// Built on rpcCall. These are what most of control_plane.cpp calls.

// Ack-only command with at most one argument byte (PING, ENABLE, DISABLE,
// SERVO_SET, SSR_SET, KNIFE_OSC, KNIFE_BLOWER, LASER).
//
// `node` may be BUS_ADDR_BROADCAST, in which case nothing answers and RPC_OK
// degrades to "the frame was sent" — never "a node acted on it". Callers must
// not read a broadcast's RPC_OK as evidence of node state.
RpcResult rpcNodeCmd(uint8_t cmd, uint8_t node, uint8_t arg);

// Command whose ack is a full status payload (NODE_STATUS, DATUM_SET, ENGAGE).
// Decodes on the way out, so callers never see raw bytes.
RpcResult rpcNodeStatus(uint8_t cmd, uint8_t node, uint8_t arg, NodeStatus* out);

// CMD_SWITCH_GET — replies with a single level byte.
RpcResult rpcSwitchGet(uint8_t node, uint8_t* level);

// CMD_HOME. Core 1 only marshals: it does not know seek from retract, does not
// interpret the reply and runs no supervision. The node decides the mode from
// its own limit pin; Core 0 polls for the outcome.
RpcResult rpcHome(uint8_t node, uint8_t dir, uint16_t startIntervalUs,
                  uint16_t floorIntervalUs, uint16_t rampSteps,
                  uint32_t maxSteps, NodeStatus* out);

// ─── Server side (Core 1) ─────────────────────────────────────────────────────
// core1/rpc_server.cpp implements these; nothing on Core 0 calls them.

// Service at most one pending request: take it, run the bus transaction, post
// the reply. Returns false if there was nothing to do. Call from Core 1's loop
// AFTER the segment queue is drained -- a request must never delay a step.
bool rpcServerPoll(void);

// Queue accessors, so the server does not need the queue_t handles themselves.
bool rpcServerTake(RpcRequest* out);
bool rpcServerReply(const RpcReply* rep);

// ─── Debug step burst — fire-and-forget, no reply ─────────────────────────────
//
// Both parameters ride the request rather than sitting in shared globals, and
// that is not tidiness: a second burst issued before Core 1 picked up the first
// would otherwise have overwritten a shared rate and run burst #1 at burst #2's
// speed. Queued, each burst carries its own.
bool rpcStepDebug(uint8_t slot, uint16_t sps, int32_t steps);

// ─── Debug-step limits ───────────────────────────────────────────────────────
// Argument bounds for RPC_OP_STEP_DEBUG. Core 0 validates against these before
// posting and core1/emit/debug_step.cpp applies the default, so they are part
// of the channel-1 contract rather than either side's private business.
#define STEP_DEBUG_SPS       1000        // default emit rate (steps/sec)
#define STEP_DEBUG_SPS_MAX  60000        // must fit the 16-bit field; also stays
                                         // under the ~92k bytes/s the bus can do
                                         // at 921.6 kbaud (one byte per step)
#define STEP_DEBUG_MAX  100000000L       // ~28 min at the max rate — a ceiling on
                                         // typos, not on anything useful
