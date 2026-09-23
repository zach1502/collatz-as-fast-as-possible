CREATE TABLE campaigns (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    active INTEGER NOT NULL DEFAULT 1,
    l0 TEXT NOT NULL,

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

    record_threshold TEXT,

    assumption_source TEXT,

    commits TEXT NOT NULL,

    digest_schema TEXT NOT NULL DEFAULT 'collatz-result-digest/2',

    quorum INTEGER NOT NULL DEFAULT 1,

    recheck_rate REAL NOT NULL DEFAULT 0.05,
    lease_seconds INTEGER NOT NULL DEFAULT 86400,

    max_open INTEGER NOT NULL DEFAULT 1000,

    end_pos INTEGER,
    created_at INTEGER NOT NULL,

    frontier_pos INTEGER NOT NULL DEFAULT 0,

    alloc_pos INTEGER NOT NULL DEFAULT 0,

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

    rechecks_account INTEGER NOT NULL DEFAULT 0,
    rechecks_host INTEGER NOT NULL DEFAULT 0,
    rechecks_backend INTEGER NOT NULL DEFAULT 0,
    rechecks_same INTEGER NOT NULL DEFAULT 0,

    results_total INTEGER NOT NULL DEFAULT 0,
    results_bad INTEGER NOT NULL DEFAULT 0,

    recent TEXT NOT NULL DEFAULT '[]',
    CHECK (quorum IN (1, 2) AND recheck_rate >= 0 AND recheck_rate <= 1 AND min_bits >= 32
           AND min_bits <= max_bits AND max_bits - min_bits <= 30 AND a >= 1 AND a < min_bits
           AND lease_seconds >= 1)
);

CREATE TABLE users (

    name_lc TEXT PRIMARY KEY,
    name TEXT NOT NULL,

    key_hash TEXT NOT NULL,
    created_at INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE units (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    campaign_id INTEGER NOT NULL REFERENCES campaigns (id),
    pos INTEGER NOT NULL,
    bits INTEGER NOT NULL,

    len INTEGER NOT NULL,

    state TEXT NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'accepted', 'conflict', 'error')),

    disputed INTEGER NOT NULL DEFAULT 0,

    wanted INTEGER NOT NULL,

    assigned INTEGER NOT NULL DEFAULT 0,

    results INTEGER NOT NULL DEFAULT 0,

    checks INTEGER NOT NULL DEFAULT 0,

    digest TEXT,
    created_at INTEGER NOT NULL,
    accepted_at INTEGER,

    recheck TEXT CHECK (recheck IN ('pending', 'passed', 'failed', 'expired')),

    recheck_kind TEXT CHECK (recheck_kind IN ('account', 'host', 'backend', 'same')),

    special_at INTEGER,

    draw_user TEXT,
    draw_at INTEGER
);

CREATE UNIQUE INDEX units_pos ON units (campaign_id, pos);

CREATE INDEX units_need ON units (campaign_id, pos) WHERE state = 'open' AND assigned < wanted;

CREATE INDEX units_special ON units (campaign_id, special_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_drawn ON units (campaign_id, draw_user, special_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_draw ON units (campaign_id, draw_at)
    WHERE (state = 'accepted' AND recheck = 'pending') OR state = 'conflict';
CREATE INDEX units_held ON units (campaign_id, special_at) WHERE state = 'conflict' AND draw_user IS NULL;

CREATE TABLE free_blocks (
    campaign_id INTEGER NOT NULL,
    pos INTEGER NOT NULL,
    bits INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, pos)
) WITHOUT ROWID;

CREATE TABLE unit_results (
    unit_id INTEGER PRIMARY KEY REFERENCES units (id),
    result_json TEXT NOT NULL
);

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

    result_json TEXT
);
CREATE INDEX runs_unit ON runs (unit_id, user);
CREATE INDEX runs_leased_user ON runs (user, expires_at) WHERE state = 'leased';
CREATE INDEX runs_leased_expiry ON runs (expires_at) WHERE state = 'leased';

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

CREATE TRIGGER units_count_delete AFTER DELETE ON units
BEGIN
    UPDATE campaigns SET units_open = units_open - (OLD.state = 'open'),
        units_disputed = units_disputed - (OLD.state = 'open' AND OLD.disputed = 1)
    WHERE id = OLD.campaign_id;
END;

CREATE TABLE records (
    campaign_id INTEGER NOT NULL,
    n TEXT NOT NULL,
    peak TEXT NOT NULL,
    unit_id INTEGER NOT NULL,
    users TEXT NOT NULL,
    found_at INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, n)
) WITHOUT ROWID;

CREATE INDEX records_recent ON records (campaign_id, found_at DESC);

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

CREATE TABLE rate (
    key TEXT PRIMARY KEY,
    bucket INTEGER NOT NULL,
    count INTEGER NOT NULL
) WITHOUT ROWID;
