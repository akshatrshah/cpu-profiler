#pragma once
/*
 * report_writer.hpp — Serialises a Profile to disk
 *
 * Two output formats:
 *
 *   1. Folded stacks (.folded)
 *      One line per unique call path: "comm;frame;frame … N\n"
 *      Compatible with Brendan Gregg's flamegraph.pl and speedscope.
 *
 *   2. Self-contained interactive HTML flamegraph
 *      Single file — no CDN, no external JS.  Opens in any modern browser.
 *      Canvas-based renderer with:
 *        • Click to zoom into a frame subtree
 *        • Double-click / Escape to reset zoom
 *        • Hover tooltip showing samples + percentage
 *        • Heat-colour scheme (red-yellow = hot, blue = cold)
 */

#include "types.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace profiler {

class ReportWriter {
public:
    // Write collapsed/folded stacks compatible with flamegraph.pl
    static Status write_folded(const std::string &path, const Profile &p) {
        std::ofstream out(path);
        if (!out) return Status::fail("Cannot open: " + path);
        for (auto &[key, cnt] : p.counts)
            out << key << " " << cnt << "\n";
        return Status::success();
    }

    // Write a self-contained interactive HTML flamegraph
    static Status write_html(const std::string &path, const Profile &p) {
        std::ofstream out(path);
        if (!out) return Status::fail("Cannot open: " + path);

        std::string json = build_json(p);

        out << html_header(p);
        out << "<script>\nconst PROFILE_DATA=" << json << ";\n";
        out << flamegraph_js();
        out << "</script>\n</body>\n</html>\n";

        if (!out.good()) return Status::fail("Write error: " + path);
        return Status::success();
    }

private:

    // ── JSON serialisation ───────────────────────────────────────────────────
    static std::string build_json(const Profile &p) {
        std::ostringstream j;
        j << "{\n"
          << "  \"comm\":"     << json_str(p.target_comm) << ",\n"
          << "  \"pid\":"      << p.target_pid            << ",\n"
          << "  \"rate\":"     << p.rate_hz               << ",\n"
          << "  \"duration\":" << p.duration_s            << ",\n"
          << "  \"total\":"    << p.total_samples         << ",\n"
          << "  \"lost\":"     << p.lost_samples          << ",\n"
          << "  \"stacks\":[\n";

        bool first = true;
        for (auto &[key, cnt] : p.counts) {
            if (!first) j << ",\n";
            first = false;
            j << "    {\"stack\":" << json_str(key)
              << ",\"value\":"     << cnt << "}";
        }
        j << "\n  ]\n}";
        return j.str();
    }

    static std::string json_str(const std::string &s) {
        std::string out = "\"";
        for (char c : s) {
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else                out += c;
        }
        return out + "\"";
    }

    // ── HTML / CSS ───────────────────────────────────────────────────────────
    static std::string html_header(const Profile &p) {
        std::ostringstream h;
        h << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>perf-profiler · )" << p.target_comm << " (PID " << p.target_pid << R"()</title>
<style>
:root {
  --bg:      #0d1117;
  --surface: #161b22;
  --border:  #30363d;
  --text:    #c9d1d9;
  --muted:   #8b949e;
  --accent:  #58a6ff;
  --hot1:    #e05252;
  --hot2:    #e09052;
  --warm:    #d4b34a;
  --cool1:   #52a8e0;
  --cool2:   #7a8fe0;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);font-size:14px}
header{padding:20px 28px;background:var(--surface);border-bottom:1px solid var(--border);
       display:flex;align-items:center;gap:16px}
header h1{font-size:1.1rem;font-weight:600;color:#f0f6fc}
header .badge{font-size:.75rem;background:#21262d;border:1px solid var(--border);
              border-radius:4px;padding:2px 8px;color:var(--muted)}
#statsbar{display:flex;gap:0;background:var(--surface);border-bottom:1px solid var(--border)}
.stat{padding:12px 24px;border-right:1px solid var(--border)}
.stat .l{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted)}
.stat .v{font-size:1rem;font-weight:600;color:var(--accent);margin-top:3px}
#hint{padding:7px 28px;font-size:.75rem;color:var(--muted);background:var(--bg);
      border-bottom:1px solid var(--border)}
#chart{padding:20px 28px;overflow-x:auto}
canvas{display:block;cursor:default}
#tooltip{position:fixed;pointer-events:none;display:none;
         background:#1c2230;border:1px solid var(--border);border-radius:6px;
         padding:7px 12px;font-size:.78rem;color:var(--text);
         box-shadow:0 4px 20px #0008;z-index:999;max-width:520px}
#breadcrumb{padding:6px 28px;font-size:.75rem;color:var(--muted);
            background:var(--bg);border-bottom:1px solid var(--border);min-height:28px}
#breadcrumb span{color:var(--accent);cursor:pointer;text-decoration:underline}
footer{padding:12px 28px;font-size:.72rem;color:var(--muted);border-top:1px solid var(--border);
       background:var(--surface)}
</style>
</head>
<body>
<header>
  <h1>CPU Flamegraph</h1>
  <span class="badge">)" << p.target_comm << "</span>"
  << "<span class=\"badge\">PID " << p.target_pid << "</span>"
  << R"(</header>
<div id="statsbar">
  <div class="stat"><div class="l">Samples</div><div class="v" id="sv-total"></div></div>
  <div class="stat"><div class="l">Lost</div><div class="v" id="sv-lost"></div></div>
  <div class="stat"><div class="l">Rate</div><div class="v" id="sv-rate"></div></div>
  <div class="stat"><div class="l">Duration</div><div class="v" id="sv-dur"></div></div>
  <div class="stat"><div class="l">Unique stacks</div><div class="v" id="sv-uniq"></div></div>
</div>
<div id="hint">Click a frame to zoom · Double-click or press <kbd>Escape</kbd> to reset</div>
<div id="breadcrumb"></div>
<div id="chart"><canvas id="fg"></canvas></div>
<div id="tooltip"></div>
<footer>Generated by <strong>perf-profiler</strong> · perf_event_open + ptrace stack sampler</footer>
)";
        return h.str();
    }

    // ── Self-contained JS flamegraph renderer ────────────────────────────────
    static std::string flamegraph_js() { return R"JS(
// ── Bootstrap stats ────────────────────────────────────────────────────────
document.getElementById('sv-total').textContent = PROFILE_DATA.total.toLocaleString();
document.getElementById('sv-lost').textContent  = PROFILE_DATA.lost.toLocaleString();
document.getElementById('sv-rate').textContent  = PROFILE_DATA.rate + ' Hz';
document.getElementById('sv-dur').textContent   = PROFILE_DATA.duration + ' s';
document.getElementById('sv-uniq').textContent  = PROFILE_DATA.stacks.length.toLocaleString();

// ── Build call tree from collapsed stacks ──────────────────────────────────
function buildTree(stacks) {
  const root = { name: 'all', value: 0, children: {} };
  for (const { stack, value } of stacks) {
    const parts = stack.split(';');
    let node = root;
    node.value += value;
    for (const name of parts) {
      if (!node.children[name])
        node.children[name] = { name, value: 0, children: {} };
      node = node.children[name];
      node.value += value;
    }
  }
  function freeze(n) {
    const children = Object.values(n.children)
      .map(freeze)
      .sort((a, b) => b.value - a.value);
    return { name: n.name, value: n.value, children };
  }
  return freeze(root);
}

const TREE  = buildTree(PROFILE_DATA.stacks);
const TOTAL = TREE.value;

// ── Color palette ──────────────────────────────────────────────────────────
const PALETTES = [
  ['#c0392b','#e74c3c'],['#d35400','#e67e22'],['#d4ac0d','#f1c40f'],
  ['#1a7a4a','#27ae60'],['#1a5276','#2980b9'],['#4a235a','#8e44ad'],
  ['#6e2f1a','#a04000'],['#1b4f72','#2471a3']
];

function colorFor(name, depth) {
  let h = depth * 31;
  for (let i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) >>> 0;
  const [dark, light] = PALETTES[h % PALETTES.length];
  return { fill: light, border: dark };
}

// ── Renderer ───────────────────────────────────────────────────────────────
const canvas  = document.getElementById('fg');
const ctx     = canvas.getContext('2d');
const tooltip = document.getElementById('tooltip');
const crumb   = document.getElementById('breadcrumb');

const ROW_H   = 22;
let frameMap  = [];   // [{x,y,w,h,node}]
let zoomPath  = [];   // stack of nodes for breadcrumb

function render(root) {
  const W = Math.max(600, canvas.parentElement.clientWidth - 56);
  const items = [];

  (function walk(node, x, w, depth) {
    if (w < 0.5) return;
    items.push({ x, depth, w, node });
    let cx = x;
    for (const child of node.children) {
      const cw = (child.value / node.value) * w;
      walk(child, cx, cw, depth + 1);
      cx += cw;
    }
  })(root, 0, W, 0);

  const maxD = items.reduce((m, f) => Math.max(m, f.depth), 0);
  const H    = (maxD + 2) * ROW_H;

  canvas.width  = W;
  canvas.height = H;
  canvas.style.height = H + 'px';
  ctx.clearRect(0, 0, W, H);

  frameMap = [];
  const DPR = window.devicePixelRatio || 1;

  for (const f of items) {
    const y = H - (f.depth + 1) * ROW_H;
    const { fill, border } = colorFor(f.node.name, f.depth);

    // Frame background
    ctx.fillStyle = fill;
    ctx.fillRect(f.x + 0.5, y + 0.5, f.w - 1, ROW_H - 1);

    // Top highlight
    ctx.fillStyle = 'rgba(255,255,255,0.15)';
    ctx.fillRect(f.x + 0.5, y + 0.5, f.w - 1, 3);

    // Label
    if (f.w > 28) {
      ctx.fillStyle = '#fff';
      ctx.font = '11px "Segoe UI",system-ui,monospace';
      ctx.save();
      ctx.beginPath();
      ctx.rect(f.x + 3, y + 1, f.w - 6, ROW_H - 2);
      ctx.clip();
      ctx.fillText(f.node.name, f.x + 4, y + 14);
      ctx.restore();
    }

    frameMap.push({ x: f.x, y, w: f.w, h: ROW_H, node: f.node });
  }

  updateBreadcrumb();
}

function updateBreadcrumb() {
  if (zoomPath.length === 0) { crumb.innerHTML = ''; return; }
  let html = '<span onclick="resetZoom()">all</span>';
  for (let i = 0; i < zoomPath.length; i++) {
    const idx = i;
    html += ' › <span onclick="zoomTo(' + idx + ')">' +
            escapeHtml(zoomPath[i].name) + '</span>';
  }
  crumb.innerHTML = html;
}

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

window.resetZoom = function() { zoomPath = []; render(TREE); };
window.zoomTo   = function(idx) { zoomPath = zoomPath.slice(0, idx+1); render(zoomPath[idx]); };

function hitTest(mx, my) {
  for (let i = frameMap.length - 1; i >= 0; i--) {
    const f = frameMap[i];
    if (mx >= f.x && mx < f.x+f.w && my >= f.y && my < f.y+f.h) return f;
  }
  return null;
}

canvas.addEventListener('mousemove', e => {
  const r   = canvas.getBoundingClientRect();
  const hit = hitTest(e.clientX - r.left, e.clientY - r.top);
  if (hit) {
    const pct = ((hit.node.value / TOTAL) * 100).toFixed(2);
    tooltip.style.display = 'block';
    tooltip.style.left    = (e.clientX + 14) + 'px';
    tooltip.style.top     = (e.clientY + 14) + 'px';
    tooltip.innerHTML     = `<b>${escapeHtml(hit.node.name)}</b><br>` +
                            `${hit.node.value.toLocaleString()} samples (${pct}% of total)`;
    canvas.style.cursor   = 'pointer';
  } else {
    tooltip.style.display = 'none';
    canvas.style.cursor   = 'default';
  }
});

canvas.addEventListener('mouseleave', () => { tooltip.style.display='none'; });

canvas.addEventListener('click', e => {
  const r   = canvas.getBoundingClientRect();
  const hit = hitTest(e.clientX - r.left, e.clientY - r.top);
  if (hit) { zoomPath.push(hit.node); render(hit.node); }
});

canvas.addEventListener('dblclick', () => resetZoom());

document.addEventListener('keydown', e => {
  if (e.key === 'Escape') resetZoom();
  if (e.key === 'Backspace' && zoomPath.length > 0) {
    zoomPath.pop();
    render(zoomPath.length ? zoomPath[zoomPath.length-1] : TREE);
  }
});

window.addEventListener('resize', () => {
  render(zoomPath.length ? zoomPath[zoomPath.length-1] : TREE);
});

render(TREE);
)JS";
    }
};

} // namespace profiler
