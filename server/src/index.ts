// Cloudflare Worker for the collatz frontier work-unit server. Static files in
// public/ are served by the assets binding before this code runs; everything
// under /api/ lands here.

import { alerts, decide, requireAdmin, resolveAlert } from "./admin";
import { audit } from "./audit";
import { authenticate } from "./auth";
import { Db, Env, HttpError, OWNER_USER, bodyCap, clientIp, json, limitsInD1, rateLimit, readBody, requireJsonBody } from "./http";
import { JsonObject } from "./json";
import { ledger } from "./ledger";
import { ResultError } from "./result";
import { stats } from "./stats";
import { TestHooks, submit } from "./submit";
import { fetchWork, release } from "./work";

async function route(request: Request, env: Env, db: Db): Promise<Response> {
    const url = new URL(request.url);
    const isLedger = ["/api/ledger", "/api/ledger.csv", "/api/ledger/head"].includes(url.pathname);
    const isRead = request.method === "GET" && (url.pathname === "/api/stats" || isLedger);
    // The per-address limit. The Workers binding counts at no D1 cost, so with
    // it every request counts first. The D1 fallback costs a row write per
    // counted request: there reads are not counted (they are cached), and any
    // other request counts only once its worker token or admin secret checked
    // out, so that a refused request writes nothing.
    const countIp = (): Promise<void> => rateLimit(db, env, request, "ip", clientIp(request, env));
    const ipInD1 = limitsInD1(env, request, "ip");
    if (isRead) {
        if (env.RL_IP !== undefined) {
            await countIp();
        }
        return isLedger ? ledger(db, env, request, url) : stats(db, env, request, url);
    }
    if (!ipInD1) {
        await countIp();
    }
    if (url.pathname.startsWith("/api/admin/")) {
        await requireAdmin(request, env);
        if (ipInD1) {
            await countIp();
        }
        const what = url.pathname.slice("/api/admin/".length);
        if (what === "alerts" && request.method === "GET") {
            return alerts(db, url);
        }
        if (["alerts/resolve", "decide"].includes(what) && request.method === "POST") {
            requireJsonBody(request);
            const body = await readBody(request, bodyCap(env, what));
            return what === "decide" ? decide(db, body) : resolveAlert(db, body);
        }
        throw new HttpError(404, `no endpoint ${request.method} ${url.pathname}`);
    }
    const endpoint = url.pathname.slice("/api/".length);
    if (!["fetch", "submit", "release", "audit"].includes(endpoint)) {
        throw new HttpError(404, `no endpoint ${request.method} ${url.pathname}`);
    }
    if (request.method !== "POST") {
        throw new HttpError(405, `${url.pathname} takes POST`);
    }
    requireJsonBody(request);
    const body = await readBody(request, bodyCap(env, endpoint));
    const host = await authenticate(env, body);
    if (ipInD1) {
        await countIp();
    }
    // Each of the owner's machines has its own request budget.
    await rateLimit(db, env, request, "user", `${OWNER_USER}/${host}`);
    if (endpoint === "fetch") {
        return fetchWork(db, env, body);
    }
    if (endpoint === "submit") {
        return submit(db, env, body, testHooks(request, env, db, body));
    }
    if (endpoint === "audit") {
        return audit(db, body);
    }
    return release(db, body);
}

/**
 * TEST_MODE only: X-Test-Race (TestHooks.race) and X-Test-Submit-Twice: 1,
 * which runs the same submit to the end between the first attempt's reads and
 * its write, as a second copy of the request arriving at that moment would.
 */
function testHooks(request: Request, env: Env, db: Db, body: JsonObject): TestHooks {
    if (env.TEST_MODE !== "1") {
        return {};
    }
    const twice = async (): Promise<void> => {
        await submit(db, env, body);
    };
    return {
        race: Number(request.headers.get("x-test-race") ?? "0"),
        ...(request.headers.get("x-test-submit-twice") === "1" ? { afterRead: twice } : {}),
    };
}

export default {
    async fetch(request: Request, env: Env): Promise<Response> {
        const url = new URL(request.url);
        if (!url.pathname.startsWith("/api/")) {
            return new Response("Not found\n", { status: 404, headers: { "content-type": "text/plain; charset=utf-8" } });
        }
        const db = new Db(env.DB);
        let response: Response;
        try {
            response = await route(request, env, db);
        } catch (e) {
            if (e instanceof HttpError) {
                response = json(e.status, { error: e.message, ...e.extra });
            } else if (e instanceof ResultError) {
                response = json(422, { error: `result rejected: ${e.message}` });
            } else {
                console.error(e);
                response = json(500, { error: "internal error" });
            }
        }
        if (env.TEST_MODE === "1") {
            // Lets the tests measure D1 cost per request.
            response = new Response(response.body, response);
            response.headers.set("x-d1-rows-read", String(db.rowsRead));
            response.headers.set("x-d1-rows-written", String(db.rowsWritten));
        }
        return response;
    },
} satisfies ExportedHandler<Env>;
