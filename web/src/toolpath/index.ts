/** urumi-host/toolpath: SVG to annotated Béziers to a job. */
// geometry types that appear in the public signatures above
export {
    type Pt,
    type CubicBezier,
} from "./geometry.js";

// ── Bézier path: SVG layer → annotated Béziers → BEZIER packets ─────────────
export {
    loadSvgPaths, toolLayer, toolLayerKey, type Subpath, type LoadedSvg, type LoadOptions, type ToolLayer,
} from "./svg.js";
export { cleanSubpath, DEFAULT_CLEAN_OPTIONS, type CleanSubpath, type CleanOptions, type JoinKind } from "./clean.js";
export {
    annotate,
    analyzeBezier,
    BezierFlag,
    DEFAULT_ANNOTATE_OPTIONS,
    type AnnotatedBezier,
    type AnnotateOptions,
} from "./annotate.js";
export { prepareBezierJob, type BezierJob, type BezierJobOptions, type BBox } from "./job.js";
