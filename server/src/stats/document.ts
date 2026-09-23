// The GET /api/stats document of a campaign: what the status page shows and a
// few plain counts.

import { Db, Env, ownerName } from "../http";
import { ledgerSummary } from "../ledger";
import { Campaign, slotNumbers } from "../result";
import { campaignById } from "../units";
import { PathHit, campaignPathRecords, pathHitsStatement } from "./path_records";
import { BLOCK_COLS, BlockRow, BucketRow, DAY, DAYS, HOUR, HOURS, LAST_BLOCKS, MAX_HOSTS, TotalsRow, log2Big, rate } from "./rows";
import { bucketHistory, speedWindows } from "./speed";
import { blockView, milestoneViews, peakView } from "./views";

/** Who had checked every number below the campaign's start, as the page names them. */
const START_CREDIT = { name: "David Barina", url: "https://pcbarina.fit.vutbr.cz/" };

/**
 * The stats JSON of campaign `which` (an id, or "" for the oldest active one,
 * else the newest). Each field is named here, so a field added to a row does
 * not reach the answer. The verifier's parameters, the per-block counters,
 * the records above the threshold (only the path records drawn from them are
 * shown) and the re-check totals stay out; the start is shown, since the page
 * says where checking began.
 */
export async function build(db: Db, env: Env, which: string, now: number): Promise<Record<string, unknown>> {
    const c =
        which !== ""
            ? await campaignById(db, Number(which))
            : ((await db.first<Campaign>("SELECT * FROM campaigns WHERE active = 1 ORDER BY id LIMIT 1")) ??
              (await db.first<Campaign>("SELECT * FROM campaigns ORDER BY id DESC LIMIT 1")));
    const head = { owner_name: ownerName(env), start_credit: START_CREDIT, generated_at: now };
    if (c === null) {
        return { ...head, campaign: null, message: "no campaign has been created yet" };
    }
    const hourStart = now - (now % HOUR);
    const dayStart = now - (now % DAY);
    const firstHour = hourStart - (HOURS - 1) * HOUR;
    const firstDay = dayStart - (DAYS - 1) * DAY;
    // The 24-hour window is the hourly buckets that start inside it plus the
    // blocks of the part-hour before the first of them.
    const dayAgo = now - DAY;
    const fullFrom = dayAgo - (dayAgo % HOUR) + HOUR;
    const [totals, buckets, recent, partial, last, hosts, alerts, ledgerHead, pathHits] = await db.batch([
        db.prep("SELECT * FROM campaign_stats WHERE campaign_id = ?1", c.id),
        // Two ranges of the primary key (campaign_id, span, start): one per span,
        // so a call reads the shown buckets only, however many older ones remain.
        db.prep(
            `SELECT span, start, blocks, slots FROM stats_buckets WHERE campaign_id = ?1 AND span = ?2 AND start >= ?3 AND start <= ?6
             UNION ALL
             SELECT span, start, blocks, slots FROM stats_buckets WHERE campaign_id = ?1 AND span = ?4 AND start >= ?5 AND start <= ?6`,
            c.id,
            HOUR,
            firstHour,
            DAY,
            firstDay,
            now,
        ),
        db.prep("SELECT submitted_at, slots FROM block_stats WHERE campaign_id = ?1 AND submitted_at > ?2 AND submitted_at <= ?3", c.id, now - HOUR, now),
        db.prep("SELECT slots FROM block_stats WHERE campaign_id = ?1 AND submitted_at > ?2 AND submitted_at < ?3", c.id, dayAgo, fullFrom),
        db.prep(
            `SELECT ${BLOCK_COLS} FROM block_stats WHERE campaign_id = ?1 AND submitted_at <= ?2
             ORDER BY submitted_at DESC, unit_id DESC LIMIT ?3`,
            c.id,
            now,
            LAST_BLOCKS,
        ),
        db.prep(
            `SELECT host, backend, blocks, slots, seconds, last_at FROM host_stats
             WHERE campaign_id = ?1 AND blocks > 0 ORDER BY last_at DESC LIMIT ?2`,
            c.id,
            MAX_HOSTS,
        ),
        db.prep("SELECT COUNT(*) AS n FROM alerts WHERE campaign_id = ?1 AND resolved_at IS NULL", c.id),
        db.prep("SELECT seq, hash FROM ledger_head WHERE id = 1"),
        pathHitsStatement(db, c.id),
    ]);
    const t = (totals?.results[0] as TotalsRow | undefined) ?? null;
    const numbersOf = (slots: number): bigint => slotNumbers(c, slots);
    const recentRows = (recent?.results ?? []) as Array<{ submitted_at: number; slots: number }>;
    const partialRows = (partial?.results ?? []) as Array<{ slots: number }>;
    const bucketRows = (buckets?.results ?? []) as BucketRow[];
    const speed = speedWindows(c, t, now, fullFrom, recentRows, partialRows, bucketRows);
    const day = speed["last_24_h"] as { seconds: number; blocks: number; numbers_per_second: number | null };
    const lastRows = (last?.results ?? []) as BlockRow[];
    const frontier = BigInt(c.l0) + slotNumbers(c, c.frontier_pos);
    const peak = peakView(t);
    return {
        ...head,
        campaign: { id: c.id, name: c.name, start: c.l0, start_log2: log2Big(BigInt(c.l0)) },
        frontier: { value: frontier.toString(), log2: log2Big(frontier) },
        totals: { numbers_verified: slotNumbers(c, c.slots_accepted).toString(), blocks: c.units_accepted, last_block_at: t?.last_at ?? null },
        speed: {
            ...speed,
            blocks_per_day: day.seconds > 0 ? (day.blocks * DAY) / day.seconds : null,
        },
        milestones: milestoneViews(frontier, day.numbers_per_second, now),
        history: bucketHistory(c, bucketRows, firstHour, firstDay),
        hosts: ((hosts?.results ?? []) as Array<{ host: string; backend: string; blocks: number; slots: number; seconds: number; last_at: number }>).map(
            (h) => ({
                host: h.host,
                backend: h.backend,
                blocks: h.blocks,
                numbers: numbersOf(h.slots).toString(),
                numbers_per_second: rate(numbersOf(h.slots), h.seconds),
                last_at: h.last_at,
            }),
        ),
        blocks: lastRows.map(blockView),
        max_excursion: peak,
        path_records: campaignPathRecords(c, (pathHits?.results ?? []) as PathHit[], frontier),
        alerts_open: ((alerts?.results ?? [])[0] as { n?: number } | undefined)?.n ?? 0,
        // The machines' random audits of accepted blocks (audit.ts).
        audits: { passed: c.audits_passed, failed: c.audits_failed },
        ledger: ledgerSummary((ledgerHead?.results ?? [])[0] as { seq: number; hash: string } | undefined),
    };
}
