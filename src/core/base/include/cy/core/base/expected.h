// cy::Expected<T, E> — the return type of every fallible operation. Task 3.1.1.
//
// A value or an error, never both, never neither. This is std::expected's shape, written here
// because the engine targets C++20 and std::expected is C++23; when the minimum moves, the alias
// can move with it without any caller changing.
//
// Reading the error alternative through value(), or the value alternative through error(), is a
// programmer error and trips CY_ASSERT. It is not itself a runtime failure: the caller was told
// which alternative it had and did not look.

#pragma once

#include <cy/core/base/assert.h>
#include <cy/core/base/error.h>
#include <cy/core/base/types.h>

#include <new>
#include <type_traits>
#include <utility>

namespace cy {

// The error alternative, distinguished from the value alternative by its type so that
// Expected<Error, Error> is still unambiguous.
template <typename E>
class Unexpected {
public:
    explicit constexpr Unexpected(E error) : error_(std::move(error)) {}

    constexpr const E& error() const& noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }

private:
    E error_;
};

template <typename E>
constexpr Unexpected<std::decay_t<E>> make_unexpected(E&& error) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(error));
}

// The spelling nearly every caller uses: `return cy::fail(ErrorCode::NotFound, "no such display");`
inline Unexpected<Error> fail(ErrorCode code, const char* message, i64 system_code = 0) {
    return Unexpected<Error>(Error{code, message, system_code});
}

template <typename T, typename E = Error>
class Expected {
public:
    using value_type = T;
    using error_type = E;

    constexpr Expected()
        requires std::is_default_constructible_v<T>
        : value_(), has_value_(true) {}

    // Implicit on purpose, both ways: `return value;` and `return cy::fail(...);` are how a
    // fallible function is written, and requiring the type name at every return would only be
    // repeated back from the signature.
    Expected(const T& value) : value_(value), has_value_(true) {}
    Expected(T&& value) : value_(std::move(value)), has_value_(true) {}
    Expected(const Unexpected<E>& e) : error_(e.error()), has_value_(false) {}
    Expected(Unexpected<E>&& e) : error_(std::move(e).error()), has_value_(false) {}

    Expected(const Expected& other) : has_value_(other.has_value_) { construct_from(other); }
    Expected(Expected&& other) noexcept : has_value_(other.has_value_) {
        construct_from(std::move(other));
    }

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(other);
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(std::move(other));
        }
        return *this;
    }

    ~Expected() { destroy(); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    explicit constexpr operator bool() const noexcept { return has_value_; }

    T& value() & {
        CY_ASSERT_MSG(has_value_, "Expected::value() on an error");
        return value_;
    }
    const T& value() const& {
        CY_ASSERT_MSG(has_value_, "Expected::value() on an error");
        return value_;
    }
    T&& value() && {
        CY_ASSERT_MSG(has_value_, "Expected::value() on an error");
        return std::move(value_);
    }

    E& error() & {
        CY_ASSERT_MSG(!has_value_, "Expected::error() on a value");
        return error_;
    }
    const E& error() const& {
        CY_ASSERT_MSG(!has_value_, "Expected::error() on a value");
        return error_;
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value_ ? value_ : static_cast<T>(std::forward<U>(fallback));
    }

private:
    // The two alternatives share storage, so exactly one of them is alive and this class is
    // responsible for saying which. Both helpers read has_value_, which the caller sets first.
    template <typename Other>
    void construct_from(Other&& other) {
        if (has_value_) {
            ::new (static_cast<void*>(&value_)) T(std::forward<Other>(other).value_);
        } else {
            ::new (static_cast<void*>(&error_)) E(std::forward<Other>(other).error_);
        }
    }

    void destroy() noexcept {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    union {
        T value_;
        E error_;
    };
    bool has_value_;
};

// The void alternative: an operation that can fail but returns nothing. Same interface minus
// value(), so `if (auto r = f(); !r) return r.error();` reads identically either way.
template <typename E>
class Expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Expected() noexcept : none_(), has_value_(true) {}

    Expected(const Unexpected<E>& e) : error_(e.error()), has_value_(false) {}
    Expected(Unexpected<E>&& e) : error_(std::move(e).error()), has_value_(false) {}

    Expected(const Expected& other) : has_value_(other.has_value_) { construct_from(other); }
    Expected(Expected&& other) noexcept : has_value_(other.has_value_) {
        construct_from(std::move(other));
    }

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(other);
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(std::move(other));
        }
        return *this;
    }

    ~Expected() { destroy(); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    explicit constexpr operator bool() const noexcept { return has_value_; }

    E& error() & {
        CY_ASSERT_MSG(!has_value_, "Expected<void>::error() on a value");
        return error_;
    }
    const E& error() const& {
        CY_ASSERT_MSG(!has_value_, "Expected<void>::error() on a value");
        return error_;
    }

private:
    template <typename Other>
    void construct_from(Other&& other) {
        if (!has_value_) {
            ::new (static_cast<void*>(&error_)) E(std::forward<Other>(other).error_);
        }
    }

    void destroy() noexcept {
        if (!has_value_) {
            error_.~E();
        }
    }

    // none_ gives the union an active member in the value state; it holds nothing.
    struct None {};
    union {
        None none_;
        E error_;
    };
    bool has_value_;
};

// The one-argument spelling. `Expected<T>` is `Expected<T, Error>`; `Status` is the void one, named
// because "an operation that either worked or says why not" reads better than Expected<void>.
using Status = Expected<void, Error>;

// The success value of a Status, spelled so that a function returning one ends in a statement
// rather than in a braced literal whose meaning has to be recalled.
inline Status ok() noexcept {
    return Status{};
}

}  // namespace cy
