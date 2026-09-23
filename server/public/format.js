// Element builders, number and time formatting, tables and bar charts for
// app.js, which index.html loads after this file.
"use strict";
/* exported $, el, digits, log2Of, pow, approx, words, duration, ago, date, hm, hms, stamp, fill, bars */

const SVG_NS = "http://www.w3.org/2000/svg";
const $ = (id) => document.getElementById(id);

function el(tag, cls, ...kids) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  e.append(...kids);
  return e;
}

function svgEl(tag, attrs, ...kids) {
  const e = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
  e.append(...kids);
  return e;
}

// An exact decimal string in groups of three digits. The gaps are margins, so
// a copied number has no spaces in it; <wbr> lets it wrap between groups.
function digits(dec) {
  const s = String(dec);
  if (!/^[0-9]+$/.test(s)) return el("span", "", s);
  const box = el("span", "digits");
  const head = s.length % 3 || 3;
  box.append(s.slice(0, head));
  for (let i = head; i < s.length; i += 3) {
    box.append(document.createElement("wbr"), el("span", "g", s.slice(i, i + 3)));
  }
  return box;
}

// log2 of a decimal string, for display.
function log2Of(dec) {
  let x;
  try {
    x = BigInt(dec);
  } catch {
    return null;
  }
  if (x <= 0n) return null;
  const shift = Math.max(0, x.toString(2).length - 53);
  return shift + Math.log2(Number(x >> BigInt(shift)));
}

function pow(exp) {
  return el("span", "", "2", el("sup", "", String(exp)));
}

// 2^x after an almost-equal sign, x to `places` decimals.
function approx(log2, places) {
  if (log2 === null || log2 === undefined || !Number.isFinite(log2)) return el("span", "", "0");
  return el("span", "", "\u2248\u00a02", el("sup", "", log2.toFixed(places)));
}

const SCALES = [
  [1e24, "septillion"],
  [1e21, "sextillion"],
  [1e18, "quintillion"],
  [1e15, "quadrillion"],
  [1e12, "trillion"],
  [1e9, "billion"],
  [1e6, "million"],
];

// A float to three significant digits with a scale word: "22.5 trillion".
function words(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return "-";
  const r = Number(x.toPrecision(3));
  for (const [v, w] of SCALES) {
    if (r >= v) {
      const q = r / v;
      return `${q >= 100 ? q.toFixed(0) : q >= 10 ? q.toFixed(1) : q.toFixed(2)} ${w}`;
    }
  }
  return Math.round(r).toLocaleString("en-US");
}

function plural(n, unit) {
  return `${n} ${unit}${n === 1 ? "" : "s"}`;
}

function duration(s) {
  if (s === null || s === undefined || !Number.isFinite(s)) return "-";
  const d = s / 86400;
  if (s < 5400) return plural(Math.max(1, Math.round(s / 60)), "minute");
  if (s < 172800) return plural(Math.round(s / 3600), "hour");
  if (d < 60) return plural(d < 10 ? Number(d.toFixed(1)) : Math.round(d), "day");
  if (d < 730) return plural(Number((d / 30.44).toFixed(1)), "month");
  return plural(Number((d / 365.25).toFixed(1)), "year");
}

function ago(s) {
  return s < 90 ? plural(Math.max(0, Math.round(s)), "second") + " ago" : duration(s) + " ago";
}

const iso = (t) => new Date(t * 1000).toISOString();
const date = (t) => iso(t).slice(0, 10);
const hm = (t) => iso(t).slice(11, 16);
const hms = (t) => iso(t).slice(11, 19);
const stamp = (t) => `${date(t)} ${hm(t)}`;

// Fills table `id` with a row per item; cells(item) gives [content, class, tag]
// per cell, content being a string or a node.
function fill(id, items, cells) {
  const table = $(id);
  const body = table.tBodies[0];
  if (items.length === 0) {
    const td = el("td", "", "None yet.");
    td.colSpan = table.tHead.rows[0].cells.length;
    body.replaceChildren(el("tr", "", td));
    return;
  }
  body.replaceChildren(
    ...items.map((item) => {
      const tr = el("tr");
      for (const [content, cls, tag] of cells(item)) {
        const cell = el(tag || "td", cls || "", content);
        if (tag === "th") cell.scope = "row";
        tr.append(cell);
      }
      return tr;
    }),
  );
}

// A bar chart of `points` ({ rate, short, long }) in box `id`. The label above
// is the tallest bar's value; the labels below are the points at `marks`.
function bars(id, points, title, marks) {
  const n = points.length;
  const W = n * 10;
  const H = 100;
  const max = Math.max(0, ...points.map((p) => p.rate));
  const top = points.find((p) => p.rate === max);
  const desc =
    max > 0
      ? `${points[0].long} to ${points[n - 1].long} UTC. Highest: ${words(max)} per second, ${top.long}.`
      : "Nothing was checked in this time.";
  const svg = svgEl(
    "svg",
    { viewBox: `0 0 ${W} ${H}`, preserveAspectRatio: "none", role: "img", "aria-labelledby": `${id}-t ${id}-d` },
    svgEl("title", { id: `${id}-t` }, title),
    svgEl("desc", { id: `${id}-d` }, desc),
  );
  points.forEach((p, i) => {
    if (!(p.rate > 0)) return;
    const h = Math.max(0.5, (p.rate / max) * H);
    svg.append(
      svgEl(
        "rect",
        { x: i * 10 + 1, y: (H - h).toFixed(2), width: 8, height: h.toFixed(2) },
        svgEl("title", {}, `${p.long}: ${words(p.rate)} per second`),
      ),
    );
  });
  const scale = el("div", "scale", max > 0 ? words(max) : "0");
  const axis = el("div", "axis", ...marks.map((i) => el("span", "", points[i].short)));
  scale.setAttribute("aria-hidden", "true");
  axis.setAttribute("aria-hidden", "true");
  $(id).replaceChildren(scale, svg, axis);
}
