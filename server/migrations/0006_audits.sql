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

ALTER TABLE campaigns ADD COLUMN audits_passed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE campaigns ADD COLUMN audits_failed INTEGER NOT NULL DEFAULT 0;
