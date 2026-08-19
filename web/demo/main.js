/**
 * main.js — DEPRECATED. The "Plan Bake" demo (SVG + config.json → .plan file).
 *
 * This demo's whole purpose was to download a `.plan`, and `.plan` is gone
 * (docs/head_binding.md stage 6). A serialised plan encodes step counts already
 * resolved against ONE head arrangement: Z/A stepsPerUnit, invert and the
 * feed/accel ceilings are per-head, so a file baked with the knife on head 0
 * (1200 steps/mm on this bench machine) is a clean 2x error if it is replayed
 * with the knife on head 1 (600). Nothing in the file records which arrangement
 * it assumed, so nothing downstream could refuse it.
 *
 * The replacement is to bake at run time from the SVG and the live config —
 * demo/bench.html for the offline view, demo/comms.html for the machine.
 *
 * Kept as a tombstone rather than deleted so the page explains itself instead
 * of failing to load a module.
 */

const WHY = [
    "The .plan file format has been removed.",
    "A baked plan holds step counts resolved against one head arrangement, and " +
        "records nothing about which one. Replayed against a different arrangement " +
        "it is silently wrong — on this bench machine, exactly half the Z depth.",
    "Bake from the SVG at run time instead: bench.html offline, comms.html on the machine.",
];

const host = document.querySelector(".panel") ?? document.body;
const note = document.createElement("div");
note.style.cssText =
    "padding:16px;margin:12px;border:1px solid #a33;border-radius:6px;" +
    "background:#2a1414;color:#e8c4c4;line-height:1.5;max-width:60ch";
note.innerHTML =
    `<strong style="color:#ff8a8a">This demo is deprecated.</strong><br><br>` +
    WHY.map((p) => `<p style="margin-bottom:8px">${p}</p>`).join("");
host.prepend(note);

for (const el of document.querySelectorAll("button, input")) el.disabled = true;
