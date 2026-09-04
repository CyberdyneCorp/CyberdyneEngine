#include <cy/core/determinism/provider.h>

#include <cy/core/determinism/ordering.h>
#include <cy/core/determinism/random.h>

#include <cstring>

namespace cy::determinism {
namespace {

/// Ordering by name, byte by byte. `std::strcmp` would do, and this is spelled out so that the
/// comparison is visibly locale-free — a name order that depended on a locale would be a
/// determinism defect on exactly the machines nobody tests on.
[[nodiscard]] bool name_less(const char* a, const char* b) noexcept {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<u8>(*a) < static_cast<u8>(*b);
}

[[nodiscard]] bool name_equal(const char* a, const char* b) noexcept {
    return std::strcmp(a, b) == 0;
}

}  // namespace

const char* ordering_name(Ordering ordering) noexcept {
    return ordering == Ordering::Stable ? "stable" : "unspecified";
}

const char* stream_purpose_name(StreamPurpose purpose) noexcept {
    return purpose == StreamPurpose::Presentation ? "presentation" : "authoritative";
}

Status StateProvider::hash(StateHashTree& /*tree*/) const noexcept {
    return fail(ErrorCode::NotImplemented,
                "this provider declared that it participates in hashing and did not implement "
                "hash()");
}

Status StateProvider::capture(Array<u8>& /*out*/) const noexcept {
    return fail(ErrorCode::NotImplemented,
                "this provider declared that it participates in capture and did not implement "
                "capture()");
}

Status StateProvider::restore(Span<const u8> /*bytes*/) noexcept {
    return fail(ErrorCode::NotImplemented,
                "this provider declared that it participates in capture and did not implement "
                "restore()");
}

Status StateProviderRegistry::add(StateProvider& provider) noexcept {
    if (finalized_) {
        return fail(ErrorCode::Unavailable,
                    "the provider registry is finalised; a provider that joined mid-session would "
                    "change the hash's shape at a tick nobody chose");
    }
    const char* name = provider.name();
    if (name == nullptr || *name == '\0') {
        return fail(ErrorCode::InvalidArgument,
                    "a state provider's name is the identifier the registry orders by, so it "
                    "cannot be empty");
    }
    if (find(name) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "a state provider with this name is registered");
    }
    return providers_.push_back(&provider);
}

void StateProviderRegistry::finalize() noexcept {
    // Insertion sort by name: stable, allocation-free, and over a list of providers that is a
    // handful long. The order is the *name's*, which is the stable identifier
    // `simulation-and-determinism` requires a registry to be finalised by — never the registration
    // order, which is plugin load order wearing a disguise.
    StateProvider** data = providers_.data();
    for (usize i = 1; i < providers_.size(); ++i) {
        for (usize j = i; j > 0 && name_less(data[j]->name(), data[j - 1]->name()); --j) {
            StateProvider* held = data[j - 1];
            data[j - 1] = data[j];
            data[j] = held;
        }
    }
    finalized_ = true;
}

StateProvider* StateProviderRegistry::find(const char* name) const noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (StateProvider* provider : providers_) {
        if (name_equal(provider->name(), name)) {
            return provider;
        }
    }
    return nullptr;
}

Status StateProviderRegistry::hash_all(StateHashTree& tree) const noexcept {
    if (!finalized_) {
        return fail(ErrorCode::Unavailable,
                    "the provider registry is not finalised; hashing it would make plugin load "
                    "order visible in the result");
    }
    for (StateProvider* provider : providers_) {
        if (!participates_in(provider->participation(), Participates::Hash)) {
            continue;
        }
        if (Status opened = tree.begin(HashLevel::Subsystem, detail::hash_name(provider->name()),
                                       provider->name());
            !opened) {
            return opened;
        }
        if (Status hashed = provider->hash(tree); !hashed) {
            return hashed;
        }
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
    }
    return ok();
}

}  // namespace cy::determinism
