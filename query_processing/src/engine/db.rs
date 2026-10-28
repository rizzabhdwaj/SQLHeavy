// engine/db.rs — safe wrapper around the C database handle. Every `unsafe`
// call into libdatabase.so is confined here (plus ffi.rs).

use std::ffi::CString;
use std::os::raw::c_void;

use super::ffi::*;
use super::types::{
    ColumnInfo, ColumnSpec, Record, RecordId, ScanResult, TableInfo, Value, c_field, read_value,
    table_info, to_metadata, write_value,
};

/// Open handle; closes the C database when dropped.
pub struct Database {
    ptr: *mut RawDatabase,
    name: String,
}

impl Database {
    pub(crate) fn open(name: &str) -> Result<Self, String> {
        let c_name = CString::new(name).map_err(|_| "database name contains NUL".to_string())?;
        let ptr = unsafe { open_db(c_name.as_ptr()) };
        if ptr.is_null() {
            return Err(format!("open_db({name}) failed"));
        }
        Ok(Self {
            ptr,
            name: name.to_string(),
        })
    }

    /// Creates the table unless it is already catalogued.
    pub(crate) fn create_table(&self, name: &str, columns: &[ColumnSpec]) -> Result<(), String> {
        let c_name = CString::new(name).map_err(|_| "table name contains NUL".to_string())?;
        if !unsafe { find_table(self.ptr, c_name.as_ptr()) }.is_null() {
            return Ok(());
        }
        let raw: Vec<ColumnMetadata> = columns.iter().map(to_metadata).collect();
        let ptr = unsafe { new_table(self.ptr, c_name.as_ptr(), raw.as_ptr(), raw.len() as u16) };
        if ptr.is_null() {
            return Err(format!("new_table({name}) failed"));
        }
        Ok(())
    }

    /// Looks up a table's schema in the C catalogue.
    pub(crate) fn table_schema(&self, table: &str) -> Result<TableInfo, String> {
        let c_table = CString::new(table).map_err(|_| "table name contains NUL".to_string())?;
        let ptr = unsafe { find_table(self.ptr, c_table.as_ptr()) };
        if ptr.is_null() {
            return Err(format!("table {table} is not catalogued"));
        }
        Ok(table_info(unsafe { &*ptr }))
    }

    /// Packs `values` into a record using `table`'s column metadata and appends
    /// it — the write-side counterpart of `scan`.
    pub(crate) fn insert(&self, table: &str, values: &[Value]) -> Result<(), String> {
        let c_table = CString::new(table).map_err(|_| "table name contains NUL".to_string())?;
        let schema = self.table_schema(table)?;
        if values.len() != schema.columns.len() {
            return Err(format!(
                "table {table} expects {} values, got {}",
                schema.columns.len(),
                values.len()
            ));
        }

        let length = schema
            .columns
            .iter()
            .map(|c| c.offset as usize + c.length as usize)
            .max()
            .unwrap_or(0);
        let mut record = vec![0u8; length];
        for (value, col) in values.iter().zip(&schema.columns) {
            write_value(&mut record, col, value)?;
        }

        let rc = unsafe {
            db_insert_record(
                self.ptr,
                c_table.as_ptr(),
                record.as_ptr() as *const c_void,
                record.len() as u16,
            )
        };
        if rc != 0 {
            return Err(format!("db_insert_record into {table} failed: rc={rc}"));
        }
        Ok(())
    }

    /// Scans `table`, taking its schema from the batch metadata the C engine
    /// fills in, and decodes every record in column order.
    pub(crate) fn scan(&self, table: &str) -> Result<ScanResult, String> {
        let c_table = CString::new(table).map_err(|_| "table name contains NUL".to_string())?;
        let c_db = CString::new(self.name.as_str())
            .map_err(|_| "database name contains NUL".to_string())?;
        let mut batch: ScanBatch = unsafe { std::mem::zeroed() };
        let rc = unsafe { scan_table(c_db.as_ptr(), c_table.as_ptr(), &mut batch) };
        if rc != 0 {
            return Err(format!(
                "scan_table({}.{}) failed: rc={rc}",
                self.name, table
            ));
        }

        decode_scan_batch(batch)
    }
    pub(crate) fn list_tables(&self) -> Result<Vec<TableInfo>, String> {
        let c_db = CString::new(self.name.as_str())
            .map_err(|_| "database name contains NUL".to_string())?;
        let cat_ptr = unsafe { catalogue_load(c_db.as_ptr()) };
        if cat_ptr.is_null() {
            return Err("catalogue_load failed".to_string());
        }
        let cat = unsafe { &*cat_ptr };

        let tables = (0..cat.table_count as usize)
            .map(|i| {
                let t = &cat.tables[i];
                TableInfo {
                    name: c_field(&t.name),
                    columns: (0..t.column_count as usize)
                        .map(|j| {
                            let c = &t.columns[j];
                            ColumnInfo {
                                name: c_field(&c.name),
                                type_: c.type_,
                                offset: c.offset,
                                length: c.length,
                            }
                        })
                        .collect(),
                }
            })
            .collect();

        unsafe { catalogue_free(cat_ptr) };
        Ok(tables)
    }
}

fn decode_scan_batch(mut batch: ScanBatch) -> Result<ScanResult, String> {
    let info = table_info(&batch.meta);
    let decoded: Result<Vec<Record>, String> = if batch.records.is_null() || batch.count == 0 {
        Ok(Vec::new())
    } else {
        let records = unsafe { std::slice::from_raw_parts(batch.records, batch.count as usize) };
        records
            .iter()
            .map(|rec| {
                let bytes = unsafe { std::slice::from_raw_parts(rec.data, rec.len as usize) };
                let values = info
                    .columns
                    .iter()
                    .map(|col| read_value(bytes, col))
                    .collect::<Result<Vec<Value>, String>>()?;
                Ok(Record {
                    id: RecordId {
                        page_id: rec.id.page_id,
                        slot_id: rec.id.slot_id,
                    },
                    values,
                })
            })
            .collect()
    };

    unsafe { scan_batch_free(&mut batch) };
    Ok(ScanResult {
        table: info,
        records: decoded?,
    })
}

pub(crate) fn scan_page_for(
    db_name: &str,
    table: &str,
    page_id: u32,
) -> Result<Option<ScanResult>, String> {
    let c_table = CString::new(table).map_err(|_| "table name contains NUL".to_string())?;
    let c_db = CString::new(db_name).map_err(|_| "database name contains NUL".to_string())?;
    let mut batch: ScanBatch = unsafe { std::mem::zeroed() };
    let rc = unsafe { scan_table_page(c_db.as_ptr(), c_table.as_ptr(), page_id, &mut batch) };
    match rc {
        0 => decode_scan_batch(batch).map(Some),
        1 => Ok(None),
        rc => Err(format!(
            "scan_table_page({db_name}.{table}, {page_id}) failed: rc={rc}"
        )),
    }
}

impl Drop for Database {
    fn drop(&mut self) {
        unsafe { close_db(self.ptr) };
    }
}
