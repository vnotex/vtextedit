use std::any::Any;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{self, File};
use std::io::{self, Read};
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use serde::Deserialize;
use serde_json::{Value, json};
use typst::diag::{FileError, FileResult};
use typst::foundations::{Bytes, Datetime, Duration};
use typst::syntax::{FileId, RootedPath, Source, VirtualPath, VirtualRoot};
use typst::text::{Font, FontBook};
use typst::utils::LazyHash;
use typst::{Library, LibraryExt, World};
use typst_kit::datetime::Time;
use typst_kit::downloader::{Downloader, SystemDownloader};
use typst_kit::packages::SystemPackages;

#[derive(Clone, Debug, Deserialize)]
#[serde(default, rename_all = "camelCase", deny_unknown_fields)]
pub struct Config {
    pub project_root: PathBuf,
    pub main_file: String,
    pub font_paths: Vec<PathBuf>,
    pub package_paths: Vec<PathBuf>,
    pub allow_downloads: bool,
    pub max_pdf_bytes: usize,
    pub max_raster_pixels: usize,
    pub max_cache_bytes: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            project_root: PathBuf::new(), main_file: "main.typ".into(),
            font_paths: vec![], package_paths: vec![], allow_downloads: false,
            max_pdf_bytes: 64 * 1024 * 1024, max_raster_pixels: 16 * 1024 * 1024,
            max_cache_bytes: 256 * 1024 * 1024,
        }
    }
}

impl Config {
    pub fn validate(mut self) -> Result<Self, String> {
        if !self.project_root.is_absolute() { return Err("projectRoot must be absolute".into()); }
        self.project_root = fs::canonicalize(&self.project_root)
            .map_err(|e| format!("cannot resolve projectRoot: {e}"))?;
        if !self.project_root.is_dir() { return Err("projectRoot must be a directory".into()); }
        project_id(&self.main_file)?;
        for path in self.font_paths.iter().chain(&self.package_paths) {
            if !path.is_absolute() { return Err("fontPaths and packagePaths must be absolute".into()); }
        }
        if self.max_pdf_bytes == 0 || self.max_pdf_bytes > crate::MAX_BINARY
            || self.max_raster_pixels == 0 || self.max_raster_pixels > crate::MAX_BINARY / 4
            || self.max_cache_bytes == 0 || self.max_cache_bytes as u64 > crate::MAX_ID
        {
            return Err("resource limits must be positive and fit the protocol ceilings".into());
        }
        Ok(self)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Overlay { pub file: String, pub text: String }

pub fn project_id(path: &str) -> Result<FileId, String> {
    if path.is_empty() || path.starts_with('/') || path.starts_with('@') || path.contains('\\')
        || path.contains(':') || path.split('/').any(|s| s.is_empty() || s == "." || s == "..")
    {
        return Err("source paths must be normalized root-relative slash paths".into());
    }
    let vpath = VirtualPath::new(path).map_err(|e| e.to_string())?;
    Ok(RootedPath::new(VirtualRoot::Project, vpath).intern())
}

pub fn wire_path(id: FileId) -> String {
    match id.root() {
        VirtualRoot::Project => id.vpath().get_without_slash().into(),
        VirtualRoot::Package(package) => format!("{package}/{}", id.vpath().get_without_slash()),
    }
}

pub fn byte_offset(source: &Source, utf16: usize) -> Option<usize> {
    let byte = source.lines().utf16_to_byte(utf16)?;
    // Typst rounds a position between surrogate code units forward. Wire
    // positions must instead reject it, including in navigation requests.
    (source.lines().byte_to_utf16(byte)? == utf16).then_some(byte)
}

pub fn wire_range(world: &dyn World, id: FileId, bytes: Range<usize>) -> Option<Value> {
    let source = world.source(id).ok()?;
    if bytes.start > bytes.end || !is_lf_source(&source) { return None; }
    let start = source.lines().byte_to_utf16(bytes.start)?;
    let end = source.lines().byte_to_utf16(bytes.end)?;
    Some(json!({"file":wire_path(id), "start":start, "end":end}))
}

#[comemo::memoize]
fn is_lf_source(source: &Source) -> bool {
    !source.text().contains('\r')
}

struct Fonts { book: LazyHash<FontBook>, fonts: Vec<Font> }
impl Fonts {
    fn bundled() -> Arc<Self> {
        static BUNDLED: OnceLock<Arc<Fonts>> = OnceLock::new();
        BUNDLED.get_or_init(|| {
            let mut book = FontBook::new();
            let mut fonts = Vec::new();
            for (font, info) in typst_kit::fonts::embedded() { book.push(info); fonts.push(font); }
            Arc::new(Self { book: LazyHash::new(book), fonts })
        }).clone()
    }
}

#[derive(Default)]
struct Slot {
    previous: Option<Source>,
    bytes: Option<FileResult<Bytes>>,
    source: Option<FileResult<Source>>,
}

#[derive(Default)]
struct Reads {
    slots: HashMap<FileId, Slot>,
    dependencies: BTreeMap<PathBuf, String>,
    bytes: usize,
    resource_error: Option<String>,
}

pub struct ProjectWorld {
    pub config: Config,
    library: Arc<LazyHash<Library>>,
    fonts: Arc<Fonts>,
    font_dependencies: BTreeMap<PathBuf, String>,
    main: FileId,
    overlays: HashMap<FileId, Bytes>,
    reads: Mutex<Reads>,
    packages: SystemPackages,
    package_roots: Vec<PathBuf>,
    time: Arc<Time>,
}

impl ProjectWorld {
    pub fn new(config: Config) -> Result<Self, String> {
        let config = config.validate()?;
        let packages = SystemPackages::new(PolicyDownloader::new(config.allow_downloads, config.max_cache_bytes));
        let package_roots = package_roots(&config, &packages);
        let main = project_id(&config.main_file)?;
        let mut world = Self {
            config, library: Arc::new(LazyHash::new(Library::default())),
            fonts: Fonts::bundled(), font_dependencies: BTreeMap::new(), main,
            overlays: HashMap::new(), reads: Mutex::default(), packages, package_roots,
            time: Arc::new(Time::system()),
        };
        world.load_fonts()?;
        world.time.today(None);
        Ok(world)
    }

    pub fn update(&mut self, config: Config, overlays: Vec<Overlay>) -> Result<(), String> {
        let config = config.validate()?;
        let mut next = HashMap::new();
        let mut total = 0usize;
        for overlay in overlays {
            let id = project_id(&overlay.file)?;
            if overlay.text.contains('\r') { return Err("overlays must use LF line endings".into()); }
            total = total.checked_add(overlay.text.len()).ok_or("overlay size overflow")?;
            if total > config.max_cache_bytes { return Err("overlays exceed maxCacheBytes".into()); }
            if next.insert(id, Bytes::from_string(overlay.text)).is_some() {
                return Err("duplicate overlay path".into());
            }
        }
        let same_root = self.config.project_root == config.project_root;
        let reload_fonts = self.config.font_paths != config.font_paths
            || self.font_dependencies.iter().any(|(path, old)| !dependency_matches(path, old));
        self.packages = SystemPackages::new(PolicyDownloader::new(config.allow_downloads, config.max_cache_bytes));
        self.package_roots = package_roots(&config, &self.packages);
        self.main = project_id(&config.main_file)?;
        self.config = config;
        self.overlays = next;
        let reads = self.reads.get_mut().unwrap();
        if !same_root { reads.slots.clear(); }
        reads.slots.retain(|_, slot| slot.bytes.is_some() || slot.source.is_some());
        // Source::replace performs the minimal single incremental Source::edit.
        for slot in reads.slots.values_mut() {
            if let Some(Ok(source)) = slot.source.take() { slot.previous = Some(source); }
            slot.bytes = None;
        }
        reads.dependencies.clear();
        reads.bytes = 0;
        reads.resource_error = None;
        if reload_fonts { self.load_fonts()?; }
        self.reads.get_mut().unwrap().dependencies.extend(self.font_dependencies.clone());
        self.time = Arc::new(Time::system());
        self.time.today(None);
        Ok(())
    }

    fn load_fonts(&mut self) -> Result<(), String> {
        let mut dependencies = BTreeMap::new();
        let mut paths = Vec::new();
        let mut seen = HashSet::new();
        for root in &self.config.font_paths { collect_fonts(root, &mut paths, &mut dependencies, &mut seen)?; }
        paths.sort();
        paths.dedup();
        if paths.is_empty() {
            self.fonts = Fonts::bundled();
        } else {
            let mut book = FontBook::new();
            let mut fonts = Vec::new();
            let mut total = 0usize;
            for path in paths {
                let bytes = read_bounded(&path, self.config.max_cache_bytes.saturating_sub(total))
                    .map_err(|e| format!("font {}: {e}", path.display()))?;
                total += bytes.len();
                let state = content_fingerprint(&path, &bytes);
                dependencies.insert(path.clone(), state.clone());
                if let Ok(real) = fs::canonicalize(&path) { dependencies.insert(real, state); }
                for font in Font::iter(Bytes::new(bytes)) {
                    book.push(font.info().clone()); fonts.push(font);
                }
            }
            // Explicit fonts win; bundled fallback order never depends on the OS.
            for font in &Fonts::bundled().fonts { book.push(font.info().clone()); fonts.push(font.clone()); }
            self.fonts = Arc::new(Fonts { book: LazyHash::new(book), fonts });
        }
        self.font_dependencies = dependencies;
        self.reads.get_mut().unwrap().dependencies.extend(self.font_dependencies.clone());
        Ok(())
    }

    fn resolve(&self, id: FileId, reads: &mut Reads) -> FileResult<PathBuf> {
        let root = match id.root() {
            VirtualRoot::Project => self.config.project_root.clone(),
            VirtualRoot::Package(spec) => {
                let suffix = format!("{}/{}/{}", spec.namespace, spec.name, spec.version);
                let mut found = None;
                for root in &self.package_roots {
                    let path = root.join(&suffix);
                    if path.is_dir() { found = Some((root.clone(), path)); break; }
                    reads.dependencies.entry(path.clone()).or_insert_with(|| fingerprint(&path));
                }
                if found.is_none() && self.config.allow_downloads {
                    self.packages.obtain(spec).map_err(FileError::Package)?;
                    for root in &self.package_roots {
                        let path = root.join(&suffix);
                        if path.is_dir() {
                            // This change was made by this operation, not an external edit.
                            reads.dependencies.remove(&path);
                            found = Some((root.clone(), path)); break;
                        }
                    }
                }
                let (allowed, path) = found.ok_or_else(|| FileError::Other(Some(
                    format!("package {spec} is not installed; configure packagePaths{}",
                        if self.config.allow_downloads { " or a writable Typst package cache" }
                        else { " or enable package downloads" }).into())))?;
                let allowed = fs::canonicalize(&allowed).map_err(|e| FileError::from_io(e, &allowed))?;
                let actual = fs::canonicalize(&path).map_err(|e| FileError::from_io(e, &path))?;
                if !actual.starts_with(allowed) {
                    reads.dependencies.entry(path.clone()).or_insert_with(|| format!("identity:{}", actual.display()));
                    return Err(FileError::AccessDenied);
                }
                reads.dependencies.entry(path.clone()).or_insert_with(|| fingerprint(&path));
                reads.dependencies.entry(actual.clone()).or_insert_with(|| fingerprint(&actual));
                actual
            }
        };
        let path = id.vpath().realize(&root).map_err(FileError::from)?;
        // Check existing ancestors too, so a missing child of an escaping
        // directory symlink cannot be used later to evade root containment.
        for ancestor in path.ancestors() {
            if let Ok(real) = fs::canonicalize(ancestor) {
                if !real.starts_with(&root) {
                    reads.dependencies.entry(ancestor.into()).or_insert_with(|| format!("identity:{}", real.display()));
                    return Err(FileError::AccessDenied);
                }
                break;
            }
        }
        Ok(path)
    }

    fn load(&self, id: FileId, reads: &mut Reads) -> FileResult<Bytes> {
        if let Some(overlay) = self.overlays.get(&id) { return Ok(overlay.clone()); }
        let path = self.resolve(id, reads)?;
        let available = self.config.max_cache_bytes.saturating_sub(reads.bytes).min(crate::MAX_BINARY);
        let result = read_bounded(&path, available);
        match result {
            Ok(data) => {
                let state = content_fingerprint(&path, &data);
                reads.dependencies.entry(path.clone()).or_insert_with(|| state.clone());
                if let Ok(real) = fs::canonicalize(&path) { reads.dependencies.entry(real).or_insert(state); }
                reads.bytes += data.len();
                Ok(Bytes::new(data))
            }
            Err(error) => {
                let state = if error.kind() == io::ErrorKind::NotFound { "missing".into() }
                    else if error.kind() == io::ErrorKind::FileTooLarge {
                        format!("oversize:{}", fs::metadata(&path).map_or(0, |m| m.len()))
                    } else { fingerprint(&path) };
                reads.dependencies.entry(path.clone()).or_insert(state);
                if error.kind() == io::ErrorKind::FileTooLarge {
                    reads.resource_error = Some(format!("read dependency {} exceeds maxCacheBytes", path.display()));
                }
                Err(FileError::from_io(error, &path))
            }
        }
    }

    pub fn dependencies(&self) -> Value {
        json!(self.reads.lock().unwrap().dependencies.iter().map(|(path, fingerprint)| {
            json!({"path":path, "fingerprint":fingerprint})
        }).collect::<Vec<_>>())
    }

    pub fn resource_error(&self) -> Option<String> {
        self.reads.lock().unwrap().resource_error.clone()
    }

    pub fn check_dependencies(&self) -> Result<(), String> {
        let reads = self.reads.lock().unwrap();
        if let Some(error) = &reads.resource_error { return Err(error.clone()); }
        for (path, frozen) in &reads.dependencies {
            if !dependency_matches(path, frozen) {
                return Err(format!("read dependency changed during compilation: {}", path.display()));
            }
        }
        Ok(())
    }

    pub fn freeze(&self) -> FrozenWorld {
        let reads = self.reads.lock().unwrap();
        FrozenWorld {
            library: self.library.clone(), fonts: self.fonts.clone(), main: self.main,
            sources: reads.slots.iter().filter_map(|(id, slot)| slot.source.clone().map(|v| (*id, v))).collect(),
            files: reads.slots.iter().filter_map(|(id, slot)| slot.bytes.clone().map(|v| (*id, v))).collect(),
            time: self.time.clone(),
        }
    }
}

impl World for ProjectWorld {
    fn library(&self) -> &LazyHash<Library> { &self.library }
    fn book(&self) -> &LazyHash<FontBook> { &self.fonts.book }
    fn main(&self) -> FileId { self.main }
    fn source(&self, id: FileId) -> FileResult<Source> {
        let mut reads = self.reads.lock().unwrap();
        if let Some(result) = reads.slots.get(&id).and_then(|s| s.source.clone()) { return result; }
        let bytes = match reads.slots.get_mut(&id).and_then(|s| s.bytes.take()) {
            Some(bytes) => bytes,
            None => self.load(id, &mut reads),
        };
        let slot = reads.slots.entry(id).or_default();
        let result = match bytes {
            Err(error) => { slot.bytes = Some(Err(error.clone())); Err(error) }
            Ok(bytes) if !bytes.starts_with(b"\xef\xbb\xbf") && !bytes.contains(&b'\r') => {
                match bytes.into_string() {
                    Ok(text) => {
                        let source = if let Some(mut source) = slot.previous.take() {
                            source.replace(&text); source
                        } else { Source::new(id, text) };
                        slot.bytes = Some(Ok(Bytes::from_string(source.clone())));
                        Ok(source)
                    }
                    Err(error) => { slot.bytes = Some(Ok(error.bytes)); Err(FileError::InvalidUtf8) }
                }
            }
            Ok(bytes) => {
                let result = std::str::from_utf8(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&bytes))
                    .map_err(|_| FileError::InvalidUtf8).map(|text| {
                        let normalized = text.replace("\r\n", "\n").replace('\r', "\n");
                        if let Some(mut source) = slot.previous.take() {
                            source.replace(&normalized); source
                        } else { Source::new(id, normalized) }
                    });
                // Preserve byte-exact read()/image data when the source needed
                // BOM removal or line-ending normalization.
                slot.bytes = Some(Ok(bytes));
                result
            }
        };
        slot.source = Some(result.clone());
        result
    }
    fn file(&self, id: FileId) -> FileResult<Bytes> {
        let mut reads = self.reads.lock().unwrap();
        if let Some(bytes) = reads.slots.get(&id).and_then(|s| s.bytes.clone()) { return bytes; }
        let bytes = self.load(id, &mut reads);
        reads.slots.entry(id).or_default().bytes = Some(bytes.clone());
        bytes
    }
    fn font(&self, index: usize) -> Option<Font> { self.fonts.fonts.get(index).cloned() }
    fn today(&self, offset: Option<Duration>) -> Option<Datetime> { self.time.today(offset) }
}

pub struct FrozenWorld {
    library: Arc<LazyHash<Library>>, fonts: Arc<Fonts>, main: FileId,
    sources: HashMap<FileId, FileResult<Source>>, files: HashMap<FileId, FileResult<Bytes>>,
    time: Arc<Time>,
}

impl FrozenWorld {
    pub fn source_named(&self, path: &str) -> Option<Source> {
        self.sources.iter().find_map(|(id, source)| (wire_path(*id) == path).then(|| source.clone().ok()).flatten())
    }
}
impl World for FrozenWorld {
    fn library(&self) -> &LazyHash<Library> { &self.library }
    fn book(&self) -> &LazyHash<FontBook> { &self.fonts.book }
    fn main(&self) -> FileId { self.main }
    fn source(&self, id: FileId) -> FileResult<Source> {
        self.sources.get(&id).cloned().unwrap_or(Err(FileError::NotSource))
    }
    fn file(&self, id: FileId) -> FileResult<Bytes> {
        self.files.get(&id).cloned().unwrap_or(Err(FileError::AccessDenied))
    }
    fn font(&self, index: usize) -> Option<Font> { self.fonts.fonts.get(index).cloned() }
    fn today(&self, offset: Option<Duration>) -> Option<Datetime> { self.time.today(offset) }
}
impl typst_ide::IdeWorld for FrozenWorld { fn upcast(&self) -> &dyn World { self } }

fn package_roots(config: &Config, packages: &SystemPackages) -> Vec<PathBuf> {
    let mut roots = config.package_paths.clone();
    for root in [packages.data(), packages.cache()].into_iter().flatten() {
        if !roots.iter().any(|p| p == root.path()) { roots.push(root.path().into()); }
    }
    roots
}

struct PolicyDownloader { inner: SystemDownloader, allowed: bool, limit: usize }
impl PolicyDownloader {
    fn new(allowed: bool, limit: usize) -> Self {
        Self { inner: SystemDownloader::new("vtextedit-typst/0.1.0"), allowed, limit: limit.min(crate::MAX_BINARY) }
    }
}
impl Downloader for PolicyDownloader {
    fn download(&self, key: &dyn Any, url: &str) -> io::Result<Vec<u8>> {
        let (hint, mut reader) = self.stream(key, url)?;
        let mut bytes = Vec::with_capacity(hint.unwrap_or(0));
        let mut buffer = [0u8; 65536];
        loop {
            let count = reader.read(&mut buffer)?;
            if count == 0 { break; }
            if count > self.limit.saturating_sub(bytes.len()) {
                return Err(io::Error::other("package download exceeds resource limit"));
            }
            bytes.try_reserve_exact(count).map_err(io::Error::other)?;
            bytes.extend_from_slice(&buffer[..count]);
        }
        Ok(bytes)
    }

    fn stream(&self, key: &dyn Any, url: &str) -> io::Result<(Option<usize>, Box<dyn Read>)> {
        if !self.allowed { return Err(io::Error::new(io::ErrorKind::PermissionDenied, "package downloads are disabled")); }
        eprintln!("Downloading Typst package: {url}");
        let (length, reader) = self.inner.stream(key, url)?;
        if length.is_some_and(|n| n > self.limit) { return Err(io::Error::other("package download exceeds resource limit")); }
        Ok((length, Box::new(LimitedDownload { reader, remaining: self.limit, url: url.into(), finished: false })))
    }
}
struct LimitedDownload { reader: Box<dyn Read>, remaining: usize, url: String, finished: bool }
impl Read for LimitedDownload {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        if buffer.is_empty() { return Ok(0); }
        if self.remaining == 0 {
            let mut extra = [0];
            if self.reader.read(&mut extra)? != 0 { return Err(io::Error::other("package download exceeds resource limit")); }
            return Ok(0);
        }
        let count = buffer.len().min(self.remaining);
        let count = self.reader.read(&mut buffer[..count])?;
        self.remaining -= count;
        if count == 0 && !self.finished { eprintln!("Downloaded Typst package: {}", self.url); self.finished = true; }
        Ok(count)
    }
}

fn read_bounded(path: &Path, limit: usize) -> io::Result<Vec<u8>> {
    let mut file = File::open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() { return Err(io::Error::new(io::ErrorKind::IsADirectory, "expected a file")); }
    if metadata.len() > limit as u64 { return Err(io::Error::new(io::ErrorKind::FileTooLarge, "file exceeds read limit")); }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    let mut buffer = [0u8; 65536];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 { break; }
        if count > limit.saturating_sub(bytes.len()) {
            return Err(io::Error::new(io::ErrorKind::FileTooLarge, "file exceeds read limit"));
        }
        bytes.try_reserve_exact(count).map_err(io::Error::other)?;
        bytes.extend_from_slice(&buffer[..count]);
    }
    Ok(bytes)
}

fn content_fingerprint(path: &Path, bytes: &[u8]) -> String {
    format!("file:{}:{}", fs::canonicalize(path).unwrap_or_else(|_| path.into()).display(), blake3::hash(bytes))
}

fn dependency_matches(path: &Path, frozen: &str) -> bool {
    if frozen == "missing" {
        return fs::metadata(path).is_err_and(|e| e.kind() == io::ErrorKind::NotFound);
    }
    if let Some(identity) = frozen.strip_prefix("identity:") {
        return fs::canonicalize(path).is_ok_and(|real| real.to_string_lossy() == identity);
    }
    if frozen.starts_with("file:") || frozen.starts_with("dir:") {
        // Check identity before opening the file: a newly escaping symlink is
        // a change, not permission to fingerprint data outside its old root.
        let Some((old_identity, _)) = frozen.rsplit_once(':') else { return false; };
        let Ok(real) = fs::canonicalize(path) else { return false; };
        let prefix = if frozen.starts_with("file:") { "file" } else { "dir" };
        if format!("{prefix}:{}", real.display()) != old_identity { return false; }
    }
    fingerprint(path) == frozen
}

fn fingerprint(path: &Path) -> String {
    let metadata = match fs::metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return "missing".into(),
        Err(error) => return format!("error:{:?}", error.kind()),
    };
    if metadata.is_dir() {
        let entries = match fs::read_dir(path) { Ok(entries) => entries, Err(e) => return format!("error:{:?}", e.kind()) };
        let mut names = Vec::new();
        for entry in entries {
            match entry { Ok(entry) => names.push(entry.file_name()), Err(e) => return format!("error:{:?}", e.kind()) }
        }
        names.sort();
        let mut hash = blake3::Hasher::new();
        for name in names { hash.update(name.as_encoded_bytes()); hash.update(&[0]); }
        return format!("dir:{}:{}", fs::canonicalize(path).unwrap_or_else(|_| path.into()).display(), hash.finalize());
    }
    if !metadata.is_file() { return "special".into(); }
    let result = (|| -> io::Result<blake3::Hash> {
        let mut file = File::open(path)?;
        let mut hash = blake3::Hasher::new();
        let mut buffer = [0u8; 65536];
        loop { let count = file.read(&mut buffer)?; if count == 0 { break; } hash.update(&buffer[..count]); }
        Ok(hash.finalize())
    })();
    match result {
        Ok(hash) => format!("file:{}:{hash}", fs::canonicalize(path).unwrap_or_else(|_| path.into()).display()),
        Err(e) => format!("error:{:?}", e.kind()),
    }
}

fn collect_fonts(path: &Path, output: &mut Vec<PathBuf>, deps: &mut BTreeMap<PathBuf, String>, seen: &mut HashSet<PathBuf>) -> Result<(), String> {
    deps.insert(path.into(), fingerprint(path));
    let real = match fs::canonicalize(path) {
        Ok(path) => path,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Err(format!("font path does not exist: {}", path.display())),
        Err(e) => return Err(format!("cannot read font path {}: {e}", path.display())),
    };
    if !seen.insert(real.clone()) { return Ok(()); }
    if real.is_dir() {
        for entry in fs::read_dir(&real).map_err(|e| e.to_string())? {
            let path = entry.map_err(|e| e.to_string())?.path();
            if path.is_dir() { collect_fonts(&path, output, deps, seen)?; }
            else if is_font(&path) { output.push(path); }
        }
    } else if is_font(&real) { output.push(real); }
    else { return Err(format!("not a supported font file: {}", path.display())); }
    Ok(())
}
fn is_font(path: &Path) -> bool {
    path.extension().and_then(|s| s.to_str()).is_some_and(|s| matches!(s.to_ascii_lowercase().as_str(), "ttf" | "otf" | "ttc" | "otc"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16_mapping_rejects_split_surrogates_and_preserves_import_identity() {
        let source = Source::detached("a😀é\nz");
        assert_eq!(byte_offset(&source, 1), Some(1));
        assert_eq!(byte_offset(&source, 2), None);
        assert_eq!(byte_offset(&source, 3), Some(5));
        assert_eq!(byte_offset(&source, 7), None);
        let mut world = ProjectWorld::new(Config { project_root: std::env::temp_dir(), ..Config::default() }).unwrap();
        world.update(world.config.clone(), vec![Overlay { file:"main.typ".into(), text:"a😀é\nz".into() }]).unwrap();
        assert_eq!(wire_range(&world, world.main(), 1..5).unwrap(), json!({"file":"main.typ","start":1,"end":3}));
        assert!(wire_range(&world, world.main(), 2..5).is_none());
        assert!(project_id("../escape.typ").is_err());
    }

    #[test]
    fn removed_overlay_restores_missing_dependency_and_old_world_stays_frozen() {
        let root = std::env::temp_dir().join(format!("vtextedit-world-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let config = Config { project_root: root.clone(), ..Config::default() };
        let mut world = ProjectWorld::new(config.clone()).unwrap();
        world.update(config.clone(), vec![Overlay { file:"main.typ".into(), text:"old".into() }]).unwrap();
        assert_eq!(world.source(world.main()).unwrap().text(), "old");
        let frozen = world.freeze();
        world.update(config.clone(), vec![]).unwrap();
        assert!(world.source(world.main()).is_err());
        assert!(world.dependencies().as_array().unwrap().iter().any(|d| d["fingerprint"] == "missing"));
        fs::write(root.join("main.typ"), "new\r\ntext").unwrap();
        assert!(world.check_dependencies().is_err());
        world.update(config, vec![]).unwrap();
        assert_eq!(world.source(world.main()).unwrap().text(), "new\ntext");
        assert_eq!(frozen.source(frozen.main()).unwrap().text(), "old");
        fs::remove_file(root.join("main.typ")).unwrap();
        fs::remove_dir(root).unwrap();
    }

    #[test]
    fn installed_package_and_unsaved_import_work_without_network() {
        let root = std::env::temp_dir().join(format!("vtextedit-package-{}", std::process::id()));
        let packages = root.join("packages");
        let package = packages.join("local/example/0.1.0");
        fs::create_dir_all(&package).unwrap();
        fs::write(package.join("typst.toml"), "[package]\nname = \"example\"\nversion = \"0.1.0\"\nentrypoint = \"lib.typ\"\n").unwrap();
        fs::write(package.join("lib.typ"), "#let value = 7").unwrap();
        let config = Config { project_root: root.clone(), package_paths: vec![packages], ..Config::default() };
        let mut world = ProjectWorld::new(config.clone()).unwrap();
        world.update(config, vec![
            Overlay { file: "main.typ".into(), text: "#import \"@local/example:0.1.0\": value\n#import \"unsaved.typ\": twice\n#assert(twice(value) == 14)\n$ x = #value $".into() },
            Overlay { file: "unsaved.typ".into(), text: "#let twice(x) = 2 * x".into() },
        ]).unwrap();
        let result = typst::compile::<typst_layout::PagedDocument>(&world);
        assert!(result.output.is_ok(), "{:?}", result.output.err());
        let deps = world.dependencies();
        assert!(deps.as_array().unwrap().iter().any(|d| d["path"].as_str().unwrap().ends_with("lib.typ")));
        assert!(world.font(usize::MAX).is_none());
        let downloader = PolicyDownloader::new(false, 1024);
        assert_eq!(downloader.stream(&(), "https://packages.typst.org/preview/test").err().unwrap().kind(), io::ErrorKind::PermissionDenied);
        fs::remove_file(package.join("lib.typ")).unwrap();
        fs::remove_file(package.join("typst.toml")).unwrap();
        fs::remove_dir(&package).unwrap();
        fs::remove_dir(package.parent().unwrap()).unwrap();
        fs::remove_dir(root.join("packages/local")).unwrap();
        fs::remove_dir(root.join("packages")).unwrap();
        fs::remove_dir(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn escaping_symlink_is_denied_and_retargeting_invalidates_it() {
        let root = std::env::temp_dir().join(format!("vtextedit-symlink-{}", std::process::id()));
        let project = root.join("project");
        fs::create_dir_all(&project).unwrap();
        fs::write(root.join("outside.typ"), "not readable").unwrap();
        std::os::unix::fs::symlink(root.join("outside.typ"), project.join("link.typ")).unwrap();
        let world = ProjectWorld::new(Config { project_root: project.clone(), ..Config::default() }).unwrap();
        assert!(matches!(world.source(project_id("link.typ").unwrap()), Err(FileError::AccessDenied)));
        assert!(world.check_dependencies().is_ok());
        fs::remove_file(project.join("link.typ")).unwrap();
        fs::write(project.join("link.typ"), "now readable").unwrap();
        assert!(world.check_dependencies().is_err());
        fs::remove_file(project.join("link.typ")).unwrap();
        fs::remove_file(root.join("outside.typ")).unwrap();
        fs::remove_dir(project).unwrap();
        fs::remove_dir(root).unwrap();
    }
}
