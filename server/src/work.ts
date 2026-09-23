// The work-unit protocol: fetch (lease) and release. Submit is in submit.ts.
// A fetch asks for a block size (`bits`) and gets a lease of an aligned block
// of at most that size: a waiting re-check (recheck.ts), an open unit that
// still needs a result, or a new block from the allocator (alloc.ts). Results
// are accepted only on a live lease.
//
// Every lease carries `lease_id`, the id of the run that holds it, and submit
// and release must name it. A result counts only on that lease, while it is
// live and held by the machine that sends it, so a result sent again after
// the unit was started over or after another machine took the lease is not
// taken on the new one. A renewed lease (prefer_unit) keeps its id. Workers
// built before lease ids send none and are refused (400), so before this
// server is deployed every worker is paused (it hands its lease back) and
// then updated; a lease that was not handed back expires after the
// campaign's lease_seconds.

export { fetchWork } from "./work/fetch";
export { release } from "./work/lease";
