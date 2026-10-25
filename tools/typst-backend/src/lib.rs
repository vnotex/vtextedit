mod compile;
mod ffi;
mod highlight;
mod math;
mod preview;
mod resources;
mod world;

use serde_json::Value;

// The C boundary must be able to contain unwinding Rust panics. An aborting
// build would silently violate the public ABI's recovery contract.
#[cfg(not(panic = "unwind"))]
compile_error!("vtextedit_typst requires panic=unwind");

pub const MAX_HEADER: usize = 16 * 1024 * 1024;
pub const MAX_BINARY: usize = 128 * 1024 * 1024;
pub const MAX_ID: u64 = (1 << 53) - 1;

pub fn id(value: &Value, field: &str) -> Result<u64, String> {
    value.get(field).and_then(Value::as_u64)
        .filter(|n| (1..=MAX_ID).contains(n))
        .ok_or_else(|| format!("{field} must be an integer in 1..2^53-1"))
}

pub fn index(value: &Value, field: &str) -> Result<usize, String> {
    value.get(field).and_then(Value::as_u64)
        .filter(|n| *n <= MAX_ID)
        .and_then(|n| usize::try_from(n).ok())
        .ok_or_else(|| format!("{field} must be a nonnegative integer"))
}

pub fn text<'a>(value: &'a Value, field: &str) -> Result<&'a str, String> {
    value.get(field).and_then(Value::as_str)
        .ok_or_else(|| format!("{field} must be a string"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn identifiers_remain_exact_above_32_bits() {
        assert_eq!(id(&json!({"snapshot":MAX_ID}), "snapshot").unwrap(), MAX_ID);
        for value in [json!(0), json!(-1), json!(1.5), json!(MAX_ID + 1), json!("1")] {
            assert!(id(&json!({"snapshot":value}), "snapshot").is_err());
        }
    }
}
