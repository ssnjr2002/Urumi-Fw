import { parseConfig } from "../config/configLoader.js";
import { bakePlan } from "../production/bakePlan.js";

// ── elements ──────────────────────────────────────────────────────────────────

const configInput   = document.getElementById("config-input");
const svgInput      = document.getElementById("svg-input");
const defaultTool   = document.getElementById("default-tool");
const bakeBtn       = document.getElementById("bake-btn");
const downloadBtn   = document.getElementById("download-btn");
const statusEl      = document.getElementById("status");
const summaryEl     = document.getElementById("summary");
const svgPreview    = document.getElementById("svg-preview");

// ── state ─────────────────────────────────────────────────────────────────────

let lastPlanBytes = null;
let lastSvgName   = "output";

// ── helpers ───────────────────────────────────────────────────────────────────

function setStatus(msg, kind = "idle") {
    statusEl.textContent = msg;
    statusEl.dataset.kind = kind;
}

function readFile(file) {
    return new Promise((resolve, reject) => {
        const r = new FileReader();
        r.onload  = () => resolve(r.result);
        r.onerror = () => reject(new Error(`failed to read ${file.name}`));
        r.readAsText(file);
    });
}

function formatBytes(n) {
    return n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`;
}

// ── SVG preview ───────────────────────────────────────────────────────────────

svgInput.addEventListener("change", async () => {
    const file = svgInput.files?.[0];
    if (!file) return;
    lastSvgName = file.name.replace(/\.svg$/i, "");
    const text = await readFile(file);
    svgPreview.innerHTML = text;
    const svg = svgPreview.querySelector("svg");
    if (svg) {
        svg.removeAttribute("width");
        svg.removeAttribute("height");
        svg.style.width  = "100%";
        svg.style.height = "100%";
    }
});

// ── bake ──────────────────────────────────────────────────────────────────────

bakeBtn.addEventListener("click", async () => {
    const configFile = configInput.files?.[0];
    const svgFile    = svgInput.files?.[0];

    if (!configFile) { setStatus("Select a config.json first.", "error"); return; }
    if (!svgFile)    { setStatus("Select an SVG file first.", "error"); return; }

    setStatus("Parsing config…", "working");
    downloadBtn.disabled = true;
    summaryEl.textContent = "";
    lastPlanBytes = null;

    let configText, svgText;
    try {
        [configText, svgText] = await Promise.all([readFile(configFile), readFile(svgFile)]);
    } catch (e) {
        setStatus(`Read error: ${e.message}`, "error");
        return;
    }

    const result = parseConfig(configText);
    if (!result.ok) {
        setStatus("Config errors — see summary.", "error");
        summaryEl.textContent = result.errors.join("\n");
        return;
    }

    setStatus("Baking plan…", "working");
    // yield to the browser so the status update paints before the heavy work
    await new Promise(r => setTimeout(r, 0));

    let plan, bytes;
    try {
        const tool = defaultTool.value.trim() || undefined;
        ({ plan, bytes } = bakePlan(result.config, svgText, { defaultTool: tool }));
    } catch (e) {
        setStatus(`Bake failed: ${e.message}`, "error");
        return;
    }

    lastPlanBytes = bytes;
    downloadBtn.disabled = false;

    const blockLines = plan.blocks.map((b, i) => {
        const slot = b.slot !== undefined ? ` slot ${b.slot}` : "";
        return `  block ${i + 1}: ${b.profile.name}${slot}  —  ${b.segments.length} segments`;
    });
    summaryEl.textContent =
        `${plan.blocks.length} block(s)  |  ${formatBytes(bytes.length)}\n` +
        blockLines.join("\n");

    setStatus("Done.", "ok");
});

// ── download ──────────────────────────────────────────────────────────────────

downloadBtn.addEventListener("click", () => {
    if (!lastPlanBytes) return;
    const blob = new Blob([lastPlanBytes], { type: "application/octet-stream" });
    const url  = URL.createObjectURL(blob);
    const a    = Object.assign(document.createElement("a"), {
        href: url,
        download: `${lastSvgName}.plan`,
    });
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
});
