# Order-stream CSV format

`obreplay` reads, and `obgen` writes, one request per line with eight
columns:

```csv
action,symbol,id,side,type,price,qty,owner
new,AAPL,1,buy,limit,10050,100,7
new,AAPL,2,sell,market,,40,8
modify,AAPL,1,,,10050,60
cancel,AAPL,1
```

| Column | Used by | Format |
| --- | --- | --- |
| `action` | all | `new`, `cancel` or `modify` |
| `symbol` | all | 1 to 32 characters from `A-Z a-z 0-9 . _ - /` |
| `id` | all | decimal, unsigned 64-bit |
| `side` | `new` | `buy` or `sell` |
| `type` | `new` | `limit`, `market`, `ioc`, `fok` or `post_only` |
| `price` | `new` (not market), `modify` | decimal ticks, signed 64-bit; must be empty for market orders |
| `qty` | `new`, `modify` | decimal lots, signed 64-bit (for `modify`: the new open quantity) |
| `owner` | `new` | decimal, unsigned 32-bit; optional, default 0 (anonymous) |

Rules:

- Columns a request does not use must be empty; trailing empty columns may be
  omitted (`cancel,AAPL,1`).
- Fields may be padded with spaces or tabs. Lines may end with `\n` or
  `\r\n`.
- Blank lines and lines whose first non-blank character is `#` are ignored.
- A header line (first field `action`) is ignored if it comes before the
  first record.
- The parser checks syntax only. Range checks (zero ids or quantities,
  prices beyond `±2^62`) belong to the book, so such lines replay as
  `rejected` events instead of failing the whole file.

Errors name the line and the 1-based column, for example
`orders.csv: line 3, field 6: invalid_number`. `obreplay` parses the whole
file before replaying any of it, and exits with status 1 on the first error.

The parser (`ob::CsvReader`, `ob::parse_csv_record`) never allocates and
never throws; it is fuzzed by `fuzz/fuzz_csv.cpp`, which also checks that
every record survives a write/parse round trip (`ob::append_csv`).
