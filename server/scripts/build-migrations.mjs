// Writes migrations/*.sql from schema/*.sql with every comment removed.
//
// D1 splits a remote migration into statements itself, and its splitter
// misreads quotes and semicolons inside `--` comments ("incomplete input",
// SQLITE_ERROR 7500). schema/ keeps the documented source; migrations/ is
// what wrangler applies. `npm test` checks the two are in step.
//     node scripts/build-migrations.mjs          write migrations/
//     node scripts/build-migrations.mjs --check  exit 1 if migrations/ is stale
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const src = path.join(root, "schema");
const dst = path.join(root, "migrations");

/** SQL text without comments: `--` to end of line and block comments, outside quotes. */
function stripComments(sql) {
    let out = "";
    for (let i = 0; i < sql.length; i++) {
        const c = sql[i];
        if (c === "'" || c === '"' || c === "`") {
            const j = sql.indexOf(c, i + 1);
            const end = j < 0 ? sql.length : j + 1;
            out += sql.slice(i, end);
            i = end - 1;
        } else if (c === "-" && sql[i + 1] === "-") {
            const j = sql.indexOf("\n", i);
            i = (j < 0 ? sql.length : j) - 1;
        } else if (c === "/" && sql[i + 1] === "*") {
            const j = sql.indexOf("*/", i + 2);
            i = (j < 0 ? sql.length : j + 2) - 1;
        } else {
            out += c;
        }
    }
    const lines = out.split(/\r?\n/).map((l) => l.replace(/\s+$/, ""));
    return lines.filter((l, k) => l !== "" || (k > 0 && lines[k - 1] !== "")).join("\n").trim() + "\n";
}

const check = process.argv.includes("--check");
let stale = [];
fs.mkdirSync(dst, { recursive: true });
for (const name of fs.readdirSync(src).filter((n) => n.endsWith(".sql")).sort()) {
    const text = stripComments(fs.readFileSync(path.join(src, name), "utf8"));
    const target = path.join(dst, name);
    const current = fs.existsSync(target) ? fs.readFileSync(target, "utf8") : null;
    if (current !== text) {
        if (check) stale.push(name);
        else fs.writeFileSync(target, text);
    }
}
if (check && stale.length > 0) {
    console.error(`migrations/ is out of date for ${stale.join(", ")}: run node scripts/build-migrations.mjs`);
    process.exit(1);
}
