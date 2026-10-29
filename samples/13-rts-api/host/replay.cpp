// SPDX-License-Identifier: MIT
#include "replay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace sample::rts {
namespace {

using cy::u64;

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { (void)std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

[[nodiscard]] cy::Status malformed(const char* what) noexcept {
    return cy::fail(cy::ErrorCode::InvalidArgument, what);
}

/// The next whitespace-separated number of `cursor`, moving `cursor` past it; false when there is
/// none or it does not end at a space or the line's end. `base` is strtoull's.
[[nodiscard]] bool next_unsigned(const char*& cursor, int base, unsigned long long& out) noexcept {
    char* end = nullptr;
    out = std::strtoull(cursor, &end, base);
    const bool read = end != cursor && (*end == ' ' || *end == '\n' || *end == '\0');
    cursor = end;
    return read;
}

[[nodiscard]] bool next_signed(const char*& cursor, long long& out) noexcept {
    char* end = nullptr;
    out = std::strtoll(cursor, &end, 10);
    const bool read = end != cursor && (*end == ' ' || *end == '\n' || *end == '\0');
    cursor = end;
    return read;
}

/// One `order` line's five numbers into `out`.
[[nodiscard]] bool parse_order(const char* line, ReplayOrder& out) noexcept {
    constexpr const char* kOrder = "order ";
    if (std::strncmp(line, kOrder, std::strlen(kOrder)) != 0) {
        return false;
    }
    const char* cursor = line + std::strlen(kOrder);
    unsigned long long tick = 0;
    unsigned long long kind = 0;
    unsigned long long group = 0;
    long long x = 0;
    long long z = 0;
    if (!next_unsigned(cursor, 10, tick) || !next_unsigned(cursor, 10, kind) ||
        !next_unsigned(cursor, 10, group) || !next_signed(cursor, x) || !next_signed(cursor, z) ||
        kind > 0xFFFF'FFFFULL || group > 0xFFFF'FFFFULL) {
        return false;
    }
    out.tick = tick;
    out.order.kind = static_cast<cy::u32>(kind);
    out.order.group = static_cast<cy::u32>(group);
    out.order.target_x = x;
    out.order.target_z = z;
    return true;
}

/// A header line, `<key> <value>`, into `out`; the value hexadecimal when `hex`.
[[nodiscard]] bool parse_header(const char* line, const char* key, bool hex, u64& out) noexcept {
    const std::size_t length = std::strlen(key);
    if (std::strncmp(line, key, length) != 0 || line[length] != ' ') {
        return false;
    }
    const char* cursor = line + length + 1;
    unsigned long long value = 0;
    if (!next_unsigned(cursor, hex ? 16 : 10, value)) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

cy::Expected<ReplayLog, cy::Error> capture_replay(const cy::game_backend::LockstepSession& session,
                                                  u64 follower_digest) noexcept {
    ReplayLog log;
    log.ticks = session.tick();
    log.digest = session.digest();
    log.follower_digest = follower_digest;
    const cy::gameplay::CommandLog& commands = session.log();
    log.orders.reserve(commands.size());
    for (cy::u32 index = 0; index < commands.size(); ++index) {
        const cy::gameplay::Command& command = commands.at(index);
        if (command.payload_size != sizeof(cy::game_backend::LockstepOrderPayload)) {
            return cy::make_unexpected(
                cy::Error{cy::ErrorCode::Internal, "a lockstep command that is not an order"});
        }
        ReplayOrder order;
        order.tick = command.tick;
        std::memcpy(&order.order, command.payload, sizeof(order.order));
        log.orders.push_back(order);
    }
    return log;
}

cy::Status write_replay(const std::string& path, const ReplayLog& log) noexcept {
    const File file(std::fopen(path.c_str(), "wb"));
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::Io, "the replay file could not be created");
    }
    std::FILE* out = file.get();
    std::fprintf(out, "cyrts-replay 1\nticks %llu\ndigest %016llx\nfollower %016llx\n",
                 static_cast<unsigned long long>(log.ticks),
                 static_cast<unsigned long long>(log.digest),
                 static_cast<unsigned long long>(log.follower_digest));
    for (const ReplayOrder& order : log.orders) {
        std::fprintf(out, "order %llu %u %u %lld %lld\n",
                     static_cast<unsigned long long>(order.tick), order.order.kind,
                     order.order.group, static_cast<long long>(order.order.target_x),
                     static_cast<long long>(order.order.target_z));
    }
    std::fprintf(out, "end\n");
    return std::ferror(out) == 0
               ? cy::ok()
               : cy::fail(cy::ErrorCode::Io, "the replay file could not be written");
}

cy::Status read_replay(const std::string& path, ReplayLog& out) noexcept {
    const File file(std::fopen(path.c_str(), "rb"));
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "the replay file could not be opened");
    }
    out = ReplayLog{};
    char line[256];
    if (std::fgets(line, sizeof(line), file.get()) == nullptr ||
        std::strncmp(line, "cyrts-replay 1", 14) != 0) {
        return malformed("not a replay: the first line is not 'cyrts-replay 1'");
    }
    bool ended = false;
    while (!ended && std::fgets(line, sizeof(line), file.get()) != nullptr) {
        ReplayOrder order;
        if (std::strncmp(line, "end", 3) == 0) {
            ended = true;
        } else if (parse_order(line, order)) {
            if (!out.orders.empty() && order.tick < out.orders.back().tick) {
                return malformed("a replay's orders go back in time");
            }
            out.orders.push_back(order);
        } else if (!parse_header(line, "ticks", false, out.ticks) &&
                   !parse_header(line, "digest", true, out.digest) &&
                   !parse_header(line, "follower", true, out.follower_digest)) {
            return malformed("a replay line is none of ticks, digest, follower, order or end");
        }
    }
    // A file cut short is refused rather than replayed as a shorter game.
    return ended ? cy::ok() : malformed("a replay that does not end with 'end' was cut short");
}

cy::Status feed_replay(const ReplayLog& log, cy::usize& cursor,
                       cy::game_backend::LockstepSession& session) noexcept {
    const u64 tick = session.tick();
    for (; cursor < log.orders.size() && log.orders[cursor].tick <= tick; ++cursor) {
        if (log.orders[cursor].tick < tick) {
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "a replay order for a tick the session has already run");
        }
        if (cy::Status recorded = session.record(log.orders[cursor].order); !recorded) {
            return recorded;
        }
    }
    return cy::ok();
}

}  // namespace sample::rts
