#include <cy/platform/sdl3_platform.h>

#include "host/host.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <utility>

namespace cy {
namespace {

// SDL reports failure by returning false or null and leaving the reason in SDL_GetError(). That
// string is thread-local and is overwritten by the next SDL call, so it is copied into a bounded
// buffer at the point of failure — an Error holds a pointer, not storage (cy/core/base/error.h).
constexpr usize kSdlErrorCapacity = 256;
thread_local char g_sdl_error[kSdlErrorCapacity];

const char* capture_sdl_error() {
    std::snprintf(g_sdl_error, sizeof(g_sdl_error), "%s", SDL_GetError());
    return g_sdl_error;
}

Unexpected<Error> sdl_failure(ErrorCode code) {
    return Unexpected<Error>(Error{code, capture_sdl_error(), 0});
}

}  // namespace

Sdl3Platform::~Sdl3Platform() {
    if (initialised_) {
        shutdown();
    }
}

void Sdl3Platform::set_application_identity(const char* organisation, const char* application) {
    if (organisation != nullptr) {
        organisation_ = organisation;
    }
    if (application != nullptr) {
        application_ = application;
    }
}

Status Sdl3Platform::initialise(int argument_count, char** arguments) {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the SDL3 platform is already initialised");
    }
    // No subsystems: process services, clocks, paths and subprocesses need the library, not video
    // or audio. The DisplayServer initialises video itself, and SDL reference-counts subsystems, so
    // the two are independent — which is what lets a headless run use this Platform unchanged.
    if (!SDL_Init(0)) {
        return sdl_failure(ErrorCode::Unavailable);
    }

    argument_count_ = argument_count > 0 ? argument_count : 0;
    arguments_ = arguments;
    initialised_ = true;
    return ok();
}

void Sdl3Platform::shutdown() {
    for (ProcessSlot& slot : processes_) {
        if (slot.process != nullptr) {
            SDL_DestroyProcess(static_cast<SDL_Process*>(slot.process));
            slot = ProcessSlot{};
        }
    }
    uninstall_crash_handler();
    if (initialised_) {
        SDL_Quit();
        initialised_ = false;
    }
    argument_count_ = 0;
    arguments_ = nullptr;
}

// --- Process lifetime -------------------------------------------------------------------------

void Sdl3Platform::request_exit(i32 exit_code) {
    // First request wins: a shutdown already under way is not re-coded by a later SIGINT.
    if (!exit_requested_) {
        exit_requested_ = true;
        exit_code_ = exit_code;
    }
}

std::string_view Sdl3Platform::argument(usize index) const {
    if (arguments_ == nullptr || std::cmp_greater_equal(index, argument_count_)) {
        return {};
    }
    const char* value = arguments_[index];
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

// --- Environment ------------------------------------------------------------------------------

Expected<usize, Error> Sdl3Platform::environment_variable(const char* name, char* buffer,
                                                          usize capacity) const {
    if (name == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an environment variable needs a name");
    }
    const char* value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name);
    if (value == nullptr) {
        return fail(ErrorCode::NotFound, "no such environment variable");
    }
    return write_to_buffer(buffer, capacity, value);
}

Status Sdl3Platform::set_environment_variable(const char* name, const char* value) {
    if (name == nullptr || value == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "setting an environment variable needs both a name "
                    "and a value");
    }
    if (!SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value, true)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

// --- Standard streams -------------------------------------------------------------------------

void Sdl3Platform::write_standard_output(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void Sdl3Platform::write_standard_error(std::string_view text) {
    // Unbuffered by convention: an error that is still in a buffer when the process dies was never
    // written at all.
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

// --- Directories ------------------------------------------------------------------------------

Expected<usize, Error> Sdl3Platform::preference_path(const char* suffix, char* buffer,
                                                     usize capacity) const {
    char* base = SDL_GetPrefPath(organisation_, application_);
    if (base == nullptr) {
        return sdl_failure(ErrorCode::Unavailable);
    }

    char path[1024];
    const int written = std::snprintf(path, sizeof(path), "%s%s", base, suffix);
    SDL_free(base);
    if (written < 0 || static_cast<usize>(written) >= sizeof(path)) {
        return fail(ErrorCode::BufferTooSmall, "the user directory path is longer than 1024 bytes");
    }
    return write_to_buffer(buffer, capacity, path);
}

// The three user directories are the engine's own convention: SDL gives one writable directory per
// application, and config and cache are subdirectories of it. Deriving them from the platform's
// separate XDG / AppData / Application Support roots instead would answer differently on each host
// for no benefit — every planned target has exactly one writable mount, and this keeps the whole of
// the engine's writable state under one path a user can delete.
Expected<usize, Error> Sdl3Platform::user_data_directory(char* buffer, usize capacity) const {
    return preference_path("", buffer, capacity);
}

Expected<usize, Error> Sdl3Platform::user_config_directory(char* buffer, usize capacity) const {
    return preference_path("config/", buffer, capacity);
}

Expected<usize, Error> Sdl3Platform::user_cache_directory(char* buffer, usize capacity) const {
    return preference_path("cache/", buffer, capacity);
}

Expected<usize, Error> Sdl3Platform::executable_path(char* buffer, usize capacity) const {
    return host::executable_path(buffer, capacity);
}

// --- Dynamic libraries ------------------------------------------------------------------------

Expected<LibraryHandle, Error> Sdl3Platform::load_library(const char* path) {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "loading a library needs a path");
    }
    SDL_SharedObject* handle = SDL_LoadObject(path);
    if (handle == nullptr) {
        return sdl_failure(ErrorCode::NotFound);
    }
    return static_cast<LibraryHandle>(handle);
}

Expected<void*, Error> Sdl3Platform::library_symbol(LibraryHandle library, const char* symbol) {
    if (library == nullptr || symbol == nullptr) {
        return fail(ErrorCode::InvalidArgument, "resolving a symbol needs a library and a name");
    }
    SDL_FunctionPointer address = SDL_LoadFunction(static_cast<SDL_SharedObject*>(library), symbol);
    if (address == nullptr) {
        return sdl_failure(ErrorCode::NotFound);
    }
    // A function pointer to void*: the round trip is what every dynamic loader's interface is, and
    // the caller casts it back to the signature it asked for.
    return reinterpret_cast<void*>(address);
}

void Sdl3Platform::unload_library(LibraryHandle library) {
    if (library != nullptr) {
        SDL_UnloadObject(static_cast<SDL_SharedObject*>(library));
    }
}

// --- Subprocesses -----------------------------------------------------------------------------

Sdl3Platform::ProcessSlot* Sdl3Platform::find_process(ProcessHandle process) {
    if (process == 0) {
        return nullptr;
    }
    for (ProcessSlot& slot : processes_) {
        if (slot.handle == process) {
            return &slot;
        }
    }
    return nullptr;
}

const Sdl3Platform::ProcessSlot* Sdl3Platform::find_process(ProcessHandle process) const {
    return const_cast<Sdl3Platform*>(this)->find_process(process);
}

Expected<ProcessHandle, Error> Sdl3Platform::spawn_process(const ProcessOptions& options) {
    if (options.arguments == nullptr || options.argument_count == 0) {
        return fail(ErrorCode::InvalidArgument, "spawning a process needs at least argv[0]");
    }

    ProcessSlot* slot = nullptr;
    for (ProcessSlot& candidate : processes_) {
        if (candidate.handle == 0) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "Sdl3Platform::kMaxProcesses subprocesses are already held; release one first");
    }

    // SDL wants a null-terminated argument vector. The caller's array need not be one, so it is
    // copied — the strings are not, and must outlive this call, which is what ProcessOptions says.
    constexpr usize kMaxArguments = 64;
    if (options.argument_count >= kMaxArguments) {
        return fail(ErrorCode::InvalidArgument, "a subprocess takes fewer than 64 arguments");
    }
    const char* argv[kMaxArguments];
    for (usize i = 0; i < options.argument_count; ++i) {
        argv[i] = options.arguments[i];
    }
    argv[options.argument_count] = nullptr;

    // Three states rather than two, and the pipe wins. A caller that asked for a channel to the
    // child and was given the parent's terminal would read the terminal, which is a hang rather
    // than an error.
    const SDL_ProcessIO stdio =
        options.inherit_standard_streams ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL;
    const SDL_ProcessIO channel = options.piped_standard_streams ? SDL_PROCESS_STDIO_APP : stdio;

    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        return sdl_failure(ErrorCode::Internal);
    }
    // SDL takes the vector as `const char * const *`, but the property setter is untyped, so the
    // constness has to be cast away to hand it over. SDL does not write through it.
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           const_cast<void*>(static_cast<const void*>(argv)));
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, channel);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, channel);
    // Standard error stays where the caller put it even when input and output are piped: a child's
    // diagnostics belong in the parent's log, not in a buffer the protocol never drains.
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, stdio);
    if (options.working_directory != nullptr) {
        SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
                              options.working_directory);
    }

    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr) {
        return sdl_failure(ErrorCode::Unavailable);
    }

    slot->handle = next_process_++;
    slot->process = process;
    slot->exited = false;
    slot->exit_code = 0;
    slot->piped = options.piped_standard_streams;
    slot->input_closed = false;
    return slot->handle;
}

Expected<ProcessStatus, Error> Sdl3Platform::poll_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (slot->exited) {
        return ProcessStatus{false, slot->exit_code};
    }

    int exit_code = 0;
    // SDL_WaitProcess with block = false is the non-blocking query: false means still running.
    if (!SDL_WaitProcess(static_cast<SDL_Process*>(slot->process), false, &exit_code)) {
        return ProcessStatus{true, 0};
    }
    slot->exited = true;
    slot->exit_code = exit_code;
    return ProcessStatus{false, exit_code};
}

Expected<i32, Error> Sdl3Platform::wait_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (slot->exited) {
        return slot->exit_code;
    }

    int exit_code = 0;
    if (!SDL_WaitProcess(static_cast<SDL_Process*>(slot->process), true, &exit_code)) {
        return sdl_failure(ErrorCode::Internal);
    }
    slot->exited = true;
    slot->exit_code = exit_code;
    return slot->exit_code;
}

Status Sdl3Platform::terminate_process(ProcessHandle process, bool force) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (slot->exited) {
        return ok();
    }
    if (!SDL_KillProcess(static_cast<SDL_Process*>(slot->process), force)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

void Sdl3Platform::release_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return;
    }
    SDL_DestroyProcess(static_cast<SDL_Process*>(slot->process));
    *slot = ProcessSlot{};
}

Expected<i64, Error> Sdl3Platform::process_id(ProcessHandle process) const {
    const ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    const SDL_PropertiesID properties =
        SDL_GetProcessProperties(static_cast<SDL_Process*>(slot->process));
    if (properties == 0) {
        return sdl_failure(ErrorCode::Unavailable);
    }
    // Zero is never a child's identifier on any platform the engine targets, so it is the sentinel
    // for "SDL did not record one" rather than a value to hand back.
    const Sint64 pid = SDL_GetNumberProperty(properties, SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (pid == 0) {
        return fail(ErrorCode::Unavailable,
                    "this SDL build does not report the operating system's process identifier");
    }
    return static_cast<i64>(pid);
}

// --- Talking to a spawned process -------------------------------------------------------------
//
// Every one of the three refuses on a child whose streams were not piped. The alternative — writing
// into a stream that does not exist and reporting success — is the shape of failure this whole
// module is written against: a caller would drive a child that is not listening and read replies
// that never come.

Expected<usize, Error> Sdl3Platform::write_process_input(ProcessHandle process,
                                                         std::string_view bytes) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported,
                    "this child's standard streams were not piped; spawn it with "
                    "ProcessOptions::piped_standard_streams to write to it");
    }
    if (slot->input_closed) {
        return fail(ErrorCode::Unavailable, "this child's standard input is already closed");
    }
    SDL_IOStream* input = SDL_GetProcessInput(static_cast<SDL_Process*>(slot->process));
    if (input == nullptr) {
        return sdl_failure(ErrorCode::Unavailable);
    }
    if (bytes.empty()) {
        return usize{0};
    }
    const size_t written = SDL_WriteIO(input, bytes.data(), bytes.size());
    if (written == 0) {
        return sdl_failure(ErrorCode::Internal);
    }
    // A pipe buffers, and SDL does not flush on its own. Without this the child blocks reading a
    // request that is sitting in this process's buffer, and the parent blocks reading the reply.
    if (!SDL_FlushIO(input)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return static_cast<usize>(written);
}

Status Sdl3Platform::close_process_input(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported, "this child's standard streams were not piped");
    }
    if (slot->input_closed) {
        return ok();
    }
    SDL_IOStream* input = SDL_GetProcessInput(static_cast<SDL_Process*>(slot->process));
    if (input == nullptr) {
        // SDL hands back null once the stream is gone, which is the state being asked for.
        slot->input_closed = true;
        return ok();
    }
    slot->input_closed = true;
    if (!SDL_CloseIO(input)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Expected<usize, Error> Sdl3Platform::read_process_output(ProcessHandle process, char* buffer,
                                                         usize capacity) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported, "this child's standard streams were not piped");
    }
    if (buffer == nullptr || capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "reading a child's output needs a buffer");
    }
    SDL_IOStream* output = SDL_GetProcessOutput(static_cast<SDL_Process*>(slot->process));
    if (output == nullptr) {
        return sdl_failure(ErrorCode::Unavailable);
    }
    const size_t read = SDL_ReadIO(output, buffer, capacity);
    if (read > 0) {
        return static_cast<usize>(read);
    }
    // Zero means one of three things and they are not interchangeable: end of stream is the child
    // having closed its output, NOT_READY cannot happen on a blocking pipe but is answered as "read
    // again" rather than as an end, and anything else is a failure that must not read as a quiet
    // child.
    const SDL_IOStatus status = SDL_GetIOStatus(output);
    if (status == SDL_IO_STATUS_EOF) {
        return usize{0};
    }
    if (status == SDL_IO_STATUS_NOT_READY) {
        return fail(ErrorCode::Unavailable, "the child has not written anything yet");
    }
    return sdl_failure(ErrorCode::Internal);
}

// --- Clocks -----------------------------------------------------------------------------------
//
// Two clocks, two sources. SDL_GetTicksNS() counts from SDL_Init() on the operating system's
// monotonic clock — CLOCK_MONOTONIC, QueryPerformanceCounter, mach_absolute_time — none of which
// the system clock's setting can move. SDL_GetCurrentTime() reads the wall clock, which it can.
// That is the specification's scenario: adjusting the system clock leaves frame timing unaffected,
// because frame timing never reads the second function.

Nanoseconds Sdl3Platform::monotonic_nanoseconds() const {
    return static_cast<Nanoseconds>(SDL_GetTicksNS());
}

i64 Sdl3Platform::wall_nanoseconds() const {
    SDL_Time now = 0;
    if (!SDL_GetCurrentTime(&now)) {
        return 0;
    }
    return static_cast<i64>(now);
}

// --- Locale, CPU, memory ----------------------------------------------------------------------

Expected<usize, Error> Sdl3Platform::locale(char* buffer, usize capacity) const {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (locales == nullptr || count == 0) {
        SDL_free(static_cast<void*>(locales));
        return fail(ErrorCode::Unavailable, "the host reports no preferred locale");
    }

    char tag[64];
    const SDL_Locale* preferred = locales[0];
    if (preferred->country != nullptr) {
        std::snprintf(tag, sizeof(tag), "%s-%s", preferred->language, preferred->country);
    } else {
        std::snprintf(tag, sizeof(tag), "%s", preferred->language);
    }
    SDL_free(static_cast<void*>(locales));
    return write_to_buffer(buffer, capacity, tag);
}

u32 Sdl3Platform::cpu_count() const {
    const int count = SDL_GetNumLogicalCPUCores();
    return count > 0 ? static_cast<u32>(count) : 1U;
}

CpuFeatures Sdl3Platform::cpu_features() const {
    CpuFeatures features;
    features.sse42 = SDL_HasSSE42();
    features.avx = SDL_HasAVX();
    features.avx2 = SDL_HasAVX2();
    features.avx512f = SDL_HasAVX512F();
    features.neon = SDL_HasNEON();
    return features;
}

Expected<MemoryStatistics, Error> Sdl3Platform::memory_statistics() const {
    MemoryStatistics statistics;
    const int megabytes = SDL_GetSystemRAM();
    if (megabytes > 0) {
        statistics.total_physical_bytes = static_cast<u64>(megabytes) * 1024ULL * 1024ULL;
    }
    // A host that cannot report the other two leaves them at zero rather than failing: the total is
    // still worth having, and "unknown" is what zero means here.
    (void)host::refine_memory_statistics(statistics);
    return statistics;
}

// --- Crash handling ---------------------------------------------------------------------------

Status Sdl3Platform::install_crash_handler(CrashHandler handler, void* user) {
    if (handler == nullptr) {
        return fail(ErrorCode::InvalidArgument, "installing a crash handler needs a handler");
    }
    return host::install_crash_handler(handler, user);
}

void Sdl3Platform::uninstall_crash_handler() {
    host::uninstall_crash_handler();
}

}  // namespace cy
