// layer — see layer.h.

#include "layer.h"
#include <math.h>

bool layerValid(const LayerHdr& l) {
    return isfinite(l.feed) && l.feed >= 0 && isfinite(l.p) &&
           isfinite(l.thickness) && l.thickness >= 0;
}

bool layerHeadFits(const MachineCfg& c, const CfgTool& t, uint8_t head) {
    if (head >= c.headCount) return false;
    return !(t.tangential || t.chamberCount > 0) || c.heads[head].a.node.present;
}

OriginRange layerOrigins(float lo, float hi, float bmin, float bmax, float d, float blade) {
    return { lo + blade - bmin + d, hi - blade - bmax + d };
}

bool layerOriginsNarrow(OriginRange* r, const OriginRange& by, bool* high) {
    *high = by.hi < r->lo;
    if (by.lo > r->lo) r->lo = by.lo;
    if (by.hi < r->hi) r->hi = by.hi;
    return r->lo <= r->hi;
}

LayerResult layerResolve(const MachineCfg& c, const uint8_t* mounted,
                         const LayerHdr& l, float machineFeed, LayerMotion* out) {
    if (l.head >= c.headCount) return LAYER_BAD;
    const CfgTool* t = configTool(c, l.tool);
    if (!t) return LAYER_TOOL_UNKNOWN;
    if (t->plunge && (!(l.p > 0) || l.p - l.thickness > c.maxOvercut)) return LAYER_BAD;
    if (t->chamberCount == 0 ? l.chamber != 0
                             : l.chamber < 1 || l.chamber > t->chamberCount)
        return LAYER_BAD;
    if (!layerHeadFits(c, *t, l.head)) return LAYER_BAD;

    out->cutFeed    = l.feed > 0 ? l.feed : t->cutFeed > 0 ? t->cutFeed : machineFeed;
    out->plungeFeed = t->plungeFeed > 0 ? t->plungeFeed : c.feed.plungeFallback;
    out->top        = l.thickness;
    out->depth      = t->plunge ? l.p : 0;
    out->tool       = t->id;
    out->power      = t->power;
    out->tangential = t->tangential;
    out->periodDeg  = 360.0f / t->periodCount;
    out->cornerAngleDeg = t->cornerAngleDeg;
    out->aMaxDeg    = t->aMaxDeg;
    out->aOffsetDeg = t->aOffsetDeg;
    out->chamber    = t->chamberCount > 0;
    out->chamberDeg = out->chamber ? t->chamberOffsets[l.chamber - 1] : 0;
    return mounted[l.head] == l.tool ? LAYER_OK : LAYER_NOT_MOUNTED;
}
