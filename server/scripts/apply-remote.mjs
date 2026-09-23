// Applies migrations/*.sql to the remote D1 database one statement at a time.
//
// `wrangler d1 migrations apply --remote` sends a whole migration in one query,
// and D1 fails on files that contain triggers ("incomplete input",
// SQLITE_ERROR 7500). This script sends every statement through `wrangler d1
// execute --file`, which uses D1's import path and runs a trigger as one
// statement. Progress is kept in .migrate-progress.json, so an interrupted run
// continues where it stopped; a finished migration is recorded in
// d1_migrations, the table wrangler itself uses.
//     node scripts/apply-remote.mjs [database-name]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const db = process.argv[2] ?? "collatz-frontier";
const progressFile = path.join(root, ".migrate-progress.json");

function wrangler(args, { json = false, alreadyOk = false } = {}) {
    // npx is a .cmd file on Windows, which only runs through a shell.
    const quoted = args.map((a) => (/[\s"]/.test(a) ? `"${a.replace(/"/g, '\\"')}"` : a));
    const r = spawnSync("npx", ["wrangler", ...quoted], { cwd: root, shell: true, encoding: "utf8" });
    // A statement that already took effect: a run cut short after D1 applied it, or an
    // import D1 retried. Creating the same object again is then not an error.
    if (r.status !== 0 && alreadyOk && /already exists|duplicate column name/i.test(`${r.stdout}${r.stderr}`)) {
        console.log("  (already applied)");
        return null;
    }
    if (r.status !== 0) {
        process.stdout.write(r.stdout ?? "");
        process.stderr.write(r.stderr ?? "");
        throw new Error(`wrangler ${args.slice(0, 3).join(" ")} failed (exit ${r.status})`);
    }
    if (!json) return null;
    const out = r.stdout;
    return JSON.parse(out.slice(out.indexOf("[")));
}

/** Statements of a comment-free migration: split at `;` outside quotes and BEGIN ... END. */
function splitStatements(sql) {
    const out = [];
    let cur = "";
    let depth = 0;
    for (let i = 0; i < sql.length; i++) {
        const c = sql[i];
        if (c === "'" || c === '"' || c === "`") {
            const j = sql.indexOf(c, i + 1);
            const end = j < 0 ? sql.length : j + 1;
            cur += sql.slice(i, end);
            i = end - 1;
            continue;
        }
        cur += c;
        if (/\s/.test(c) || c === ";") {
            const word = cur.slice(0, -1).match(/(?:^|\s)(BEGIN|END)$/i);
            if (word) depth += word[1].toUpperCase() === "BEGIN" ? 1 : -1;
        }
        if (c === ";" && depth === 0) {
            out.push(cur.trim());
            cur = "";
        }
    }
    if (cur.trim() !== "") out.push(cur.trim());
    return out;
}

/** CREATE TABLE/INDEX/TRIGGER/VIEW with IF NOT EXISTS, so running it twice is harmless. */
function idempotent(stmt) {
    return stmt.replace(/^CREATE\s+(UNIQUE\s+)?(TABLE|INDEX|TRIGGER|VIEW)\s+(?!IF\s+NOT\s+EXISTS)/i,
        (_m, u, kind) => `CREATE ${u ? "UNIQUE " : ""}${kind.toUpperCase()} IF NOT EXISTS `);
}

function main() {
    const progress = fs.existsSync(progressFile) ? JSON.parse(fs.readFileSync(progressFile, "utf8")) : {};
    const save = () => fs.writeFileSync(progressFile, JSON.stringify(progress, null, 2));
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "collatz-migrate-"));
    try {
        const stmtFile = path.join(tmp, "statement.sql");
        const run = (sql) => {
            fs.writeFileSync(stmtFile, idempotent(sql) + "\n");
            wrangler(["d1", "execute", db, "--remote", "--yes", "--file", stmtFile], { alreadyOk: true });
        };
        run("CREATE TABLE IF NOT EXISTS d1_migrations(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP NOT NULL);");
        const rows = wrangler(["d1", "execute", db, "--remote", "--json", "--command", "SELECT name FROM d1_migrations"], { json: true });
        const applied = new Set((rows[0]?.results ?? []).map((r) => r.name));
        const names = fs.readdirSync(path.join(root, "migrations")).filter((n) => n.endsWith(".sql")).sort();
        for (const name of names) {
            if (applied.has(name)) {
                console.log(`${name}: already applied`);
                continue;
            }
            const stmts = splitStatements(fs.readFileSync(path.join(root, "migrations", name), "utf8"));
            let done = progress[name] ?? 0;
            for (; done < stmts.length; done++) {
                console.log(`${name}: statement ${done + 1} of ${stmts.length}`);
                run(stmts[done]);
                progress[name] = done + 1;
                save();
            }
            run(`INSERT OR IGNORE INTO d1_migrations (name) VALUES ('${name}');`);
            delete progress[name];
            save();
            console.log(`${name}: applied`);
        }
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        main();
    } catch (e) {
        console.error(String(e.message ?? e));
        process.exit(1);
    }
}
