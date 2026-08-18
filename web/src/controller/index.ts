/**
 * controller/ — the join between the machine description and the live link.
 *
 * `Controller` owns the three invariants nothing below it can own (exclusivity,
 * setup reconciliation, frame-correct live position); `runWalk` drives a baked
 * walk over one, applying the four timing rules that separate "the stream
 * returned" from "the machine stopped".
 *
 * See docs/controller.md for the charter.
 */

export {
    Controller,
    BusyError,
    type Lease,
    type ControllerOptions,
    type ControllerEvents,
} from "./controller.js";

export {
    runWalk,
    type RunWalkHooks,
    type RunWalkResult,
    type SwapRequest,
} from "./runWalk.js";
