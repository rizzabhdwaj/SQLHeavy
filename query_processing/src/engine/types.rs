// engine/types.rs — the SQL-free domain types shared across the engine, plus
// the byte/layout helpers that convert between records and `Value`s using the
// column metadata (offset/length) the C catalogue provides.

use std::os::raw::c_int;

use super::ffi::{
    ColumnMetadata, TYPE_FLOAT64, TYPE_INT32, TYPE_INT64, TYPE_STRING, TYPE_UINT16, TableMetadata,
};

/// Column type codes accepted by the C engine.
#[derive(Clone, Copy, Debug)]
pub enum ColType {
    U16,
    I32,
    I64,
    F64,
    Str,
}

impl ColType {
    pub(crate) fn code(self) -> c_int {
        match self {
            ColType::U16 => TYPE_UINT16,
            ColType::I32 => TYPE_INT32,
            ColType::I64 => TYPE_INT64,
            ColType::F64 => TYPE_FLOAT64,
            ColType::Str => TYPE_STRING,
        }
    }

    pub(crate) fn from_code(code: c_int) -> Option<Self> {
        match code {
            TYPE_UINT16 => Some(ColType::U16),
            TYPE_INT32 => Some(ColType::I32),
            TYPE_INT64 => Some(ColType::I64),
            TYPE_FLOAT64 => Some(ColType::F64),
            TYPE_STRING => Some(ColType::Str),
            _ => None,
        }
    }
}

/// A column declared when creating a table.
pub struct ColumnSpec {
    pub name: String,
    pub type_: ColType,
    pub offset: u16,
    pub length: u16,
}

impl ColumnSpec {
    pub fn new(name: &str, type_: ColType, offset: u16, length: u16) -> Self {
        Self {
            name: name.to_string(),
            type_,
            offset,
            length,
        }
    }
}

/// One cell: either read from a record or supplied for an insert.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    U16(u16),
    I32(i32),
    I64(i64),
    F64(f64),
    Str(String),
    Null,
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::U16(v) => write!(f, "{v}"),
            Value::I32(v) => write!(f, "{v}"),
            Value::I64(v) => write!(f, "{v}"),
            Value::F64(v) => write!(f, "{v}"),
            Value::Str(v) => write!(f, "{v}"),
            Value::Null => write!(f, "NULL"),
        }
    }
}

/// A column as stored in the catalogue.
#[derive(Clone, Debug)]
pub struct ColumnInfo {
    pub name: String,
    pub type_: i32,
    pub offset: u16,
    pub length: u16,
}

impl ColumnInfo {
    /// Decoded column type, `None` for a type code the engine does not define.
    pub fn kind(&self) -> Option<ColType> {
        ColType::from_code(self.type_)
    }
}

/// A table's schema.
#[derive(Clone, Debug)]
pub struct TableInfo {
    pub name: String,
    pub columns: Vec<ColumnInfo>,
}

/// Physical location of one record: unique within its table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecordId {
    pub page_id: u32,
    pub slot_id: u16,
}

impl RecordId {
    /// The id packed into one number: `page_id << 16 | slot_id`.
    pub fn as_u64(self) -> u64 {
        ((self.page_id as u64) << 16) | self.slot_id as u64
    }
}

impl std::fmt::Display for RecordId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.as_u64())
    }
}

/// One scanned record: its unique id plus the decoded column values.
pub struct Record {
    pub id: RecordId,
    pub values: Vec<Value>,
}

/// Ordered result of a raw scan: table schema plus every record.
pub struct ScanResult {
    pub table: TableInfo,
    pub records: Vec<Record>,
}

/// Data contents returned by `fetch_data`: column names plus decoded rows.
pub struct QueryResult {
    pub columns: Vec<String>,
    pub rows: Vec<Vec<Value>>,
}

// ---------- byte/layout helpers ----------

pub(crate) fn c_field(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

fn fill<const N: usize>(dst: &mut [u8; N], src: &str) {
    let bytes = src.as_bytes();
    let n = bytes.len().min(N - 1);
    dst[..n].copy_from_slice(&bytes[..n]);
}

pub(crate) fn to_metadata(spec: &ColumnSpec) -> ColumnMetadata {
    let mut col = ColumnMetadata {
        name: [0; 32],
        type_: spec.type_.code(),
        offset: spec.offset,
        length: spec.length,
    };
    fill(&mut col.name, &spec.name);
    col
}

pub(crate) fn table_info(meta: &TableMetadata) -> TableInfo {
    TableInfo {
        name: c_field(&meta.name),
        columns: (0..meta.column_count as usize)
            .map(|j| {
                let c = &meta.columns[j];
                ColumnInfo {
                    name: c_field(&c.name),
                    type_: c.type_,
                    offset: c.offset,
                    length: c.length,
                }
            })
            .collect(),
    }
}

pub(crate) fn read_value(record: &[u8], col: &ColumnInfo) -> Result<Value, String> {
    let start = col.offset as usize;
    // A truncated record leaves the field missing; mirror the C printer, which
    // shows "<invalid field>" rather than failing the whole scan.
    let Some(raw) = record.get(start..start + col.length as usize) else {
        return Ok(Value::Null);
    };

    match (col.kind(), col.length) {
        (Some(ColType::U16), 2) => Ok(Value::U16(u16::from_le_bytes(raw.try_into().unwrap()))),
        (Some(ColType::I32), 4) => Ok(Value::I32(i32::from_le_bytes(raw.try_into().unwrap()))),
        (Some(ColType::I64), 8) => Ok(Value::I64(i64::from_le_bytes(raw.try_into().unwrap()))),
        (Some(ColType::F64), 8) => Ok(Value::F64(f64::from_le_bytes(raw.try_into().unwrap()))),
        (Some(ColType::Str), _) => Ok(Value::Str(c_field(raw))),
        (Some(_), len) => Err(format!(
            "column {} has a type/length mismatch (len={len})",
            col.name
        )),
        (None, _) => Err(format!(
            "column {} has unknown type code {}",
            col.name, col.type_
        )),
    }
}

pub(crate) fn write_value(
    record: &mut [u8],
    col: &ColumnInfo,
    value: &Value,
) -> Result<(), String> {
    let start = col.offset as usize;
    let dst = record
        .get_mut(start..start + col.length as usize)
        .ok_or_else(|| format!("record too short for column {} at offset {start}", col.name))?;

    let mismatch = || {
        format!(
            "value {value} does not fit column {} ({:?} len {})",
            col.name,
            col.kind(),
            col.length
        )
    };

    match (col.kind(), value) {
        (_, Value::Null) => {} // leave the field zeroed
        (Some(ColType::U16), Value::U16(v)) if col.length == 2 => {
            dst.copy_from_slice(&v.to_le_bytes())
        }
        (Some(ColType::I32), Value::I32(v)) if col.length == 4 => {
            dst.copy_from_slice(&v.to_le_bytes())
        }
        (Some(ColType::I64), Value::I64(v)) if col.length == 8 => {
            dst.copy_from_slice(&v.to_le_bytes())
        }
        (Some(ColType::F64), Value::F64(v)) if col.length == 8 => {
            dst.copy_from_slice(&v.to_le_bytes())
        }
        (Some(ColType::Str), Value::Str(v)) => {
            let bytes = v.as_bytes();
            if bytes.len() >= col.length as usize {
                return Err(mismatch());
            }
            dst[..bytes.len()].copy_from_slice(bytes); // rest stays NUL padding
        }
        _ => return Err(mismatch()),
    }
    Ok(())
}
