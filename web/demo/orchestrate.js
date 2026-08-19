/**
 * orchestrate.js — DEPRECATED. The ".plan + config.json → stream" demo.
 *
 * Built entirely on `loadPlan`, which is gone with the rest of the `.plan`
 * format (docs/head_binding.md stage 6). See demo/main.js for why a serialised
 * plan cannot be replayed safely: it encodes step counts resolved against one
 * head arrangement and records nothing about which.
 *
 * demo/comms.html does what this did, from an SVG rather than a file: bake,
 * schedule, walk, stream, with the mount table checked against the machine
 * before anything cuts.
 *
 * Kept as a tombstone rather than deleted so the page explains itself instead
 * of failing to load a module.
 */

const note = document.createElement("div");
note.style.cssText =
    "padding:16px;margin:12px;border:1px solid #a33;border-radius:6px;" +
    "background:#2a1414;color:#e8c4c4;line-height:1.5;max-width:60ch";
note.innerHTML =
    `<strong style="color:#ff8a8a">This demo is deprecated.</strong>` +
    `<p style="margin-top:12px">The .plan file format has been removed — a baked plan ` +
    `holds step counts resolved against one head arrangement and records nothing about ` +
    `which one, so replaying it against another is silently wrong.</p>` +
    `<p style="margin-top:8px">Use <a href="comms.html" style="color:#8ab4ff">comms.html</a>, ` +
    `which bakes from the SVG against the live config and verifies the mounts before it cuts.</p>`;
document.body.prepend(note);

for (const el of document.querySelectorAll("button, input, select")) el.disabled = true;
