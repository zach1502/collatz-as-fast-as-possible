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

CREATE TABLE ledger_head (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    seq INTEGER NOT NULL,
    hash TEXT NOT NULL,
    at INTEGER NOT NULL
);
INSERT OR IGNORE INTO ledger_head (id, seq, hash, at)
VALUES (1, 0, '0000000000000000000000000000000000000000000000000000000000000000', 0);

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
