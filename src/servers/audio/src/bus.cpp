// The bus graph: creation, routing, cycle rejection, solo, and the compiled processing order.
// See cy/servers/audio/bus.h.

#include <cy/servers/audio/bus.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <algorithm>

namespace cy::audio {
namespace {

/// Slot 0 is `Master`, created by the constructor, and is the only bus that never has an output.
constexpr u32 kMasterIndex = 0;

}  // namespace

const char* effect_kind_name(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Gain:
            return "gain";
        case EffectKind::LowPass:
            return "low-pass";
        case EffectKind::HighPass:
            return "high-pass";
        case EffectKind::Limiter:
            return "limiter";
        case EffectKind::Count:
            break;
    }
    return "unknown";
}

BusGraph::BusGraph(Allocator& allocator) noexcept : buses_(allocator), order_(allocator) {
    Bus master;
    master.description.name = Name::intern("Master");
    master.live = true;
    master.generation = 1;
    master.handle = BusHandle::from_slot(kMasterIndex, 1);
    // A failed reserve here would leave a graph with no Master, which nothing above could use. The
    // allocation is one element and the constructor cannot report — so the invariant is asserted
    // and `create()` reports every failure after it.
    const Status pushed = buses_.push_back(master);
    CY_ASSERT_MSG(static_cast<bool>(pushed), "the Master bus could not be allocated");
    master_ = master.handle;
}

BusGraph::Bus* BusGraph::find(BusHandle handle) noexcept {
    return const_cast<Bus*>(static_cast<const BusGraph*>(this)->find(handle));
}

const BusGraph::Bus* BusGraph::find(BusHandle handle) const noexcept {
    const u32 index = handle.index();
    if (index >= buses_.size()) {
        return nullptr;
    }
    const Bus& bus = buses_[index];
    // The generation is what makes a handle held across a destroy answer "no" rather than resolve
    // to whichever bus took the slot.
    if (!bus.live || bus.generation != handle.generation()) {
        return nullptr;
    }
    return &bus;
}

u32 BusGraph::index_of(BusHandle handle) const noexcept {
    return (find(handle) == nullptr) ? kInvalidIndex : handle.index();
}

BusHandle BusGraph::handle_at(u32 index) const noexcept {
    if (index >= buses_.size() || !buses_[index].live) {
        return BusHandle{};
    }
    return buses_[index].handle;
}

bool BusGraph::alive(BusHandle handle) const noexcept {
    return find(handle) != nullptr;
}

Expected<BusHandle, Error> BusGraph::create(const BusDescription& description) noexcept {
    // Reuse a dead slot before growing, so a graph that creates and destroys buses does not grow
    // without bound and so the dense index the mixer's buffers are sized by stays dense.
    for (usize i = 0; i < buses_.size(); ++i) {
        Bus& bus = buses_[i];
        if (bus.live) {
            continue;
        }
        ++bus.generation;
        bus.live = true;
        bus.description = description;
        bus.send_count = 0;
        bus.effect_count = 0;
        bus.handle = BusHandle::from_slot(static_cast<u32>(i), bus.generation);
        compiled_ = false;
        return bus.handle;
    }

    Bus bus;
    bus.description = description;
    bus.live = true;
    bus.generation = 1;
    bus.handle = BusHandle::from_slot(static_cast<u32>(buses_.size()), 1);
    if (Status pushed = buses_.push_back(bus); !pushed) {
        return make_unexpected(pushed.error());
    }
    compiled_ = false;
    return bus.handle;
}

Status BusGraph::destroy(BusHandle handle) noexcept {
    if (handle == master_) {
        return fail(ErrorCode::InvalidArgument,
                    "the Master bus is the root and is not destroyable");
    }
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    // A bus that something still routes into cannot be destroyed: the routing would dangle and the
    // submix would silently vanish. Reported naming the problem rather than repaired by rerouting,
    // because rerouting to Master is a decision a sound designer makes, not one a destroy makes.
    for (const Bus& other : buses_) {
        if (!other.live || other.handle == handle) {
            continue;
        }
        if (other.description.output == handle) {
            return fail(ErrorCode::InvalidArgument,
                        "another bus still routes its output into this one");
        }
        for (u32 i = 0; i < other.send_count; ++i) {
            if (other.send_storage[i].target == handle) {
                return fail(ErrorCode::InvalidArgument, "another bus still sends into this one");
            }
        }
    }
    bus->live = false;
    compiled_ = false;
    return ok();
}

const BusDescription* BusGraph::description(BusHandle handle) const noexcept {
    const Bus* bus = find(handle);
    return (bus == nullptr) ? nullptr : &bus->description;
}

Status BusGraph::set_volume(BusHandle handle, f32 volume) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    bus->description.volume = math::max(volume, 0.0F);
    return ok();
}

Status BusGraph::set_mute(BusHandle handle, bool mute) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    bus->description.mute = mute;
    return ok();
}

Status BusGraph::set_solo(BusHandle handle, bool solo) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    bus->description.solo = solo;
    return ok();
}

Status BusGraph::set_bypass(BusHandle handle, bool bypass) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    bus->description.bypass = bypass;
    return ok();
}

bool BusGraph::reaches(BusHandle from, BusHandle to) const noexcept {
    if (from == to) {
        return true;
    }
    const Bus* bus = find(from);
    if (bus == nullptr) {
        return false;
    }
    // Depth first over outputs and sends. Recursive, and bounded by the graph being acyclic — which
    // holds because this function is what refuses every edge that would break it.
    const Outgoing edges = outgoing_of(*bus);
    return std::ranges::any_of(
        edges, [this, to](const BusHandle target) noexcept { return reaches(target, to); });
}

BusGraph::Outgoing BusGraph::outgoing_of(const Bus& bus) noexcept {
    // The output edge and every send edge, in one iterable, because every walk of this graph — the
    // cycle check, the in-degree count, the topological emit — treats them identically. Three
    // copies of "the output, then the sends" is three places for one of them to forget the output.
    Outgoing edges;
    if (!bus.description.output.is_null()) {
        edges.storage[edges.count++] = bus.description.output;
    }
    for (u32 i = 0; i < bus.send_count; ++i) {
        edges.storage[edges.count++] = bus.send_storage[i].target;
    }
    return edges;
}

Status BusGraph::set_output(BusHandle handle, BusHandle output) noexcept {
    if (handle == master_) {
        return fail(ErrorCode::InvalidArgument,
                    "the Master bus is the root and routes nowhere; nothing is above it");
    }
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    if (!output.is_null()) {
        if (find(output) == nullptr) {
            return fail(ErrorCode::NotFound, "no such output bus");
        }
        // THE CYCLE CHECK. Asked before the edge exists: if the prospective output already reaches
        // this bus, adding the edge would close a loop, and a loop in a mixing graph hangs the
        // realtime thread rather than sounding wrong. See the header comment.
        if (reaches(output, handle)) {
            return fail(ErrorCode::InvalidArgument,
                        "routing this bus there would create a cycle in the mixing graph");
        }
    }
    bus->description.output = output;
    compiled_ = false;
    return ok();
}

Status BusGraph::add_send(BusHandle from, BusHandle to, f32 level) noexcept {
    Bus* bus = find(from);
    if (bus == nullptr || find(to) == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    if (from == to) {
        return fail(ErrorCode::InvalidArgument, "a bus cannot send to itself");
    }
    if (reaches(to, from)) {
        return fail(ErrorCode::InvalidArgument,
                    "this send would create a cycle in the mixing graph");
    }
    if (bus->send_count >= kMaxSends) {
        return fail(ErrorCode::OutOfRange, "this bus already has the maximum number of sends");
    }
    for (u32 i = 0; i < bus->send_count; ++i) {
        if (bus->send_storage[i].target == to) {
            // Two sends to one target are one send with the sum of their levels, and keeping both
            // would make the resulting level depend on the order they were added in.
            return fail(ErrorCode::AlreadyExists, "this bus already sends to that one");
        }
    }
    bus->send_storage[bus->send_count] = BusSend{to, math::max(level, 0.0F)};
    ++bus->send_count;
    compiled_ = false;
    return ok();
}

Status BusGraph::set_send_level(BusHandle from, BusHandle to, f32 level) noexcept {
    Bus* bus = find(from);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    for (u32 i = 0; i < bus->send_count; ++i) {
        if (bus->send_storage[i].target == to) {
            bus->send_storage[i].level = math::max(level, 0.0F);
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "no such send");
}

Status BusGraph::remove_send(BusHandle from, BusHandle to) noexcept {
    Bus* bus = find(from);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    for (u32 i = 0; i < bus->send_count; ++i) {
        if (bus->send_storage[i].target != to) {
            continue;
        }
        for (u32 j = i + 1; j < bus->send_count; ++j) {
            bus->send_storage[j - 1] = bus->send_storage[j];
        }
        --bus->send_count;
        compiled_ = false;
        return ok();
    }
    return fail(ErrorCode::NotFound, "no such send");
}

Span<const BusSend> BusGraph::sends(BusHandle handle) const noexcept {
    const Bus* bus = find(handle);
    if (bus == nullptr) {
        return Span<const BusSend>{};
    }
    return {bus->send_storage, bus->send_count};
}

Status BusGraph::add_effect(BusHandle handle, const BusEffect& effect) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    if (bus->effect_count >= kMaxEffects) {
        return fail(ErrorCode::OutOfRange, "this bus already has the maximum number of effects");
    }
    bus->effect_storage[bus->effect_count] = effect;
    ++bus->effect_count;
    return ok();
}

Status BusGraph::clear_effects(BusHandle handle) noexcept {
    Bus* bus = find(handle);
    if (bus == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    bus->effect_count = 0;
    return ok();
}

Span<const BusEffect> BusGraph::effects(BusHandle handle) const noexcept {
    const Bus* bus = find(handle);
    if (bus == nullptr) {
        return Span<const BusEffect>{};
    }
    return {bus->effect_storage, bus->effect_count};
}

u32 BusGraph::latency_frames(BusHandle handle) const noexcept {
    const Bus* bus = find(handle);
    if (bus == nullptr) {
        return 0;
    }
    u32 total = 0;
    for (u32 i = 0; i < bus->effect_count; ++i) {
        if (!bus->effect_storage[i].bypass) {
            total += bus->effect_storage[i].latency_frames;
        }
    }
    if (!bus->description.output.is_null()) {
        total += latency_frames(bus->description.output);
    }
    return total;
}

namespace {

/// The marker a slot carries once it has been emitted, so its remaining count is never decremented
/// again. Distinct from any real in-degree because a bus has fewer than four billion sources.
constexpr u32 kEmitted = 0xFFFFFFFFU;

}  // namespace

void BusGraph::count_incoming(Array<u32>& remaining) const noexcept {
    for (u32& count : remaining) {
        count = 0;
    }
    for (const Bus& bus : buses_) {
        if (!bus.live) {
            continue;
        }
        for (const BusHandle target : outgoing_of(bus)) {
            const u32 index = index_of(target);
            if (index != kInvalidIndex) {
                ++remaining[index];
            }
        }
    }
}

void BusGraph::release_outgoing(const Bus& bus, Array<u32>& remaining) const noexcept {
    for (const BusHandle target : outgoing_of(bus)) {
        const u32 index = index_of(target);
        if (index != kInvalidIndex && remaining[index] != kEmitted) {
            --remaining[index];
        }
    }
}

Status BusGraph::compile() noexcept {
    order_.clear();
    if (Status reserved = order_.reserve(buses_.size()); !reserved) {
        return reserved;
    }

    // Kahn's algorithm over "feeds" edges: a bus depends on every bus that routes into it, so a bus
    // may be emitted once every one of its sources has been. The counts are held in a small array
    // indexed by slot, which is why slots are dense.
    Array<u32> remaining(buses_.allocator());
    if (Status sized = remaining.resize(buses_.size()); !sized) {
        return sized;
    }
    count_incoming(remaining);

    usize live = 0;
    for (const Bus& bus : buses_) {
        live += bus.live ? 1U : 0U;
    }

    // Emitted in slot order among the ready ones, so the order is a function of the graph and of
    // creation order and never of a container's layout. `simulation-and-determinism` requires that
    // of anything ordered, and a mix that summed in a different order would differ in its last bits
    // between two runs of the same frame.
    usize emitted = 0;
    while (emitted < live) {
        bool progressed = false;
        for (usize i = 0; i < buses_.size(); ++i) {
            if (!buses_[i].live || remaining[i] != 0) {
                continue;
            }
            if (Status pushed = order_.push_back(buses_[i].handle); !pushed) {
                return pushed;
            }
            remaining[i] = kEmitted;
            ++emitted;
            progressed = true;
            release_outgoing(buses_[i], remaining);
        }
        if (!progressed) {
            // Unreachable while every edge went through the cycle check, and reported rather than
            // looped forever if one ever does not: a mixing graph that cannot be ordered is one the
            // realtime thread would hang in.
            return fail(ErrorCode::Internal,
                        "the bus graph could not be ordered, which means it contains a cycle");
        }
    }

    compiled_ = true;
    return ok();
}

bool BusGraph::any_solo() const noexcept {
    return std::ranges::any_of(
        buses_, [](const Bus& bus) noexcept { return bus.live && bus.description.solo; });
}

bool BusGraph::audible(BusHandle handle) const noexcept {
    const Bus* bus = find(handle);
    if (bus == nullptr || bus->description.mute) {
        return false;
    }
    if (!any_solo()) {
        return true;
    }
    // "**WHEN** a bus is soloed **THEN** buses that do not feed it SHALL be silenced for the mix."
    // Feeding a soloed bus is a reachability question, which is why it is asked of the graph rather
    // than answered from the bus's own flags.
    return std::ranges::any_of(buses_, [this, handle](const Bus& other) noexcept {
        return other.live && other.description.solo && reaches(handle, other.handle);
    });
}

}  // namespace cy::audio
