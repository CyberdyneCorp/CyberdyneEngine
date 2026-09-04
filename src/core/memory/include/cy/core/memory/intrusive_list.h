#pragma once
// A doubly-linked list whose links live in the elements. Task 2.4.
//
// `core-memory-and-containers` — "Sequence containers": `IntrusiveList<T>` has embedded links and
// O(1) removal from the element itself. That last clause is the reason it exists: a task that has
// finished, a resource that has been retired, a node that has been destroyed — each of them knows
// its own address and nothing else, and removing it must not require finding it first.
//
// The list allocates nothing, ever. It also owns nothing: an element's lifetime is the caller's,
// and an element destroyed while linked leaves a dangling neighbour, which is why `IntrusiveNode`
// unlinks itself in its own destructor.
//
// AN INTRUSIVE ELEMENT IS NOT TRIVIALLY RELOCATABLE. Its neighbours hold its address. Moving one
// with memcpy leaves the list pointing at the old location; `IsTriviallyRelocatable` therefore must
// not be declared for a type containing one, and the node's non-trivial destructor means the
// default answer is already no.

#include <cy/core/base/assert.h>
#include <cy/core/base/types.h>

namespace cy {

/// The links, embedded in the element as a member.
///
/// `owner_size` is the third pointer, and it is what makes self-removal correct rather than nearly
/// correct: a node that unlinks itself — from its own destructor, or from a caller that has the
/// element and not the list — has to tell the list its count changed, and this is the only way back
/// to the list from an element that was never given one.
struct IntrusiveNode {
    IntrusiveNode* previous = nullptr;
    IntrusiveNode* next = nullptr;
    usize* owner_size = nullptr;

    IntrusiveNode() noexcept = default;
    ~IntrusiveNode() { unlink(); }

    IntrusiveNode(const IntrusiveNode&) = delete;
    IntrusiveNode& operator=(const IntrusiveNode&) = delete;
    IntrusiveNode(IntrusiveNode&&) = delete;
    IntrusiveNode& operator=(IntrusiveNode&&) = delete;

    [[nodiscard]] bool is_linked() const noexcept { return previous != nullptr || next != nullptr; }

    /// Remove this node from whatever list holds it. O(1), and safe on a node that is not linked.
    void unlink() noexcept {
        if (previous != nullptr) {
            previous->next = next;
        }
        if (next != nullptr) {
            next->previous = previous;
        }
        if (owner_size != nullptr) {
            --*owner_size;
            owner_size = nullptr;
        }
        previous = nullptr;
        next = nullptr;
    }
};

/// A list of `T`, linked through `T::*Member`.
///
///   struct Task { cy::IntrusiveNode link; ... };
///   cy::IntrusiveList<Task, &Task::link> ready;
template <class T, IntrusiveNode T::* Member>
class IntrusiveList {
public:
    IntrusiveList() noexcept {
        head_.next = &head_;
        head_.previous = &head_;
    }
    ~IntrusiveList() { clear(); }

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator=(IntrusiveList&&) = delete;

    void push_back(T& element) noexcept { insert_before(&head_, element); }
    void push_front(T& element) noexcept { insert_before(head_.next, element); }

    /// Remove `element` from this list. O(1): the element's own links are all that is read.
    void remove(T& element) noexcept { (element.*Member).unlink(); }

    [[nodiscard]] T* front() noexcept { return empty() ? nullptr : to_element(head_.next); }
    [[nodiscard]] T* back() noexcept { return empty() ? nullptr : to_element(head_.previous); }

    [[nodiscard]] T* pop_front() noexcept {
        T* element = front();
        if (element != nullptr) {
            remove(*element);
        }
        return element;
    }

    void clear() noexcept {
        while (pop_front() != nullptr) {
        }
    }

    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return head_.next == &head_; }

    /// A forward iterator. Written out rather than aliased because the step is `node->next` and the
    /// dereference is a cast back to the element, and neither is expressible any other way.
    class Iterator {
    public:
        explicit Iterator(IntrusiveNode* node) noexcept : node_(node) {}

        [[nodiscard]] T& operator*() const noexcept { return *to_element(node_); }
        [[nodiscard]] T* operator->() const noexcept { return to_element(node_); }
        Iterator& operator++() noexcept {
            node_ = node_->next;
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept {
            return node_ == other.node_;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            return node_ != other.node_;
        }

    private:
        IntrusiveNode* node_;
    };

    [[nodiscard]] Iterator begin() noexcept { return Iterator(head_.next); }
    [[nodiscard]] Iterator end() noexcept { return Iterator(&head_); }

private:
    /// The offset from an element to its node, computed once from a null-based address. This is the
    /// standard intrusive-container trick and is the only way back from a node to its element
    /// without storing a second pointer in every element.
    // NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose: the element and its
    // node are the same object at different offsets, and a direct reinterpret_cast between them is
    // what -Wcast-align reports under the engine's -Werror.
    [[nodiscard]] static T* to_element(IntrusiveNode* node) noexcept {
        auto* base = static_cast<u8*>(static_cast<void*>(node));
        return static_cast<T*>(static_cast<void*>(base - member_offset()));
    }

    [[nodiscard]] static usize member_offset() noexcept {
        // The offset of a member from the front of its object, computed the way offsetof is
        // implemented: over a correctly sized and aligned buffer, so that `T` need not be
        // default-constructible and no object is created to answer a question about the layout.
        alignas(T) static u8 probe[sizeof(T)];
        auto* element = static_cast<T*>(static_cast<void*>(probe));
        const auto* node = static_cast<const u8*>(static_cast<const void*>(&(element->*Member)));
        return static_cast<usize>(node - probe);
    }
    // NOLINTEND(bugprone-casting-through-void)

    void insert_before(IntrusiveNode* position, T& element) noexcept {
        IntrusiveNode& node = element.*Member;
        CY_ASSERT_MSG(!node.is_linked(), "element is already in a list");
        node.previous = position->previous;
        node.next = position;
        node.owner_size = &size_;
        position->previous->next = &node;
        position->previous = &node;
        ++size_;
    }

    /// The sentinel. A circular list with a head node means insertion and removal have no special
    /// cases at the ends, which is what makes `unlink()` correct without knowing the list.
    IntrusiveNode head_;
    usize size_ = 0;
};

}  // namespace cy
