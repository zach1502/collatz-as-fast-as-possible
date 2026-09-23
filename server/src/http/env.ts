// The Worker's bindings and variables (Env), the settings read from them, and
// randomFraction for the re-check draw.

export interface Env {
    DB: D1Database;
    /** Workers rate-limiting bindings; without them the D1 fallback counts requests. */
    RL_IP?: RateLimit;
    /** Counts the requests of one of the owner's machines. */
    RL_USER?: RateLimit;
    /**
     * Secret (`npx wrangler secret put RL_SECRET`) keying the HMAC that turns an
     * IP address or a machine's host id into a rate-limit key, so no counter,
     * binding key or D1 row can be traced back to an address. Requests fail
     * until it is set.
     */
    RL_SECRET?: string;
    /** Live leases per machine (default 4). */
    MAX_LEASES_PER_USER?: string;
    /** Body cap of every request but /api/submit, in bytes (default 32768). */
    MAX_BODY_BYTES?: string;
    /** Body cap of /api/submit, in bytes (default 262144): a result lists every record of its block. */
    MAX_SUBMIT_BYTES?: string;
    RL_IP_PER_MIN?: string;
    /** Requests per machine per minute in the D1 fallback (default 12). */
    RL_USER_PER_MIN?: string;
    /**
     * Seconds after acceptance within which a unit's re-check must be
     * submitted (default 604800, 7 days); a re-check lease is offered only if
     * it ends by then.
     */
    EMBARGO_SECONDS?: string;
    /**
     * A waiting re-check goes out after this many regular results of the
     * owner's machines since the last re-check lease (default 20), or sooner
     * when it has waited RECHECK_MAX_WAIT_SECONDS or there is no regular work.
     */
    RECHECK_EVERY?: string;
    /** Seconds after which a waiting re-check goes to the next fetch that can take it (default 86400). */
    RECHECK_MAX_WAIT_SECONDS?: string;
    /**
     * The secret every request of the owner's workers carries as `token`
     * (`npx wrangler secret put WORKER_TOKEN`, at least 32 characters). Those
     * requests answer 503 until it is set.
     */
    WORKER_TOKEN?: string;
    /** The name the status page shows for the owner. */
    OWNER_NAME?: string;
    /** Seconds /api/stats is cached per isolate and in the Cache API (default 60, at most 300; 0 = off). */
    STATS_CACHE_SECONDS?: string;
    /** Bearer secret of /api/admin/* (`npx wrangler secret put ADMIN_SECRET`); unset disables it. */
    ADMIN_SECRET?: string;
    /** "1" only in the `test` environment: honours the X-Test-* request headers. */
    TEST_MODE?: string;
}

/** runs.user (and hosts.user, user_stats.user) of every run: the owner's machines share this name. */
export const OWNER_USER = "owner";

/** OWNER_NAME, or null until it is set to 1-64 printable ASCII characters other than the wrangler.jsonc placeholder. */
export function ownerName(env: Env): string | null {
    const name = env.OWNER_NAME?.trim() ?? "";
    return /^[ -~]{1,64}$/.test(name) && !name.startsWith("REPLACE") ? name : null;
}

export const embargoSeconds = (env: Env): number => Math.max(0, intVar(env.EMBARGO_SECONDS, 604800));
export const recheckEvery = (env: Env): number => Math.max(0, Math.floor(intVar(env.RECHECK_EVERY, 20)));
export const recheckMaxWait = (env: Env): number => Math.max(0, intVar(env.RECHECK_MAX_WAIT_SECONDS, 86400));

/** A uniform number in [0, 1) with 53 random bits from crypto.getRandomValues. */
export function randomFraction(): number {
    const x = new Uint32Array(2);
    crypto.getRandomValues(x);
    return (((x[0] ?? 0) >>> 5) * 67108864 + ((x[1] ?? 0) >>> 6)) / 9007199254740992;
}

export function intVar(v: string | undefined, fallback: number): number {
    const n = Number(v);
    return v !== undefined && v !== "" && Number.isFinite(n) ? n : fallback;
}
