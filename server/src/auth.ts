// Who is calling: every request of fetch, submit, release and audit comes from
// one of the owner's machines and carries the secret WORKER_TOKEN.

import { JsonObject } from "./json";
import { Env, HttpError, checkHost, sameString } from "./http";
import { sha256Hex } from "./result";

/**
 * Checks the body's `token` against WORKER_TOKEN and returns the body's host
 * id. The SHA-256 digests are compared, so the time depends neither on the
 * secret's length nor on where a guess differs. Surrounding whitespace of the
 * secret is ignored, in case `wrangler secret put` kept a trailing newline of
 * the file it was given.
 */
export async function authenticate(env: Env, body: JsonObject): Promise<string> {
    const host = checkHost(body["host"]);
    const secret = env.WORKER_TOKEN?.trim() ?? "";
    if (secret.length < 32) {
        throw new HttpError(503, "the server has no WORKER_TOKEN (at least 32 characters) yet; see README.md (\"Running your own server\")", {
            retry_after: 600,
        });
    }
    const token = body["token"];
    if (typeof token !== "string" || !sameString(await sha256Hex(token), await sha256Hex(secret))) {
        throw new HttpError(401, "this server needs its worker token: collatz worker --token-file PATH, or COLLATZ_WORKER_TOKEN");
    }
    return host;
}
