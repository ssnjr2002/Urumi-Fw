"""
sim_config.py — the machine the simulator pretends to be.

Edit this freely to exercise the config-driven GUI; it is ONLY used by
`python -m host.gui --sim`. Real hardware uses the production config.default().

Things you can do here and watch the GUI adapt to:
  - add peripherals  → they appear in the Peripherals panel (presence-pingable)
  - drop an axis     → replace its node with present=False; its jog row + status
                       readout disappear (e.g. a pen machine with no A axis)
  - add a second head → a head with its own Z/A + x_offset (head switching UI is
                       future work, but the config carries it)

The simulator's fake bus reports every node present, so declared peripherals
ping OK — enough to see the config-driven UI light up.
"""

from dataclasses import replace

from config import default, BusNode, AxisConfig


def sim_machine():
    """Start from the production default and bolt on demo extras. Edit me."""
    base = default().machine
    return replace(base, peripherals=(
        BusNode(7, role="oscillator"),   # driven-knife blade controller
        BusNode(8, role="suction", present=False),      # vacuum hold-down (dropped)
        # add more bus nodes here — they show up in the GUI automatically
    ))


# ── examples to copy from (not used unless you wire them into sim_machine) ──────
#
# Drop the A axis (pen-only machine): the A jog row + readout vanish.
#   base = default().machine
#   head = replace(base.heads[0], a=replace(base.a, node=replace(base.a.node, present=False)))
#   return replace(base, heads=(head,))
