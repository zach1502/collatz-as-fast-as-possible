# Path records

`path-records.csv` holds the known path records #1 to #97. A starting number
`n` is a path record when the largest value on its orbit under the 3n+1 map
(`n -> 3n+1` for odd `n`, `n -> n/2` for even `n`, down to 1) exceeds that of
every smaller starting number; `peak` is that value. The numbering is
Barina's and Roosendaal's: #1 is n = 2 (Barina's #0, n = 1, is left out) and
#97, the last record below 2^71, is n = 2358909599867980429759.

`server/scripts/build-path-records.mjs` turns the file into
`server/public/path-records.json`, which the status page lists before the
records this server finds (#98 on; see "Path records" in the
[README](../../README.md#path-records)). It refuses rows that are not numbered
1, 2, 3, ..., starting numbers or peaks that do not increase, and a peak that is
not the largest value on the orbit of its `n`. `npm test` in `server/` checks
that the JSON is in step with the CSV.

Columns: `number`, `n`, `peak`, `discoverer` and `source`. `discoverer` is
Roosendaal's "first found/published by", blank where he names no one (#1 to
#44), with names in ASCII (Tomas and Barina, without diacritics). The years of
#83 to #88 come from Oliveira e Silva's news list; Roosendaal dates #88 to
2008. `source` lists the sources that give the record and agree with it,
separated by `;`:

- `barina`: D. Barina, https://pcbarina.fit.vutbr.cz/path-records.htm, #0 to
  #97. Its peaks are those of `T(n) = (3n+1)/2` for odd `n`, half the values
  here from #2 on.
- `A006884`, `A006885`: OEIS, https://oeis.org/A006884 (the starting numbers)
  and https://oeis.org/A006885 (the peaks; its b-file ends at #96). The OEIS
  index is the number here plus 1.
- `ericr`: E. Roosendaal, http://www.ericr.nl/wondrous/pathrecs.html, #1 to
  #97, and the peaks its script computes.
- `tos`: T. Oliveira e Silva, https://sweet.ua.pt/tos/3x_plus_1/t0.txt.gz, #1
  to #88 (T-map peaks); news list at https://sweet.ua.pt/tos/3x_plus_1.html.
- `recomputed`: the peak computed exactly from `n`.

All of them were read on 2026-10-10 and agree on every value they give.
