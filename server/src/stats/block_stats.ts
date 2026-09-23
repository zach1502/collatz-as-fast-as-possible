// The statements that write and delete a unit's block_stats row; the triggers
// on block_stats keep the summary tables (migrations/0003_stats.sql).

import { Db } from "../http";
import { BlockFigures, Campaign } from "../result";

export const removeBlockStats = (db: Db, unitId: number): D1PreparedStatement =>
    db.prep("DELETE FROM block_stats WHERE unit_id = ?1", unitId);

/**
 * Records unit `u` of campaign `c` as accepted at `at` with the figures of
 * its agreed result, which ran on `host`. An older row of the unit is deleted
 * first (the triggers take it back out of the sums), so the pair also serves a
 * unit that is accepted again.
 */
export function blockStatsStatements(
    db: Db,
    c: Campaign,
    u: { id: number; pos: number; bits: number; len: number },
    at: number,
    host: string,
    f: BlockFigures,
): D1PreparedStatement[] {
    return [
        removeBlockStats(db, u.id),
        // The u64 counters are bound as decimal text; INTEGER affinity stores them exactly.
        db.prep(
            `INSERT INTO block_stats (unit_id, campaign_id, pos, bits, slots, submitted_at, seconds, backend, host,
                                      candidates, walked, overflow, max_excursion, max_excursion_n)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)`,
            u.id,
            c.id,
            u.pos,
            u.bits,
            u.len,
            at,
            f.seconds,
            f.backend,
            host,
            f.candidates,
            f.walked,
            f.overflow,
            f.maxExcursion,
            f.maxExcursionN,
        ),
    ];
}
