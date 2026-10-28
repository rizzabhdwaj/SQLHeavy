// engine/ffi.rs — the only module that touches storage_engine/libdatabase.so.
// Raw C types and extern "C" declarations live here; no safe API.

use std::os::raw::{c_char, c_int, c_void};

pub(crate) const TYPE_UINT16: c_int = 0;
pub(crate) const TYPE_INT32: c_int = 1;
pub(crate) const TYPE_INT64: c_int = 2;
pub(crate) const TYPE_FLOAT64: c_int = 3;
pub(crate) const TYPE_STRING: c_int = 4;

#[repr(C)]
pub(crate) struct RecordId {
    pub page_id: u32,
    pub slot_id: u16,
}

#[repr(C)]
pub(crate) struct RawRecord {
    pub data: *const u8,
    pub len: u32,
    pub id: RecordId,
}

#[repr(C)]
pub(crate) struct ScanBatch {
    pub records: *mut RawRecord,
    pub count: u32,
    pub meta: TableMetadata,
}

#[repr(C)]
pub(crate) struct ColumnMetadata {
    pub name: [u8; 32],
    pub type_: c_int,
    pub offset: u16,
    pub length: u16,
}

#[repr(C)]
pub(crate) struct TableMetadata {
    pub name: [u8; 64],
    pub column_count: u16,
    pub columns: [ColumnMetadata; 32],
}

#[repr(C)]
pub(crate) struct Catalogue {
    pub table_count: u16,
    pub tables: [TableMetadata; 128],
}

#[repr(C)]
pub(crate) struct RawDatabase {
    _private: [u8; 0],
}

unsafe extern "C" {
    pub(crate) fn open_db(name: *const c_char) -> *mut RawDatabase;
    pub(crate) fn close_db(db: *mut RawDatabase);

    pub(crate) fn new_table(
        db: *mut RawDatabase,
        name: *const c_char,
        columns: *const ColumnMetadata,
        column_count: u16,
    ) -> *mut TableMetadata;

    pub(crate) fn find_table(db: *mut RawDatabase, name: *const c_char) -> *mut TableMetadata;

    pub(crate) fn db_insert_record(
        db: *mut RawDatabase,
        table_name: *const c_char,
        data: *const c_void,
        length: u16,
    ) -> c_int;

    pub(crate) fn scan_table(
        db_name: *const c_char,
        table_name: *const c_char,
        out: *mut ScanBatch,
    ) -> c_int;
    pub(crate) fn scan_table_page(
        db_name: *const c_char,
        table_name: *const c_char,
        page_id: u32,
        out: *mut ScanBatch,
    ) -> c_int;
    pub(crate) fn scan_batch_free(batch: *mut ScanBatch);

    pub(crate) fn catalogue_load(db_name: *const c_char) -> *mut Catalogue;
    pub(crate) fn catalogue_free(catalogue: *mut Catalogue);
}
