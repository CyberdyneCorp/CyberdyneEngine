// The intern table behind `cy::Name`. Task 1.3.5.
//
// One mutex, one hash map from text to index, one vector from index to text, and text stored in
// chunks that are never moved and never freed. The map's keys are `std::string_view`s into that
// chunk storage, so a key costs a pointer and a length rather than a second copy of the text.
//
// LIFETIME, AND WHY THIS IS NOT A LEAK. The table is allocated once with `new` and never destroyed.
// A `Name` obtained anywhere must still resolve during static destruction — a destructor that logs
// a name is ordinary — and a table destroyed at exit would make that a use-after-free that only
// appears on shutdown. The pointer is held in a namespace-scope variable, which is a root a leak
// detector scans: the allocation is reachable at exit and is reported as live, not as leaked. That
// is the declaration `testing-and-quality` asks for, made where the detector can see it rather than
// in a suppression file.

#include <cy/core/values/name.h>

#include "counters.h"

#include <cstring>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cy {
namespace {

/// Text is bump-allocated out of chunks. A chunk is never reallocated, so every `std::string_view`
/// handed out stays valid; an entry longer than the chunk gets a chunk of its own.
constexpr usize kChunkBytes = 64 * 1024;

struct Entry {
    const char* text;
    u32 length;
};

class NameTable {
public:
    NameTable() {
        // Index 0 is the empty name, interned before anything else so that a default-constructed
        // Name resolves to "" rather than to whatever was interned first.
        entries_.push_back(Entry{"", 0});
        index_.emplace(std::string_view{"", 0}, 0u);
    }

    u32 intern(std::string_view text, bool create) noexcept {
        values::detail::bump(values::detail::counters().name_lookups);
        if (text.size() > Name::kMaxLength) {
            values::detail::bump(values::detail::counters().name_rejections);
            return 0;
        }

        const std::lock_guard<std::mutex> guard(mutex_);
        if (const auto found = index_.find(text); found != index_.end()) {
            return found->second;
        }
        if (!create) {
            return 0;
        }

        const char* stored = store(text);
        if (stored == nullptr) {
            values::detail::bump(values::detail::counters().name_rejections);
            return 0;
        }

        const auto assigned = static_cast<u32>(entries_.size());
        entries_.push_back(Entry{stored, static_cast<u32>(text.size())});
        index_.emplace(std::string_view{stored, text.size()}, assigned);
        values::detail::bump(values::detail::counters().name_insertions);
        return assigned;
    }

    std::string_view text(u32 index) const noexcept {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (index >= entries_.size()) {
            return {};
        }
        const Entry& entry = entries_[index];
        return std::string_view{entry.text, entry.length};
    }

    NameTableStats stats() const noexcept {
        NameTableStats out;
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            out.entries = static_cast<u32>(entries_.size());
            out.bytes = bytes_used_;
        }
        const values::detail::Counters& c = values::detail::counters();
        out.lookups = c.name_lookups.load(std::memory_order_relaxed);
        out.insertions = c.name_insertions.load(std::memory_order_relaxed);
        out.rejections = c.name_rejections.load(std::memory_order_relaxed);
        return out;
    }

private:
    /// Copy `text` into chunk storage with a trailing NUL, so `c_str()` needs no second copy.
    /// Called with the mutex held. Returns nullptr when the allocation fails, which under
    /// -fno-exceptions is the only way `new` reports it.
    const char* store(std::string_view text) noexcept {
        const usize needed = text.size() + 1;
        if (chunk_ == nullptr || chunk_used_ + needed > chunk_size_) {
            const usize size = needed > kChunkBytes ? needed : kChunkBytes;
            char* chunk = new (std::nothrow) char[size];
            if (chunk == nullptr) {
                return nullptr;
            }
            chunks_.push_back(chunk);
            chunk_ = chunk;
            chunk_size_ = size;
            chunk_used_ = 0;
        }
        char* destination = chunk_ + chunk_used_;
        std::memcpy(destination, text.data(), text.size());
        destination[text.size()] = '\0';
        chunk_used_ += needed;
        bytes_used_ += needed;
        return destination;
    }

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string_view, u32> index_;
    std::vector<char*> chunks_;
    char* chunk_ = nullptr;
    usize chunk_size_ = 0;
    usize chunk_used_ = 0;
    u64 bytes_used_ = 0;
};

// The process-lifetime root described at the top of this file. Namespace scope so the pointer is a
// root the leak detector scans; `table()` constructs it once, under a call_once so that two threads
// racing to intern their first name do not each build one.
NameTable* g_table = nullptr;
std::once_flag g_table_once;

NameTable& table() noexcept {
    std::call_once(g_table_once, []() noexcept { g_table = new (std::nothrow) NameTable(); });
    // A failed allocation here is not recoverable: every caller of intern() returns a Name by
    // value and there is no error channel. It is also not survivable — the process could not
    // allocate 200 bytes at startup — so the null dereference that follows is the honest outcome
    // rather than a silent wrong answer.
    return *g_table;
}

}  // namespace

Name Name::intern(std::string_view text) noexcept {
    return Name::from_index(table().intern(text, /*create=*/true));
}

Name Name::find(std::string_view text) noexcept {
    return Name::from_index(table().intern(text, /*create=*/false));
}

std::string_view Name::text() const noexcept {
    return table().text(index_);
}

const char* Name::c_str() const noexcept {
    const std::string_view view = table().text(index_);
    // Every entry is stored NUL-terminated, and index 0 is the literal "". A view whose data is
    // null cannot happen, but an out-of-range index yields one, so it is answered rather than
    // dereferenced.
    return view.data() != nullptr ? view.data() : "";
}

NameTableStats name_table_stats() noexcept {
    return table().stats();
}

}  // namespace cy
