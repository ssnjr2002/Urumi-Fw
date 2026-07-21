"""
test_reader.py — exercise the demultiplexing reader's state machine.

No hardware and no serial port: Demux is pure, so it is driven by handing it byte
strings directly. That is the point of splitting it from the Reader thread — the
interesting failures are all classification failures, and they are deterministic.

The load-bearing test is `test_cfg_data_payload_full_of_magics`. Everything else
guards framing; that one guards the reason the state machine exists at all.

Run:  python -m host.diagnostics.test_reader
"""

import sys

from host.protocol.reader import (
    Demux, Sink, LatestSink, Ack, Nack, CfgReply, make_sinks,
    MAX_TEXT_LINE, MAX_CFG_PAYLOAD,
)
from host.protocol.packets import (
    MAGIC_ACK, MAGIC_NACK, MAGIC_STATUS_RSP, MAGIC_CFG_RDY, MAGIC_CFG_ACK,
    MAGIC_CFG_NACK, MAGIC_CFG_DATA, NACK_FULL, CFG_NACK_CRC,
    pack_status_rsp,
)

_failures = []


def check(cond, label):
    if cond:
        print(f"  ok   {label}")
    else:
        print(f"  FAIL {label}")
        _failures.append(label)


def new_demux():
    s = make_sinks()
    return Demux(s["ack"], s["status"], s["text"], s["cfg"]), s


def drain(sink):
    out = []
    while True:
        item = sink.get(timeout=0)
        if item is None:
            break
        out.append(item)
    return out


def ack(seq):   return bytes([MAGIC_ACK, seq, 0x00])
def nack(r):    return bytes([MAGIC_NACK, r, 0x00])
def status(buf_count=0):
    return pack_status_rsp(state=1, axes_enabled=0x07, axes_homed=0x07,
                           alarm=0, running=1, buf_count=buf_count)


def cfg_data(payload, crc=0xDEADBEEF):
    return (bytes([MAGIC_CFG_DATA])
            + len(payload).to_bytes(4, "little")
            + crc.to_bytes(4, "little")
            + payload)


# ── framing basics ────────────────────────────────────────────────────────────

def test_routes_by_magic():
    print("\nrouting by magic")
    d, s = new_demux()
    d.feed(ack(5) + status(3) + b"ok\n" + nack(NACK_FULL))

    acks = drain(s["ack"])
    check(len(acks) == 2, "two frames on the ack sink")
    check(isinstance(acks[0], Ack) and acks[0].expected_seq == 5, "ACK carries expected_seq")
    check(isinstance(acks[1], Nack) and acks[1].reason == NACK_FULL, "NACK carries reason")
    check(s["status"].value == status(3), "status landed in the latest-wins slot")
    check(drain(s["text"]) == ["ok"], "text line landed on the text sink")
    check(d.idle, "demux is idle after complete input")


def test_split_across_chunks():
    """A frame split across reads is the NORMAL case at the 64-byte USB quantum,
    not an edge case. Feed one byte at a time and require identical results."""
    print("\nframes split across chunks")
    whole = ack(1) + status(9) + b"hello\n" + cfg_data(b"\x01\x02\x03")

    d1, s1 = new_demux()
    d1.feed(whole)

    d2, s2 = new_demux()
    for b in whole:
        d2.feed(bytes([b]))

    check(len(drain(s1["ack"])) == len(drain(s2["ack"])) == 1, "same ack count")
    check(s1["status"].value == s2["status"].value, "same status sample")
    check(drain(s1["text"]) == drain(s2["text"]) == ["hello"], "same text")
    c1, c2 = drain(s1["cfg"]), drain(s2["cfg"])
    check(len(c1) == len(c2) == 1 and c1[0].payload == c2[0].payload == b"\x01\x02\x03",
          "same cfg payload")
    check(d1.idle and d2.idle, "both idle")

    # And split at every possible offset, since a bad boundary is offset-specific.
    ok = True
    for cut in range(len(whole) + 1):
        d, s = new_demux()
        d.feed(whole[:cut])
        d.feed(whole[cut:])
        if not (d.idle and len(drain(s["ack"])) == 1
                and drain(s["text"]) == ["hello"]
                and len(drain(s["cfg"])) == 1):
            ok = False
            print(f"       mismatch at split offset {cut}")
    check(ok, f"identical result at all {len(whole) + 1} split offsets")


# ── the reason this is a state machine ────────────────────────────────────────

def test_cfg_data_payload_full_of_magics():
    """THE test. A CFG_DATA payload is opaque msgpack and may contain any byte,
    including 0xAA/0xBB/0xA6. A magic-scanning reader emits phantom ACKs from
    inside it, advancing a session's `base` against packets never sent."""
    print("\nCFG_DATA payload containing magic bytes")

    hostile = bytes([MAGIC_ACK, 0xFF, MAGIC_NACK, 0x02, MAGIC_STATUS_RSP,
                     MAGIC_ACK, MAGIC_ACK, MAGIC_CFG_DATA, 0x00, MAGIC_CFG_ACK])
    d, s = new_demux()
    d.feed(cfg_data(hostile))

    check(drain(s["ack"]) == [], "NO phantom ACK/NACK emitted from payload bytes")
    check(s["status"].value is None, "NO phantom status sample")
    cfg = drain(s["cfg"])
    check(len(cfg) == 1 and cfg[0].payload == hostile, "payload delivered verbatim")
    check(d.idle, "demux resynced after the opaque payload")

    # ...and a real ACK immediately after is still seen: the payload consumed
    # exactly `length` bytes and not one more.
    d.feed(ack(7))
    acks = drain(s["ack"])
    check(len(acks) == 1 and acks[0].expected_seq == 7, "real ACK after payload is seen")


def test_cfg_data_empty():
    print("\nCFG_DATA with length 0 (no config stored)")
    d, s = new_demux()
    d.feed(cfg_data(b"") + ack(1))
    cfg = drain(s["cfg"])
    check(len(cfg) == 1 and cfg[0].payload == b"", "empty payload delivered")
    check(len(drain(s["ack"])) == 1, "following ACK still seen")
    check(d.idle, "idle")


def test_cfg_data_absurd_length():
    """A corrupted length must not park the reader in the payload state, where it
    would swallow every other plane's traffic until that many bytes arrived."""
    print("\nCFG_DATA with an absurd length")
    d, s = new_demux()
    d.feed(bytes([MAGIC_CFG_DATA])
           + (MAX_CFG_PAYLOAD + 1).to_bytes(4, "little")
           + (0).to_bytes(4, "little"))
    check(d.idle, "did not enter the payload state")
    check(d.stats()["overruns"] == 1, "counted as an overrun")
    d.feed(ack(3))
    check(len(drain(s["ack"])) == 1, "reader still functional afterwards")


# ── interleaving and resync ───────────────────────────────────────────────────

def test_text_interleaved_with_binary():
    """The firmware interleaves command echoes with stream ACKs on one pipe.
    Neither may disturb the other's framing."""
    print("\ntext interleaved with binary")
    d, s = new_demux()
    d.feed(ack(1) + b"Node 2: Enabled\n" + ack(2) + b"ok\n" + status(1) + ack(3))

    acks = drain(s["ack"])
    check([a.expected_seq for a in acks] == [1, 2, 3], "all three ACKs in order")
    check(drain(s["text"]) == ["Node 2: Enabled", "ok"], "both text lines intact")
    check(s["status"].value is not None, "status still routed")


def test_unknown_byte_resync():
    """An unknown high byte is discarded ONE at a time — discarding more could
    eat the start of a valid frame."""
    print("\nunknown byte handling")
    d, s = new_demux()
    d.feed(bytes([0xF0, 0xF1]) + ack(4))
    check(d.stats()["unknown_bytes"] == 2, "both unknown bytes counted")
    acks = drain(s["ack"])
    check(len(acks) == 1 and acks[0].expected_seq == 4, "next valid frame recovered")


def test_truncated_frame_then_resync():
    """A frame cut short leaves the demux mid-frame; it consumes subsequent bytes
    as that frame's body. This is expected — the guard against it is the
    firmware-side inter-byte timeout, not host-side guessing."""
    print("\ntruncated frame")
    d, s = new_demux()
    d.feed(bytes([MAGIC_ACK, 0x01]))          # 2 of 3 bytes
    check(not d.idle, "demux correctly waiting for the third byte")
    check(drain(s["ack"]) == [], "no frame emitted yet")
    d.feed(bytes([0x00]))
    check(len(drain(s["ack"])) == 1 and d.idle, "completes when the byte arrives")


def test_single_byte_cfg_replies():
    print("\nsingle-byte CFG replies")
    d, s = new_demux()
    d.feed(bytes([MAGIC_CFG_RDY]) + bytes([MAGIC_CFG_ACK])
           + bytes([MAGIC_CFG_NACK, CFG_NACK_CRC]))
    cfg = drain(s["cfg"])
    check([c.kind for c in cfg] == [MAGIC_CFG_RDY, MAGIC_CFG_ACK, MAGIC_CFG_NACK],
          "three cfg replies in order")
    check(cfg[2].reason == CFG_NACK_CRC, "CFG_NACK carries its reason")


def test_text_overrun():
    print("\nrunaway text line")
    d, s = new_demux()
    d.feed(b"A" * (MAX_TEXT_LINE + 50) + b"\n" + ack(2))
    check(d.stats()["overruns"] >= 1, "overrun counted")
    check(len(drain(s["ack"])) == 1, "reader recovered")


# ── sink semantics ────────────────────────────────────────────────────────────

def test_latest_wins():
    """Status is a state sample, not a transaction result: only the newest
    matters, and an old one is worthless rather than merely late (D9)."""
    print("\nlatest-wins status sink")
    d, s = new_demux()
    d.feed(status(1) + status(2) + status(9))
    check(s["status"].value == status(9), "slot holds only the newest sample")

    lat = LatestSink("t")
    check(lat.value is None, "empty slot reads None, does not block")
    lat.put("a")
    v, stamp = lat.wait_update(timeout=0, since=0)
    check(v == "a", "wait_update returns immediately when newer than `since`")
    v2, _ = lat.wait_update(timeout=0.01)
    check(v2 == "a", "wait_update times out to the current value, not None")


def test_ack_sink_never_drops():
    print("\nack sink ordering")
    d, s = new_demux()
    d.feed(b"".join(ack(i) for i in range(50)))
    acks = drain(s["ack"])
    check([a.expected_seq for a in acks] == list(range(50)), "all 50 ACKs, in order")


def main():
    print("=" * 62)
    print("demux reader — state machine tests")
    print("=" * 62)

    for fn in (test_routes_by_magic, test_split_across_chunks,
               test_cfg_data_payload_full_of_magics, test_cfg_data_empty,
               test_cfg_data_absurd_length, test_text_interleaved_with_binary,
               test_unknown_byte_resync, test_truncated_frame_then_resync,
               test_single_byte_cfg_replies, test_text_overrun,
               test_latest_wins, test_ack_sink_never_drops):
        fn()

    print("\n" + "=" * 62)
    if _failures:
        print(f"{len(_failures)} FAILED:")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
