#!/usr/bin/env python3
"""hall_analyze.py — index-estimator bake-off against a captured Hall sweep.

Throwaway bench tool. Runs every candidate centre-finding algorithm over the
SAME captured dips and ranks them, so the firmware implements whichever one
actually wins instead of whichever one sounded best.

  python hall_analyze.py hall_i1000_d0_n128000.csv
  python hall_analyze.py run_slow.csv run_fast.csv --plot

Method — why this is decidable without ground truth
---------------------------------------------------
The true index angle is unknown, and does not need to be known: it is the SAME
physical angle every lap. So scatter across laps is a complete figure of merit.
Index positions should be linear in lap number, so residuals from a straight-line
fit are the error. Absolute residual includes belt error, which is NOT the
estimator's fault — but every estimator sees identical belt error on identical
data, so the RANKING is valid even though the absolute number is inflated.

The pairwise table isolates the estimators from the belt entirely: for any two
estimators, std(A - B) cancels belt error exactly, since both saw the same dip.
Two estimators agreeing tightly there are both tracking the same feature.

A second capture at a different sweep speed is what exposes BIAS, which scatter
cannot see: compare the reported steps/rev and the residual structure between a
slow and a fast run.
"""
import argparse
import sys

import numpy as np


# ─── loading ────────────────────────────────────────────────────────────────

def load(path, skip=0):
    step, adc, meta = [], [], ""
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if "BEGIN" in line:
                    meta = line
                continue
            if line.startswith("step,"):
                continue
            a, _, b = line.partition(",")
            try:
                step.append(int(a)); adc.append(int(b))
            except ValueError:
                continue
    s = np.asarray(step, float); v = np.asarray(adc, float)
    if skip:
        s, v = s[skip:], v[skip:]
    return s, v, meta


# ─── dip segmentation ───────────────────────────────────────────────────────

def find_dips(v, frac=0.4, pad=1.5, min_width=4):
    """Coarse segmentation only. This commits to no estimator — it just decides
    which samples belong to which dip; the estimators run on the windows."""
    baseline = float(np.median(v))
    depth = baseline - float(np.min(v))
    if depth <= 0:
        return baseline, 0.0, []

    below = v < (baseline - frac * depth)
    edges = np.diff(below.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    ends = list(np.flatnonzero(edges == -1) + 1)
    if below[0]:
        starts.insert(0, 0)
    if below[-1]:
        ends.append(len(v))

    spans = []
    for a, b in zip(starts, ends):
        if b - a < min_width:
            continue
        w = b - a
        lo = max(0, int(a - pad * w))
        hi = min(len(v), int(b + pad * w))
        spans.append((lo, hi))
    return baseline, depth, spans


# ─── estimators (all return a position in ORIGINAL sample index units) ──────

def _parabolic_vertex(y, i):
    """Sub-sample vertex offset from the 3 points around index i. Known to be
    biased when the peak is not near a sample or the shape is not parabolic —
    included precisely so that bias shows up in the ranking."""
    if i <= 0 or i >= len(y) - 1:
        return 0.0
    d = y[i - 1] - 2 * y[i] + y[i + 1]
    if d == 0:
        return 0.0
    return 0.5 * (y[i - 1] - y[i + 1]) / d


def est_argmin(x, y, b):
    return x[int(np.argmin(y))]


def est_parabolic(x, y, b):
    i = int(np.argmin(y))
    return x[i] + _parabolic_vertex(y, i)


def est_centroid(x, y, b):
    """Baseline-subtracted centre of mass. Sensitive to any unsubtracted
    pedestal, which is why the baseline is measured per-capture, not stored."""
    w = np.clip(b - y, 0, None)
    tot = w.sum()
    return float((x * w).sum() / tot) if tot > 0 else np.nan


def _crossings(x, y, level):
    """Linearly-interpolated first and last crossings of `level`."""
    below = y < level
    idx = np.flatnonzero(below)
    if idx.size == 0:
        return None
    out = []
    for i, step in ((idx[0], -1), (idx[-1], +1)):
        j = i + step
        if j < 0 or j >= len(y):
            out.append(x[i]); continue
        y0, y1 = y[i], y[j]
        out.append(x[i] if y1 == y0 else x[i] + (level - y0) / (y1 - y0) * (x[j] - x[i]))
    return out[0], out[1]


def make_cfd(frac):
    """Constant-fraction discrimination. Trigger at a fixed FRACTION of depth,
    not an absolute level, and take the midpoint of the two crossings. The
    midpoint is invariant to depth (sensitivity drift, air gap) and to baseline
    offset — both of which the A1324 has in quantity (+-10 G quiescent drift,
    -3.5/+8.5% sensitivity drift). Crossings also sit on the steepest part of
    the curve, where noise displaces position least, unlike the flat minimum."""
    def est(x, y, b):
        depth = b - float(np.min(y))
        c = _crossings(x, y, b - frac * depth)
        return np.nan if c is None else 0.5 * (c[0] + c[1])
    est.__name__ = f"cfd_{int(frac * 100)}"
    return est


def est_gaussian(x, y, b):
    """Log-parabola fit on the inverted dip. Near the Cramer-Rao bound IF the
    dip really is Gaussian — the assumption is the risk."""
    g = np.clip(b - y, 1e-9, None)
    m = g > 0.2 * g.max()
    if m.sum() < 3:
        return np.nan
    try:
        c2, c1, _ = np.polyfit(x[m], np.log(g[m]), 2)
    except (np.linalg.LinAlgError, ValueError):
        return np.nan
    return np.nan if c2 >= 0 else -c1 / (2 * c2)


def est_mirror(x, y, b):
    """Symmetry axis by correlating the dip against its own reverse. Assumes
    only symmetry — no template, no shape model, no depth. The autoconvolution
    of a bump centred at c peaks at 2c."""
    g = np.clip(b - y, 0, None)
    if g.sum() <= 0:
        return np.nan
    ac = np.correlate(g, g[::-1], mode="full")
    p = int(np.argmax(ac))
    c = (p + _parabolic_vertex(-ac, p)) / 2.0
    return x[0] + c * (x[1] - x[0]) if len(x) > 1 else np.nan


ESTIMATORS = [est_argmin, est_parabolic, est_centroid,
              make_cfd(0.3), make_cfd(0.5), make_cfd(0.7),
              est_gaussian, est_mirror]


# ─── matched filter (needs a template, so it runs as a second pass) ─────────

def matched_positions(x_all, v, spans, baseline, ref):
    """Cross-correlate each dip against the mean dip shape. The ML estimator for
    a known shape in white noise — but the template is built from this very
    capture, so it cannot detect a shape that drifts between runs."""
    if len(spans) < 2:
        return np.full(len(spans), np.nan)
    half = min(min(b - a for a, b in spans) // 2, 64)
    if half < 3:
        return np.full(len(spans), np.nan)

    stack = []
    for r in ref:
        c = int(round(r))
        if c - half < 0 or c + half >= len(v):
            stack.append(None); continue
        stack.append(np.clip(baseline - v[c - half:c + half + 1], 0, None))
    good = [s for s in stack if s is not None]
    if len(good) < 2:
        return np.full(len(spans), np.nan)
    template = np.mean(good, axis=0)
    template -= template.mean()

    out = []
    for s, r in zip(stack, ref):
        if s is None:
            out.append(np.nan); continue
        seg = s - s.mean()
        cc = np.correlate(seg, template, mode="same")
        p = int(np.argmax(cc))
        out.append(r + (p - half) + _parabolic_vertex(-cc, p))
    return np.asarray(out, float)


# ─── scoring ────────────────────────────────────────────────────────────────

def residual_sigma(pos):
    """Scatter about a straight line in lap number. Index positions must be
    linear in lap if steps-per-rev is constant; deviations are belt error plus
    estimator error, and the belt part is common to all estimators."""
    p = np.asarray(pos, float)
    m = np.isfinite(p)
    if m.sum() < 3:
        return np.nan, np.nan
    i = np.arange(len(p))[m]
    a, c = np.polyfit(i, p[m], 1)
    return float(np.std(p[m] - (a * i + c))), float(a)


def analyze(path, skip, plot=False):
    x, v, meta = load(path, skip)
    if len(v) < 100:
        print(f"{path}: too few samples ({len(v)})", file=sys.stderr)
        return

    baseline, depth, spans = find_dips(v)
    noise = float(np.std(v[v > baseline - 0.1 * depth])) if depth > 0 else float("nan")

    print(f"\n=== {path} ===")
    if meta:
        print(f"  {meta}")
    print(f"  samples {len(v)}  baseline {baseline:.1f}  depth {depth:.1f}  "
          f"noise(sd) {noise:.2f}  SNR {depth / noise if noise else float('nan'):.0f}")
    print(f"  dips found: {len(spans)}")
    if len(spans) < 3:
        print("  need >=3 dips (sweep more revolutions) to rank estimators")
        return
    widths = [b - a for a, b in spans]
    print(f"  dip window width: min {min(widths)} max {max(widths)} samples")

    results = {}
    for est in ESTIMATORS:
        pos = []
        for a, b in spans:
            xs, ys = x[a:b], v[a:b]
            try:
                pos.append(float(est(xs, ys, baseline)))
            except Exception:
                pos.append(np.nan)
        results[est.__name__.replace("est_", "")] = np.asarray(pos, float)

    ref = results["cfd_50"]
    if np.isfinite(ref).sum() >= 2:
        results["matched"] = matched_positions(x, v, spans, baseline, ref)

    # Deviation from the per-lap consensus across estimators. Belt error is
    # identical for every estimator on a given lap, so it cancels exactly here —
    # this is the column that actually discriminates between estimators, whereas
    # `resid sd` is usually dominated by belt error common to all of them and
    # will rank near-randomly when the belt term is large.
    stack = np.vstack([results[k] for k in results])
    consensus = np.nanmedian(stack, axis=0)

    rows = []
    for name, pos in results.items():
        sd, slope = residual_sigma(pos)
        d = pos - consensus
        d = d[np.isfinite(d)]
        dev = float(np.std(d)) if d.size >= 2 else np.nan
        rows.append((dev, sd, name, slope))
    rows.sort(key=lambda r: (np.isnan(r[0]), r[0]))

    print(f"\n  {'estimator':<12} {'dev vs consensus':>17} {'resid sd':>10} {'steps/rev':>12}")
    print(f"  {'-' * 12} {'-' * 17} {'-' * 10} {'-' * 12}")
    for dev, sd, name, slope in rows:
        dv_s = "              n/a" if np.isnan(dev) else f"{dev:17.3f}"
        sd_s = "       n/a" if np.isnan(sd) else f"{sd:10.3f}"
        sl_s = "         n/a" if np.isnan(slope) else f"{slope:12.2f}"
        print(f"  {name:<12} {dv_s} {sd_s} {sl_s}")
    print("  (rank on 'dev vs consensus'; 'resid sd' includes belt error "
          "common to all estimators)")

    names = [r[2] for r in rows if not np.isnan(r[1])][:5]
    if len(names) >= 2:
        print("\n  pairwise disagreement sd (belt error cancels exactly):")
        print("  " + " " * 13 + "".join(f"{n:>12}" for n in names))
        for a in names:
            cells = []
            for b in names:
                d = results[a] - results[b]
                d = d[np.isfinite(d)]
                cells.append("           -" if a == b or d.size < 2
                             else f"{np.std(d):12.3f}")
            print(f"  {a:<13}" + "".join(cells))

    if plot:
        try:
            import matplotlib.pyplot as plt
        except ImportError:
            print("\n  (matplotlib not installed — skipping plot)", file=sys.stderr)
            return
        fig, ax = plt.subplots(2, 1, figsize=(11, 7))
        ax[0].plot(x, v, lw=0.5)
        ax[0].axhline(baseline, color="k", ls=":", lw=0.8)
        for a, b in spans:
            ax[0].axvspan(x[a], x[b - 1], color="C1", alpha=0.15)
        ax[0].set_title(f"{path} — raw sweep"); ax[0].set_xlabel("step")
        for sd, name, _ in rows[:4]:
            if np.isnan(sd):
                continue
            p = results[name]
            i = np.arange(len(p))
            m = np.isfinite(p)
            a_, c_ = np.polyfit(i[m], p[m], 1)
            ax[1].plot(i[m], p[m] - (a_ * i[m] + c_), "o-", ms=3, lw=0.8,
                       label=f"{name} (sd {sd:.3f})")
        ax[1].set_title("per-lap residual from linear fit")
        ax[1].set_xlabel("lap"); ax[1].set_ylabel("steps"); ax[1].legend()
        plt.tight_layout(); plt.show()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--skip", type=int, default=0,
                    help="drop N leading samples (startup transient / ramp)")
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()
    for p in args.csv:
        analyze(p, args.skip, args.plot)
    if len(args.csv) > 1:
        print("\nCompare steps/rev across runs at different speeds: a shift there "
              "is a speed-dependent bias, which per-run scatter cannot reveal.")


if __name__ == "__main__":
    main()
