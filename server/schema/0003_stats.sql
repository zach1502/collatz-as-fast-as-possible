-- Statistics for the status page (GET /api/stats, src/stats.ts). block_stats
-- holds one row per accepted unit: submit.ts and admin_units.ts insert it when
-- a unit becomes accepted and delete it when the unit leaves that state (a
-- re-check that disagrees, a withdrawal). The triggers below keep the
-- campaign totals, the hourly and daily buckets and the per-host sums in step
-- with it, so a stats call reads a bounded number of rows however many blocks
-- a campaign has.

CREATE TABLE block_stats (
    unit_id INTEGER PRIMARY KEY,
    campaign_id INTEGER NOT NULL,
    -- The block: 2^bits numbers from l0 + pos*2^min_bits, `slots` slots of 2^min_bits.
    pos INTEGER NOT NULL,
    bits INTEGER NOT NULL,
    slots INTEGER NOT NULL,
    -- When the result that made the unit accepted arrived (Unix seconds).
    submitted_at INTEGER NOT NULL,
    -- result.seconds_total: the verify run's wall time as its worker reports
    -- it (the digest does not bind it).
    seconds REAL NOT NULL,
    -- The result's backend label ("gpu", "cpu", "cpu (fallback)", ...) and its host id.
    backend TEXT NOT NULL,
    host TEXT NOT NULL,
    -- result.candidates_checked, blocks_total (Step-3 blocks walked) and
    -- overflow_fallbacks. A block has at most 2^64 numbers and the sieve keeps
    -- a small fraction as candidates, so these stay far below 2^63.
    candidates INTEGER NOT NULL,
    walked INTEGER NOT NULL,
    overflow INTEGER NOT NULL,
    -- result.max_excursion and max_excursion_n, canonical decimals.
    max_excursion TEXT NOT NULL,
    max_excursion_n TEXT NOT NULL
);
-- The latest blocks, and the blocks of the last hour.
CREATE INDEX block_stats_time ON block_stats (campaign_id, submitted_at);

-- Totals of a campaign's block_stats rows. candidates and walked are sums of
-- u64 counters, kept in two limbs (sum = hi*2^32 + lo, 0 <= lo < 2^32) so that
-- they cannot overflow SQLite's signed 64-bit integers.
CREATE TABLE campaign_stats (
    campaign_id INTEGER PRIMARY KEY,
    blocks INTEGER NOT NULL DEFAULT 0,
    slots INTEGER NOT NULL DEFAULT 0,
    seconds REAL NOT NULL DEFAULT 0,
    candidates_hi INTEGER NOT NULL DEFAULT 0,
    candidates_lo INTEGER NOT NULL DEFAULT 0,
    walked_hi INTEGER NOT NULL DEFAULT 0,
    walked_lo INTEGER NOT NULL DEFAULT 0,
    overflow INTEGER NOT NULL DEFAULT 0,
    -- The earliest block start (submitted_at - seconds) and the latest
    -- submitted_at; a delete leaves them.
    first_at INTEGER,
    last_at INTEGER,
    -- The fastest block (slots per second of seconds_total), and the block
    -- with the largest max_excursion (the smallest n among equal ones).
    best_unit INTEGER,
    best_speed REAL,
    max_unit INTEGER,
    max_excursion TEXT,
    max_excursion_n TEXT
);

-- Sums of the blocks by the hour (span 3600) and the day (span 86400) of
-- submitted_at in UTC; start is a multiple of span.
CREATE TABLE stats_buckets (
    campaign_id INTEGER NOT NULL,
    span INTEGER NOT NULL,
    start INTEGER NOT NULL,
    blocks INTEGER NOT NULL DEFAULT 0,
    slots INTEGER NOT NULL DEFAULT 0,
    seconds REAL NOT NULL DEFAULT 0,
    PRIMARY KEY (campaign_id, span, start)
) WITHOUT ROWID;

-- Sums per machine (host id), and the backend of its latest block.
CREATE TABLE host_stats (
    campaign_id INTEGER NOT NULL,
    host TEXT NOT NULL,
    backend TEXT NOT NULL,
    blocks INTEGER NOT NULL DEFAULT 0,
    slots INTEGER NOT NULL DEFAULT 0,
    seconds REAL NOT NULL DEFAULT 0,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (campaign_id, host)
) WITHOUT ROWID;

-- The latest /api/stats answer per ?campaign= value ('' for none), shared by
-- every Worker isolate: within STATS_CACHE_SECONDS a call reads this one row
-- instead of the summaries.
CREATE TABLE stats_cache (
    key TEXT PRIMARY KEY,
    at INTEGER NOT NULL,
    body TEXT NOT NULL
) WITHOUT ROWID;

-- In an UPDATE (and an upsert's DO UPDATE) every expression reads the row as
-- it was, so the carry of a limb sum uses the old lo. SQLite's >> is an
-- arithmetic shift and & works on two's complement, so the same formulas
-- borrow correctly on a delete.
CREATE TRIGGER block_stats_insert AFTER INSERT ON block_stats
BEGIN
    INSERT INTO campaign_stats (campaign_id, blocks, slots, seconds, candidates_hi, candidates_lo, walked_hi, walked_lo,
                                overflow, first_at, last_at)
    VALUES (NEW.campaign_id, 1, NEW.slots, NEW.seconds, NEW.candidates >> 32, NEW.candidates & 4294967295,
            NEW.walked >> 32, NEW.walked & 4294967295, NEW.overflow, NEW.submitted_at - CAST(NEW.seconds AS INTEGER),
            NEW.submitted_at)
    ON CONFLICT (campaign_id) DO UPDATE SET
        blocks = blocks + 1,
        slots = slots + excluded.slots,
        seconds = seconds + excluded.seconds,
        candidates_hi = candidates_hi + excluded.candidates_hi + ((candidates_lo + excluded.candidates_lo) >> 32),
        candidates_lo = (candidates_lo + excluded.candidates_lo) & 4294967295,
        walked_hi = walked_hi + excluded.walked_hi + ((walked_lo + excluded.walked_lo) >> 32),
        walked_lo = (walked_lo + excluded.walked_lo) & 4294967295,
        overflow = overflow + excluded.overflow,
        first_at = MIN(COALESCE(first_at, excluded.first_at), excluded.first_at),
        last_at = MAX(COALESCE(last_at, excluded.last_at), excluded.last_at);
    -- These two write the row only when the new block beats the stored one.
    UPDATE campaign_stats SET best_unit = NEW.unit_id, best_speed = NEW.slots / NEW.seconds
    WHERE campaign_id = NEW.campaign_id AND NEW.seconds > 0 AND (best_speed IS NULL OR NEW.slots / NEW.seconds > best_speed);
    -- Canonical decimals compare by length, then as text.
    UPDATE campaign_stats SET max_unit = NEW.unit_id, max_excursion = NEW.max_excursion, max_excursion_n = NEW.max_excursion_n
    WHERE campaign_id = NEW.campaign_id
      AND (max_excursion IS NULL
           OR length(NEW.max_excursion) > length(max_excursion)
           OR (length(NEW.max_excursion) = length(max_excursion) AND NEW.max_excursion > max_excursion)
           OR (NEW.max_excursion = max_excursion
               AND (length(NEW.max_excursion_n) < length(max_excursion_n)
                    OR (length(NEW.max_excursion_n) = length(max_excursion_n) AND NEW.max_excursion_n < max_excursion_n))));
    INSERT INTO stats_buckets (campaign_id, span, start, blocks, slots, seconds)
    VALUES (NEW.campaign_id, 3600, NEW.submitted_at - NEW.submitted_at % 3600, 1, NEW.slots, NEW.seconds),
           (NEW.campaign_id, 86400, NEW.submitted_at - NEW.submitted_at % 86400, 1, NEW.slots, NEW.seconds)
    ON CONFLICT (campaign_id, span, start) DO UPDATE SET
        blocks = blocks + 1, slots = slots + excluded.slots, seconds = seconds + excluded.seconds;
    INSERT INTO host_stats (campaign_id, host, backend, blocks, slots, seconds, last_at)
    VALUES (NEW.campaign_id, NEW.host, NEW.backend, 1, NEW.slots, NEW.seconds, NEW.submitted_at)
    ON CONFLICT (campaign_id, host) DO UPDATE SET
        blocks = blocks + 1, slots = slots + excluded.slots, seconds = seconds + excluded.seconds,
        backend = iif(excluded.last_at >= last_at, excluded.backend, backend),
        last_at = MAX(last_at, excluded.last_at);
END;

CREATE TRIGGER block_stats_delete AFTER DELETE ON block_stats
BEGIN
    UPDATE campaign_stats SET
        blocks = blocks - 1,
        slots = slots - OLD.slots,
        seconds = seconds - OLD.seconds,
        candidates_hi = candidates_hi - (OLD.candidates >> 32) + ((candidates_lo - (OLD.candidates & 4294967295)) >> 32),
        candidates_lo = (candidates_lo - (OLD.candidates & 4294967295)) & 4294967295,
        walked_hi = walked_hi - (OLD.walked >> 32) + ((walked_lo - (OLD.walked & 4294967295)) >> 32),
        walked_lo = (walked_lo - (OLD.walked & 4294967295)) & 4294967295,
        overflow = overflow - OLD.overflow
    WHERE campaign_id = OLD.campaign_id;
    -- When the fastest or the largest block leaves, the next one is looked up
    -- among the remaining rows. That scans the campaign's blocks, but blocks
    -- leave the accepted state rarely.
    UPDATE campaign_stats SET (best_unit, best_speed) = (
        SELECT unit_id, slots / seconds FROM block_stats WHERE campaign_id = OLD.campaign_id AND seconds > 0
        ORDER BY slots / seconds DESC, unit_id LIMIT 1)
    WHERE campaign_id = OLD.campaign_id AND best_unit = OLD.unit_id;
    UPDATE campaign_stats SET (max_unit, max_excursion, max_excursion_n) = (
        SELECT unit_id, max_excursion, max_excursion_n FROM block_stats WHERE campaign_id = OLD.campaign_id
        ORDER BY length(max_excursion) DESC, max_excursion DESC, length(max_excursion_n), max_excursion_n LIMIT 1)
    WHERE campaign_id = OLD.campaign_id AND max_unit = OLD.unit_id;
    UPDATE stats_buckets SET blocks = blocks - 1, slots = slots - OLD.slots, seconds = seconds - OLD.seconds
    WHERE campaign_id = OLD.campaign_id AND span = 3600 AND start = OLD.submitted_at - OLD.submitted_at % 3600;
    UPDATE stats_buckets SET blocks = blocks - 1, slots = slots - OLD.slots, seconds = seconds - OLD.seconds
    WHERE campaign_id = OLD.campaign_id AND span = 86400 AND start = OLD.submitted_at - OLD.submitted_at % 86400;
    UPDATE host_stats SET blocks = blocks - 1, slots = slots - OLD.slots, seconds = seconds - OLD.seconds
    WHERE campaign_id = OLD.campaign_id AND host = OLD.host;
END;
