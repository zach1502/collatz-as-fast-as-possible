// Client addresses as the rate limits count them, the request limits per
// minute, in a Workers binding or in D1, and the ledger page budget of an
// address.

import { Env, intVar } from "./env";
import { Db, HttpError, nowSeconds } from "./request";

/** The 8 groups of an IPv6 address in text form (with "::", a zone or a trailing IPv4 part), or null. */
function ipv6Groups(text: string): number[] | null {
    const s = text.toLowerCase().replace(/%.*$/, "");
    const parse = (part: string, last: boolean): number[] | null => {
        const out: number[] = [];
        const items = part === "" ? [] : part.split(":");
        for (const [i, it] of items.entries()) {
            const dotted = /^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$/.exec(it);
            if (dotted !== null && last && i === items.length - 1) {
                const b = dotted.slice(1).map(Number);
                if (b.some((x) => x > 255)) {
                    return null;
                }
                out.push(((b[0] ?? 0) << 8) | (b[1] ?? 0), ((b[2] ?? 0) << 8) | (b[3] ?? 0));
            } else if (/^[0-9a-f]{1,4}$/.test(it)) {
                out.push(parseInt(it, 16));
            } else {
                return null;
            }
        }
        return out;
    };
    const halves = s.split("::");
    if (halves.length > 2) {
        return null;
    }
    const head = parse(halves[0] ?? "", halves.length === 1);
    const tail = halves.length === 2 ? parse(halves[1] ?? "", true) : [];
    if (head === null || tail === null) {
        return null;
    }
    if (halves.length === 1) {
        return head.length === 8 ? head : null;
    }
    const fill = 8 - head.length - tail.length;
    return fill >= 1 ? [...head, ...new Array<number>(fill).fill(0), ...tail] : null;
}

/**
 * The address a per-address limit counts: an IPv6 address by its /64 prefix
 * ("2001:db8:1:2::/64"), since one subscriber usually gets a whole /64 and
 * could otherwise take a fresh address per request. IPv4 addresses, and
 * IPv4-mapped IPv6 ones (::ffff:a.b.c.d), count as the IPv4 address.
 */
function rateAddress(ip: string): string {
    const g = ip.includes(":") ? ipv6Groups(ip) : null;
    if (g === null) {
        return ip;
    }
    if (g.slice(0, 5).every((x) => x === 0) && g[5] === 0xffff) {
        const [hi = 0, lo = 0] = g.slice(6);
        return `${hi >> 8}.${hi & 255}.${lo >> 8}.${lo & 255}`;
    }
    return `${g
        .slice(0, 4)
        .map((x) => x.toString(16))
        .join(":")}::/64`;
}

/** The client address as the rate limits count it (rateAddress). */
export function clientIp(request: Request, env: Env): string {
    if (env.TEST_MODE === "1") {
        const t = request.headers.get("x-test-client-ip");
        if (t !== null) {
            return rateAddress(t);
        }
    }
    return rateAddress(request.headers.get("cf-connecting-ip") ?? "unknown");
}

const hmacKeys = new Map<string, Promise<CryptoKey>>();

/** Lowercase hex HMAC-SHA256(RL_SECRET, id), truncated to 128 bits. */
async function rateKey(env: Env, kind: string, id: string): Promise<string> {
    const secret = env.RL_SECRET;
    if (secret === undefined || secret.length < 16) {
        throw new HttpError(500, "the server has no RL_SECRET (at least 16 characters); see README.md (\"Running your own server\")");
    }
    let key = hmacKeys.get(secret);
    if (key === undefined) {
        key = crypto.subtle.importKey("raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-256" }, false, [
            "sign",
        ]);
        hmacKeys.set(secret, key);
    }
    const mac = await crypto.subtle.sign("HMAC", await key, new TextEncoder().encode(id));
    const hex = Array.from(new Uint8Array(mac).slice(0, 16), (b) => b.toString(16).padStart(2, "0")).join("");
    return `${kind}:${hex}`;
}

/**
 * Counts one request against `limit` per fixed one-minute window (aligned to
 * the clock) in the D1 counter `key`, and returns false when the window is
 * already full. A refused request only reads the counter, so a client over its
 * limit cannot turn requests into row writes; an allowed one costs one read
 * and one write. Rows older than two hours are deleted on about one allowed
 * request in 200.
 */
async function windowAllows(db: Db, key: string, limit: number): Promise<boolean> {
    const now = nowSeconds();
    const bucket = now - (now % 60);
    const cur = await db.first<{ count: number }>("SELECT count FROM rate WHERE key = ?1 AND bucket = ?2", key, bucket);
    if (cur !== null && cur.count >= limit) {
        return false;
    }
    const row = await db.first<{ count: number }>(
        `INSERT INTO rate (key, bucket, count) VALUES (?1, ?2, 1)
         ON CONFLICT (key) DO UPDATE SET count = CASE WHEN bucket = excluded.bucket THEN count + 1 ELSE 1 END,
                                         bucket = excluded.bucket
         RETURNING count`,
        key,
        bucket,
    );
    if (Math.random() < 0.005) {
        await db.run("DELETE FROM rate WHERE bucket < ?1", now - 7200);
    }
    return (row?.count ?? 0) <= limit;
}

/**
 * Whether rateLimit counts `kind` in the D1 fallback: its binding is not
 * configured, or (TEST_MODE only) X-Test-Rate-Limiter: d1 asks for it.
 */
export function limitsInD1(env: Env, request: Request, kind: "ip" | "user"): boolean {
    const binding = kind === "ip" ? env.RL_IP : env.RL_USER;
    return binding === undefined || (env.TEST_MODE === "1" && request.headers.get("x-test-rate-limiter") === "d1");
}

/**
 * Per-IP or per-machine request limit ("user": the RL_USER binding and
 * RL_USER_PER_MIN, keyed on one of the owner's machines). Uses the
 * rate-limiting binding when it is configured; otherwise a fixed one-minute
 * window in the D1 `rate` table. Either way the key is an HMAC of the IP
 * address or the machine (rateKey).
 */
export async function rateLimit(db: Db, env: Env, request: Request, kind: "ip" | "user", id: string): Promise<void> {
    const key = await rateKey(env, kind, id);
    const binding = kind === "ip" ? env.RL_IP : env.RL_USER;
    if (binding !== undefined && !limitsInD1(env, request, kind)) {
        const { success } = await binding.limit({ key });
        if (!success) {
            throw new HttpError(429, "too many requests; slow down", { retry_after: 60 });
        }
        return;
    }
    const perMin = intVar(kind === "ip" ? env.RL_IP_PER_MIN : env.RL_USER_PER_MIN, kind === "ip" ? 120 : 12);
    if (perMin > 0 && !(await windowAllows(db, key, perMin))) {
        throw new HttpError(429, "too many requests; slow down", { retry_after: 60 });
    }
}

/**
 * What a ledger page read from D1 (up to 1000 rows) costs an address, in
 * requests of the RL_IP binding's limit. They are counted under a key of
 * their own, so at the deployed 120 requests a minute an address reads at
 * most 12 pages a minute from D1 (12,000 rows) on top of its other requests.
 */
export const LEDGER_PAGE_WEIGHT = 10;

/**
 * Counts one ledger page read from D1 against the client's address
 * (LEDGER_PAGE_WEIGHT on the RL_IP binding), and throws 429 once its budget
 * is spent. Pages from a cache do not count. Without the binding nothing is
 * counted: the D1 fallback would cost a row write per page.
 */
export async function ledgerPageLimit(env: Env, request: Request): Promise<void> {
    const binding = env.RL_IP;
    if (binding === undefined || limitsInD1(env, request, "ip")) {
        return;
    }
    const key = await rateKey(env, "ledger", clientIp(request, env));
    for (let i = 0; i < LEDGER_PAGE_WEIGHT; i++) {
        const { success } = await binding.limit({ key });
        if (!success) {
            throw new HttpError(429, "too many ledger pages from this address; slow down", { retry_after: 60 });
        }
    }
}
