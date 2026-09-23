-- Random certify audits: a worker re-derives a seeded sample of the sieve
-- decisions of a unit it ran and reports the outcome with POST /api/audit
-- (src/audit.ts). Rows written by 0001-0005 keep working.

-- One reported audit of a submitted run of an accepted unit with its digest:
-- the run's user ('owner') and host, its result digest as the seed the
-- residues were drawn from, the residues drawn, the failed checks (0: the
-- audit passed) and its seconds. An audit with failures also raises an alert
-- of kind 'audit'; the unit is not changed until the owner withdraws it. A
-- run reports an audit of a given seed once. The rows of a unit the owner
-- starts over (admin_units.ts: decide with requeue or withdraw) are deleted
-- with its runs.
CREATE TABLE audits (
    id INTEGER PRIMARY KEY,
    campaign_id INTEGER NOT NULL,
    unit_id INTEGER NOT NULL REFERENCES units (id),
    user TEXT NOT NULL,
    host TEXT NOT NULL,
    seed TEXT NOT NULL,
    samples INTEGER NOT NULL,
    failures INTEGER NOT NULL,
    seconds REAL NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX audits_run ON audits (unit_id, user, host, seed);

-- The audits per campaign without and with failures (the page's audit line).
-- The counts stay when the audited unit is started over.
ALTER TABLE campaigns ADD COLUMN audits_passed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE campaigns ADD COLUMN audits_failed INTEGER NOT NULL DEFAULT 0;
