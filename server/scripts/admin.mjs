#!/usr/bin/env node
// Owner commands for the work-unit server's /api/admin/ endpoints. The secret
// is read from the environment, so it never appears on a command line:
//
//   export ADMIN_SECRET=...      (the value given to `npx wrangler secret put ADMIN_SECRET`)
//   node scripts/admin.mjs --server URL alerts [--all]
//   node scripts/admin.mjs --server URL resolve --alert ID --note TEXT
//   node scripts/admin.mjs --server URL decide --unit ID (--digest HEX | --requeue | --withdraw) --note TEXT
//
// decide settles a unit in conflict: --digest accepts the result with that
// digest, --requeue runs the unit again from scratch (its runs are deleted);
// --withdraw takes an accepted unit out of the frontier (after an audit with
// failures, for example) and runs it again from scratch the same way, with a
// 'withdrawn' ledger entry.

import { pathToFileURL } from "node:url";
import { parseArgs, usage } from "./admin-args.mjs";

async function call(o, secret, method, path, body) {
    for (let attempt = 0; ; attempt++) {
        const r = await fetch(`${o.server}${path}`, {
            method,
            headers: { authorization: `Bearer ${secret}`, "content-type": "application/json" },
            body: body === undefined ? undefined : JSON.stringify(body),
            redirect: "manual",
        });
        const text = await r.text();
        let parsed = null;
        try {
            parsed = JSON.parse(text);
        } catch {
            // reported below
        }
        // The per-address limit, or a unit busy with a concurrent update: wait and repeat.
        if ((r.status === 429 || r.status === 503) && attempt < 20) {
            const wait = Math.min(60, Math.max(1, Number(parsed?.retry_after) || 10));
            process.stderr.write(`HTTP ${r.status}; retrying in ${wait} s\n`);
            await new Promise((resolve) => setTimeout(resolve, wait * 1000));
            continue;
        }
        if (r.status !== 200 || parsed === null) {
            throw new Error(`HTTP ${r.status}: ${parsed?.error ?? text.slice(0, 200)}`);
        }
        return parsed;
    }
}

/** Prints the alerts of GET /api/admin/alerts, one line per alert and one per run of a re-check alert. */
function printAlerts(alerts, all) {
    if (alerts.length === 0) {
        console.log(all ? "no alerts" : "no open alerts");
    }
    for (const a of alerts) {
        const when = new Date(a.created_at * 1000).toISOString().slice(0, 16).replace("T", " ");
        const head = `${when}  alert ${a.id}  campaign ${a.campaign_id} unit ${a.unit_id} (block ${a.h} of 2^${a.k}, now ${a.state})  `;
        const resolved = a.resolved_at ? ` [resolved: ${a.note}]` : "";
        if (a.kind === "audit") {
            // A machine's random audit of its own accepted result found failures (src/audit.ts).
            for (const x of a.runs) {
                console.log(
                    `${head}an audit found ${x.failures} failures in ${x.samples} residues (${x.seconds} s); ` +
                        `${x.user} host ${x.host.slice(0, 8)} ${x.backend}, seed ${x.digest.slice(0, 16)}; ` +
                        `its files are in audits/${a.unit_id}/ next to that worker's config${resolved}`,
                );
            }
            continue;
        }
        const fields = a.fields.length > 0 ? a.fields.join(", ") : "(documents not available)";
        // 'resend': a result that came on no live lease and differs from the unit's (src/submit/resend.ts).
        const what = a.kind === "resend" ? "a result sent on no live lease" : `re-check by ${a.kind ?? "?"}`;
        console.log(
            head +
                `${what}; differs in ${fields}; ` +
                `${a.outcome ? `settled: ${a.outcome}` : "not settled yet"}${a.needs ? `; ${a.needs}` : ""}${resolved}`,
        );
        for (const x of a.runs) {
            console.log(`    ${x.role.padEnd(8)} ${x.user} host ${x.host.slice(0, 8)} ${x.backend}${x.kind ? ` ${x.kind}` : ""}  ${x.digest.slice(0, 16)}`);
        }
    }
}

async function main() {
    const o = parseArgs(process.argv.slice(2));
    if (o.server === undefined || o.command === undefined) {
        usage("--server URL and a command are required");
    }
    if (!/^https:\/\/[^/\s]+/.test(o.server) && !/^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(o.server)) {
        usage("--server must be https:// (http:// only for 127.0.0.1 or localhost)");
    }
    const secret = process.env.ADMIN_SECRET ?? "";
    if (secret.length < 32) {
        usage("set ADMIN_SECRET (at least 32 characters) in the environment");
    }
    if (o.command === "alerts") {
        const r = await call(o, secret, "GET", `/api/admin/alerts${o.all ? "?all=1" : ""}`);
        printAlerts(r.alerts, o.all);
        return;
    }
    if (o.command === "resolve") {
        if (o.alert === undefined || typeof o.note !== "string" || o.note === "") {
            usage("resolve needs --alert ID and --note TEXT");
        }
        const r = await call(o, secret, "POST", "/api/admin/alerts/resolve", { id: o.alert, note: o.note });
        console.log(`alert ${r.id}: ${r.resolved ? "resolved" : "was already resolved or does not exist"}`);
        return;
    }
    const choices = [o.digest !== undefined, o.requeue === true, o.withdraw === true].filter(Boolean).length;
    if (o.unit === undefined || choices !== 1 || typeof o.note !== "string" || o.note === "") {
        usage("decide needs --unit ID, one of --digest HEX, --requeue and --withdraw, and --note TEXT");
    }
    const body = o.requeue
        ? { unit_id: o.unit, requeue: true, note: o.note }
        : o.withdraw
          ? { unit_id: o.unit, withdraw: true, note: o.note }
          : { unit_id: o.unit, digest: o.digest, note: o.note };
    const r = await call(o, secret, "POST", "/api/admin/decide", body);
    console.log(
        o.requeue
            ? `unit ${r.unit_id}: requeued (its runs are deleted; it is ${r.state} for new results)`
            : o.withdraw
              ? `unit ${r.unit_id}: withdrawn from the frontier (its runs are deleted; it is ${r.state} for new results)`
              : `unit ${r.unit_id}: ${r.state} with digest ${r.digest}`,
    );
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
    main().catch((e) => {
        process.stderr.write(`error: ${e.message}\n`);
        process.exit(1);
    });
}
