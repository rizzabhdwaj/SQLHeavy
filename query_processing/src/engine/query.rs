// engine/query.rs — the DataFusion boundary. It exposes C table pages as lazy
// Arrow batches, runs the SQL `query`, and decodes the result back into engine
// `Value`s so callers never see Arrow types.

use std::any::Any;
use std::collections::HashSet;
use std::fmt;
use std::sync::{Arc, Mutex, OnceLock};

use async_trait::async_trait;
use datafusion::arrow::array::{
    ArrayRef, Float64Array, Int32Array, Int64Array, StringArray, UInt16Array,
};
use datafusion::arrow::datatypes::{DataType, Field, Schema};
use datafusion::arrow::record_batch::RecordBatch;
use datafusion::catalog::{Session, TableProvider};
use datafusion::common::{DataFusionError, Result as DataFusionResult, ScalarValue};
use datafusion::logical_expr::{Expr, TableProviderFilterPushDown, TableType};
use datafusion::physical_plan::memory::{LazyBatchGenerator, LazyMemoryExec};
use datafusion::prelude::*;
use parking_lot::RwLock;

use super::db::{Database, scan_page_for};
use super::types::{ColType, QueryResult, Record, TableInfo, Value};

static C_SCAN_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

fn c_scan_lock() -> &'static Mutex<()> {
    C_SCAN_LOCK.get_or_init(|| Mutex::new(()))
}

/// Runs `query` via DataFusion over lazily scanned table batches.
pub(crate) async fn fetch_data(db: &Database, query: &str) -> Result<QueryResult, String> {
    let ctx = SessionContext::new();

    // SQL identifiers are case-insensitive, so two catalogue tables whose names
    // differ only by case would collide. Keep the first and warn on the rest.
    let mut registered = HashSet::new();
    for table in db.list_tables()? {
        if !registered.insert(table.name.to_ascii_lowercase()) {
            eprintln!(
                "warning: table {} skipped — its name collides (case-insensitively) with another table",
                table.name
            );
            continue;
        }
        let provider = CTableProvider::new(db.name(), table);
        let table_name = provider.table_name().to_string();
        ctx.register_table(&table_name, Arc::new(provider))
            .map_err(|e| e.to_string())?;
    }

    let df = ctx.sql(query).await.map_err(|e| e.to_string())?;
    let batches = df.collect().await.map_err(|e| e.to_string())?;
    decode_batches(&batches)
}

#[derive(Debug)]
struct CTableProvider {
    db_name: String,
    table: TableInfo,
    schema: Arc<Schema>,
}

impl CTableProvider {
    fn new(db_name: &str, table: TableInfo) -> Self {
        let schema = Arc::new(arrow_schema(&table));
        Self {
            db_name: db_name.to_string(),
            table,
            schema,
        }
    }

    fn table_name(&self) -> &str {
        &self.table.name
    }
}

#[async_trait]
impl TableProvider for CTableProvider {
    fn schema(&self) -> Arc<Schema> {
        Arc::clone(&self.schema)
    }

    fn table_type(&self) -> TableType {
        TableType::Base
    }

    async fn scan(
        &self,
        _state: &dyn Session,
        projection: Option<&Vec<usize>>,
        _filters: &[Expr],
        limit: Option<usize>,
    ) -> DataFusionResult<Arc<dyn datafusion::physical_plan::ExecutionPlan>> {
        let generator = CScanGenerator {
            db_name: self.db_name.clone(),
            table_name: self.table.name.clone(),
            table: self.table.clone(),
            schema: Arc::clone(&self.schema),
            page_id: 0,
            remaining: limit,
        };
        let mut exec = LazyMemoryExec::try_new(
            Arc::clone(&self.schema),
            vec![Arc::new(RwLock::new(generator))],
        )?;
        exec = exec.with_projection(projection.cloned());
        Ok(Arc::new(exec))
    }

    fn supports_filters_pushdown(
        &self,
        filters: &[&Expr],
    ) -> DataFusionResult<Vec<TableProviderFilterPushDown>> {
        Ok(vec![
            TableProviderFilterPushDown::Unsupported;
            filters.len()
        ])
    }
}

#[derive(Debug)]
struct CScanGenerator {
    db_name: String,
    table_name: String,
    table: TableInfo,
    schema: Arc<Schema>,
    page_id: u32,
    remaining: Option<usize>,
}

impl fmt::Display for CScanGenerator {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CScanGenerator(table={})", self.table_name)
    }
}

impl LazyBatchGenerator for CScanGenerator {
    fn as_any(&self) -> &dyn Any {
        self
    }

    fn generate_next_batch(&mut self) -> DataFusionResult<Option<RecordBatch>> {
        if self.remaining == Some(0) {
            return Ok(None);
        }

        loop {
            let _guard = c_scan_lock()
                .lock()
                .map_err(|_| DataFusionError::Execution("C scan lock poisoned".into()))?;
            let scan = scan_page_for(&self.db_name, &self.table_name, self.page_id)
                .map_err(DataFusionError::Execution)?;
            self.page_id += 1;

            let Some(scan) = scan else {
                return Ok(None);
            };
            if scan.records.is_empty() {
                continue;
            }

            let mut batch = arrow_batch(self.schema.as_ref().clone(), &scan.table, &scan.records)
                .map_err(DataFusionError::Execution)?;
            if let Some(remaining) = self.remaining.as_mut() {
                if batch.num_rows() > *remaining {
                    batch = batch.slice(0, *remaining);
                    *remaining = 0;
                } else {
                    *remaining -= batch.num_rows();
                }
            }
            return Ok(Some(batch));
        }
    }

    fn reset_state(&self) -> Arc<RwLock<dyn LazyBatchGenerator>> {
        Arc::new(RwLock::new(Self {
            db_name: self.db_name.clone(),
            table_name: self.table_name.clone(),
            table: self.table.clone(),
            schema: Arc::clone(&self.schema),
            page_id: 0,
            remaining: self.remaining,
        }))
    }
}

/// Prints a result as an ordered `col | col` table.
pub(crate) fn print_records(result: &QueryResult) {
    if !result.columns.is_empty() {
        println!("{}", result.columns.join(" | "));
    }
    for row in &result.rows {
        let cells: Vec<String> = row.iter().map(Value::to_string).collect();
        println!("{}", cells.join(" | "));
    }
}

fn arrow_type(kind: Option<ColType>) -> DataType {
    match kind {
        Some(ColType::U16) => DataType::UInt16,
        Some(ColType::I32) => DataType::Int32,
        Some(ColType::I64) => DataType::Int64,
        Some(ColType::F64) => DataType::Float64,
        Some(ColType::Str) => DataType::Utf8,
        None => DataType::Null,
    }
}

fn arrow_schema(info: &TableInfo) -> Schema {
    let fields = info
        .columns
        .iter()
        .map(|c| Field::new(&c.name, arrow_type(c.kind()), true))
        .collect::<Vec<_>>();
    Schema::new(fields)
}

fn arrow_batch(
    schema: Schema,
    info: &TableInfo,
    records: &[Record],
) -> Result<RecordBatch, String> {
    let mut arrays: Vec<ArrayRef> = Vec::with_capacity(info.columns.len());
    for (ci, col) in info.columns.iter().enumerate() {
        let array: ArrayRef = match col.kind() {
            Some(ColType::U16) => Arc::new(
                records
                    .iter()
                    .map(|r| match &r.values[ci] {
                        Value::U16(v) => Some(*v),
                        _ => None,
                    })
                    .collect::<UInt16Array>(),
            ),
            Some(ColType::I32) => Arc::new(
                records
                    .iter()
                    .map(|r| match &r.values[ci] {
                        Value::I32(v) => Some(*v),
                        _ => None,
                    })
                    .collect::<Int32Array>(),
            ),
            Some(ColType::I64) => Arc::new(
                records
                    .iter()
                    .map(|r| match &r.values[ci] {
                        Value::I64(v) => Some(*v),
                        _ => None,
                    })
                    .collect::<Int64Array>(),
            ),
            Some(ColType::F64) => Arc::new(
                records
                    .iter()
                    .map(|r| match &r.values[ci] {
                        Value::F64(v) => Some(*v),
                        _ => None,
                    })
                    .collect::<Float64Array>(),
            ),
            Some(ColType::Str) => Arc::new(
                records
                    .iter()
                    .map(|r| match &r.values[ci] {
                        Value::Str(v) => Some(v.as_str()),
                        _ => None,
                    })
                    .collect::<StringArray>(),
            ),
            None => return Err(format!("column {} has an unknown type", col.name)),
        };
        arrays.push(array);
    }
    RecordBatch::try_new(Arc::new(schema), arrays).map_err(|e| e.to_string())
}

fn decode_batches(batches: &[RecordBatch]) -> Result<QueryResult, String> {
    let mut columns: Vec<String> = Vec::new();
    let mut rows: Vec<Vec<Value>> = Vec::new();

    for (b, batch) in batches.iter().enumerate() {
        if b == 0 {
            columns = batch
                .schema()
                .fields()
                .iter()
                .map(|f| f.name().clone())
                .collect();
        }
        for r in 0..batch.num_rows() {
            let mut row = Vec::with_capacity(batch.num_columns());
            for c in 0..batch.num_columns() {
                let scalar = ScalarValue::try_from_array(batch.column(c).as_ref(), r)
                    .map_err(|e| e.to_string())?;
                row.push(scalar_value(scalar));
            }
            rows.push(row);
        }
    }

    Ok(QueryResult { columns, rows })
}

fn scalar_value(s: ScalarValue) -> Value {
    if s.is_null() {
        return Value::Null;
    }
    match s {
        ScalarValue::UInt16(Some(v)) => Value::U16(v),
        ScalarValue::Int32(Some(v)) => Value::I32(v),
        ScalarValue::Int64(Some(v)) => Value::I64(v),
        ScalarValue::Float64(Some(v)) => Value::F64(v),
        ScalarValue::Utf8(Some(v))
        | ScalarValue::Utf8View(Some(v))
        | ScalarValue::LargeUtf8(Some(v)) => Value::Str(v),
        other => Value::Str(other.to_string()),
    }
}
