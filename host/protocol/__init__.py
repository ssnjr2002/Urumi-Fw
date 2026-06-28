"""protocol — owns the Pico USB link and the frozen wire contract.

link.py      serial open/close, two-plane dispatch, RX read loop
packets.py   binary packers/parsers (MSEG / JOG / MCFG / ACK / NACK)
commands.py  text control plane (getstate/pause/resume/...) + reply parsing
stream.py    windowed ACK/NACK streaming engine
state.py     MachineState / AlarmReason / RunningReason enums + getstate parse
"""
