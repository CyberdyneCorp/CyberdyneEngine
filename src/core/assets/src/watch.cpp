// The filesystem watcher. See cy/core/assets/watch.h for why it polls and why it fingerprints.

#include <cy/core/assets/watch.h>

namespace cy::assets {

const char* file_change_name(FileChange change) noexcept {
    switch (change) {
        case FileChange::Added:
            return "added";
        case FileChange::Modified:
            return "modified";
        case FileChange::Removed:
            return "removed";
    }
    return "unknown";
}

FileWatcher::FileWatcher(Allocator& allocator) noexcept
    : allocator_(&allocator), roots_(allocator), tracked_(allocator), scratch_(allocator) {}

Status FileWatcher::start(VirtualFileSystem& files, const FileWatcherConfig& config) noexcept {
    if (config.settle_ns < 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a negative settle period would report a change before it happened");
    }
    files_ = &files;
    config_ = config;
    primed_ = false;
    return ok();
}

void FileWatcher::stop() noexcept {
    files_ = nullptr;
    roots_.clear();
    tracked_.clear();
    scratch_.clear();
    primed_ = false;
}

Status FileWatcher::watch(const VirtualPath& path) noexcept {
    if (files_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the watcher has not been started");
    }
    for (const VirtualPath& existing : roots_) {
        if (existing == path) {
            return ok();  // Idempotent: watching twice watches once.
        }
    }
    return roots_.push_back(path);
}

Status FileWatcher::unwatch(const VirtualPath& path) noexcept {
    usize found = roots_.size();
    for (usize index = 0; index < roots_.size(); ++index) {
        if (roots_[index] == path) {
            found = index;
            break;
        }
    }
    if (found == roots_.size()) {
        return fail(ErrorCode::NotFound, "that path is not watched");
    }

    // Drop what this root brought in, and renumber the entries of the roots after it. An index
    // rather than a copy of the path in every entry, because a VirtualPath is 256 bytes and a
    // tracked entry already carries one.
    // A two-index compaction, so it is a while rather than a range-for: `read` and `write` advance
    // at different rates, which is the whole shape of dropping entries in place. `Tracked` is
    // trivially copyable, so the assignments below are copies and want no std::move.
    usize write = 0;
    usize read = 0;
    while (read < tracked_.size()) {
        Tracked entry = tracked_[read];
        ++read;
        if (entry.root == found) {
            continue;
        }
        if (entry.root > found) {
            --entry.root;
        }
        tracked_[write] = entry;
        ++write;
    }
    while (tracked_.size() > write) {
        tracked_.pop_back();
    }
    roots_.erase(found);
    return ok();
}

usize FileWatcher::find_tracked(const VirtualPath& path) const noexcept {
    usize low = 0;
    usize high = tracked_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (tracked_[middle].path < path) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

Expected<FileWatcher::Fingerprint, Error> FileWatcher::fingerprint_of(const VirtualPath& path,
                                                                      u64 size) noexcept {
    Fingerprint print;
    print.size = size;
    if (size > config_.max_fingerprint_bytes) {
        // Size only. Recorded rather than silent: a caller that finds a same-size edit missed can
        // read the counter and raise the ceiling instead of doubting the watcher.
        ++stats_.size_only;
        return print;
    }
    if (Status read = files_->read(path, scratch_); !read) {
        return make_unexpected(read.error());
    }
    stats_.bytes_read += scratch_.size();
    print.size = scratch_.size();
    print.hash = content_hash(scratch_.data(), scratch_.size());
    return print;
}

Status FileWatcher::observe(const VirtualPath& path, u32 root, u64 size, i64 now_ns) noexcept {
    ++stats_.paths_examined;

    const usize at = find_tracked(path);
    const bool known = at < tracked_.size() && tracked_[at].path == path;

    Expected<Fingerprint, Error> print = fingerprint_of(path, size);
    if (!print) {
        // A file that cannot be read right now is not a change: it is most often a file being
        // written. Left as it was, so the next poll sees it settle or disappear.
        if (known) {
            tracked_[at].present = true;
        }
        return ok();
    }

    if (!known) {
        Tracked entry;
        entry.path = path;
        entry.root = root;
        entry.present = true;
        if (primed_) {
            entry.is_new = true;
            // A new path settles before it is reported, exactly as a modified one does — a file
            // being created is written incrementally too, and an `Added` event carrying the first
            // four kilobytes of a shader is worse than one that arrives a poll later. Its committed
            // fingerprint stays empty until the settle, so nothing reads a size it does not have.
            entry.settling = true;
            entry.settling_since_ns = now_ns;
            entry.settling_size = print->size;
            entry.settling_hash = print->hash;
            ++stats_.settling;
        } else {
            // The baseline poll. What is already there is recorded as it is and reported as
            // nothing: it settles nowhere, because there is no change to settle — the file existed
            // before the watcher did.
            entry.size = print->size;
            entry.hash = print->hash;
        }
        if (Status made = tracked_.push_back(Tracked{}); !made) {
            return made;
        }
        for (usize index = tracked_.size() - 1; index > at; --index) {
            tracked_[index] = tracked_[index - 1];
        }
        tracked_[at] = entry;
        return ok();
    }

    Tracked& entry = tracked_[at];
    entry.present = true;
    if (entry.size == print->size && entry.hash == print->hash) {
        // Unchanged. A settle that was in progress is abandoned: the file went back to what it was,
        // which is what an editor's save-then-undo looks like from here.
        entry.settling = false;
        return ok();
    }

    if (!entry.settling || entry.settling_size != print->size ||
        !(entry.settling_hash == print->hash)) {
        // The first sighting of this new content, or a different new content from the one that was
        // settling. Either way the clock starts now.
        entry.settling = true;
        entry.settling_since_ns = now_ns;
        entry.settling_size = print->size;
        entry.settling_hash = print->hash;
        ++stats_.settling;
    }
    return ok();
}

bool FileWatcher::visit_entry(void* user, const VirtualEntry& entry) noexcept {
    auto* sweep = static_cast<Sweep*>(user);
    if (entry.is_directory || entry.path == nullptr) {
        return true;
    }
    sweep->status = sweep->watcher->observe(*entry.path, sweep->root, entry.size, sweep->now_ns);
    return static_cast<bool>(sweep->status);
}

Status FileWatcher::prime(i64 now_ns) noexcept {
    Expected<u32, Error> primed = poll(now_ns, nullptr, nullptr);
    if (!primed) {
        return make_unexpected(primed.error());
    }
    return ok();
}

Expected<u32, Error> FileWatcher::poll(i64 now_ns, WatchObserver observer, void* user) noexcept {
    if (files_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the watcher has not been started");
    }
    ++stats_.polls;

    for (Tracked& entry : tracked_) {
        entry.present = false;
    }

    // The sweep. A watched path is either a file, which is examined directly, or a directory, whose
    // files are enumerated recursively; `exists` distinguishes them without the caller having said
    // which it meant, because a path that is a file today can be a directory tomorrow.
    for (u32 index = 0; index < static_cast<u32>(roots_.size()); ++index) {
        const VirtualPath& root = roots_[index];
        Expected<VirtualFileSystem::Resolution, Error> resolved = files_->resolve(root);
        if (resolved) {
            if (Status seen = observe(root, index, resolved->size, now_ns); !seen) {
                return make_unexpected(seen.error());
            }
            continue;
        }
        Sweep sweep;
        sweep.watcher = this;
        sweep.root = index;
        sweep.now_ns = now_ns;
        // A directory that does not exist enumerates to nothing, which is the right answer: every
        // path it held is reported as removed by the pass below.
        (void)files_->enumerate(root, true, &FileWatcher::visit_entry, &sweep);
        if (!sweep.status) {
            return make_unexpected(sweep.status.error());
        }
    }

    // The report, in path order, which is `tracked_`'s own order — so two runs over the same change
    // report it in the same sequence whatever order the mounts enumerated in.
    u32 reported = 0;
    const bool report = primed_ && observer != nullptr;
    usize write = 0;
    usize read = 0;
    while (read < tracked_.size()) {
        Tracked entry = tracked_[read];
        ++read;

        if (!entry.present) {
            if (report) {
                WatchEvent event;
                event.path = &entry.path;
                event.change = FileChange::Removed;
                event.root = &roots_[entry.root];
                observer(user, event);
                ++reported;
            }
            ++stats_.removed;
            continue;  // Dropped: what is gone is no longer tracked.
        }

        const bool settled =
            entry.settling && (now_ns - entry.settling_since_ns) >= config_.settle_ns;
        if (settled) {
            const bool added = entry.is_new;
            entry.size = entry.settling_size;
            entry.hash = entry.settling_hash;
            entry.settling = false;
            entry.is_new = false;
            if (report) {
                WatchEvent event;
                event.path = &entry.path;
                event.change = added ? FileChange::Added : FileChange::Modified;
                event.size = entry.size;
                event.hash = entry.hash;
                event.root = &roots_[entry.root];
                observer(user, event);
                ++reported;
            }
            if (added) {
                ++stats_.added;
            } else {
                ++stats_.modified;
            }
        }

        tracked_[write] = entry;
        ++write;
    }
    while (tracked_.size() > write) {
        tracked_.pop_back();
    }

    // The first poll is the baseline: it records what is there and reports none of it, so a watch
    // added over an existing tree does not open with a hundred Added events for files nobody
    // touched. Set at the end so that `report` above saw the state this poll ran under.
    primed_ = true;
    return reported;
}

}  // namespace cy::assets
