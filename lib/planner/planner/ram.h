/**
 * ram.h — marks the functions a tick calls.
 *
 * A build that must keep the tick path out of slow memory defines PLANNER_RAM
 * as a section attribute, e.g. RP2350's `.time_critical`. Empty by default.
 */

#ifndef PLANNER_RAM_H
#define PLANNER_RAM_H

#ifndef PLANNER_RAM
#define PLANNER_RAM
#endif

#endif
