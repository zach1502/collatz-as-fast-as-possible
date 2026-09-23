-- Account columns, worker keys and units.rev. The server uses units.rev; the
-- users columns, the keys table and the runs columns below are unused. Rows
-- written by 0001 keep working.

-- Unused.
ALTER TABLE users ADD COLUMN github_id INTEGER;
ALTER TABLE users ADD COLUMN login TEXT;
ALTER TABLE users ADD COLUMN account_created_at INTEGER;
ALTER TABLE users ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN banned INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN banned_reason TEXT;
ALTER TABLE users ADD COLUMN banned_at INTEGER;
CREATE UNIQUE INDEX users_github_id ON users (github_id) WHERE github_id IS NOT NULL;

-- Unused.
CREATE TABLE keys (
    key_hash TEXT PRIMARY KEY,
    user TEXT NOT NULL,
    host TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER
);
CREATE UNIQUE INDEX keys_live ON keys (user, host) WHERE revoked_at IS NULL;

-- rev: bumped by every write to the unit (a submit, a fetch that looks at a
-- waiting unit, an owner decision), so a write based on an older read fails
-- its guard.
ALTER TABLE units ADD COLUMN rev INTEGER NOT NULL DEFAULT 0;

-- Unused: the server writes none of these.
ALTER TABLE runs ADD COLUMN valid INTEGER NOT NULL DEFAULT 1;
ALTER TABLE runs ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;
ALTER TABLE runs ADD COLUMN credited INTEGER NOT NULL DEFAULT 0;
CREATE INDEX runs_invalid ON runs (unit_id) WHERE valid < 0;
