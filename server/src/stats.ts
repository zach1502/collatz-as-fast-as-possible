// GET /api/stats (progress, speed and totals of a campaign for the status
// page) and the statements that keep block_stats in step with acceptance.
// Everything comes from the summary tables the block_stats triggers keep
// (migrations/0003_stats.sql), the blocks of the last hour and the record
// hits above path record #97's peak (a few a year, at most MAX_PATH_HITS), so
// a call reads a bounded number of rows. The JSON is cached STATS_CACHE_SECONDS
// (default 60) in the isolate, in the Cache API and in one D1 row shared by
// all isolates. The Cache API does nothing on a workers.dev address; the D1
// row keeps the reads per call at one row there too.

export { blockStatsStatements, removeBlockStats } from "./stats/block_stats";
export { stats } from "./stats/endpoint";
