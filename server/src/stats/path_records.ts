// The path records /api/stats lists: #97 of Barina's list, then the record
// hits of accepted units that beat it (README.md, "Path records").

import { Db } from "../http";
import { Campaign } from "../result";
import { log2Big } from "./rows";

/**
 * Path record #97 of Barina's list, the last one below 2^71, where that list
 * is complete: its starting number and its peak in T-map units, T(x) =
 * (3x+1)/2 for odd x (verify::kBarinaRecord97N and kBarinaRecord97PeakT; the
 * default record threshold, RECORD97_PEAK_T in scripts/campaign.mjs). The
 * list gives the peak of the 3n+1 map, twice the T-map value.
 */
const RECORD97 = { number: 97, n: 2358909599867980429759n, peakT: 494114680693632162318569565806744468898092n };
const RECORD97_PEAK_T = RECORD97.peakT.toString();
/** Record hits read for the path records; those above #97's peak come a few a year. */
const MAX_PATH_HITS = 1000;

/** A path record as /api/stats gives it. Peaks are values of the 3n+1 map: twice the T-map peak. */
interface PathRecord {
    number: number;
    n: string;
    peak: string;
    peak_log2: number;
    /** #97, from the published list. */
    known: boolean;
    /** When the block holding it was accepted; null for #97. */
    found_at: number | null;
}

/** A records row of an accepted unit as pathHitsStatement reads it; found_at is when the unit was accepted. */
export interface PathHit {
    n: string;
    peak: string;
    found_at: number;
}

function pathRecord(number: number, n: bigint, peakT: bigint, foundAt: number | null): PathRecord {
    const peak = 2n * peakT;
    return { number, n: n.toString(), peak: peak.toString(), peak_log2: log2Big(peak), known: foundAt === null, found_at: foundAt };
}

/**
 * The path records of a campaign with frontier `frontier`: #97, then each hit
 * (a walked number of an accepted unit whose T-map peak passed the record
 * threshold) whose peak exceeds #97's and that of every smaller hit, numbered
 * from 98 while it lies below the frontier. Such a hit at or above the
 * frontier is left out: a block below it that is not accepted yet may hold a
 * higher peak. A hit with a lower peak than a smaller one is never a record,
 * wherever it lies. Below #97 the published list is complete, so a hit there
 * is ignored (it cannot beat #97 in a correct result).
 */
function pathRecords(hits: readonly PathHit[], frontier: bigint): PathRecord[] {
    const list = [pathRecord(RECORD97.number, RECORD97.n, RECORD97.peakT, null)];
    let best = RECORD97.peakT;
    const sorted = hits
        .map((h) => ({ n: BigInt(h.n), peakT: BigInt(h.peak), at: h.found_at }))
        .sort((a, b) => (a.n < b.n ? -1 : a.n > b.n ? 1 : 0));
    for (const h of sorted) {
        if (h.n <= RECORD97.n || h.peakT <= best) {
            continue;
        }
        best = h.peakT;
        if (h.n < frontier) {
            list.push(pathRecord(RECORD97.number + list.length, h.n, h.peakT, h.at));
        }
    }
    return list;
}

/**
 * The hits above #97's peak of campaign `campaignId`'s accepted units (a unit
 * that leaves that state also deletes its hits), smallest n first: canonical
 * decimals compare by length, then as text, so that order is numeric.
 */
export const pathHitsStatement = (db: Db, campaignId: number): D1PreparedStatement =>
    db.prep(
        `SELECT r.n, r.peak, COALESCE(u.accepted_at, r.found_at) AS found_at
         FROM records r JOIN units u ON u.id = r.unit_id
         WHERE r.campaign_id = ?1 AND u.state = 'accepted'
           AND (length(r.peak) > ?2 OR (length(r.peak) = ?2 AND r.peak > ?3))
         ORDER BY length(r.n), r.n LIMIT ?4`,
        campaignId,
        RECORD97_PEAK_T.length,
        RECORD97_PEAK_T,
        MAX_PATH_HITS,
    );

/**
 * The path records of campaign `c`, or null when its units need not list
 * them all. A unit lists every walked number above the record threshold, so
 * with a threshold at most #97's peak no path record of an accepted block is
 * missed (README.md, "Path records"); with none or a higher one some
 * could be.
 */
export function campaignPathRecords(c: Campaign, hits: readonly PathHit[], frontier: bigint): PathRecord[] | null {
    return c.record_threshold !== null && BigInt(c.record_threshold) <= RECORD97.peakT ? pathRecords(hits, frontier) : null;
}
