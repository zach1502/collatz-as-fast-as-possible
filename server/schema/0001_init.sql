-- Work-unit server schema. Times are Unix seconds. Integers
-- that can exceed 2^53 (L0, n, peaks) are canonical decimal TEXT.
--
-- A unit is one aligned block of a campaign: 2^bits numbers, min_bits <= bits
-- <= max_bits, starting at a multiple of 2^bits, i.e. `collatz verify --block H
-- --K bits`. Positions are counted in slots of 2^min_bits numbers from l0: the
-- unit at pos with len = 2^(bits - min_bits) slots covers [l0 + pos*2^min_bits,
-- l0 + (pos+len)*2^min_bits), and pos is a multiple of len. l0 is a multiple of
-- 2^max_bits, so that block is aligned in absolute terms too.

CREATE TABLE campaigns (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    active INTEGER NOT NULL DEFAULT 1,
    l0 TEXT NOT NULL,
    -- Block sizes a worker can ask for (fetch `bits`), as powers of two.
    min_bits INTEGER NOT NULL,
    max_bits INTEGER NOT NULL,
    a INTEGER NOT NULL,
    b INTEGER NOT NULL,
    kernel TEXT NOT NULL DEFAULT 'table16',
    bitvectors INTEGER NOT NULL DEFAULT 8,
    mod3t INTEGER NOT NULL DEFAULT 6,
    partial_oee INTEGER NOT NULL DEFAULT 1,
    window_chains INTEGER NOT NULL DEFAULT 6,
    window_templates_d INTEGER NOT NULL DEFAULT 12,
    -- Decimal, or NULL for --record-threshold none.
    record_threshold TEXT,
    -- Passed as --assumption-source; NULL leaves the binary's default.
    assumption_source TEXT,
    -- JSON array of the binary commits whose results are accepted.
    commits TEXT NOT NULL,
    -- First line of the result digest text these results use
    -- (checkpoint::kResultDigestSchema); results with another one are refused.
    digest_schema TEXT NOT NULL DEFAULT 'collatz-result-digest/2',
    -- 1: a unit is accepted on one result, the only value the server runs
    -- (src/units/campaign.ts requireRunnable).
    quorum INTEGER NOT NULL DEFAULT 1,
    -- Probability that an accepted unit is queued for one more run (a re-check).
    recheck_rate REAL NOT NULL DEFAULT 0.05,
    lease_seconds INTEGER NOT NULL DEFAULT 86400,
    -- At most this many units in state open.
    max_open INTEGER NOT NULL DEFAULT 1000,
    -- End of the campaign in slots from l0; NULL = open-ended.
    end_pos INTEGER,
    created_at INTEGER NOT NULL,
    -- End of the contiguous accepted prefix, in slots from l0: every number
    -- in [l0, l0 + frontier_pos*2^min_bits) lies in an accepted unit.
    frontier_pos INTEGER NOT NULL DEFAULT 0,
    -- High-water mark of the allocator: no unit or free block reaches past
    -- it; [0, alloc_pos) is covered by units and free_blocks.
    alloc_pos INTEGER NOT NULL DEFAULT 0,
    -- Kept by the units_count_* triggers from the units rows, so the counts
    -- are one row instead of a scan of the units. slots_* are sizes in slots.
    units_open INTEGER NOT NULL DEFAULT 0,
    units_disputed INTEGER NOT NULL DEFAULT 0,
    units_accepted INTEGER NOT NULL DEFAULT 0,
    slots_accepted INTEGER NOT NULL DEFAULT 0,
    units_twice INTEGER NOT NULL DEFAULT 0,
    slots_twice INTEGER NOT NULL DEFAULT 0,
    units_conflict INTEGER NOT NULL DEFAULT 0,
    units_error INTEGER NOT NULL DEFAULT 0,
    conflicts_total INTEGER NOT NULL DEFAULT 0,
    rechecks_passed INTEGER NOT NULL DEFAULT 0,
    rechecks_failed INTEGER NOT NULL DEFAULT 0,
    -- Done re-checks by kind (units.recheck_kind).
    rechecks_account INTEGER NOT NULL DEFAULT 0,
    rechecks_host INTEGER NOT NULL DEFAULT 0,
    rechecks_backend INTEGER NOT NULL DEFAULT 0,
    rechecks_same INTEGER NOT NULL DEFAULT 0,
    -- Unused: the server does not write results_total, results_bad and recent.
    results_total INTEGER NOT NULL DEFAULT 0,
    results_bad INTEGER NOT NULL DEFAULT 0,
    -- A JSON array.
    recent TEXT NOT NULL DEFAULT '[]',
    CHECK (quorum IN (1, 2) AND recheck_rate >= 0 AND recheck_rate <= 1 AND min_bits >= 32
           AND min_bits <= max_bits AND max_bits - min_bits <= 30 AND a >= 1 AND a < min_bits
           AND lease_seconds >= 1)
);

CREATE TABLE users (
    -- Unused: the server has no user accounts (runs.user is always 'owner').
    name_lc TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    -- Lowercase hex SHA-256 of a key.
    key_hash TEXT NOT NULL,
    created_at INTEGER NOT NULL
) WITHOUT ROWID;

-- AUTOINCREMENT: a dissolved unit's id is never given to a later unit, so a
-- lease or a result that names an old id cannot reach another block.
CREATE TABLE units (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    campaign_id INTEGER NOT NULL REFERENCES campaigns (id),
    pos INTEGER NOT NULL,
    bits INTEGER NOT NULL,
    -- 2^(bits - min_bits): the block's size in slots.
    len INTEGER NOT NULL,
    -- open: needs a result; accepted: counts toward the frontier; conflict: a
    -- re-check disagreed with the accepted result, so the unit left the
    -- frontier and waits for the owner's decision; error: not written.
    state TEXT NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'accepted', 'conflict', 'error')),
    -- 1 once the unit's results disagree; 0 again when it starts over.
    disputed INTEGER NOT NULL DEFAULT 0,
    -- Runs (leased or submitted) the unit takes at present.
    wanted INTEGER NOT NULL,
    -- Runs in state leased or submitted; maintained by the runs_* triggers.
    assigned INTEGER NOT NULL DEFAULT 0,
    -- Submitted runs of any role.
    results INTEGER NOT NULL DEFAULT 0,
    -- 1 while the unit is accepted (src/units/derive.ts derive), else 0: a
    -- re-check that agrees adds no vote.
    checks INTEGER NOT NULL DEFAULT 0,
    -- The accepted result_digest.
    digest TEXT,
    created_at INTEGER NOT NULL,
    accepted_at INTEGER,
    -- The random re-check: NULL (not drawn), 'pending', 'passed', 'failed'
    -- (the unit went to conflict) or 'expired' (no run before the embargo ended).
    recheck TEXT CHECK (recheck IN ('pending', 'passed', 'failed', 'expired')),
    -- How the re-check run differed from the accepted run: 'host', 'backend'
    -- or 'same' (src/recheck.ts runKind; 'account' is not written).
    recheck_kind TEXT CHECK (recheck_kind IN ('account', 'host', 'backend', 'same')),
    -- Since when the unit waits for a re-check, or in conflict for the owner's
    -- decision.
    special_at INTEGER,
    -- draw_user: unused (NULL). draw_at: when a fetch last looked at the
    -- waiting unit; src/recheck.ts expireDue looks again once lease_seconds
    -- pass without a live lease of it.
    draw_user TEXT,
    draw_at INTEGER
);
-- Units never overlap, so one unit at most starts at a position.
CREATE UNIQUE INDEX units_pos ON units (campaign_id, pos);
-- Open units that can take another lease, lowest first (the fetch scan).
CREATE INDEX units_need ON units (campaign_id, pos) WHERE state = 'open' AND assigned < wanted;
-- Units waiting for a re-check or in conflict, oldest first (units_special),
-- and by draw_at for src/recheck.ts expireDue (units_draw). units_drawn and
-- units_held are unused.
CREATE INDEX units_special ON units (campaign_id, special_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_drawn ON units (campaign_id, draw_user, special_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_draw ON units (campaign_id, draw_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_held ON units (campaign_id, special_at) WHERE state = 'conflict' AND draw_user IS NULL;

-- Free aligned blocks below campaigns.alloc_pos (padding left by aligning a
-- larger block, the rest of a split free block, or a dissolved unit).
CREATE TABLE free_blocks (
    campaign_id INTEGER NOT NULL,
    pos INTEGER NOT NULL,
    bits INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, pos)
) WITHOUT ROWID;

-- The agreed result document of an accepted unit (result/check.ts
-- canonicalDocument), one row per unit. It has a table of its own so that
-- scripts/prune.sql deletes whole rows, which frees whole pages for reuse.
CREATE TABLE unit_results (
    unit_id INTEGER PRIMARY KEY REFERENCES units (id),
    result_json TEXT NOT NULL
);

-- user: always 'owner', the owner's machines (OWNER_USER in src/http/env.ts).
-- role: 'regular' (the result that accepts the unit) or 'recheck' (the
-- random re-check of an accepted unit); 'conflict' is not written. At most
-- one run of a unit is live at a time.
-- backend: 'gpu' or 'cpu', the backend the lease asked for; set to the
-- backend of the result (params.gpu) on submit.
-- kind: for re-check runs, how the run differed from the run before it
-- (src/recheck.ts runKind).
CREATE TABLE runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    unit_id INTEGER NOT NULL REFERENCES units (id),
    user TEXT NOT NULL,
    host TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'regular' CHECK (role IN ('regular', 'recheck', 'conflict')),
    state TEXT NOT NULL CHECK (state IN ('leased', 'released', 'expired', 'submitted')),
    backend TEXT NOT NULL DEFAULT 'cpu' CHECK (backend IN ('gpu', 'cpu')),
    kind TEXT CHECK (kind IN ('account', 'host', 'backend', 'same')),
    leased_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    submitted_at INTEGER,
    digest TEXT,
    -- The canonical result document while the unit is not accepted. At
    -- acceptance it is dropped from the runs that agree (unit_results keeps
    -- one copy) and kept for the runs that disagree.
    result_json TEXT
);
CREATE INDEX runs_unit ON runs (unit_id, user);
CREATE INDEX runs_leased_user ON runs (user, expires_at) WHERE state = 'leased';
CREATE INDEX runs_leased_expiry ON runs (expires_at) WHERE state = 'leased';

-- units.assigned = number of the unit's runs in state leased or submitted.
CREATE TRIGGER runs_assign_insert AFTER INSERT ON runs
WHEN NEW.state IN ('leased', 'submitted')
BEGIN
    UPDATE units SET assigned = assigned + 1 WHERE id = NEW.unit_id;
END;

CREATE TRIGGER runs_assign_update AFTER UPDATE OF state ON runs
WHEN (OLD.state IN ('leased', 'submitted')) <> (NEW.state IN ('leased', 'submitted'))
BEGIN
    UPDATE units
    SET assigned = assigned + iif(NEW.state IN ('leased', 'submitted'), 1, -1)
    WHERE id = NEW.unit_id;
END;

-- The campaign's unit counters follow every unit row. IS compares NULLs
-- as values (recheck and recheck_kind can be NULL).
CREATE TRIGGER units_count_insert AFTER INSERT ON units
BEGIN
    UPDATE campaigns SET units_open = units_open + (NEW.state = 'open') WHERE id = NEW.campaign_id;
END;

CREATE TRIGGER units_count_update AFTER UPDATE OF state, checks, disputed, recheck, recheck_kind ON units
WHEN OLD.state IS NOT NEW.state OR OLD.checks IS NOT NEW.checks OR OLD.disputed IS NOT NEW.disputed
     OR OLD.recheck IS NOT NEW.recheck OR OLD.recheck_kind IS NOT NEW.recheck_kind
BEGIN
    UPDATE campaigns SET
        units_open = units_open + (NEW.state = 'open') - (OLD.state = 'open'),
        units_disputed = units_disputed + (NEW.state = 'open' AND NEW.disputed = 1) - (OLD.state = 'open' AND OLD.disputed = 1),
        units_accepted = units_accepted + (NEW.state = 'accepted') - (OLD.state = 'accepted'),
        slots_accepted = slots_accepted + (NEW.state = 'accepted') * NEW.len - (OLD.state = 'accepted') * OLD.len,
        units_twice = units_twice + (NEW.state = 'accepted' AND NEW.checks >= 2) - (OLD.state = 'accepted' AND OLD.checks >= 2),
        slots_twice = slots_twice + (NEW.state = 'accepted' AND NEW.checks >= 2) * NEW.len
                                  - (OLD.state = 'accepted' AND OLD.checks >= 2) * OLD.len,
        units_conflict = units_conflict + (NEW.state = 'conflict') - (OLD.state = 'conflict'),
        conflicts_total = conflicts_total + (NEW.state = 'conflict' AND OLD.state <> 'conflict'),
        units_error = units_error + (NEW.state = 'error') - (OLD.state = 'error'),
        rechecks_passed = rechecks_passed + (NEW.recheck IS 'passed') - (OLD.recheck IS 'passed'),
        rechecks_failed = rechecks_failed + (NEW.recheck IS 'failed') - (OLD.recheck IS 'failed'),
        rechecks_account = rechecks_account + (NEW.recheck IN ('passed', 'failed') AND NEW.recheck_kind IS 'account')
                                            - (OLD.recheck IN ('passed', 'failed') AND OLD.recheck_kind IS 'account'),
        rechecks_host = rechecks_host + (NEW.recheck IN ('passed', 'failed') AND NEW.recheck_kind IS 'host')
                                      - (OLD.recheck IN ('passed', 'failed') AND OLD.recheck_kind IS 'host'),
        rechecks_backend = rechecks_backend + (NEW.recheck IN ('passed', 'failed') AND NEW.recheck_kind IS 'backend')
                                            - (OLD.recheck IN ('passed', 'failed') AND OLD.recheck_kind IS 'backend'),
        rechecks_same = rechecks_same + (NEW.recheck IN ('passed', 'failed') AND NEW.recheck_kind IS 'same')
                                      - (OLD.recheck IN ('passed', 'failed') AND OLD.recheck_kind IS 'same')
    WHERE id = NEW.campaign_id;
END;

-- Only open units without results are deleted (dissolved into free blocks).
CREATE TRIGGER units_count_delete AFTER DELETE ON units
BEGIN
    UPDATE campaigns SET units_open = units_open - (OLD.state = 'open'),
        units_disputed = units_disputed - (OLD.state = 'open' AND OLD.disputed = 1)
    WHERE id = OLD.campaign_id;
END;

-- Walked numbers above the campaign's record threshold, from accepted units;
-- users is 'owner'.
CREATE TABLE records (
    campaign_id INTEGER NOT NULL,
    n TEXT NOT NULL,
    peak TEXT NOT NULL,
    unit_id INTEGER NOT NULL,
    users TEXT NOT NULL,
    found_at INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, n)
) WITHOUT ROWID;
-- The newest records first.
CREATE INDEX records_recent ON records (campaign_id, found_at DESC);

-- One row per campaign for user 'owner'. since_recheck: the regular results
-- of the owner's machines since the last re-check lease (RECHECK_EVERY).
-- units, slots and bad are unused.
CREATE TABLE user_stats (
    campaign_id INTEGER NOT NULL,
    user TEXT NOT NULL,
    units INTEGER NOT NULL DEFAULT 0,
    slots INTEGER NOT NULL DEFAULT 0,
    bad INTEGER NOT NULL DEFAULT 0,
    since_recheck INTEGER NOT NULL DEFAULT 0,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, user)
) WITHOUT ROWID;
CREATE INDEX user_stats_rank ON user_stats (campaign_id, slots DESC);

-- Machines that fetched work in a campaign (fetch `host`, `backend`,
-- `recheck_cpu`, `bits`), refreshed at most every 10 minutes. Re-checks go
-- to another active host first, then the other backend (src/recheck.ts).
CREATE TABLE hosts (
    campaign_id INTEGER NOT NULL,
    user TEXT NOT NULL,
    host TEXT NOT NULL,
    backend TEXT NOT NULL CHECK (backend IN ('gpu', 'cpu')),
    recheck_cpu INTEGER NOT NULL DEFAULT 0,
    bits INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, user, host)
) WITHOUT ROWID;
CREATE INDEX hosts_seen ON hosts (campaign_id, last_seen);

-- kind 'host', 'backend' or 'same': a re-check of that kind disagreed with
-- an accepted result. fields: JSON array of the digest-text fields
-- (result/digest.ts digestText) whose values differ; runs: JSON array of the
-- runs involved ({user, host, role, kind, backend, digest}). outcome: how the
-- conflict was settled ('owner': the owner picked a digest, 'requeued': the
-- owner started the unit over), NULL while open. kind 'audit' (migration
-- 0006): a machine's audit found failures. kind 'resend' (src/submit/resend.ts):
-- a result that came on no live lease differs from the unit's submitted
-- results; runs then also holds that result's lease_id and document, and one
-- alert keeps each such digest. GET /api/admin/alerts lists them;
-- resolving one is the owner's call (POST /api/admin/alerts/resolve,
-- /api/admin/decide).
CREATE TABLE alerts (
    id INTEGER PRIMARY KEY,
    campaign_id INTEGER NOT NULL,
    unit_id INTEGER NOT NULL,
    kind TEXT NOT NULL,
    fields TEXT NOT NULL,
    runs TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    outcome TEXT CHECK (outcome IN ('original', 'recheck', 'error', 'owner', 'requeued', 'taken')),
    settled_at INTEGER,
    resolved_at INTEGER,
    note TEXT
);
CREATE INDEX alerts_open ON alerts (campaign_id, id) WHERE resolved_at IS NULL;
CREATE INDEX alerts_unit ON alerts (unit_id);

-- Fixed-window request counters: requests per IP or per machine when the
-- Workers rate-limiting bindings are not configured. Keys hold
-- HMAC-SHA256(RL_SECRET, id) prefixes, never a raw IP address; bucket is the
-- window start in Unix seconds.
CREATE TABLE rate (
    key TEXT PRIMARY KEY,
    bucket INTEGER NOT NULL,
    count INTEGER NOT NULL
) WITHOUT ROWID;
