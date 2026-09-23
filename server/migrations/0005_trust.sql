ALTER TABLE units ADD COLUMN trusted INTEGER NOT NULL DEFAULT 0;
UPDATE units SET trusted = 1
WHERE EXISTS (SELECT 1 FROM runs r WHERE r.unit_id = units.id AND r.user = 'owner' AND r.state = 'submitted' AND r.valid = 1);

CREATE INDEX units_known ON units (campaign_id, bits, pos) WHERE state = 'accepted' AND trusted = 1;

CREATE TABLE known_runs (
    id INTEGER PRIMARY KEY,
    unit_id INTEGER NOT NULL REFERENCES units (id),
    user TEXT NOT NULL,
    host TEXT NOT NULL,
    backend TEXT NOT NULL CHECK (backend IN ('gpu', 'cpu')),
    state TEXT NOT NULL CHECK (state IN ('leased', 'released', 'submitted')),
    leased_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    submitted_at INTEGER,
    digest TEXT,
    verdict TEXT CHECK (verdict IN ('pass', 'fail')),
    result_json TEXT
);
CREATE INDEX known_runs_unit ON known_runs (unit_id, user);
CREATE INDEX known_runs_live ON known_runs (user, expires_at) WHERE state = 'leased';

ALTER TABLE users ADD COLUMN known_failed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN flagged_at INTEGER;
ALTER TABLE users ADD COLUMN suspended_at INTEGER;

CREATE INDEX runs_user_counted ON runs (user) WHERE state = 'submitted' AND valid = 1;

ALTER TABLE user_stats ADD COLUMN known_passed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE user_stats ADD COLUMN known_failed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE campaigns ADD COLUMN known_passed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE campaigns ADD COLUMN known_failed INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ledger ADD COLUMN format TEXT NOT NULL DEFAULT 'collatz-ledger/1';
ALTER TABLE ledger ADD COLUMN who TEXT NOT NULL DEFAULT '';
ALTER TABLE ledger ADD COLUMN votes INTEGER NOT NULL DEFAULT 0;
