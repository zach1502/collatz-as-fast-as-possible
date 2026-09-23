-- Frees D1 space held by data the work-unit server does not need. It runs as
-- one batch, after a backup; `node scripts/prune.mjs` does both:
--
--   npx wrangler d1 export collatz-frontier --remote --output backup-$(date +%F).sql
--   npx wrangler d1 execute collatz-frontier --remote --file scripts/prune.sql
--
-- 2592000 below is the age (30 days, in seconds) after acceptance at which a
-- unit's documents are archived; it can be changed before a run. Archived
-- units keep their state, digest, run digests, records and block statistics;
-- the backup holds the documents.

-- Leases that were handed back or ran out, on units that are accepted.
DELETE FROM runs
WHERE state IN ('released', 'expired')
  AND unit_id IN (SELECT id FROM units WHERE state = 'accepted');

-- Documents of runs that disagreed, on units accepted before the cutoff.
UPDATE runs SET result_json = NULL
WHERE result_json IS NOT NULL
  AND unit_id IN (SELECT id FROM units
                  WHERE state = 'accepted'
                    AND accepted_at < CAST(strftime('%s', 'now') AS INTEGER) - 2592000);

-- The agreed document of units accepted before the cutoff. Deleting whole
-- rows frees whole pages, which D1 reuses for new rows.
DELETE FROM unit_results
WHERE unit_id IN (SELECT id FROM units
                  WHERE state = 'accepted'
                    AND accepted_at < CAST(strftime('%s', 'now') AS INTEGER) - 2592000);

-- Rate-limit counters of past windows (the server also deletes them, on about
-- one counted request in 200).
DELETE FROM rate WHERE bucket < CAST(strftime('%s', 'now') AS INTEGER) - 7200;

-- Hourly statistics buckets older than the cutoff; the page shows the last 48
-- hours of them, and the daily buckets and block_stats stay.
DELETE FROM stats_buckets WHERE span = 3600 AND start < CAST(strftime('%s', 'now') AS INTEGER) - 2592000;
