// Core 0 control plane: text command dispatch (docs/wire_protocol.md).
// One command per line; replies with exactly one text line.
//
// Two tables: primitives (cmd/), atomic and config-free, and controller
// commands (controller/cmd/), which read the config and are refused without
// one. What is left here is the lookup -- see cmd/table.h for why it is a
// table and not the chain of startsWith() it replaced.

#include <Arduino.h>
#include "control_plane.h"
#include "cmd/table.h"
#include "controller/cmd/table.h"
#include "config/machine_cfg.h"
#include <string.h>

// Order is irrelevant: the match is the whole command word, so no row can
// shadow another. Grouped to read alongside cmd/'s files, nothing more.
static const Cmd kCommands[] = {
    // query.cpp
    { "ping",         cmdPing        },
    { "status",       cmdStatus      },
    { "cfg",          cmdCfg         },
    { "?",            cmdStatus      },   // human alias, not host-facing
    { "pingnode",     cmdPingNode    },
    { "nodepos",      cmdNodePos     },
    { "nodestat",     cmdNodeStat    },
    { "busstat",      cmdBusStat     },
    { "vac_switch",   cmdVacSwitch   },
    // get.cpp
    { "get",          cmdGet         },
    // lifecycle.cpp
    { "stop",         cmdStop        },
    { "unstop",       cmdUnstop      },
    { "reset",        cmdReset       },
    { "rst",          cmdReset       },
#ifdef PICO_ALLOW_UNCONFIGURED
    { "uncfg",        cmdUncfg       },
#endif
    { "seqreset",     cmdSeqReset    },
    { "pause",        cmdPause       },
    { "resume",       cmdResume      },
    { "cancel",       cmdCancel      },
    // periph.cpp
    { "vac_servo",    cmdVacServo    },
    { "vac_pump",     cmdVacPump     },
    { "knife_osc",    cmdKnifeOsc    },
    { "knife_blower", cmdKnifeBlower },
    { "laser",        cmdLaser       },
    { "makesafe",     cmdMakeSafe    },
    // axis.cpp
    { "slot_map",     cmdSlotMap     },
    { "axes_map",     cmdAxesMap     },
    { "setorigin",    cmdSetOrigin   },
    { "home_end",     cmdHomeEnd     },
    { "step",         cmdStep        },
    { "line",         cmdLine        },
    { "bez",          cmdBez         },
    { "jog",          cmdJog         },
    { "jogto",        cmdJogTo       },
    { "feed",         cmdFeed        },
    { "hallscan",     cmdHallScan    },
    { "leg",          cmdLeg         },
    { "leg_abort",    cmdLegAbort    },
    { "dummy_leg",    cmdDummyLeg    },
    { "axes_enable",  cmdAxesEnable  },
    { "bus_enable",   cmdBusEnable   },
    { "bus_exclude",  cmdBusExclude  },
    { "enable",       cmdEnable      },
    { "disable",      cmdDisable     },
    { "probe_map",    cmdProbeMap    },
    { "probe_leg",    cmdProbeLeg    },
    { "probe_end",    cmdProbeEnd    },
    { "setprobe",     cmdSetProbe    },
    { "unprobe",      cmdUnprobe     },
};

static const Cmd kControllerCommands[] = {
    { "unalarm",      cmdUnalarm     },
    { "home",         cmdHome        },
    { "home_unhomed", cmdHomeUnhomed },
    { "home_cycle",   cmdHomeCycle   },
    { "home_head",    cmdHomeHead    },
    { "select",       cmdSelect      },
    { "wzero",        cmdWzero       },
    { "wset",         cmdWset        },
    { "wclear",       cmdWclear      },
    { "mesh",         cmdMesh        },
};

template <size_t N>
static const Cmd* lookup(const Cmd (&table)[N], const char* s, size_t wordLen) {
    for (size_t i = 0; i < N; i++)
        if (strlen(table[i].name) == wordLen && strncmp(s, table[i].name, wordLen) == 0)
            return &table[i];
    return nullptr;
}

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract (docs/wire_protocol.md): `ok` / `err <reason>` / a typed read.
// Returns false if the line names no command -- the caller decides what to say.
bool handleCommand(const String& input) {
    const char* s = input.c_str();

    // The command word is everything up to the first space. Matching the WHOLE
    // word is what makes the table order-independent: `enable` cannot swallow
    // `axes_enable`, which under startsWith() dispatch was only prevented by
    // testing the longer one first.
    size_t wordLen = 0;
    while (s[wordLen] && s[wordLen] != ' ') wordLen++;
    if (wordLen == 0) return false;

    const Cmd* c = lookup(kCommands, s, wordLen);
    if (!c) {
        c = lookup(kControllerCommands, s, wordLen);
        if (!c) return false;
        if (!machineCfgValid()) { Serial.println("err unconfigured"); return true; }
    }

    const char* args = s + wordLen;        // hand the handler its arguments
    while (*args == ' ') args++;           // already positioned -- no offsets
    return c->fn(args);
}
