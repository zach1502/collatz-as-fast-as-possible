-- The public ledger (src/ledger.ts). Each change of whether a block counts
-- adds one entry, in the same batch as the change: 'block' when a unit is
-- accepted (again), 'withdrawn' when an accepted unit leaves the accepted
-- state (a re-check that disagrees, the owner's withdrawal), and
-- 'recheck' when a re-check of an accepted unit agrees with it. Rows are
-- never changed or deleted; the triggers below refuse it.
--
-- seq counts 1, 2, 3, ... without gaps. hash is the lowercase hex SHA-256 of
-- the entry's canonical line (src/ledger_chain.ts ledgerLine), which ends with prev,
-- the hash of entry seq - 1 (64 zeros for entry 1), so a changed entry breaks
-- every hash after it. The block is [start, end), 2^bits numbers, both
-- decimal; result is the result digest; peak and peak_n are the result's
-- max_excursion and its starting number; seconds is its seconds_total as
-- submitted; machine the first 8 hex digits of the host id;
-- backend 'gpu' or 'cpu'; version the binary commit; schema the digest
-- schema. A field the server does not know is '' (a 'withdrawn' entry names
-- only the block and the digest it was accepted with).
--
-- The end column is written with quotes: scripts/apply-remote.mjs would take a
-- bare END for the end of a trigger.
CREATE TABLE ledger (
    seq INTEGER PRIMARY KEY,
    campaign_id INTEGER NOT NULL,
    at INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('block', 'recheck', 'withdrawn')),
    start TEXT NOT NULL,
    bits INTEGER NOT NULL,
    "end" TEXT NOT NULL,
    result TEXT NOT NULL,
    peak TEXT NOT NULL,
    peak_n TEXT NOT NULL,
    seconds TEXT NOT NULL,
    machine TEXT NOT NULL,
    backend TEXT NOT NULL,
    version TEXT NOT NULL,
    schema TEXT NOT NULL,
    prev TEXT NOT NULL,
    hash TEXT NOT NULL
);

-- The latest entry: seq 0 and 64 zeros while there is none. ledger_append
-- moves it in the statement that inserts the entry, so an insert conditioned
-- on the head a request read inserts nothing once another request appended.
CREATE TABLE ledger_head (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    seq INTEGER NOT NULL,
    hash TEXT NOT NULL,
    at INTEGER NOT NULL
);
INSERT OR IGNORE INTO ledger_head (id, seq, hash, at)
VALUES (1, 0, '0000000000000000000000000000000000000000000000000000000000000000', 0);

-- An entry must follow the head: the next seq, linked to the head's hash.
CREATE TRIGGER ledger_follows BEFORE INSERT ON ledger
WHEN NEW.seq IS NOT (SELECT seq FROM ledger_head WHERE id = 1) + 1
  OR NEW.prev IS NOT (SELECT hash FROM ledger_head WHERE id = 1)
BEGIN
    SELECT RAISE(ABORT, 'a ledger entry must follow the head');
END;

CREATE TRIGGER ledger_append AFTER INSERT ON ledger
BEGIN
    UPDATE ledger_head SET seq = NEW.seq, hash = NEW.hash, at = NEW.at WHERE id = 1;
END;

CREATE TRIGGER ledger_no_update BEFORE UPDATE ON ledger
BEGIN
    SELECT RAISE(ABORT, 'ledger entries are never changed');
END;

CREATE TRIGGER ledger_no_delete BEFORE DELETE ON ledger
BEGIN
    SELECT RAISE(ABORT, 'ledger entries are never deleted');
END;

-- The head only moves to the last entry, so the next entry always links to a
-- real one.
CREATE TRIGGER ledger_head_is_last BEFORE UPDATE ON ledger_head
WHEN NEW.seq IS NOT (SELECT MAX(seq) FROM ledger)
  OR NEW.hash IS NOT (SELECT hash FROM ledger WHERE seq = NEW.seq)
BEGIN
    SELECT RAISE(ABORT, 'the ledger head must be its last entry');
END;

CREATE TRIGGER ledger_head_no_delete BEFORE DELETE ON ledger_head
BEGIN
    SELECT RAISE(ABORT, 'the ledger head is never deleted');
END;
