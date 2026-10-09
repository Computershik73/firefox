/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use nsstring::nsAString;
use rayon::ThreadPool;
use std::cell::RefCell;
use std::ffi::OsString;
use std::fs::{create_dir_all, read_dir, read_to_string, remove_dir_all, File};
use std::io::{Error, ErrorKind};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;
use webrender::{ProgramBinary, ProgramCache, ProgramCacheObserver, ProgramSourceDigest};
#[cfg(windows)]
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
#[cfg(windows)]
use webrender::{gecko_w10m_note_always, gecko_w10m_shader_stats};

const MAX_LOAD_TIME_MS: u64 = 400;

// GeckoW10m: a relaunch of the same engine build links its shaders from the
// binaries kept here instead of compiling them again, and that is about a
// second of the second frame on the phone. These say what the cache did this
// run -- programs read on demand, files written, whether the startup list
// went out -- for the few shader-cache notes below. Each note is capped per
// run, and none of this is touched per frame.
#[cfg(windows)]
static DISK_READS: AtomicU32 = AtomicU32::new(0);
#[cfg(windows)]
static WRITTEN: AtomicU32 = AtomicU32::new(0);
#[cfg(windows)]
static STARTUP_LIST_WRITTEN: AtomicBool = AtomicBool::new(false);
#[cfg(windows)]
static NOTED_LIST: AtomicU32 = AtomicU32::new(0);
#[cfg(windows)]
static LATE: AtomicU32 = AtomicU32::new(0);

// A program file or the startup list could not be written. Only the first
// failure of the run is noted: the rest are almost always the same cause.
#[cfg(windows)]
fn gecko_w10m_write_failed(file_name: &str, err: &dyn std::fmt::Display) {
    static NOTED: AtomicU32 = AtomicU32::new(0);
    if NOTED.fetch_add(1, Ordering::Relaxed) == 0 {
        gecko_w10m_note_always(&format!(
            "gfx: shader cache -- could not write {}: {}",
            file_name, err
        ));
    }
}

fn hash_bytes(v: &[u8]) -> u64 {
    use rustc_hash::FxHasher;
    use std::hash::{Hash, Hasher};
    let mut state = FxHasher::default();
    v.hash(&mut state);
    state.finish()
}

fn deserialize_program_binary(path: &Path) -> Result<(Arc<ProgramBinary>, usize), Error> {
    let mut buf = vec![];
    let mut file = File::open(path)?;
    file.read_to_end(&mut buf)?;

    if buf.len() <= 8 + 4 {
        return Err(Error::new(ErrorKind::InvalidData, "File size is too small"));
    }
    let magic = &buf[0..4];
    let hash = &buf[4..8 + 4];
    let data = &buf[8 + 4..];

    // Check if magic + version are correct.
    let mv: u32 = bincode::deserialize(&magic).unwrap();
    if mv != MAGIC_AND_VERSION {
        return Err(Error::new(
            ErrorKind::InvalidData,
            "File data is invalid (magic+version)",
        ));
    }

    // Check if hash is correct
    let hash: u64 = bincode::deserialize(&hash).unwrap();
    let hash_data = hash_bytes(&data);
    if hash != hash_data {
        return Err(Error::new(ErrorKind::InvalidData, "File data is invalid (hash)"));
    }

    // Deserialize ProgramBinary
    let binary = match bincode::deserialize(&data) {
        Ok(binary) => binary,
        Err(_) => {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Failed to deserialize ProgramBinary",
            ))
        },
    };

    Ok((Arc::new(binary), buf.len()))
}

#[cfg(target_os = "windows")]
fn get_cache_path_from_prof_path(prof_path: &nsAString) -> Option<PathBuf> {
    if prof_path.is_empty() {
        // Empty means that we do not use disk cache.
        return None;
    }

    use std::os::windows::prelude::*;

    let prof_path = OsString::from_wide(prof_path.as_ref());
    let mut cache_path = PathBuf::from(&prof_path);
    cache_path.push("shader-cache");

    Some(cache_path)
}

#[cfg(not(target_os = "windows"))]
fn get_cache_path_from_prof_path(prof_path: &nsAString) -> Option<PathBuf> {
    if prof_path.is_empty() {
        // Empty means that we do not use disk cache.
        return None;
    }

    let utf8 = String::from_utf16(prof_path.as_ref()).unwrap();
    let prof_path = OsString::from(utf8);
    let mut cache_path = PathBuf::from(&prof_path);
    cache_path.push("shader-cache");

    Some(cache_path)
}

struct WrProgramBinaryDiskCache {
    cache_path: PathBuf,
    workers: Arc<ThreadPool>,
    cached_shader_filenames: Vec<OsString>,
}

// Magic number + version. Increment the version when the binary format changes.
const MAGIC: u32 = 0xB154AD30; // BI-SHADE + version.
const VERSION: u32 = 2;
const MAGIC_AND_VERSION: u32 = MAGIC + VERSION;

const WHITELIST_FILENAME: &str = "startup_shaders";
const WHITELIST_SEPARATOR: &str = "\n";

/// Helper to convert a closure returning a `Result` to one that returns void.
/// This allows the enclosed code to use the question-mark operator in a
/// context where the calling function doesn't expect a `Result`.
#[allow(unused_must_use)]
fn result_to_void<F: FnOnce() -> Result<(), ()>>(f: F) {
    f();
}

impl WrProgramBinaryDiskCache {
    #[allow(dead_code)]
    fn new(cache_path: PathBuf, workers: &Arc<ThreadPool>) -> Self {
        WrProgramBinaryDiskCache {
            cache_path,
            workers: Arc::clone(workers),
            cached_shader_filenames: Vec::new(),
        }
    }

    /// Saves the specified binaries to the on-disk shader cache.
    fn save_shaders_to_disk(&mut self, entries: Vec<Arc<ProgramBinary>>) {
        info!("Saving binaries to on-disk shader cache");
        #[cfg(windows)]
        let count = entries.len();

        // Write the entries to disk on a worker thread.
        for entry in entries {
            let file_name = entry.source_digest().to_string();
            let file_path = self.cache_path.join(&file_name);

            self.workers.spawn(move || {
                result_to_void(move || {
                    info!("Writing shader: {}", file_name);

                    use std::time::Instant;
                    let start = Instant::now();

                    let data: Vec<u8> = bincode::serialize(&*entry).map_err(|e| {
                        error!("shader-cache: Failed to serialize: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(&file_name, &e);
                    })?;

                    let mut file = File::create(&file_path).map_err(|e| {
                        error!("shader-cache: Failed to create file: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(&file_name, &e);
                    })?;

                    // Write magic + version.
                    let mv = MAGIC_AND_VERSION;
                    let mv = bincode::serialize(&mv).unwrap();
                    assert!(mv.len() == 4);
                    file.write_all(&mv).map_err(|e| {
                        error!("shader-cache: Failed to write magic + version: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(&file_name, &e);
                    })?;

                    // Write hash
                    let hash = hash_bytes(&data);
                    let hash = bincode::serialize(&hash).unwrap();
                    assert!(hash.len() == 8);
                    file.write_all(&hash).map_err(|e| {
                        error!("shader-cache: Failed to write hash: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(&file_name, &e);
                    })?;

                    // Write serialized data
                    file.write_all(&data).map_err(|e| {
                        error!("shader-cache: Failed to write program binary: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(&file_name, &e);
                    })?;

                    info!("Wrote shader {} in {:?}", file_name, start.elapsed());
                    #[cfg(windows)]
                    WRITTEN.fetch_add(1, Ordering::Relaxed);
                    Ok(())
                })
            });
        }

        // GeckoW10m: programs first drawn after the startup list went out --
        // blur, gradients, video -- are compiled from source this run and come
        // from disk on the next one. The note is queued behind the writes on
        // the same pool, a few times a run at most.
        #[cfg(windows)]
        {
            if count > 0
                && STARTUP_LIST_WRITTEN.load(Ordering::Relaxed)
                && LATE.fetch_add(1, Ordering::Relaxed) < 4
            {
                self.workers.spawn(move || {
                    let stats = gecko_w10m_shader_stats();
                    gecko_w10m_note_always(&format!(
                        "gfx: shader cache -- {} more programs compiled after startup and saved (this run {} compiled in {} ms; {} read from disk on demand)",
                        count,
                        stats.compiled,
                        stats.compiled_ms,
                        DISK_READS.load(Ordering::Relaxed)
                    ));
                });
            }
        }
    }

    /// Writes the whitelist containing the set of startup shaders to disk.
    fn set_startup_shaders(&mut self, entries: Vec<Arc<ProgramBinary>>) {
        // GeckoW10m: every device writes this list at its tenth frame, and
        // that includes the shader-warmup device, whose frames are warmup
        // steps. When those run before anything has been linked, the list is
        // empty -- and File::create below would truncate the good list from
        // the last run, so a kill before the renderer's own tenth frame left
        // the next launch with nothing to preload. An empty list says nothing
        // a missing one does not, so it is not written.
        #[cfg(windows)]
        {
            if entries.is_empty() {
                return;
            }
            STARTUP_LIST_WRITTEN.store(true, Ordering::Relaxed);
        }
        #[cfg(windows)]
        let count = entries.len();

        let whitelist = entries
            .iter()
            .map(|e| e.source_digest().to_string())
            .collect::<Vec<String>>()
            .join(WHITELIST_SEPARATOR);

        let mut whitelist_path = self.cache_path.clone();
        whitelist_path.push(WHITELIST_FILENAME);
        self.workers.spawn(move || {
            result_to_void(move || {
                info!("Writing startup shader whitelist");
                File::create(&whitelist_path)
                    .and_then(|mut file| file.write_all(whitelist.as_bytes()))
                    .map_err(|e| {
                        error!("shader-cache: Failed to write startup whitelist: {}", e);
                        #[cfg(windows)]
                        gecko_w10m_write_failed(WHITELIST_FILENAME, &e);
                    })?;
                // GeckoW10m: one line on what the cache did for the first
                // frames -- linked from binaries or compiled from source, and
                // how long the render thread waited for each. Written here,
                // on the pool, after the list is on disk.
                #[cfg(windows)]
                {
                    if NOTED_LIST.fetch_add(1, Ordering::Relaxed) < 2 {
                        let stats = gecko_w10m_shader_stats();
                        let slowest = if stats.compiled > 0 {
                            format!(" (slowest {} ms, {})", stats.slowest_ms, stats.slowest)
                        } else {
                            String::new()
                        };
                        gecko_w10m_note_always(&format!(
                            "gfx: shader cache -- startup list written, {} programs; {} linked from binaries in {} ms, {} compiled from source in {} ms{}, {} binaries refused, {} without a binary; {} read from disk on demand, {} written so far",
                            count,
                            stats.from_binary,
                            stats.from_binary_ms,
                            stats.compiled,
                            stats.compiled_ms,
                            slowest,
                            stats.refused,
                            stats.no_binary,
                            DISK_READS.load(Ordering::Relaxed),
                            WRITTEN.load(Ordering::Relaxed)
                        ));
                    }
                }
                Ok(())
            })
        });
    }

    /// Returns the size of the program binary read into `program_cache`, or
    /// None when there was none to read or it could not be read.
    pub fn try_load_shader_from_disk(&mut self, filename: &str, program_cache: &Rc<ProgramCache>) -> Option<usize> {
        if let Some(index) = self.cached_shader_filenames.iter().position(|e| e == filename) {
            let mut path = self.cache_path.clone();
            path.push(filename);

            self.cached_shader_filenames.swap_remove(index);

            info!("Loading shader: {}", filename);

            match deserialize_program_binary(&path) {
                Ok((program, size)) => {
                    program_cache.load_program_binary(program);
                    return Some(size);
                },
                Err(err) => {
                    error!("shader-cache: Failed to deserialize program binary: {}", err);
                    // GeckoW10m: an error path. The program is compiled from
                    // source when it is first drawn, and its new binary
                    // overwrites this file. Noted once a run.
                    #[cfg(windows)]
                    {
                        static NOTED: AtomicU32 = AtomicU32::new(0);
                        if NOTED.fetch_add(1, Ordering::Relaxed) == 0 {
                            gecko_w10m_note_always(&format!(
                                "gfx: shader cache -- {} could not be read ({}); compiled from source and saved again",
                                filename, err
                            ));
                        }
                    }
                },
            };
        } else {
            info!("shader-cache: Program binary not found in disk cache");
        }
        None
    }

    /// Returns the number of startup programs read into `program_cache`.
    #[cfg_attr(not(windows), allow(unused_variables, unused_assignments))]
    pub fn try_load_startup_shaders_from_disk(&mut self, program_cache: &Rc<ProgramCache>) -> u32 {
        use std::time::Instant;
        let start = Instant::now();

        // Load and parse the whitelist if it exists
        let mut whitelist_path = self.cache_path.clone();
        whitelist_path.push(WHITELIST_FILENAME);
        let whitelist = match read_to_string(&whitelist_path) {
            Ok(whitelist) => whitelist
                .split(WHITELIST_SEPARATOR)
                .map(|s| s.to_string())
                .collect::<Vec<String>>(),
            Err(err) => {
                info!("shader-cache: Could not read startup whitelist: {}", err);
                Vec::new()
            },
        };
        info!("Loaded startup shader whitelist in {:?}", start.elapsed());

        self.cached_shader_filenames = read_dir(&self.cache_path)
            .map_err(|err| {
                error!(
                    "shader-cache: Error reading directory whilst loading startup shaders: {}",
                    err
                )
            })
            .into_iter()
            .flatten()
            .filter_map(Result::ok)
            .map(|entry| entry.file_name())
            .filter(|filename| filename != WHITELIST_FILENAME)
            .collect::<Vec<OsString>>();

        // An empty list file still splits into one empty name.
        let listed = whitelist.iter().filter(|name| !name.is_empty()).count();
        let mut loaded: u32 = 0;
        let mut loaded_bytes: usize = 0;
        let mut timed_out = false;

        // Load whitelisted program binaries if they exist
        for entry in &whitelist {
            if let Some(size) = self.try_load_shader_from_disk(&entry, program_cache) {
                loaded += 1;
                loaded_bytes += size;
            }

            let elapsed = start.elapsed();
            info!("Loaded shader in {:?}", elapsed);
            let elapsed_ms = (elapsed.as_secs() * 1_000) + elapsed.subsec_millis() as u64;

            if elapsed_ms > MAX_LOAD_TIME_MS {
                // Loading the startup shaders is taking too long, so bail out now.
                // Additionally clear the list of remaining shaders cached on disk,
                // so that we do not attempt to load any on demand during rendering.
                error!("shader-cache: Timed out before finishing loads");
                // GeckoW10m: not cleared. Reading one more file when its
                // program is first drawn costs a few milliseconds on the
                // phone's eMMC; compiling it from source instead costs a few
                // hundred, and turning on-demand loads off for the whole run
                // made a slow preload the most expensive outcome of all.
                #[cfg(not(windows))]
                self.cached_shader_filenames.clear();
                timed_out = true;
                break;
            }
        }

        // GeckoW10m: once a run, on the render thread inside InitDeviceTask --
        // before the window's compositor exists, so nothing waits on it.
        #[cfg(windows)]
        {
            let ms = start.elapsed().as_millis();
            let rest = self.cached_shader_filenames.len();
            let dir = self.cache_path.display();
            let note = if listed == 0 && rest == 0 {
                format!(
                    "gfx: shader cache at {} -- empty (first run, or emptied); programs are saved as they are compiled",
                    dir
                )
            } else if timed_out {
                format!(
                    "gfx: shader cache at {} -- stopped reading after {} of {} at {} ms; the rest load when first drawn",
                    dir, loaded, listed, ms
                )
            } else {
                format!(
                    "gfx: shader cache at {} -- {} of {} startup programs read in {} ms ({} KB), {} more on disk for later",
                    dir,
                    loaded,
                    listed,
                    ms,
                    (loaded_bytes + 1023) / 1024,
                    rest
                )
            };
            gecko_w10m_note_always(&note);
        }

        loaded
    }
}

pub struct WrProgramCacheObserver {
    disk_cache: Rc<RefCell<WrProgramBinaryDiskCache>>,
}

impl WrProgramCacheObserver {
    #[allow(dead_code)]
    fn new(disk_cache: Rc<RefCell<WrProgramBinaryDiskCache>>) -> Self {
        WrProgramCacheObserver { disk_cache }
    }
}

impl ProgramCacheObserver for WrProgramCacheObserver {
    fn save_shaders_to_disk(&self, entries: Vec<Arc<ProgramBinary>>) {
        self.disk_cache.borrow_mut().save_shaders_to_disk(entries);
    }

    fn set_startup_shaders(&self, entries: Vec<Arc<ProgramBinary>>) {
        self.disk_cache.borrow_mut().set_startup_shaders(entries);
    }

    fn try_load_shader_from_disk(&self, digest: &ProgramSourceDigest, program_cache: &Rc<ProgramCache>) {
        let filename = digest.to_string();
        let loaded = self
            .disk_cache
            .borrow_mut()
            .try_load_shader_from_disk(&filename, program_cache);
        // GeckoW10m: a program that was not preloaded, read when first drawn.
        #[cfg(windows)]
        {
            if loaded.is_some() {
                DISK_READS.fetch_add(1, Ordering::Relaxed);
            }
        }
        #[cfg(not(windows))]
        let _ = loaded;
    }

    fn notify_program_binary_failed(&self, _program_binary: &Arc<ProgramBinary>) {
        error!("shader-cache: Failed program_binary");
    }
}

pub struct WrProgramCache {
    pub program_cache: Rc<ProgramCache>,
    disk_cache: Option<Rc<RefCell<WrProgramBinaryDiskCache>>>,
}

impl WrProgramCache {
    pub fn new(prof_path: &nsAString, workers: &Arc<ThreadPool>) -> Self {
        let cache_path = get_cache_path_from_prof_path(prof_path);
        let use_disk_cache = cache_path.as_ref().map_or(false, |p| match create_dir_all(p) {
            Ok(()) => true,
            Err(_err) => {
                // GeckoW10m: without the folder every shader is compiled
                // from source on every launch; say why. Once a process.
                #[cfg(windows)]
                {
                    static NOTED: AtomicBool = AtomicBool::new(false);
                    if !NOTED.swap(true, Ordering::Relaxed) {
                        gecko_w10m_note_always(&format!(
                            "gfx: shader cache off -- could not make {}: {}",
                            p.display(),
                            _err
                        ));
                    }
                }
                false
            },
        });
        #[cfg(windows)]
        {
            static NOTED: AtomicBool = AtomicBool::new(false);
            if cache_path.is_none() && !NOTED.swap(true, Ordering::Relaxed) {
                gecko_w10m_note_always("gfx: shader cache off -- no profile directory");
            }
        }
        let (disk_cache, program_cache_observer) = if use_disk_cache {
            let cache = Rc::new(RefCell::new(WrProgramBinaryDiskCache::new(
                cache_path.unwrap(),
                workers,
            )));
            let obs = Box::new(WrProgramCacheObserver::new(Rc::clone(&cache))) as Box<dyn ProgramCacheObserver>;
            (Some(cache), Some(obs))
        } else {
            (None, None)
        };
        let program_cache = ProgramCache::new(program_cache_observer);

        WrProgramCache {
            program_cache,
            disk_cache,
        }
    }

    pub fn rc_get(&self) -> &Rc<ProgramCache> {
        &self.program_cache
    }

    /// Returns the number of startup programs read into memory.
    pub fn try_load_startup_shaders_from_disk(&self) -> u32 {
        if let Some(ref disk_cache) = self.disk_cache {
            disk_cache
                .borrow_mut()
                .try_load_startup_shaders_from_disk(&self.program_cache)
        } else {
            error!("shader-cache: Shader disk cache is not supported");
            0
        }
    }
}

pub fn remove_disk_cache(prof_path: &nsAString) -> Result<(), Error> {
    use std::time::Instant;

    if let Some(cache_path) = get_cache_path_from_prof_path(prof_path) {
        if cache_path.exists() {
            let start = Instant::now();
            remove_dir_all(&cache_path)?;
            info!("removed all disk cache shaders in {:?}", start.elapsed());
        }
    }
    Ok(())
}
