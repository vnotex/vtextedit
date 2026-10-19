mod compile;
mod highlight;
mod preview;
mod world;

use std::io::{self, BufReader, BufWriter, Read, Write};
use std::sync::Arc;

use serde_json::{Value, json};

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

fn read_frame(reader: &mut impl Read, limit: usize) -> io::Result<Option<Value>> {
    let mut sizes = [0u8; 8];
    loop {
        match reader.read(&mut sizes[..1]) {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    reader.read_exact(&mut sizes[1..])?;
    let header = u32::from_be_bytes(sizes[..4].try_into().unwrap()) as usize;
    let binary = u32::from_be_bytes(sizes[4..].try_into().unwrap()) as usize;
    // No v1 request operation accepts binary data. Do not allocate or drain an
    // untrusted payload: a framing violation ends the process session.
    if header == 0 || header > MAX_HEADER.min(limit) || binary != 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid frame lengths"));
    }
    let mut bytes = vec![0; header];
    reader.read_exact(&mut bytes)?;
    serde_json::from_slice(&bytes).map(Some).map_err(io::Error::other)
}

struct HeaderBuffer(Vec<u8>);
impl Write for HeaderBuffer {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_HEADER.saturating_sub(self.0.len()) {
            return Err(io::Error::other("response header exceeds 16 MiB"));
        }
        let required = self.0.len() + bytes.len();
        if required > self.0.capacity() {
            let capacity = self.0.capacity().saturating_mul(2).max(required).min(MAX_HEADER);
            self.0.try_reserve_exact(capacity - self.0.len()).map_err(io::Error::other)?;
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> { Ok(()) }
}

fn write_frame(writer: &mut impl Write, header: &Value, binary: &[u8]) -> Result<(), String> {
    if binary.len() > MAX_BINARY { return Err("response payload exceeds 128 MiB".into()); }
    let mut bytes = HeaderBuffer(Vec::new());
    serde_json::to_writer(&mut bytes, header).map_err(|e| e.to_string())?;
    writer.write_all(&(bytes.0.len() as u32).to_be_bytes())
        .and_then(|_| writer.write_all(&(binary.len() as u32).to_be_bytes()))
        .and_then(|_| writer.write_all(&bytes.0))
        .and_then(|_| writer.write_all(binary))
        .and_then(|_| writer.flush())
        .map_err(|e| e.to_string())
}

fn run() -> Result<(), String> {
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut reader = BufReader::new(stdin.lock());
    let mut writer = BufWriter::new(stdout.lock());
    let mut session = compile::Session::default();
    while let Some(request) = read_frame(&mut reader, session.frame_limit()).map_err(|e| e.to_string())? {
        let epoch = id(&request, "epoch")?;
        let generation = id(&request, "generation")?;
        let request_id = id(&request, "request")?;
        let op = text(&request, "op")?;
        let mut send = |mut reply: Value, bytes: Arc<Vec<u8>>| {
            reply["v"] = json!(1);
            reply["epoch"] = json!(epoch);
            reply["generation"] = json!(generation);
            reply["request"] = json!(request_id);
            write_frame(&mut writer, &reply, &bytes)
        };
        let result = if request.get("v").and_then(Value::as_u64) != Some(1) {
            Err("unsupported protocol version".to_string())
        } else {
            session.handle(&request, &mut send)
        };
        match result {
            Ok(true) => break,
            Ok(false) => {},
            Err(message) => {
                send(json!({"op":"error", "for":op, "message":message,
                    "dependencies":session.dependencies()}), Arc::default())?;
            }
        }
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("vtextedit-typst: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_oversize_frames_before_reading_payload() {
        let mut data = Vec::new();
        data.extend_from_slice(&((MAX_HEADER + 1) as u32).to_be_bytes());
        data.extend_from_slice(&0u32.to_be_bytes());
        assert_eq!(read_frame(&mut &data[..], MAX_HEADER).unwrap_err().kind(), io::ErrorKind::InvalidData);
        data[..4].copy_from_slice(&1u32.to_be_bytes());
        data[4..].copy_from_slice(&1u32.to_be_bytes());
        assert!(read_frame(&mut &data[..], MAX_HEADER).is_err());
    }

    #[test]
    fn identifiers_remain_exact_above_32_bits() {
        assert_eq!(id(&json!({"snapshot":MAX_ID}), "snapshot").unwrap(), MAX_ID);
        for value in [json!(0), json!(-1), json!(1.5), json!(MAX_ID + 1), json!("1")] {
            assert!(id(&json!({"snapshot":value}), "snapshot").is_err());
        }
    }
}
