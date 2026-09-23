// Shared by the test helpers that fetch, submit and release (helpers-*.mjs):
// the lease id of each machine's latest lease of each unit, as a worker keeps
// it. The fetch helpers note every lease they get, and the submit and release
// helpers send the id of the sending machine's lease of the unit: as text in
// a submit body (leaseText), as a member of a release body (leaseObject).

const leaseIds = new Map();

/** Notes the lease in fetch answer `res` as `host`'s; returns `res`. */
export function noteLease(host, res) {
    const l = res?.body?.lease;
    if (l) {
        leaseIds.set(`${host}/${l.unit_id}`, l.lease_id);
    }
    return res;
}

/**
 * The lease id `host` holds for the unit, or undefined, so that a submit or
 * release by a machine that never held a lease of the unit sends none, as a
 * worker from before lease ids would.
 */
export const leaseIdOf = (host, unitId) => leaseIds.get(`${host}/${unitId}`);

export const leaseText = (host, unitId) => (leaseIdOf(host, unitId) === undefined ? "" : `,"lease_id":${leaseIdOf(host, unitId)}`);

export const leaseObject = (host, unitId) => (leaseIdOf(host, unitId) === undefined ? {} : { lease_id: leaseIdOf(host, unitId) });
