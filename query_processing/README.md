# query_processing

The Rust front-end. It adds SQL on top of the C engine: it reads/writes rows
through FFI, exposes page-sized Arrow batches to **Apache DataFusion**, and runs
SQL. The C engine stays SQL-free. DataFusion receives bounded batches instead
of a preloaded copy of the entire table.

## File map

| File | Responsibility |
|------|----------------|
| `src/main.rs` | Demo driver. Only calls the high-level API: `open_db`, `create_table`, `insert_data`, `print_table`, `fetch_data`, `print_records`. No FFI, no Arrow. |
| `src/engine/mod.rs` | The public API and re-exports. This is the only module `main.rs` uses. |
| `src/engine/db.rs` | Safe wrapper over the C database handle (`Database`: open / create_table / insert / scan / list_tables), plus the `Drop` that closes it. |
| `src/engine/ffi.rs` | The **only** module that touches `libdatabase.so`: raw `#[repr(C)]` types and `extern "C"` declarations. |
| `src/engine/types.rs` | SQL-free domain types (`Value`, `ColType`, `ColumnSpec`, `QueryResult`, …) and the byte/layout helpers that encode/decode records from column metadata. |
| `src/engine/query.rs` | The DataFusion boundary: `fetch_data(query)` registers lazy table providers, reads page batches, runs SQL, and decodes results; `print_records`. |
| `build.rs` | Tells Cargo where to find and link the C `.so`. It does **not** build it. |

## Linking (build.rs)

`build.rs` emits:

```
cargo:rustc-link-search=native=../lib
cargo:rustc-link-lib=dylib=database
cargo:rustc-link-arg=-Wl,-rpath,../lib
```

So `lib/libdatabase.so` must exist first. It is built by the top-level Makefile
(`make lib`), which runs `make -C storage_engine` and copies the result.

## Flow

What `main.rs` sees:

```
open_db("TEST_DB")                    -> Database::open     -> open_db           (C)
create_table(&db, "USERS", cols)      -> Database::create_table -> new_table    (C)
insert_data(&db, "USERS", &[Value])   -> Database::insert   -> db_insert_record  (C)
    -> look up the table schema, pack values into a record by column offset/length
fetch_data(&db, "SELECT ...").await   -> query::fetch_data
    -> list_tables (catalogue_load) + register a lazy provider per table
    -> DataFusion requests one page-sized Arrow batch at a time
    -> C scan_table_page() reads only the requested page
    -> ctx.sql(query).collect()
    -> decode results back into QueryResult { columns, rows }
print_records(&result)
```

The raw structs in `engine/ffi.rs` (`ColumnMetadata`, `TableMetadata`,
`Catalogue`, `ScanBatch`) must stay byte-for-byte identical to the C structs in
`../storage_engine/include/`. Rows are read and written generically from the
column metadata (`offset`/`length`), so no per-table Rust struct is needed.

## Build & run

From the repo root:

```sh
make            # builds the C .so (lib) then cargo build
make run        # builds, then cargo run
make clean
```

Or here:

```sh
cargo build
cargo run
```

> Note: `cargo build` alone fails if `../lib/libdatabase.so` is missing — run
> `make lib` first.

See `../storage_engine/README.md` for the storage engine internals.
