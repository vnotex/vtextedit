use std::cell::{Cell, UnsafeCell};
use std::ffi::c_void;
use std::io::{self, Write};
use std::marker::PhantomData;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::rc::Rc;
use std::thread::{self, ThreadId};

use serde_json::{Value, json};
use typst::foundations::Bytes;

use crate::{MAX_BINARY, MAX_HEADER, compile, id, resources, text};

const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = 1;
const ERROR: i32 = 2;
const PANIC: i32 = 3;
const POISONED: i32 = 4;
const CALLBACK_FAILED: i32 = 5;
const PANIC_MESSAGE: &str = "Typst backend panicked; destroy and recreate the session";

type ReplyCallback = unsafe extern "C" fn(*mut c_void, *mut VteTypstReply) -> i32;

pub struct VteTypstSession {
    owner: ThreadId,
    active: Cell<bool>,
    poisoned: Cell<bool>,
    // Only borrowed mutably after the owner and active-call checks. Keeping the
    // admission flags separate allows a callback's invalid reentry to be rejected
    // without aliasing the mutable compiler borrow held by the outer invocation.
    inner: UnsafeCell<compile::Session>,
    _thread_affine: PhantomData<Rc<()>>,
    #[cfg(test)]
    panic_next: Cell<bool>,
}

pub struct VteTypstReply {
    header: Vec<u8>,
    payload: Bytes,
}

// A caught panic payload is caller-defined Rust data and may itself panic on
// drop. Contain that destructor too; only a recursively panicking payload leaks.
fn discard_panic(payload: Box<dyn std::any::Any + Send>) {
    if let Err(nested) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
        std::mem::forget(nested);
    }
}

fn boundary<T>(failure: T, call: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(call)) {
        Ok(result) => result,
        Err(payload) => {
            discard_panic(payload);
            failure
        }
    }
}

struct ActiveCall<'a>(&'a Cell<bool>);
impl Drop for ActiveCall<'_> {
    fn drop(&mut self) { self.0.set(false); }
}

#[derive(Clone, Copy)]
struct Identity { epoch: u64, generation: u64, request: u64 }
impl Identity {
    fn parse(request: &Value) -> Result<Self, i32> {
        Ok(Self {
            epoch: id(request, "epoch").map_err(|_| INVALID_ARGUMENT)?,
            generation: id(request, "generation").map_err(|_| INVALID_ARGUMENT)?,
            request: id(request, "request").map_err(|_| INVALID_ARGUMENT)?,
        })
    }
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

struct Emitter {
    callback: ReplyCallback,
    context: *mut c_void,
    identity: Option<Identity>,
    failed: bool,
}
impl Emitter {
    fn new(callback: ReplyCallback, context: *mut c_void) -> Self {
        Self { callback, context, identity: None, failed: false }
    }

    fn send(&mut self, mut header: Value, payload: Bytes) -> Result<(), String> {
        if self.failed { return Err("reply callback failed".into()); }
        if payload.len() > MAX_BINARY { return Err("response payload exceeds 128 MiB".into()); }
        header["v"] = json!(1);
        if let Some(identity) = self.identity {
            header["epoch"] = json!(identity.epoch);
            header["generation"] = json!(identity.generation);
            header["request"] = json!(identity.request);
        }
        let mut bytes = HeaderBuffer(Vec::new());
        serde_json::to_writer(&mut bytes, &header).map_err(|error| error.to_string())?;
        let reply = Box::into_raw(Box::new(VteTypstReply { header: bytes.0, payload }));
        // Ownership transfers at callback entry, even on failure. In particular,
        // never inspect or free `reply` after this call: it may already be freed.
        if unsafe { (self.callback)(self.context, reply) } != 0 {
            self.failed = true;
            return Err("reply callback failed".into());
        }
        Ok(())
    }

    fn error(&mut self, op: &str, message: &str, dependencies: Value) -> i32 {
        match self.send(json!({"op":"error", "for":op, "message":message,
            "dependencies":dependencies}), Bytes::new([])) {
            Ok(()) => OK,
            Err(_) if self.failed => CALLBACK_FAILED,
            Err(_) => ERROR,
        }
    }
}

unsafe fn parse_request(request: *const u8, length: usize, limit: usize) -> Result<Value, i32> {
    // Reject before forming a slice or touching input memory. The ABI never
    // allocates a request-sized intermediate buffer, nor accepts binary input.
    if request.is_null() || length == 0 || length > MAX_HEADER.min(limit) {
        return Err(INVALID_ARGUMENT);
    }
    let bytes = unsafe { std::slice::from_raw_parts(request, length) };
    let source = std::str::from_utf8(bytes).map_err(|_| INVALID_ARGUMENT)?;
    serde_json::from_str(source).map_err(|_| INVALID_ARGUMENT)
}

#[unsafe(no_mangle)]
pub extern "C" fn vte_typst_abi_version() -> u32 { 1 }

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_session_create_v1(out_session: *mut *mut VteTypstSession) -> i32 {
    if out_session.is_null() { return INVALID_ARGUMENT; }
    unsafe { *out_session = ptr::null_mut(); }
    boundary(PANIC, || {
        let session = Box::new(VteTypstSession {
            owner: thread::current().id(), active: Cell::new(false), poisoned: Cell::new(false),
            inner: UnsafeCell::new(compile::Session::default()), _thread_affine: PhantomData,
            #[cfg(test)]
            panic_next: Cell::new(false),
        });
        unsafe { *out_session = Box::into_raw(session); }
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_session_destroy_v1(session: *mut VteTypstSession) -> i32 {
    boundary(PANIC, || {
        if session.is_null() { return OK; }
        let state = unsafe { &*session };
        if state.owner != thread::current().id() || state.active.get() { return INVALID_ARGUMENT; }
        unsafe { drop(Box::from_raw(session)); }
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_session_execute_v1(
    session: *mut VteTypstSession,
    request: *const u8,
    length: usize,
    callback: Option<ReplyCallback>,
    context: *mut c_void,
) -> i32 {
    boundary(PANIC, || {
        let Some(callback) = callback else { return INVALID_ARGUMENT; };
        if session.is_null() { return INVALID_ARGUMENT; }
        let state = unsafe { &*session };
        if state.owner != thread::current().id() || state.active.replace(true) { return INVALID_ARGUMENT; }
        let _active = ActiveCall(&state.active);
        if state.poisoned.get() { return POISONED; }
        let mut emitter = Emitter::new(callback, context);
        let mut decoded = None;
        let result = catch_unwind(AssertUnwindSafe(|| {
            // The sole mutable engine borrow is lane-confined and ends before
            // panic reporting or returning control to the caller.
            let engine = unsafe { &mut *state.inner.get() };
            decoded = Some(unsafe { parse_request(request, length, engine.request_limit()) }?);
            let request = decoded.as_ref().unwrap();
            let op = text(request, "op").map_err(|_| INVALID_ARGUMENT)?;
            emitter.identity = Some(Identity::parse(request)?);
            #[cfg(test)]
            if state.panic_next.replace(false) { panic!("injected session boundary panic"); }
            let result = if request.get("v").and_then(Value::as_u64) != Some(1) {
                Err("unsupported protocol version".into())
            } else {
                engine.handle(request, &mut |header, payload| emitter.send(header, payload))
            };
            Ok(match result {
                Ok(()) => OK,
                Err(_) if emitter.failed => CALLBACK_FAILED,
                Err(message) => emitter.error(op, &message, engine.dependencies()),
            })
        }));
        match result {
            Ok(Ok(status)) | Ok(Err(status)) => status,
            Err(payload) => {
                state.poisoned.set(true);
                discard_panic(payload);
                // Do not inspect the poisoned compiler/world. A diagnostic is
                // best-effort; PANIC remains authoritative if delivery fails.
                boundary((), || {
                    if let Some(request) = decoded.as_ref().filter(|_| emitter.identity.is_some()) {
                        emitter.error(request["op"].as_str().unwrap_or(""), PANIC_MESSAGE, json!([]));
                    }
                });
                PANIC
            }
        }
    })
}

fn scan(request: &Value) -> Result<Value, String> {
    let sources = request["sources"].as_array().unwrap();
    let references = sources.iter().map(|source| resources::image_references(source.as_str().unwrap()))
        .collect::<Result<Vec<_>, _>>()?;
    Ok(json!({"op":"imageReferences", "sources":references}))
}

fn valid_scan_request(request: &Value) -> bool {
    request.as_object().is_some_and(|object| object.len() == 3)
        && request.get("v").and_then(Value::as_u64) == Some(1)
        && request.get("op").and_then(Value::as_str) == Some("scanImageReferences")
        && request.get("sources").and_then(Value::as_array).is_some_and(|sources|
            (1..=3).contains(&sources.len()) && sources.iter().all(Value::is_string))
}

#[cfg(test)]
thread_local! { static PANIC_NEXT_SCAN: Cell<bool> = const { Cell::new(false) }; }

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_scan_image_references_v1(
    request: *const u8,
    length: usize,
    callback: Option<ReplyCallback>,
    context: *mut c_void,
) -> i32 {
    boundary(PANIC, || {
        let Some(callback) = callback else { return INVALID_ARGUMENT; };
        let mut emitter = Emitter::new(callback, context);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let request = unsafe { parse_request(request, length, MAX_HEADER) }?;
            if !valid_scan_request(&request) { return Err(INVALID_ARGUMENT); }
            #[cfg(test)]
            if PANIC_NEXT_SCAN.replace(false) { panic!("injected scanner boundary panic"); }
            Ok(match scan(&request).and_then(|header| emitter.send(header, Bytes::new([]))) {
                Ok(()) => OK,
                Err(_) if emitter.failed => CALLBACK_FAILED,
                Err(message) => {
                    let status = emitter.error("scanImageReferences", &message, json!([]));
                    if status == OK { ERROR } else { status }
                }
            })
        }));
        match result {
            Ok(Ok(status)) | Ok(Err(status)) => status,
            Err(payload) => {
                discard_panic(payload);
                boundary((), || {
                    emitter.error("scanImageReferences", "Typst image-reference scanner panicked", json!([]));
                });
                PANIC
            }
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_reply_header(reply: *const VteTypstReply, length: *mut usize) -> *const u8 {
    if length.is_null() { return ptr::null(); }
    unsafe { *length = 0; }
    boundary(ptr::null(), || {
        if reply.is_null() { return ptr::null(); }
        let header = unsafe { &(*reply).header };
        unsafe { *length = header.len(); }
        header.as_ptr()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_reply_payload(reply: *const VteTypstReply, length: *mut usize) -> *const u8 {
    if length.is_null() { return ptr::null(); }
    unsafe { *length = 0; }
    boundary(ptr::null(), || {
        if reply.is_null() { return ptr::null(); }
        let payload = unsafe { &(*reply).payload };
        let bytes = payload.as_slice();
        unsafe { *length = bytes.len(); }
        if bytes.is_empty() { ptr::null() } else { bytes.as_ptr() }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vte_typst_reply_release(reply: *mut VteTypstReply) -> i32 {
    boundary(PANIC, || {
        if !reply.is_null() { unsafe { drop(Box::from_raw(reply)); } }
        OK
    })
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};

    struct SessionOwner(*mut VteTypstSession);
    impl SessionOwner {
        fn new() -> Self {
            let mut session = ptr::null_mut();
            assert_eq!(unsafe { vte_typst_session_create_v1(&mut session) }, OK);
            assert!(!session.is_null());
            Self(session)
        }
    }
    impl Drop for SessionOwner {
        fn drop(&mut self) { unsafe { vte_typst_session_destroy_v1(self.0); } }
    }

    struct ReplyOwner(*mut VteTypstReply);
    impl ReplyOwner {
        fn header(&self) -> Value {
            let mut length = 0;
            let data = unsafe { vte_typst_reply_header(self.0, &mut length) };
            assert!(!data.is_null());
            serde_json::from_slice(unsafe { std::slice::from_raw_parts(data, length) }).unwrap()
        }
        fn payload(&self) -> &[u8] {
            let mut length = 0;
            let data = unsafe { vte_typst_reply_payload(self.0, &mut length) };
            if length == 0 { return &[]; }
            assert!(!data.is_null());
            unsafe { std::slice::from_raw_parts(data, length) }
        }
    }
    impl Drop for ReplyOwner {
        fn drop(&mut self) { unsafe { vte_typst_reply_release(self.0); } }
    }

    #[derive(Default)]
    struct Collector { replies: Vec<ReplyOwner>, result: i32 }
    unsafe extern "C" fn collect(context: *mut c_void, reply: *mut VteTypstReply) -> i32 {
        let output = unsafe { &mut *context.cast::<Collector>() };
        output.replies.push(ReplyOwner(reply));
        output.result
    }
    fn execute_bytes(session: &SessionOwner, bytes: &[u8], output: &mut Collector) -> i32 {
        unsafe { vte_typst_session_execute_v1(session.0, bytes.as_ptr(), bytes.len(),
            Some(collect), ptr::from_mut(output).cast()) }
    }
    fn execute(session: &SessionOwner, request: &Value, output: &mut Collector) -> i32 {
        execute_bytes(session, &serde_json::to_vec(request).unwrap(), output)
    }
    fn scan_bytes(bytes: &[u8], output: &mut Collector) -> i32 {
        unsafe { vte_typst_scan_image_references_v1(bytes.as_ptr(), bytes.len(),
            Some(collect), ptr::from_mut(output).cast()) }
    }
    fn request(op: &str) -> Value {
        json!({"v":1, "op":op, "epoch":crate::MAX_ID, "generation":crate::MAX_ID, "request":crate::MAX_ID})
    }
    fn config() -> Value {
        json!({"projectRoot":std::env::temp_dir(), "mainFile":"main.typ", "allowDownloads":false})
    }
    fn initialized() -> SessionOwner {
        let session = SessionOwner::new();
        let mut initialize = request("initialize");
        initialize["config"] = config();
        let mut output = Collector::default();
        assert_eq!(execute(&session, &initialize, &mut output), OK);
        assert_eq!(output.replies.len(), 1);
        let ready = output.replies[0].header();
        assert_eq!(ready["op"], "ready");
        for key in ["epoch", "generation", "request"] { assert_eq!(ready[key], crate::MAX_ID); }
        session
    }
    fn compile_request() -> Value {
        let mut compile = request("compile");
        compile["config"] = config();
        compile["snapshots"] = json!([]);
        compile["activeFile"] = json!("main.typ");
        compile["overlays"] = json!([{"file":"main.typ", "text":
            "#set page(width:120pt,height:80pt,margin:4pt)\n= Library\nHello"}]);
        compile
    }
    fn compiled(session: &SessionOwner) -> Value {
        let mut output = Collector::default();
        assert_eq!(execute(session, &compile_request(), &mut output), OK);
        assert_eq!(output.replies.len(), 2);
        assert_eq!(output.replies[0].header()["op"], "parsed");
        let result = output.replies[1].header();
        assert_eq!(result["op"], "compiled");
        assert_eq!(result["pages"], json!([[120.0, 80.0]]));
        assert!(result["snapshot"].as_u64().is_some());
        result
    }

    #[test]
    fn image_reference_scan_returns_ordered_parse_only_metadata() {
        let request = json!({"v":1, "op":"scanImageReferences", "sources":[
            "#if false { image(\"never-read.png\") }",
            "#image(\"literal.png\")\n#include \"never-read.typ\"",
            "",
        ]});
        let mut output = Collector::default();
        assert_eq!(scan_bytes(&serde_json::to_vec(&request).unwrap(), &mut output), OK);
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].header(), json!({"v":1, "op":"imageReferences", "sources":[
            {"images":["never-read.png"], "strings":["never-read.png"], "complete":true},
            {"images":["literal.png"], "strings":["literal.png", "never-read.typ"], "complete":false},
            {"images":[], "strings":[], "complete":true},
        ]}));
        assert!(output.replies[0].payload().is_empty());
    }

    #[test]
    fn image_reference_scan_rejects_invalid_request_shapes_without_a_reply() {
        for request in [
            json!(null), json!([]),
            json!({"op":"scanImageReferences", "sources":[""]}),
            json!({"v":2, "op":"scanImageReferences", "sources":[""]}),
            json!({"v":1.0, "op":"scanImageReferences", "sources":[""]}),
            json!({"v":1, "op":"compile", "sources":[""]}),
            json!({"v":1, "op":"scanImageReferences"}),
            json!({"v":1, "op":"scanImageReferences", "sources":""}),
            json!({"v":1, "op":"scanImageReferences", "sources":[]}),
            json!({"v":1, "op":"scanImageReferences", "sources":["", "", "", ""]}),
            json!({"v":1, "op":"scanImageReferences", "sources":["", 1]}),
            json!({"v":1, "op":"scanImageReferences", "sources":[null]}),
            json!({"v":1, "op":"scanImageReferences", "sources":[""], "extra":true}),
        ] {
            let mut output = Collector::default();
            assert_eq!(scan_bytes(&serde_json::to_vec(&request).unwrap(), &mut output), INVALID_ARGUMENT, "{request}");
            assert!(output.replies.is_empty());
        }
    }

    #[test]
    fn rejects_invalid_spans_json_utf8_and_identities_without_delivery() {
        let session = SessionOwner::new();
        let bytes = serde_json::to_vec(&json!({"v":1,"op":"scanImageReferences","sources":[""]})).unwrap();
        let mut output = Collector::default();
        for length in 0..bytes.len() {
            assert_eq!(scan_bytes(&bytes[..length], &mut output), INVALID_ARGUMENT);
        }
        for invalid in [vec![0xff], b"{}\0".to_vec(), b"{}{}".to_vec(), b"{!}".to_vec()] {
            assert_eq!(scan_bytes(&invalid, &mut output), INVALID_ARGUMENT);
            assert_eq!(execute_bytes(&session, &invalid, &mut output), INVALID_ARGUMENT);
        }
        for field in ["epoch", "generation", "request"] {
            for invalid in [json!(0), json!(-1), json!(1.5), json!(crate::MAX_ID + 1), json!("1")] {
                let mut value = request("initialize");
                value[field] = invalid;
                assert_eq!(execute(&session, &value, &mut output), INVALID_ARGUMENT);
            }
        }
        let context = ptr::from_mut(&mut output).cast();
        for length in [0, 1, MAX_HEADER + 1, usize::MAX] {
            assert_eq!(unsafe { vte_typst_scan_image_references_v1(ptr::null(), length, Some(collect), context) }, INVALID_ARGUMENT);
            assert_eq!(unsafe { vte_typst_session_execute_v1(session.0, ptr::null(), length, Some(collect), context) }, INVALID_ARGUMENT);
        }
        // A one-byte allocation is sufficient: oversized spans must be rejected
        // before the boundary reads or forms a slice of the claimed length.
        assert_eq!(unsafe { vte_typst_scan_image_references_v1(bytes.as_ptr(), MAX_HEADER + 1, Some(collect), context) }, INVALID_ARGUMENT);
        assert_eq!(unsafe { vte_typst_session_execute_v1(session.0, bytes.as_ptr(), MAX_HEADER + 1, Some(collect), context) }, INVALID_ARGUMENT);
        assert_eq!(unsafe { vte_typst_scan_image_references_v1(bytes.as_ptr(), bytes.len(), None, context) }, INVALID_ARGUMENT);
        assert_eq!(unsafe { vte_typst_session_execute_v1(session.0, bytes.as_ptr(), bytes.len(), None, context) }, INVALID_ARGUMENT);
        assert_eq!(unsafe { vte_typst_session_execute_v1(ptr::null_mut(), bytes.as_ptr(), bytes.len(), Some(collect), context) }, INVALID_ARGUMENT);
        assert!(output.replies.is_empty());
        assert_eq!(unsafe { vte_typst_session_create_v1(ptr::null_mut()) }, INVALID_ARGUMENT);
    }

    #[test]
    fn image_reference_scan_bounds_the_entire_batch_before_publication() {
        let source = format!("#image(\"{}\")", "x".repeat(MAX_HEADER / 4));
        let request = json!({"v":1,"op":"scanImageReferences","sources":[source, source]});
        let mut output = Collector::default();
        assert_eq!(scan_bytes(&serde_json::to_vec(&request).unwrap(), &mut output), ERROR);
        assert_eq!(output.replies.len(), 1);
        let error = output.replies[0].header();
        assert_eq!(error["op"], "error");
        assert!(error["message"].as_str().unwrap().contains("response header exceeds"));
    }

    #[test]
    fn semantic_errors_preserve_identity_and_shutdown_is_not_an_operation() {
        let session = initialized();
        for op in ["shutdown", "unknown"] {
            let mut output = Collector::default();
            assert_eq!(execute(&session, &request(op), &mut output), OK);
            assert_eq!(output.replies.len(), 1);
            let error = output.replies[0].header();
            assert_eq!(error["op"], "error");
            assert_eq!(error["for"], op);
            assert_eq!(error["epoch"], crate::MAX_ID);
        }
        let mut unsupported = request("retain");
        unsupported["v"] = json!(2);
        let mut output = Collector::default();
        assert_eq!(execute(&session, &unsupported, &mut output), OK);
        assert_eq!(output.replies[0].header()["message"], "unsupported protocol version");
        compiled(&session);
    }

    #[test]
    fn configured_request_budget_is_enforced_before_parsing() {
        let session = SessionOwner::new();
        let mut initialize = request("initialize");
        initialize["config"] = config();
        initialize["config"]["maxCacheBytes"] = json!(1);
        let mut output = Collector::default();
        assert_eq!(execute(&session, &initialize, &mut output), OK);
        assert_eq!(output.replies[0].header()["op"], "ready");
        output.replies.clear();
        assert_eq!(execute(&session, &request("retain"), &mut output), INVALID_ARGUMENT);
        assert!(output.replies.is_empty());
    }

    #[test]
    fn caught_session_panic_poisoning_and_fresh_session_recovery() {
        let session = initialized();
        unsafe { (*session.0).panic_next.set(true); }
        let mut output = Collector::default();
        assert_eq!(execute(&session, &compile_request(), &mut output), PANIC);
        assert_eq!(output.replies.len(), 1);
        let error = output.replies[0].header();
        assert_eq!(error["op"], "error");
        assert_eq!(error["message"], PANIC_MESSAGE);
        assert_eq!(error["request"], crate::MAX_ID);
        output.replies.clear();
        assert_eq!(execute(&session, &compile_request(), &mut output), POISONED);
        assert!(output.replies.is_empty());
        drop(session);
        compiled(&initialized());
    }

    #[test]
    fn scanner_panic_is_contained_and_does_not_poison_future_scans() {
        let bytes = br#"{"v":1,"op":"scanImageReferences","sources":[""]}"#;
        PANIC_NEXT_SCAN.set(true);
        let mut output = Collector::default();
        assert_eq!(scan_bytes(bytes, &mut output), PANIC);
        assert_eq!(output.replies[0].header()["op"], "error");
        output.replies.clear();
        assert_eq!(scan_bytes(bytes, &mut output), OK);
        assert_eq!(output.replies[0].header()["op"], "imageReferences");
    }

    #[test]
    fn callback_failure_stops_streaming_before_evaluation() {
        let session = initialized();
        let mut output = Collector { result: 1, ..Collector::default() };
        assert_eq!(execute(&session, &compile_request(), &mut output), CALLBACK_FAILED);
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].header()["op"], "parsed");
        drop(output);
        compiled(&session);
    }

    struct TrackedBytes { data: Vec<u8>, drops: Arc<AtomicUsize>, panic_on_drop: bool }
    impl AsRef<[u8]> for TrackedBytes {
        fn as_ref(&self) -> &[u8] { &self.data }
    }
    impl Drop for TrackedBytes {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::SeqCst);
            if self.panic_on_drop { panic!("injected reply destructor panic"); }
        }
    }

    #[test]
    fn failed_callback_owns_payload_without_a_boundary_copy_or_double_release() {
        let drops = Arc::new(AtomicUsize::new(0));
        let payload = Bytes::new(TrackedBytes { data: vec![1, 2, 3], drops: drops.clone(), panic_on_drop: false });
        let data = payload.as_slice().as_ptr();
        let mut output = Collector { result: 1, ..Collector::default() };
        let mut emitter = Emitter::new(collect, ptr::from_mut(&mut output).cast());
        assert!(emitter.send(json!({"op":"pdf"}), payload).is_err());
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].payload().as_ptr(), data);
        assert_eq!(output.replies[0].payload(), &[1, 2, 3]);
        assert_eq!(drops.load(Ordering::SeqCst), 0);
        assert!(emitter.send(json!({"op":"error"}), Bytes::new([])).is_err());
        assert_eq!(output.replies.len(), 1);
        drop(output);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
    }

    unsafe extern "C" fn release_and_fail(context: *mut c_void, reply: *mut VteTypstReply) -> i32 {
        unsafe { *context.cast::<i32>() = vte_typst_reply_release(reply); }
        1
    }
    #[test]
    fn callback_can_release_immediately_before_returning_failure() {
        let drops = Arc::new(AtomicUsize::new(0));
        let payload = Bytes::new(TrackedBytes { data: vec![1], drops: drops.clone(), panic_on_drop: false });
        let mut status = -1;
        let mut emitter = Emitter::new(release_and_fail, ptr::from_mut(&mut status).cast());
        assert!(emitter.send(json!({"op":"pdf"}), payload).is_err());
        assert_eq!(status, OK);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn reply_release_contains_destructor_panics_and_consumes_handle() {
        let drops = Arc::new(AtomicUsize::new(0));
        let reply = Box::into_raw(Box::new(VteTypstReply {
            header: b"{}".to_vec(),
            payload: Bytes::new(TrackedBytes { data: vec![1], drops: drops.clone(), panic_on_drop: true }),
        }));
        assert_eq!(unsafe { vte_typst_reply_release(reply) }, PANIC);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn exported_pdf_reply_survives_session_destruction_and_releases_on_another_thread() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<VteTypstReply>();
        let session = initialized();
        let result = compiled(&session);
        let mut request = request("pdf");
        request["snapshot"] = result["snapshot"].clone();
        let mut output = Collector::default();
        assert_eq!(execute(&session, &request, &mut output), OK);
        let reply = output.replies.pop().unwrap();
        let address = reply.payload().as_ptr() as usize;
        drop(session);
        assert_eq!(reply.payload().as_ptr() as usize, address);
        assert!(reply.payload().starts_with(b"%PDF-"));
        let handle = reply.0 as usize;
        std::mem::forget(reply);
        thread::spawn(move || {
            let reply = ReplyOwner(handle as *mut VteTypstReply);
            assert_eq!(reply.header()["op"], "pdf");
            assert!(reply.payload().starts_with(b"%PDF-"));
            drop(reply);
        }).join().unwrap();
    }

    #[test]
    fn wrong_thread_calls_reject_without_consuming_or_poisoning_session() {
        let session = SessionOwner::new();
        let address = session.0 as usize;
        thread::spawn(move || {
            let session = address as *mut VteTypstSession;
            assert_eq!(unsafe { vte_typst_session_destroy_v1(session) }, INVALID_ARGUMENT);
            assert_eq!(unsafe { vte_typst_session_execute_v1(session, b"{}".as_ptr(), 2, Some(collect), ptr::null_mut()) }, INVALID_ARGUMENT);
        }).join().unwrap();
        let mut output = Collector::default();
        assert_eq!(execute(&session, &request("compile"), &mut output), OK);
        assert_eq!(output.replies[0].header()["op"], "error");
    }

    struct Reentry { session: *mut VteTypstSession, execute: i32, destroy: i32, release: i32 }
    unsafe extern "C" fn reenter(context: *mut c_void, reply: *mut VteTypstReply) -> i32 {
        let state = unsafe { &mut *context.cast::<Reentry>() };
        state.execute = unsafe { vte_typst_session_execute_v1(state.session, b"{}".as_ptr(), 2, Some(collect), ptr::null_mut()) };
        state.destroy = unsafe { vte_typst_session_destroy_v1(state.session) };
        state.release = unsafe { vte_typst_reply_release(reply) };
        0
    }
    #[test]
    fn callback_reentry_rejects_before_borrowing_active_engine() {
        let session = initialized();
        let mut state = Reentry { session: session.0, execute: -1, destroy: -1, release: -1 };
        let mut request = request("retain");
        request["snapshots"] = json!([]);
        let bytes = serde_json::to_vec(&request).unwrap();
        assert_eq!(unsafe { vte_typst_session_execute_v1(session.0, bytes.as_ptr(), bytes.len(), Some(reenter), ptr::from_mut(&mut state).cast()) }, OK);
        assert_eq!(state.execute, INVALID_ARGUMENT);
        assert_eq!(state.destroy, INVALID_ARGUMENT);
        assert_eq!(state.release, OK);
        compiled(&session);
    }

    #[test]
    fn null_releases_and_accessor_failures_are_defined() {
        assert_eq!(vte_typst_abi_version(), 1);
        assert_eq!(unsafe { vte_typst_session_destroy_v1(ptr::null_mut()) }, OK);
        assert_eq!(unsafe { vte_typst_reply_release(ptr::null_mut()) }, OK);
        let mut length = 42;
        assert!(unsafe { vte_typst_reply_header(ptr::null(), &mut length) }.is_null());
        assert_eq!(length, 0);
        length = 42;
        assert!(unsafe { vte_typst_reply_payload(ptr::null(), &mut length) }.is_null());
        assert_eq!(length, 0);
        assert!(unsafe { vte_typst_reply_header(ptr::null(), ptr::null_mut()) }.is_null());
        assert!(unsafe { vte_typst_reply_payload(ptr::null(), ptr::null_mut()) }.is_null());
    }
}
