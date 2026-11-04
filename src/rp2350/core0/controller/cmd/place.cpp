// place.cpp — `place`, in JOB.PLACE (docs/plans/motion-sessions.md, Placement).
//
//   place set                the dot is the job's (xmin, ymin); the zero follows
//   place reset              clear it; the dot back to the first corner, jogs again
//   place go <xmin|xmax> <ymin|ymax>   the dot to that corner
//   place frame              trace the box once, back to (xmin, ymin)
//   place ok                 sign off: laser off, to BUFFERING

#include <Arduino.h>
#include <string.h>
#include "table.h"
#include "../seq/place.h"

bool cmdPlace(const char* args) {
    const char* why = "usage";
    if (strcmp(args, "set") == 0)        why = placeSet();
    else if (strcmp(args, "reset") == 0) why = placeReset();
    else if (strcmp(args, "frame") == 0) why = placeFrame();
    else if (strcmp(args, "ok") == 0)    why = placeOk();
    else if (strncmp(args, "go ", 3) == 0) {
        const char* a = args + 3;
        const bool x0 = strncmp(a, "xmin ", 5) == 0, x1 = strncmp(a, "xmax ", 5) == 0;
        const char* b = a + 5;
        const bool y0 = strcmp(b, "ymin") == 0, y1 = strcmp(b, "ymax") == 0;
        if ((x0 || x1) && (y0 || y1)) why = placeGo(x1, y1);
    }
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}
