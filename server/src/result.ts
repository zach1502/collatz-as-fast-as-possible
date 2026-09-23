// Checks a `collatz verify --json` work-unit document against its campaign and
// recomputes result_digest from the fields. digestText() must produce exactly
// the text of checkpoint::result_digest_text (src/core/checkpoint_digest.cpp)
// for the document checkpoint::result_json writes; test/server.test.mjs
// checks it against documents written by the real binary (test/fixtures/).

export { type Block, type Campaign, allowedCommits, blockRange, slotNumbers } from "./result/campaign";
export { type BlockFigures, checkResult, storedFigures } from "./result/check";
export { DIGEST_SCHEMA, fieldDiff, sha256Hex } from "./result/digest";
export { ResultError, storedRecords } from "./result/fields";
