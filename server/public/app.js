// Fills index.html from /api/stats and loads it again once a minute while the
// tab is visible; the published path records come from path-records.json,
// loaded once. Exact integers arrive as decimal strings and stay strings
// (cut into digit groups, or read with BigInt for a log2); floats are used
// only for speeds, durations, log2 values and bar heights. Data reaches the
// page as text nodes, never as HTML.
"use strict";
/* global $, el, digits, log2Of, pow, approx, words, duration, ago, date, hm, hms, stamp, fill, bars */

const REFRESH_MS = 60000;

function renderHistory(s) {
  const now = s.generated_at;
  // The current hour or day counts the part of it that has passed.
  const rate = (b, span) => Number(b.numbers) / Math.max(60, Math.min(span, now - b.start));
  const hourly = s.history.hourly.map((b) => ({
    b,
    rate: rate(b, 3600),
    short: `${date(b.start).slice(5)} ${hm(b.start)}`,
    long: stamp(b.start),
  }));
  const daily = s.history.daily.map((b) => ({ b, rate: rate(b, 86400), short: date(b.start), long: date(b.start) }));
  const mid = (a) => [0, Math.floor(a.length / 2), a.length - 1];
  bars("hourly-chart", hourly, "Numbers per second, by hour, last 48 hours", mid(hourly));
  bars("daily-chart", daily, "Numbers per second, by day, last 90 days", mid(daily));
  const row = (p) => [[p.long], [words(Number(p.b.numbers)), "num"], [words(p.rate), "num"]];
  fill("hourly-table", hourly.filter((p) => p.b.blocks > 0).reverse(), row);
  fill("daily-table", daily.filter((p) => p.b.blocks > 0).reverse(), row);
}

function renderStatus(s) {
  const p = $("status");
  const last = s.totals ? s.totals.last_block_at : null;
  p.replaceChildren(
    `Updated ${stamp(s.generated_at)} UTC. ` +
      (last === null || last === undefined ? "No blocks yet." : `Last block ${ago(s.generated_at - last)}.`),
  );
  if (s.alerts_open > 0) p.append(" ", el("strong", "warn", `Open alerts: ${s.alerts_open}.`));
}

// The machines' random audits of accepted blocks, which re-derive a sample of
// each block's decisions; one that fails also opens an alert.
function renderAudits(s) {
  const a = s.audits;
  $("audits").hidden = !a;
  $("audits").textContent = a ? `Audits: ${a.passed} passed, ${a.failed} failed.` : "";
}

// The footer line: how many entries the ledger has and the start of the latest one's hash.
function renderLedger(s) {
  const l = s.ledger;
  $("ledger").hidden = !l;
  if (!l) return;
  $("ledger-entries").replaceChildren(digits(String(l.entries)), l.entries === 1 ? " entry" : " entries");
  const head = el("code", "", `${l.head}...`);
  head.title = l.head_full;
  $("ledger-latest").replaceChildren(...(l.entries > 0 ? [", latest ", head] : []));
}

// The campaign's start as k x 2^60 when it is a multiple of 2^60, else in digits.
function startValue(dec) {
  const x = BigInt(dec);
  const k = x >> 60n;
  return k << 60n === x ? el("span", "", `${k}\u00a0\u00d7\u00a02`, el("sup", "", "60")) : digits(dec);
}

// Links `a` to `url` when it is an https address; anything else leaves a
// plain name. Every link built from data goes through here.
function linkTo(a, url) {
  if (typeof url === "string" && /^https:\/\//.test(url)) a.href = url;
  else a.removeAttribute("href");
}

// Every number below the start was checked before this verifier began;
// start_credit says by whom.
function renderStart(s) {
  const c = s.campaign;
  $("start").hidden = !c;
  if (!c) return;
  $("start-value").replaceChildren(startValue(c.start));
  const credit = s.start_credit;
  $("start-credit").hidden = !credit;
  if (!credit) return;
  const link = $("start-credit-link");
  link.textContent = credit.name;
  linkTo(link, credit.url);
}

// Rows shown before "All N records"; records found here are always shown too.
const SHOWN_RECORDS = 15;

// One list, oldest first: the published records (path-records.json; without
// it, the ones /api/stats gives), then those found here. path_records is null
// when the campaign cannot tell them all; then none are shown.
function renderPathRecords(s, published) {
  const paths = s.path_records;
  $("path-records").hidden = !Array.isArray(paths);
  if (!Array.isArray(paths)) return;
  const found = paths.filter((r) => !r.known);
  const finder = s.owner_name || "This verifier";
  const rows = [
    ...(published ?? paths.filter((r) => r.known)).map((r) => [r, r.discoverer || ""]),
    ...found.map((r) => [r, `${finder} (${date(r.found_at)})`]),
  ];
  const cells = ([r, by]) => [[String(r.number), "num", "th"], [digits(r.n), "num"], [digits(r.peak), "num"], [by, "wrap"]];
  fill("path-records-table", rows.slice(-Math.max(SHOWN_RECORDS, found.length)), cells);
  fill("path-records-all-table", rows, cells);
  $("path-records-all-n").textContent = String(rows.length);
  $("path-records-all").hidden = !published;
}

function render(s, published) {
  $("by").textContent = s.owner_name
    ? `A Collatz conjecture verifier by ${s.owner_name}, checking numbers as fast as it can.`
    : "A Collatz conjecture verifier, checking numbers as fast as it can.";
  renderStatus(s);
  renderAudits(s);
  renderLedger(s);
  renderStart(s);
  if (!s.campaign) {
    $("frontier").textContent = "Nothing has been checked yet.";
    $("checked").replaceChildren();
    $("data").hidden = true;
    return;
  }
  const now = s.generated_at;
  const f = s.frontier;
  $("frontier").replaceChildren("Every number below ", el("b", "", digits(f.value)), " (", approx(f.log2, 4), ") has been checked.");
  const done = s.totals.numbers_verified;
  $("checked").replaceChildren(
    ...(done === "0"
      ? ["This verifier has not checked any numbers yet."]
      : ["This verifier has checked ", digits(done), " numbers so far (", approx(log2Of(done), 2), ")."]),
  );

  const sp = s.speed;
  const windows = [
    ["Last 10 minutes", sp.last_10_min],
    ["Last hour", sp.last_hour],
    ["Last 24 hours", sp.last_24_h],
    ["All time", sp.all_time],
  ];
  fill("speed", windows, ([name, w]) => {
    const r = w.numbers_per_second;
    return [[name, "", "th"], [words(r), "num"], [r === null ? "-" : words(r * 86400), "num"]];
  });

  const marks = s.milestones.filter((m) => /^2\^[0-9.]+$/.test(m.label));
  fill("eta", marks, (m) => [
    [pow(m.label.slice(2)), "", "th"],
    [duration(m.eta_seconds), "num"],
    [m.eta_at === null ? "-" : date(m.eta_at)],
  ]);

  renderHistory(s);

  const today = date(now);
  const when = (t, seconds) => (date(t) === today ? "" : `${date(t)} `) + (seconds ? hms(t) : hm(t));
  fill("hosts", s.hosts, (h) => {
    const kind = /^(gpu|cpu)\b/i.exec(h.backend || "");
    const name = el("span", "", el("code", "", String(h.host).slice(0, 8)), kind ? ` ${kind[1].toUpperCase()}` : "");
    return [[name, "", "th"], [words(Number(h.numbers)), "num"], [words(h.numbers_per_second), "num"], [when(h.last_at, false)]];
  });

  fill("blocks", s.blocks, (b) => [
    [when(b.submitted_at, true)],
    [pow(b.k), "num"],
    [b.seconds.toFixed(1), "num"],
    [words(b.numbers_per_second), "num"],
  ]);

  const mx = s.max_excursion;
  $("peak").hidden = !mx;
  $("no-peak").hidden = !!mx;
  if (mx) {
    $("peak-value").replaceChildren(digits(mx.peak), " (", approx(mx.peak_log2, 2), ")");
    $("peak-n").replaceChildren(digits(mx.n));
  }

  renderPathRecords(s, published);
  $("data").hidden = false;
}

// ?campaign=ID shows another campaign than the server's default one.
const wanted = new URLSearchParams(location.search).get("campaign") ?? "";
const query = /^[0-9]{1,15}$/.test(wanted) ? `?campaign=${wanted}` : "";
let lastLoad = 0;
// path-records.json, once it has loaded.
let publishedRecords = null;

async function loadPublished() {
  if (publishedRecords === null) {
    const r = await fetch("/path-records.json");
    if (r.ok) publishedRecords = await r.json();
  }
  return publishedRecords;
}

async function load() {
  lastLoad = Date.now();
  try {
    const [r, published] = await Promise.all([fetch(`/api/stats${query}`, { cache: "no-store" }), loadPublished().catch(() => null)]);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    render(await r.json(), published);
  } catch (e) {
    $("status").textContent = `Could not load the numbers (${e.message}).`;
  }
}

load();
setInterval(() => {
  if (document.visibilityState === "visible") load();
}, REFRESH_MS);
// A tab that comes back after a minute or more loads at once.
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && Date.now() - lastLoad >= REFRESH_MS) load();
});
