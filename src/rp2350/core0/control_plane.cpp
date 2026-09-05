// Core 0 control plane: text command dispatch (docs/wire_protocol.md).
// One command per line; replies with exactly one text line.
//
// The handlers live in cmd/. What is left here is the lookup -- see cmd/table.h
// for why it is a table and not the chain of startsWith() it replaced.

#include <Arduino.h>
#include "control_plane.h"
#include "cmd/table.h"
#include <string.h>

// Order is irrelevant: the match is the whole command word, so no row can
// shadow another. Grouped to read alongside cmd/'s files, nothing more.
static const Cmd kCommands[] = {
    // query.cpp
    { "ping",         cmdPing        },
    { "getstate",     cmdGetState    },
    { "getpos",       cmdGetPos      },
    { "status",       cmdStatus      },   // also `status cfg`
    { "?",            cmdStatus      },   // human alias, not host-facing
    { "pingnode",     cmdPingNode    },
    { "nodepos",      cmdNodePos     },
    { "nodestat",     cmdNodeStat    },
    { "vac_switch",   cmdVacSwitch   },
    // lifecycle.cpp
    { "stop",         cmdStop        },
    { "reset",        cmdReset       },
    { "rst",          cmdReset       },
    { "seqreset",     cmdSeqReset    },
    { "pause",        cmdPause       },
    { "resume",       cmdResume      },
    { "cancel",       cmdCancel      },
    { "unalarm",      cmdUnalarm     },
    // periph.cpp
    { "vac_servo",    cmdVacServo    },
    { "vac_pump",     cmdVacPump     },
    { "knife_osc",    cmdKnifeOsc    },
    { "knife_blower", cmdKnifeBlower },
    { "laser",        cmdLaser       },
    // axis.cpp
    { "axis_map",     cmdAxisMap     },
    { "setorigin",    cmdSetOrigin   },
    { "step",         cmdStep        },
    { "hallscan",     cmdHallScan    },
    { "lin_leg",      cmdLinLeg      },
    { "rot_leg",      cmdRotLeg      },
    { "axes_enable",  cmdAxesEnable  },
    { "bus_enable",   cmdBusEnable   },
    { "enable",       cmdEnable      },
    { "disable",      cmdDisable     },
    { "probe_map",    cmdProbeMap    },
    { "probe_leg",    cmdProbeLeg    },
    { "probe_end",    cmdProbeEnd    },
};

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

    for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        const Cmd& c = kCommands[i];
        if (strlen(c.name) != wordLen || strncmp(s, c.name, wordLen) != 0) continue;

        const char* args = s + wordLen;    // hand the handler its arguments
        while (*args == ' ') args++;       // already positioned -- no offsets
        return c.fn(args);
    }
    return false;
}
