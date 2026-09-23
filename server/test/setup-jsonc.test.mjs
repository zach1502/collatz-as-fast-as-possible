// The edits scripts/windows/setup-server.ps1 makes to server/wrangler.jsonc, which holds
// the live server settings. When pwsh or Windows PowerShell is installed (or $PWSH names
// one), Set-JsoncValue and the functions it calls, read out of setup-server.ps1, run on
// copies of server/wrangler.example.jsonc and of live files made from it under os.tmpdir() (LF and
// CRLF, tabs and spaces, a byte order mark, comments that name a key), built in
// test/helpers-setup-jsonc.mjs: only the characters between the quotes of the value set may
// change, a second run changes nothing, each change prints its line, and a missing key is
// refused. The template is only read.
// Run from server/: node --test test/setup-jsonc.test.mjs

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, test } from "node:test";

import { SERVER_DIR, TEMPLATE, TEMPLATE_PATH, bytesOf, cases, printed, refusals } from "./helpers-setup-jsonc.mjs";

const SETUP = path.join(SERVER_DIR, "..", "scripts", "windows", "setup-server.ps1");
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "collatz-setup-jsonc-"));
after(() => fs.rmSync(TMP, { recursive: true, force: true }));

function place(dir, name, input) {
    const file = path.join(TMP, dir, name, "wrangler.jsonc");
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, input);
    return file;
}

// Reads the functions and the two encodings out of setup-server.ps1 (whose parse must
// succeed), then runs a plan of setup runs and prints "OP n" before each operation, "SAID x"
// for each Write-Host line and "RET json" for the return value, as they happen: Fail ends
// the process with exit 1, as in the setup, after its message is out.
const DRIVER = String.raw`param([string]$Setup, [string]$Plan)
$ErrorActionPreference = "Continue"
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Setup, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { Write-Output ("PARSE ERROR line " + $errors[0].Extent.StartLineNumber + ": " + $errors[0].Message); exit 2 }
$wanted = @("Fail", "Read-Jsonc", "Write-Jsonc", "Get-JsoncCode", "Find-JsoncBlock", "Get-JsoncKeyRegex", "Format-JsonString", "Get-JsoncValue", "Set-JsoncValue")
$defined = @()
foreach ($f in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    if ($wanted -contains $f.Name) { . ([scriptblock]::Create($f.Extent.Text)); $defined += $f.Name }
}
foreach ($s in $ast.EndBlock.Statements) {
    if ($s -is [System.Management.Automation.Language.AssignmentStatementAst] -and @('$Utf8', '$Utf8Strict') -contains $s.Left.Extent.Text) {
        . ([scriptblock]::Create($s.Extent.Text))
        $defined += $s.Left.Extent.Text
    }
}
if ($defined.Count -ne $wanted.Count + 2) { Write-Output ("MISSING: found only " + ($defined -join ", ")); exit 2 }
$n = 0
foreach ($run in (ConvertFrom-Json (Get-Content $Plan -Raw)).runs) {
    $script:JsoncBackedUp = $false
    foreach ($op in $run.ops) {
        Write-Output "OP $n"
        $n++
        if ($op.op -eq "get") {
            Write-Output ("RET " + (ConvertTo-Json -InputObject (Get-JsoncValue $op.file $op.key $op.block) -Compress))
            continue
        }
        $p = @{ File = $op.file; Key = $op.key; Value = $op.value; Block = $op.block }
        if ($null -ne $op.from) { $p.From = $op.from }
        if ($op.whatIf) { $p.WhatIf = $true }
        Set-JsoncValue @p 6>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.InformationRecord]) { Write-Output ("SAID " + [string]$_.MessageData) }
            else { Write-Output ("RET " + (ConvertTo-Json -InputObject $_ -Compress)) }
        }
    }
}
Write-Output "END"
`;

function findPowerShell() {
    for (const exe of [process.env.PWSH, "pwsh", "powershell"].filter(Boolean)) {
        const r = spawnSync(exe, ["-NoProfile", "-NonInteractive", "-Command", "$PSVersionTable.PSVersion.ToString()"], { encoding: "utf8" });
        if (r.status === 0) return { exe, version: r.stdout.trim() };
    }
    return null;
}
const ps = findPowerShell();

// Runs a plan in PowerShell; per operation, its return value and the lines it printed.
function runPs(runs) {
    const plan = path.join(TMP, `plan-${Math.random().toString(36).slice(2)}.json`);
    const driver = path.join(TMP, "driver.ps1");
    fs.writeFileSync(driver, DRIVER);
    fs.writeFileSync(plan, JSON.stringify({ runs: runs.map((ops) => ({ ops })) }));
    const r = spawnSync(ps.exe, ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", driver, "-Setup", SETUP, "-Plan", plan], { encoding: "utf8" });
    assert.notEqual(r.status, 2, r.stdout);
    const ops = [];
    for (const line of r.stdout.split(/\r?\n/)) {
        if (line.startsWith("OP ")) ops.push({ ret: undefined, said: [] });
        else if (line.startsWith("SAID ")) ops[ops.length - 1].said.push(line.slice(5));
        else if (line.startsWith("RET ")) ops[ops.length - 1].ret = JSON.parse(line.slice(4));
    }
    return { status: r.status, stdout: r.stdout, ops, ended: r.stdout.includes("\nEND") };
}

test(`setup-server.ps1: only the intended values change, in place, and a second run changes nothing${ps ? ` (PowerShell ${ps.version})` : ""}`, { skip: ps ? false : "no pwsh or powershell" }, () => {
    const runs = [];
    const where = [];
    for (const c of cases) {
        const file = place("ps", c.name, bytesOf(c.text, c.bom));
        where.push({ c, file, at: runs.flat().length });
        for (const ops of c.runs) runs.push(ops.map((op) => ({ ...op, file })));
    }
    const { status, stdout, ops, ended } = runPs(runs);
    assert.equal(status, 0, stdout);
    assert.ok(ended, stdout);
    for (const { c, file, at } of where) {
        const mine = ops.slice(at, at + c.rets.length);
        assert.deepEqual(mine.map((r) => r.ret), c.rets, `${c.name}: return values`);
        assert.deepEqual(mine.flatMap((r) => r.said), printed(c), `${c.name}: printed lines`);
        assert.ok(fs.readFileSync(file).equals(bytesOf(c.expected, c.bom)), `${c.name}: bytes`);
        const changed = c.rets.some((r) => r === true);
        assert.equal(fs.existsSync(file + ".bak"), changed, `${c.name}: backup`);
        if (changed) assert.ok(fs.readFileSync(file + ".bak").equals(bytesOf(c.text, c.bom)), `${c.name}: the backup is the file before the run`);
    }
    assert.match(stdout.replace(/\s+/g, " "), /What if: .*set "OWNER_NAME" to "Zach \\"Z\\" \\\\ 1502"/);
});

test("setup-server.ps1 refuses a missing key, a missing block, a bad key or a non-UTF-8 file", { skip: ps ? false : "no pwsh or powershell" }, () => {
    for (const r of refusals) {
        const input = r.raw ?? bytesOf(r.text);
        const file = place("ps", r.name, input);
        const out = runPs([[{ ...r.op, file }]]);
        assert.equal(out.status, 1, `${r.name}: ${out.stdout}`);
        const said = out.ops.flatMap((o) => o.said).join(" ").replace(/\s+/g, " ");
        assert.match(said, /^ERROR: /, r.name);
        assert.match(said, r.message, r.name);
        assert.ok(fs.readFileSync(file).equals(input), `${r.name}: unchanged`);
        assert.equal(fs.existsSync(file + ".bak"), false, `${r.name}: no backup`);
    }
});

test("server/wrangler.example.jsonc was only read", () => {
    assert.ok(fs.readFileSync(TEMPLATE_PATH).equals(TEMPLATE));
});
