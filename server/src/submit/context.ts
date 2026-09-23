// Part of submit.ts: the test-mode hooks, the row of the lease a result is
// sent on, and the plain objects that carry a submit's state from one phase
// of an attempt to the next: the request, the attempt, what its reads found,
// the decision and the batch.

import { JsonValue } from "../json";
import { Db, Env } from "../http";
import { LedgerFields } from "../ledger";
import { Block, Campaign, checkResult } from "../result";
import { Derived, SubmittedRun } from "../units";

/**
 * Test-mode hooks (index.ts, TEST_MODE only). `race` makes the first attempt
 * of a submit lose a guard: 1 = the unit update matches no row, 2 = the run
 * update matches no row after the unit update succeeded, so the batch must
 * roll that write back before the retry, 3 = the ledger insert finds another
 * entry appended first (only when the submit adds one). `afterRead` runs
 * once, between the first attempt's reads and its write, to interleave
 * another request.
 */
export interface TestHooks {
    race?: number;
    afterRead?: () => Promise<void>;
}

export interface MineRow {
    run_id: number;
    rstate: string;
    role: string;
    /** The machine that holds the run's lease now (a renewal can move it to another one). */
    rhost: string;
    expires_at: number;
    rdigest: string | null;
    campaign_id: number;
    pos: number;
    bits: number;
    len: number;
    ustate: string;
    results: number;
    udigest: string | null;
    accepted_at: number | null;
    recheck: string | null;
    recheck_kind: string | null;
    special_at: number | null;
    draw_at: number | null;
    rev: number;
}

/**
 * A submit's request, the same in every attempt: the machine, the unit, the
 * lease the result is sent on (the run id /api/fetch gave as lease_id) and
 * the result. `testRace` is TestHooks.race, 0 without it.
 */
export interface Submission {
    db: Db;
    env: Env;
    hooks: TestHooks;
    testRace: number;
    host: string;
    unitId: number;
    leaseId: number;
    doc: JsonValue;
}

/** One attempt of the retry loop: its index from 0 and its time. */
export interface Attempt {
    attempt: number;
    now: number;
}

/** A result document checked against its unit and campaign (result.ts checkResult). */
type Checked = Awaited<ReturnType<typeof checkResult>>;

/**
 * What the reads of an attempt found: the live lease the result is sent on,
 * with the unit's columns, the campaign and the block, the checked result,
 * the unit's other submitted runs and, for a re-check, the agreed document.
 */
export interface LeaseRead {
    row: MineRow;
    c: Campaign;
    block: Block;
    checked: Checked;
    others: SubmittedRun[];
    agreedOld: string | null;
}

/**
 * What an attempt decides from its reads: the run as it is stored, the
 * unit's runs with it, the re-check state, the derived state, since when the
 * unit waits, and the digest and embargo start the unit keeps.
 */
export interface Decision {
    kind: string | null;
    mineRun: SubmittedRun;
    runs: SubmittedRun[];
    recheck: string | null;
    recheckKind: string | null;
    failedNow: boolean;
    d: Derived;
    wasAccepted: boolean;
    nowAccepted: boolean;
    specialAt: number | null;
    drawAt: number | null;
    digest: string | null;
    acceptedAt: number | null;
}

/** The statements of an attempt's batch, in order, and the ledger entries pushLedger appends after them. */
export interface Batch {
    stmts: D1PreparedStatement[];
    ledger: LedgerFields[];
}
