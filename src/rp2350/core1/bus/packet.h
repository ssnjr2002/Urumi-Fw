#pragma once
#include <stdint.h>
#include "RS485Bus.h"

// packet.h — the frame layer over RS485Bus's byte layer.
//
// RS485Bus moves 9-bit words; this file moves [ID][CMD][LEN][payload...][CRC8]
// packets and knows what an answer looks like. Split out of core1.cpp so
// rpc_server.cpp can send frames without owning the whole of Core 1.

extern RS485Bus rs485;

// Stamps the CRC into packet[len-1] and writes the frame. `packet` is mutated.
void sendPacket(uint8_t* packet, uint8_t len);

// Waits for a frame from `expectedNode` answering `expectedCmd`. Returns the
// payload length, or 0xFF on timeout. Stream bytes seen while waiting are
// discarded and restart the frame.
//
// CMD_NAK is ALSO accepted, whatever `expectedCmd` is: a refusal is an answer to
// the command, and the frame filter is the one place that would otherwise throw
// it away and let the caller time out — reintroducing the exact ambiguity the
// opcode removes. `outCmd`, when given, receives the opcode that actually
// arrived, so the caller can tell the two apart. Callers that pass nullptr get
// the old behaviour and read a NAK as a (short, unparseable) success, so every
// caller that can receive one must pass it.
uint8_t receivePacket(uint8_t expectedNode, uint8_t expectedCmd,
                      uint8_t* outPayload, uint32_t timeoutMs,
                      uint8_t* outCmd = nullptr);

// Send one broadcast command. Refuses anything outside the allowlist, so the
// deny-by-default rule is enforced at both ends rather than trusted at one: the
// node would drop it anyway, but a silent no-op on the wire is a worse bug to
// find than a call that never compiles into an effect.
bool sendBroadcast(uint8_t cmd);

// ─── Whole-bus safe-off ──────────────────────────────────────────────────────
// CMD_DISABLE to every address, replies consumed and discarded.
//
// The sweep covers the WHOLE bus, not the axis map, because CMD_DISABLE is the
// generic "park yourself" hook and each node type implements it as its own safe
// state: a stepper de-energises, a vacuum node stops the pump, a knife node
// kills the oscillator and the blower. Peripherals hold no motion slot, so
// slotNode[] cannot reach them — and they are precisely the ones that must not
// keep running after an estop, since the blade is still in the material.
//
// Costs up to RESPONSE_TIMEOUT_MS per ABSENT address (a present node answers in
// microseconds), so a sparsely-populated bus makes this the slowest thing on
// the estop path. That is acceptable: motion has already stopped by flushing
// the queue, and this is the cleanup behind it.
// One unacknowledged frame that starts every node parking at once, ~50us on the
// wire. The serial sweep below still runs and still gates the ALARM transition —
// this only changes WHEN each node begins stopping, from "at its turn in an
// 8-address walk, most of it spent timing out on empty addresses" to "now".
//
// Nodes dispatch from cmdQueue in loop(), not in the RX ISR, so the honest claim
// is that every node starts within one loop() of every other — not that the stop
// is instantaneous. The serial sweep pays that same per-node latency anyway, plus
// the round trips.
void busDisableAll(void);
