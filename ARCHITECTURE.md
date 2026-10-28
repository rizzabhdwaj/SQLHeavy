# Database Architecture Map

This document describes how the code works today. It is intended to be used as
a reference while drawing an Excalidraw diagram.

The project has two main layers:

```text
Rust query layer
    SQL, DataFusion, Arrow, safe Rust API, FFI calls

C storage engine
    database catalog, table files, pages, records, scans
```

The C engine does not parse or execute SQL. The Rust layer exposes page-sized C
scan results as lazy Arrow batches to DataFusion.

## 1. Repository Map

```text
sql_heavy/
|
|-- Makefile
|      Builds the C shared library, then builds or runs Rust.
|
|-- storage_engine/
|   |-- include/
|   |   |-- page_manager.h   Page, slot, and file layout definitions
|   |   |-- catalog.h        Table schema and catalog definitions
|   |   |-- storage.h        Public C database API
|   |   `-- scan.h           Raw scan result definitions
|   |
|   |-- src/
|   |   |-- page_manager.c   File I/O, pages, slots, record insertion
|   |   |-- catalog.c        Catalog persistence and loading
|   |   |-- storage.c        Database handle and table API
|   |   `-- scan.c           Full table scans
|   |
|   |-- tests/
|   |   |-- test_storage.c          Basic insert and scan test
|   |   `-- test_insert_stress.c    MAX_TABLES/MAX_RECORDS insertion test
|   |
|   `-- libdatabase.so
|       Generated C shared library. It is linked by Rust.
|
|-- query_processing/
|   |-- src/main.rs          Demo program
|   `-- src/engine/
|       |-- mod.rs           High-level Rust API
|       |-- db.rs            Safe wrapper around the C API
|       |-- ffi.rs           Raw C-compatible types and declarations
|       |-- types.rs         Rust values and record encoding/decoding
|       `-- query.rs         Arrow and DataFusion integration
|
`-- data/
    Runtime database files. Ignored by Git.
```

## 2. High-Level Component Diagram

```text
+----------------------+
| Rust application      |
| main.rs               |
+----------+-----------+
           |
           v
+----------------------+
| Rust engine API       |
| open_db               |
| create_table          |
| insert_data           |
| fetch_data            |
+----------+-----------+
           |
           v
+----------------------+
| Rust FFI boundary     |
| ffi.rs                |
| #[repr(C)] structs    |
+----------+-----------+
           | C ABI calls
           v
+----------------------+       +----------------------+
| C storage API         |       | C catalog            |
| storage.c             +------>| catalog.c            |
| Database              |       | TableMetadata        |
+----------+-----------+       +----------+-----------+
           |                              |
           +--------------+---------------+
                          v
              +-------------------------+
              | Page manager            |
              | page_manager.c          |
              | files, pages, slots     |
              +------------+------------+
                           |
                           v
              +-------------------------+
              | Binary files on disk    |
              | catalog.dat             |
              | tables/*.dat            |
              +-------------------------+
```

## 3. Opening a Database

Rust call:

```rust
let db = open_db("TEST_DB")?;
```

Flow:

```text
Rust open_db("TEST_DB")
    |
    v
Database::open()
    |
    v
C open_db("TEST_DB")
    |
    +--> catalogue_create()
    |        Creates the database directory and catalog.dat if needed
    |
    `--> catalogue_load()
             Reads every TableMetadata record into memory
    |
    v
Rust Database handle
    |
    `--> owns a pointer to the C Database
```

The C `Database` currently contains:

```c
    char name[NAME_LEN];
    Catalogue *catalogue;
} Database;
```

The in-memory catalog is metadata only. It does not contain table rows.

## 4. Creating a Table

Rust call:

```rust
create_table(
    &db,
    "USERS",
    &[ColumnSpec::new("id", ColType::I32, 0, 4)],
)?;
```

Flow:

```text
ColumnSpec values
    |
    v
Rust to_metadata()
    |
    v
C new_table()
    |
    +--> Build TableMetadata
    |
    +--> catalogue_put()
    |        Append metadata to catalog.dat
    |
    `--> Add metadata to the in-memory Catalogue
```

A table schema currently contains fixed-size column metadata:

```text
TableMetadata
    table name
    column_count
    columns[]
        column name
        data type
        byte offset inside a record
        byte length
```

Creating a table does not necessarily create the table data file immediately.
The table file is created when the first record is inserted.

## 5. Inserting a Record

Rust call:

```rust
insert_data(&db, "USERS", &[Value::I32(1), Value::Str("Alice".into())])?;
```

Flow:

```text
Rust Value objects
    |
    v
Database::insert()
    |
    +--> Look up table schema
    |
    +--> Calculate record byte length
    |
    +--> Allocate zero-filled byte buffer
    |
    +--> write_value() for each column
    |        Example:
    |        bytes 0..4  = id
    |        bytes 4..20 = name
    |
    v
C db_insert_record()
    |
    +--> find_table()
    |
    +--> set_target(db, table)
    |        Selects data/<db>/tables/<table>.dat
    |
    `--> insert_record()
             |
             +--> Read latest page
             |
             +--> If page is full, allocate a new page
             |
             +--> Copy record bytes into page data area
             |
             +--> Write a Slot { offset, length }
             |
             `--> Write the page back to disk
```

The C record is currently not a C struct on the storage side. It is a byte
array whose fields are interpreted using the table's column offsets and lengths.

## 6. On-Disk File Layout

There is currently no magic number or version marker. Files start with a raw
`FileHeader`.

### 6.1 Database directory

```text
data/
`-- TEST_DB/
    |-- catalog.dat
    `-- tables/
        |-- USERS.dat
        `-- ORDERS.dat
```

`MYDB_DATA_DIR` can replace the default data root.

### 6.2 Every page file

Both `catalog.dat` and table files use this general layout:

```text
offset 0
+-----------------------------+
| FileHeader                  |
| page_count                  |
| page_size                   |
+-----------------------------+
| Page 0                      |
+-----------------------------+
| Page 1                      |
+-----------------------------+
| Page 2                      |
+-----------------------------+
| ...                         |
+-----------------------------+
```

The page location is:

```text
page_offset = sizeof(FileHeader) + page_id * PAGE_SIZE
```

Currently:

```text
FileHeader:  page_count + page_size
Page size:   2400 data bytes + sizeof(PageHeader)
```

The current implementation writes C structs directly to disk. Therefore the
format currently depends on C struct padding, integer representation, and host
endianness.

### 6.3 Slotted page layout

```text
Page start
    |
    v
+-----------------------------+
| PageHeader                  |
| page_id                     |
| num_slots                   |
| free_space_start            |
| free_space_end              |
+-----------------------------+
| Record bytes                |  records grow upward
| Record bytes                |
|                             |
|        free space           |
|                             |
| Slot N                      |  slots grow downward
| Slot 1                      |
| Slot 0                      |
+-----------------------------+
Page end
```

The page header tells the reader where the records and slot directory are:

```c
    PageId page_id;
    uint16_t num_slots;
    uint16_t free_space_start;
    uint16_t free_space_end;
} PageHeader;
```

Each slot describes one record:

```c
    uint16_t offset;
    uint16_t length;
} Slot;
```

Slot `i` is found at:

```text
PAGE_SIZE - (i + 1) * sizeof(Slot)
```

The slot does not contain the record. It points to the record bytes inside the
same page.

Example:

```text
Slot 0: offset=12, length=36
        |
        `--> bytes 12..47 in the page
```

This allows record bytes to move later while keeping the logical slot ID
stable.

## 7. How Records Are Read

The C scan entry point is:

```c
scan_table(db_name, table_name, &batch);
```

Read flow:

```text
table.dat
    |
    v
Read FileHeader
    |
    v
For each page ID
    |
    +--> read Page
    |
    +--> read PageHeader
    |
    +--> for each slot ID
             |
             +--> read Slot { offset, length }
             |
             `--> copy record bytes
```

The scan uses two passes:

```text
Pass 1: count records and total record bytes
Pass 2: allocate one block and copy all RawRecords and bytes into it
```

The result is:

```c
    RawRecord *records;
    uint32_t count;
    TableMetadata meta;
} ScanBatch;
```

`ScanBatch` currently means the complete result of scanning one table. It is
not a streaming batch and it does not limit memory usage.

Each raw record contains:

```c
    const uint8_t *data;
    uint32_t len;
    RecordId id;
} RawRecord;
```

The C caller must release it with:

```c
scan_batch_free(&batch);
```

## 8. Rust FFI and Decoding

The Rust FFI structs must match the C structs in memory layout:

```text
C ScanBatch
    |
    | FFI boundary: #[repr(C)]
    v
Rust ScanBatch
    |
    +--> Read RawRecord.data bytes
    |
    +--> Use TableMetadata column offsets and lengths
    |
    `--> Decode into Rust Value objects
```

Example:

```text
record bytes:
    [ id bytes ][ name bytes ]
       0..4       4..20

metadata:
    id:   I32, offset 0, length 4
    name: Str, offset 4, length 16

decoded:
    [Value::I32(1), Value::Str("Alice")]
```

The unsafe operations are concentrated in `engine/ffi.rs` and `engine/db.rs`.

## 9. SQL Query Flow

Current query execution does not ask the C engine to execute SQL.

```text
SQL string
    |
    v
Rust fetch_data()
    |
    +--> list_tables()
    |
    +--> register a lazy provider for every catalogued C table
    |
    +--> DataFusion requests page-sized batches from each provider
    |
    +--> C reads one page through scan_table_page()
    |
    +--> Decode that page into Rust Values and Arrow arrays
    |
    +--> DataFusion parses and executes SQL
    |
    +--> Decode DataFusion output into QueryResult
    |
    v
QueryResult { columns, rows }
```

Visual form:

```text
Disk
  -> C page scan
  -> bounded RawRecord bytes
  -> Rust Values
  -> Arrow RecordBatch
  -> DataFusion execution
  -> next page
  -> QueryResult
```

The source side now keeps only one page-sized scan batch in memory at a time.
The public `fetch_data()` API still calls DataFusion `collect()` and returns a
fully materialized `QueryResult`, so the final query result can still be large.
Indexes, predicate pushdown, and buffer-pool reads do not exist yet.

## 10. Important Terminology

| Term | Meaning in this project |
|---|---|
| Database | A named directory containing a catalog and table files |
| Database handle | In-memory `Database *` used by C callers |
| Catalog | Metadata describing which tables and columns exist |
| Table metadata | Table name plus column definitions |
| Table file | Binary file containing pages for one table |
| File header | Header containing page count and page size |
| Page | Fixed-size block of a table file |
| Page header | Per-page bookkeeping for slots and free space |
| Record | Raw serialized bytes for one row |
| Slot | Fixed-size pointer describing a record's offset and length |
| Slotted page | Page layout with records at the front and slots at the end |
| Record ID / RID | `{ page_id, slot_id }`, the physical location of a record |
| Scan | Sequentially reading every page and slot in a table |
| RawRecord | C representation of copied record bytes plus its RID |
| ScanBatch | In-memory result for one full scan or one page batch |
| FFI | Foreign Function Interface between Rust and C |
| `#[repr(C)]` | Rust representation that follows C memory layout |
| Arrow array | Typed columnar in-memory data used by DataFusion |
| RecordBatch | Arrow's columnar group of rows; separate from C `ScanBatch` |
| Lazy table provider | DataFusion table that requests page-sized batches on demand |
| DataFusion | Rust SQL parser, planner, optimizer, and executor |

## 11. Current Test Flow

```text
make -C storage_engine test
    |
    +--> Build libdatabase.so
    |
    +--> Build test_storage
    |        temporary DB
    |        create table
    |        insert rows
    |        scan and assert bytes
    |        delete temporary files
    |
    `--> Build test_insert_stress
             temporary DB
             create MAX_TABLES tables
             insert MAX_RECORDS rows per table
             delete temporary files
```

The test database cleanup removes the temporary database files. A catalog-level
`drop_table()` operation is not implemented yet.

## 12. Excalidraw Diagram Plan

Draw these as separate boxes and connect them with arrows.

### Diagram A: Insert path

```text
Application
  -> Rust API
  -> Rust schema/value packing
  -> FFI
  -> C Database API
  -> Catalog lookup
  -> Page manager
  -> Table file
  -> Page
  -> Record bytes + Slot
  -> Disk
```

### Diagram B: Physical storage

```text
data/
  -> database directory
      -> catalog.dat
          -> FileHeader
          -> Pages
              -> TableMetadata records
              -> Slots
      -> tables/USERS.dat
          -> FileHeader
          -> Pages
              -> User record bytes
              -> Slots
```

### Diagram C: Query path

```text
SQL query
  -> Rust fetch_data
  -> list catalog tables
  -> lazy TableProvider
  -> C scan_table_page (one page)
  -> ScanBatch for one page
  -> decode Values
  -> Arrow RecordBatch
  -> DataFusion SQL execution
  -> next page batch
  -> QueryResult
```

### Diagram D: One page

```text
PageHeader | record bytes -> free space <- Slot directory
```

Label the arrows with:

```text
PageHeader.free_space_start
PageHeader.free_space_end
Slot.offset
Slot.length
```

## 13. Current Boundaries and Future Work

Already present:

```text
catalog persistence
table metadata
fixed-size pages
slotted records
record scans
record IDs
C/Rust FFI
DataFusion demo queries
```

Not present yet:

```text
update
delete
catalog-level drop table
buffer pool
indexes
transactions
locking
WAL
crash recovery
streaming scans
explicit portable serialization
magic/version/checksum validation
```

The next correctness milestone is a reliable C storage API with strong error
handling and tests for persistence, page rollover, invalid input, and corrupted
files. Features such as indexes and transactions should come after that.
