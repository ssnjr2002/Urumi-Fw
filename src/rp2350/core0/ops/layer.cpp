// layer — see layer.h.

#include "layer.h"
#include <math.h>

LayerResult layerResolve(const MachineCfg& c, const uint8_t* mounted,
                         const LayerHdr& l, float machineFeed, float* cutFeed) {
    if (!isfinite(l.feed) || l.feed < 0 || !isfinite(l.k) || l.k < 0 || l.k > 2 ||
        l.head >= c.headCount)
        return LAYER_BAD;
    const CfgTool* t = configTool(c, l.tool);
    if (!t) return LAYER_TOOL_UNKNOWN;
    if (t->chamberCount == 0 ? l.chamber != 0
                             : l.chamber < 1 || l.chamber > t->chamberCount)
        return LAYER_BAD;

    *cutFeed = l.feed > 0 ? l.feed : t->pathFeed > 0 ? t->pathFeed : machineFeed;
    return mounted[l.head] == l.tool ? LAYER_OK : LAYER_NOT_MOUNTED;
}
