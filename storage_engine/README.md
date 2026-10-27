# storage_engine

The C storage engine. It knows nothing about SQL — it turns logical tables and
records into **pages on disk** and reads them back. The Rust/DataFusion front-end
(`../query_processing`) links this as `libdatabase.so`.

Build the shared library (from the repo root):

```sh
make lib          # -> storage_engine/libdatabase.so, copied to ../lib/
# or, inside this folder:
make
make clean
```

`MYDB_DATA_DIR=/some/path` overrides where the data tree lives (default `../data`).

## File map

| File | Responsibility |
|------|----------------|
| `include/page_manager.h`, `src/page_manager.c` | The **only** layer doing file I/O. File header, page allocation, slotted-page records. |
| `include/catalog.h`, `src/catalog.c` | Catalogue: *what tables exist* (metadata only, never rows). |
| `include/storage.h`, `src/storage.c` | Database API used by clients: open/close, create table, insert. |
| `include/scan.h`, `src/scan.c` | Full table scan (walk all pages/slots). |

## On-disk layout

```
<root>/                       # $MYDB_DATA_DIR or ../data
  <db_name>/
    catalog.dat               # one TableMetadata record per table
    tables/
      <table_name>.dat        # page file, one per table
```

Every `.dat` file is:

```
+-----------------------------+ offset 0
| FileHeader                  | page_count, page_size
+-----------------------------+
| Page 0                      | sizeof(Page) bytes
+-----------------------------+
| Page 1                      |
+-----------------------------+
| ...                         |
```

Page `id` lives at `sizeof(FileHeader) + id * PAGE_SIZE`.

## Page structure

A page is a fixed-size **slotted page**: `PAGE_SIZE = 2400` data bytes +
`sizeof(PageHeader)` = **2412 bytes**.

```
+-------------------------------+ offset 0        <- free_space_start starts here
| PageHeader (12 bytes)         |   page_id, num_slots,
+-------------------------------+   free_space_start, free_space_end
| record 0                      |
| record 1                      |   records grow DOWN from the header
| ...   free_space_start ->     |
|                               |
|          free space           |
|                               |
|     <- free_space_end         |
| slot N                        |
| ...                           |   slot directory grows UP from the tail
| slot 1                        |
| slot 0                        |
+-------------------------------+ offset PAGE_SIZE
```

- A record's bytes start at `free_space_start` and grow upward.
- A `Slot {offset, length}` entry is `PAGE_SIZE - (i+1) * sizeof(Slot)`, growing
  downward, so `slot[i]` points at record `i`.
- A new insert needs `length + sizeof(Slot)` free; `free_space_start + length +
  sizeof(Slot) > free_space_end` means the page is full.
- Records can be moved inside a page without changing their logical id, because
  only the slot offset changes.

## How pages are created

1. `create_page_file()` makes the dirs and writes an **empty** file: just
   `FileHeader { page_count = 0, page_size = PAGE_SIZE }`.
2. `allocate_page()` reads `FileHeader`, uses `page_count` as the new id, writes
   a zeroed page at its offset, increments `page_count`, and rewrites the header.
3. `read_page(id)` seeks to the page offset and reads `sizeof(Page)` bytes.

## Flow of code

Open a database:

```
open_db("TEST_DB")
  -> catalogue_create()     # make data/TEST_DB/catalog.dat (header only)
  -> catalogue_load()       # read all TableMetadata into an in-memory Catalogue
```

Create a table:

```
new_table(db, "USERS", cols, n)
  -> catalogue_put()        # append a TableMetadata record to catalog.dat
  -> cache it in db->catalogue
```

Insert a record:

```
db_insert_record(db, "USERS", data, len)
  -> find_table()           # must exist
  -> set_target()           # g_path = <root>/TEST_DB/tables/USERS.dat
  -> insert_record()        # slotted-page append (new page when full)
```

Scan a table:

```
scan_table(db, "USERS", &batch)
  -> set_target() + target_exists()
  -> pass 1: count records + bytes
  -> pass 2: copy every slot's bytes into one malloc'd buffer
  -> ScanBatch { records, count, meta }   # free with scan_batch_free()
       each RawRecord carries RecordId { page_id, slot_id }, unique per record
```

Layering:

```
Rust/DataFusion / tests
        |
        v
  storage.c  (Database API)          catalog.c (metadata)
        |                                   |
        +----------------+------------------+
                         v
               page_manager.c  (pages, slots, files)
                         |
                       disk
```
