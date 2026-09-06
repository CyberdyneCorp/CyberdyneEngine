//! Engine hosting modes, and the one place a library is loaded. Task 2.2.
//!
//! `editor-rust-application` names three modes and makes one of them the default:
//!
//! | Mode | Engine location | Used for |
//! |---|---|---|
//! | `NoRuntime` | None | Project browsing, source assets, build configuration, source control |
//! | `Embedded` | In the editor process | Schema and reflection queries, asset metadata, previews |
//! | `Hosted` | A separate process or a remote device | Rendering, play mode, physics, streaming |
//!
//! **Hosted is the production default**, and this crate does not implement it — `cy-editor-protocol`
//! does, because a hosted runtime is reached over a transport rather than through a symbol table.
//! What lives here is `Embedded`: opening a library that exports `cy_get_interface`, checking the
//! header it publishes, and refusing anything the SDK cannot safely talk to.
//!
//! The measured reason Hosted is the default rather than a fallback is in the milestone's
//! live-bridge spike: the process boundary costs about 60 microseconds at the median, and the 8.6 ms
//! that dominates a gizmo drag is the runtime's own frame — paid identically with no boundary at
//! all. Out-of-process buys crash isolation and remote editing for +0.3 to +1.3 ms at p50.
//!
//! --- WHY A LIBRARY IS NEVER CLOSED ------------------------------------------------------------------
//!
//! [`RuntimeLibrary`] does not call `dlclose`, and its `Drop` says so. That is the engine's own
//! reload model, proved at M4: serialize, migrate by name, recreate, and never unload — because a
//! retired image's string literals are still referenced by component and behaviour registrations
//! (`cy/abi/host.h` states the rule at the descriptor), and unmapping it turns every one of them
//! into a dangling pointer that still looks like a valid name. Leaking a mapping is the cheap side
//! of that trade and it is the side the engine already chose.
//!
//! --- THE EMBEDDED ENTRY POINT IS THIS SDK'S CONVENTION, NOT THE ABI'S -------------------------------
//!
//! `cy_abi.h` exports exactly one symbol, `cy_get_interface`, and it hands out an interface *table*.
//! It has no entry point by which a client creates an engine: the ABI is written for a module that
//! is *given* a `CyEngine` through `CyModuleEntryFn`, not for a host that wants to make one. So an
//! embedded host needs a symbol the ABI does not define, and this crate names the pair it looks for
//! — [`CREATE_SYMBOL`] and [`DESTROY_SYMBOL`] — rather than pretending the ABI provides them.
//!
//! No engine build exports them today. A library that has the interface table and not the pair opens
//! successfully and reports exactly that, which is a diagnosis rather than a crash, and it is what
//! makes adding the export a one-line engine change instead of an investigation.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::path::{Path, PathBuf};

use cy_editor_core::problem::{Problem, Result};

use crate::generated::abi;
use crate::generated::ffi;
use crate::generated::interface::Interface;

/// Where the engine a session talks to is running.
///
/// `editor-rust-application` requires every editor feature to "remain capable of operating against a
/// hosted runtime unless it has a documented reason to require in-process execution, and such
/// reasons SHALL be enumerated rather than accumulated". **The enumeration is empty**, and this
/// comment is where it lives until it is not: a feature that needs in-process execution adds its
/// reason here, so that the list is impossible to grow without a reviewer seeing the whole of it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum HostingMode {
    /// No engine at all: project browsing, source assets, build configuration, source control.
    NoRuntime,
    /// The engine in the editor's own process, through a loaded library.
    Embedded,
    /// The engine in a separate process or on a remote device. The production default.
    #[default]
    Hosted,
}

impl HostingMode {
    /// Whether a crash of the engine can take the editor with it.
    ///
    /// The whole argument for `Hosted` being the default in one predicate: it is the only mode for
    /// which this is false.
    #[must_use]
    pub const fn shares_the_editors_fate(self) -> bool {
        matches!(self, HostingMode::Embedded)
    }

    /// The mode's name as a command line or a configuration file spells it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            HostingMode::NoRuntime => "no-runtime",
            HostingMode::Embedded => "embedded",
            HostingMode::Hosted => "hosted",
        }
    }

    /// Parse a mode from a command line or a configuration file.
    pub fn parse(text: &str) -> Result<Self> {
        match text {
            "no-runtime" => Ok(HostingMode::NoRuntime),
            "embedded" => Ok(HostingMode::Embedded),
            "hosted" => Ok(HostingMode::Hosted),
            other => Err(Problem::new(
                format!("the hosting mode {other:?}"),
                "it is not one of the three modes the editor supports",
            )
            .with_remedy("use no-runtime, embedded, or hosted")),
        }
    }
}

/// The symbol an embedded host library must export to create an engine. This SDK's convention.
pub const CREATE_SYMBOL: &str = "cy_editor_embedded_create";

/// The symbol an embedded host library must export to destroy an engine. This SDK's convention.
pub const DESTROY_SYMBOL: &str = "cy_editor_embedded_destroy";

/// The one symbol the C ABI itself exports.
pub const INTERFACE_SYMBOL: &str = "cy_get_interface";

type GetInterfaceFn = unsafe extern "C" fn(u32, u32) -> *const ffi::CyInterface;
type EmbeddedCreateFn = unsafe extern "C" fn() -> ffi::CyEngine;
type EmbeddedDestroyFn = unsafe extern "C" fn(ffi::CyEngine);

/// A loaded engine library, kept mapped for the process's lifetime.
///
/// Holding the raw module handle is confined to this type, in this crate, which is the "narrow,
/// audited interoperation module" the specification permits. Nothing above the SDK ever sees it.
pub struct RuntimeLibrary {
    module: *mut c_void,
    path: PathBuf,
    interface: Interface,
    create: Option<EmbeddedCreateFn>,
    destroy: Option<EmbeddedDestroyFn>,
}

impl RuntimeLibrary {
    /// Open a library, resolve `cy_get_interface`, and check the table it publishes.
    ///
    /// Fails, with a reason and a remedy, when the file cannot be loaded, when it exports no
    /// interface, when the ABI major differs, or when the table is smaller than this SDK was
    /// generated against — the last of which means a runtime *older* than the SDK, whose table
    /// simply does not have the entries the SDK would call.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let module = platform::open(&path)?;

        // SAFETY: `platform::open` returned a live module handle, and the cast is to the signature
        // `cy_abi.h` declares for this symbol. A library that exports `cy_get_interface` with a
        // different signature is a library that is not this ABI, and no check in any language can
        // catch that — which is exactly why the header it returns is checked below.
        let get_interface: GetInterfaceFn =
            unsafe { platform::symbol(module, &path, INTERFACE_SYMBOL)? };
        // SAFETY: the ABI requires `cy_get_interface` to be callable at any time with any version
        // pair, returning null for a version it cannot serve.
        let table = unsafe { get_interface(abi::MAJOR, abi::MINOR) };
        if table.is_null() {
            return Err(Problem::new(
                format!("load {}", path.display()),
                format!(
                    "its `cy_get_interface` refused ABI {}.{}, so it is an engine this editor \
                     cannot talk to",
                    abi::MAJOR,
                    abi::MINOR
                ),
            )
            .with_remedy("rebuild the engine and the editor from the same revision"));
        }

        // SAFETY: non-null, and the ABI requires the table to have static storage duration.
        let header = unsafe { (*table).header };
        check_header(&path, header)?;

        // SAFETY: the pointer is non-null and its table is at least as large as this SDK declares,
        // which is exactly `Interface::from_raw`'s contract.
        let interface = unsafe { Interface::from_raw(table) };

        // Optional, and absent in every engine build today. See the module note.
        // SAFETY: same reasoning as `get_interface`; a symbol of this name with another signature
        // is a library that is not honouring this SDK's convention.
        let create: Option<EmbeddedCreateFn> =
            unsafe { platform::optional_symbol(module, CREATE_SYMBOL) };
        let destroy: Option<EmbeddedDestroyFn> =
            unsafe { platform::optional_symbol(module, DESTROY_SYMBOL) };

        Ok(Self {
            module,
            path,
            interface,
            create,
            destroy,
        })
    }

    /// The interface table this library published.
    #[must_use]
    pub const fn interface(&self) -> Interface {
        self.interface
    }

    /// The path the library was loaded from, for a diagnostic.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Whether this library can create an engine in this process.
    #[must_use]
    pub const fn can_embed(&self) -> bool {
        self.create.is_some() && self.destroy.is_some()
    }

    /// Create an engine in this process, for [`HostingMode::Embedded`].
    ///
    /// Fails when the library exports an interface table but no embedded-host entry point, which is
    /// every engine build today and is a diagnosis rather than a defect.
    pub fn create_engine(&self) -> Result<EmbeddedEngine<'_>> {
        let (Some(create), Some(destroy)) = (self.create, self.destroy) else {
            return Err(Problem::new(
                format!("host {} in this process", self.path.display()),
                format!(
                    "it exports `{INTERFACE_SYMBOL}` but not `{CREATE_SYMBOL}`, so it publishes an \
                     interface table and no way for a client to create an engine"
                ),
            )
            .with_remedy("run the runtime as a hosted process instead, which is the default"));
        };
        // SAFETY: the symbol was resolved from a module that is never unloaded, and the ABI's
        // contract for a creation entry point is that it returns a live handle or null.
        let engine = unsafe { create() };
        if engine.is_null() {
            return Err(Problem::new(
                format!("create an engine in {}", self.path.display()),
                "the library's creation entry point returned null",
            ));
        }
        Ok(EmbeddedEngine {
            engine,
            destroy,
            interface: self.interface,
            library: self,
        })
    }
}

// SAFETY: the fields are a module handle and function pointers, all of which are immutable for the
// process's lifetime because the library is never unloaded. What is *not* thread-safe is the engine
// behind them, and that is confined to `EmbeddedEngine`, which is deliberately neither `Send` nor
// `Sync`.
unsafe impl Send for RuntimeLibrary {}
unsafe impl Sync for RuntimeLibrary {}

// The module handle and the resolved function pointers are addresses, and
// `editor-rust-application` is explicit that an address is not editor-side identity. Printing them
// in a diagnostic is how one ends up quoted in a bug report as if it meant something.
#[allow(
    clippy::missing_fields_in_debug,
    reason = "an address is not identity; see above"
)]
impl std::fmt::Debug for RuntimeLibrary {
    /// Names the file and whether it can host in process, and nothing else.
    ///
    /// A derived implementation would print the module handle, which is an address — the one thing
    /// `editor-rust-application` says must not be editor-side identity, and printing it invites
    /// exactly that.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RuntimeLibrary")
            .field("path", &self.path)
            .field("can_embed", &self.can_embed())
            .finish()
    }
}

impl Drop for RuntimeLibrary {
    fn drop(&mut self) {
        // Deliberately no `dlclose`. See the module note: a retired image's string literals are
        // still referenced by every component and behaviour it registered, and unmapping it turns
        // each of them into a dangling pointer that still looks like a valid name. The engine's own
        // reload model made this choice at M4 and this is the same choice, not a second one.
        let _ = self.module;
    }
}

/// An engine created in the editor's own process.
///
/// Neither `Send` nor `Sync`, by the absence of those implementations rather than by a marker: the
/// C ABI's engine is not thread-safe, and `cy/abi/live_reload.h` states the same rule for the reload
/// path — "call it from the tick thread". A handle that could be moved to another thread would make
/// that rule unenforceable.
pub struct EmbeddedEngine<'library> {
    engine: ffi::CyEngine,
    destroy: EmbeddedDestroyFn,
    interface: Interface,
    library: &'library RuntimeLibrary,
}

impl EmbeddedEngine<'_> {
    /// The world bound to this engine, or a diagnosis when none is.
    pub fn world(&self) -> Result<crate::world::World<'_>> {
        // SAFETY: `engine` is the live handle the library's creation entry point returned.
        let world = unsafe { self.interface.engine_world(self.engine) }
            .map_err(|error| call_problem("read the engine's world", error))?;
        if world.is_null() {
            return Err(Problem::new(
                "read the engine's world",
                "no world is bound to this engine",
            )
            .with_remedy("open a document, which is what binds a world"));
        }
        // SAFETY: non-null, and it belongs to an engine that outlives this borrow.
        Ok(unsafe { crate::world::World::from_raw(self.interface, world) })
    }

    /// The library this engine came from.
    #[must_use]
    pub const fn library(&self) -> &RuntimeLibrary {
        self.library
    }

    /// The raw interface, for the SDK's own use and for a tool that needs an entry the SDK has not
    /// wrapped yet.
    #[must_use]
    pub const fn interface(&self) -> Interface {
        self.interface
    }
}

impl Drop for EmbeddedEngine<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle was produced by this library's creation entry point and has not been
        // destroyed — `EmbeddedEngine` is not `Copy` and `destroy` runs exactly once.
        unsafe { (self.destroy)(self.engine) };
    }
}

/// A session's engine, whichever mode produced it.
///
/// Present so that the services above the SDK name one type rather than branching on the mode: the
/// hosted arm carries no engine at all, because a hosted runtime is reached over a transport and the
/// SDK is not what reaches it.
pub enum Runtime<'library> {
    /// No engine.
    None,
    /// An engine in this process.
    Embedded(EmbeddedEngine<'library>),
}

impl Runtime<'_> {
    /// The hosting mode this runtime represents.
    #[must_use]
    pub const fn mode(&self) -> HostingMode {
        match self {
            Runtime::None => HostingMode::NoRuntime,
            Runtime::Embedded(_) => HostingMode::Embedded,
        }
    }
}

/// Turn a failed table call into a [`Problem`] with a reason and, where one exists, a remedy.
pub(crate) fn call_problem(what: &str, error: crate::generated::interface::CallError) -> Problem {
    use crate::generated::interface::CallError;
    match error {
        CallError::Missing(name) => Problem::new(
            what.to_string(),
            format!("the runtime's interface table has no `{name}` entry"),
        )
        .with_remedy("the runtime is older than this editor; rebuild them from one revision"),
        CallError::Failed(status) => Problem::new(what.to_string(), status.reason().to_string()),
        CallError::UnknownStatus(raw) => Problem::new(
            what.to_string(),
            format!("the engine returned CyResult {raw}, which this editor has no name for"),
        )
        .with_remedy("the runtime is newer than this editor; rebuild them from one revision"),
    }
}

/// Refuse a table this SDK cannot safely call, naming which of the three checks failed.
fn check_header(path: &Path, header: ffi::CyInterfaceHeader) -> Result<()> {
    if header.abi_major != abi::MAJOR {
        return Err(Problem::new(
            format!("talk to {}", path.display()),
            format!(
                "it publishes ABI major {} and this editor was generated against {}",
                header.abi_major,
                abi::MAJOR
            ),
        )
        .with_remedy("rebuild the engine and the editor from the same revision"));
    }
    if header.abi_minor < abi::MINOR {
        return Err(Problem::new(
            format!("talk to {}", path.display()),
            format!(
                "it publishes ABI 1.{} and this editor was generated against 1.{}, so entries the \
                 editor calls are not in its table",
                header.abi_minor,
                abi::MINOR
            ),
        )
        .with_remedy("update the engine, or rebuild the editor against the engine's ABI"));
    }
    if header.table_size < abi::TABLE_SIZE {
        return Err(Problem::new(
            format!("talk to {}", path.display()),
            format!(
                "its interface table is {} bytes and this editor's declaration is {} bytes",
                header.table_size,
                abi::TABLE_SIZE
            ),
        )
        .with_remedy("rebuild the engine and the editor from the same revision"));
    }
    Ok(())
}

/// The dynamic loader, declared rather than depended on.
///
/// Three `extern "C"` declarations against libc, which is linked into every Rust program on these
/// platforms anyway. The alternative is the `libc` crate, and adding a dependency to spell three
/// symbols that have not changed since 1988 is not a trade this workspace makes — see the note on
/// dependencies in `editor/Cargo.toml`.
#[cfg(unix)]
mod platform {
    use super::{CStr, CString, Path, Problem, Result, c_char, c_int, c_void};

    unsafe extern "C" {
        fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
        fn dlerror() -> *mut c_char;
    }

    /// `RTLD_NOW | RTLD_LOCAL`: resolve every symbol at load time rather than discovering a missing
    /// one at the first call, and keep the library's symbols out of the global namespace so that two
    /// runtime images cannot resolve each other's.
    const RTLD_NOW: c_int = 2;
    const RTLD_LOCAL: c_int = 0;

    /// The loader's last message, or a stand-in when it has none.
    fn last_error() -> String {
        // SAFETY: `dlerror` returns either null or a pointer to a NUL-terminated string owned by
        // the loader and valid until the next call on this thread. It is read immediately.
        let message = unsafe { dlerror() };
        if message.is_null() {
            return "the dynamic loader gave no reason".to_string();
        }
        // SAFETY: non-null and NUL-terminated by the loader's contract.
        unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned()
    }

    pub(super) fn open(path: &Path) -> Result<*mut c_void> {
        let text = path.to_str().ok_or_else(|| {
            Problem::new(
                format!("load {}", path.display()),
                "the path is not valid UTF-8, and the dynamic loader takes a C string",
            )
        })?;
        let c_path = CString::new(text).map_err(|_| {
            Problem::new(
                format!("load {}", path.display()),
                "the path contains a NUL byte",
            )
        })?;
        // Clear any stale message so that `last_error` below reports THIS failure. `dlerror` is
        // documented to be sticky until read, and a stale one attached to a fresh failure is the
        // kind of diagnostic that costs an afternoon.
        let _ = last_error();
        // SAFETY: `c_path` is a live NUL-terminated string for the duration of the call.
        let module = unsafe { dlopen(c_path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        if module.is_null() {
            return Err(
                Problem::new(format!("load {}", path.display()), last_error()).with_remedy(
                    "check that the file exists and that its own dependencies resolve",
                ),
            );
        }
        Ok(module)
    }

    /// Resolve a required symbol.
    ///
    /// # Safety
    ///
    /// `F` must be the exact signature the library declares for `name`. Nothing can check this; the
    /// callers are in this file and each says why its cast is the ABI's declared signature.
    pub(super) unsafe fn symbol<F: Copy>(
        module: *mut c_void,
        path: &Path,
        name: &str,
    ) -> Result<F> {
        // SAFETY: the caller's contract, discharged at each call site.
        match unsafe { optional_symbol::<F>(module, name) } {
            Some(function) => Ok(function),
            None => Err(Problem::new(
                format!("load {}", path.display()),
                format!("it exports no `{name}`, so it is not a Cyberdyne engine image"),
            )
            .with_remedy("point at the engine's shared library, not at a module or a game binary")),
        }
    }

    /// Resolve a symbol that may legitimately be absent.
    ///
    /// # Safety
    ///
    /// As [`symbol`].
    pub(super) unsafe fn optional_symbol<F: Copy>(module: *mut c_void, name: &str) -> Option<F> {
        assert_eq!(
            size_of::<F>(),
            size_of::<*mut c_void>(),
            "a symbol can only be transmuted to a pointer-sized function type"
        );
        let c_name = CString::new(name).ok()?;
        // SAFETY: `c_name` is live for the call and `module` came from `open`.
        let address = unsafe { dlsym(module, c_name.as_ptr()) };
        if address.is_null() {
            return None;
        }
        // SAFETY: `address` is a code address the loader resolved, and the caller's contract is
        // that `F` is the signature the library declares for it. The size assertion above is what
        // makes the read well-formed; the signature is what makes the call correct, and only the
        // caller can know that.
        Some(unsafe { *std::ptr::from_ref(&address).cast::<F>() })
    }
}

#[cfg(not(unix))]
mod platform {
    use super::{Path, Problem, Result, c_void};

    pub(super) fn open(path: &Path) -> Result<*mut c_void> {
        Err(Problem::new(
            format!("load {}", path.display()),
            "embedded hosting is implemented for Unix only in this build",
        )
        .with_remedy("use hosted mode, which is the default and is platform independent"))
    }

    /// # Safety
    ///
    /// Unreachable: `open` never returns a module on this platform.
    pub(super) unsafe fn symbol<F: Copy>(
        _module: *mut c_void,
        path: &Path,
        _name: &str,
    ) -> Result<F> {
        Err(Problem::new(
            format!("load {}", path.display()),
            "no library was loaded",
        ))
    }

    /// # Safety
    ///
    /// Unreachable: `open` never returns a module on this platform.
    pub(super) unsafe fn optional_symbol<F: Copy>(_module: *mut c_void, _name: &str) -> Option<F> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hosted_is_the_default_and_the_only_mode_that_survives_a_crash() {
        assert_eq!(HostingMode::default(), HostingMode::Hosted);
        assert!(!HostingMode::Hosted.shares_the_editors_fate());
        assert!(!HostingMode::NoRuntime.shares_the_editors_fate());
        assert!(HostingMode::Embedded.shares_the_editors_fate());
    }

    #[test]
    fn a_mode_round_trips_through_its_name() {
        for mode in [
            HostingMode::NoRuntime,
            HostingMode::Embedded,
            HostingMode::Hosted,
        ] {
            assert_eq!(HostingMode::parse(mode.name()).unwrap(), mode);
        }
    }

    #[test]
    fn an_unknown_mode_names_the_three_that_exist() {
        let problem = HostingMode::parse("in-process").unwrap_err();
        assert_eq!(
            problem.remedy.as_deref(),
            Some("use no-runtime, embedded, or hosted")
        );
    }

    #[test]
    fn a_missing_library_fails_with_the_loaders_reason() {
        let problem = RuntimeLibrary::open("/nonexistent/libcyberdyne.so").unwrap_err();
        assert!(problem.what.contains("libcyberdyne.so"));
        assert!(
            problem.remedy.is_some(),
            "a load failure must say what to check"
        );
    }

    #[test]
    fn a_table_older_than_the_sdk_is_refused_by_name() {
        let older = ffi::CyInterfaceHeader {
            abi_major: abi::MAJOR,
            abi_minor: abi::MINOR,
            abi_patch: 0,
            table_size: abi::TABLE_SIZE - 8,
        };
        let problem = check_header(Path::new("libold.so"), older).unwrap_err();
        assert!(
            problem.because.contains("interface table"),
            "{}",
            problem.because
        );
    }

    #[test]
    fn a_different_major_is_refused_before_anything_is_called() {
        let other = ffi::CyInterfaceHeader {
            abi_major: abi::MAJOR + 1,
            abi_minor: 0,
            abi_patch: 0,
            table_size: abi::TABLE_SIZE,
        };
        let problem = check_header(Path::new("libnext.so"), other).unwrap_err();
        assert!(problem.because.contains("ABI major"), "{}", problem.because);
    }
}
