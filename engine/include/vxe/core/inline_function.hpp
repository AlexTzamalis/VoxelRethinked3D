#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace vxe {

// A move only `void()` callable stored entirely inside the object, without
// ever allocating.
//
// std::function allocates when the callable is larger than its small buffer,
// and its buffer size is unspecified. The job system schedules millions of
// small tasks, so every job must be heap free: this type stores the callable
// in `Capacity` bytes and refuses, at compile time, any callable that does
// not fit. A job that needs more data should capture a pointer to it instead.
template <usize Capacity>
class InlineFunction {
public:
    InlineFunction() noexcept = default;

    template <typename Fn>
        requires(!std::is_same_v<std::decay_t<Fn>, InlineFunction> &&
                 std::is_invocable_r_v<void, std::decay_t<Fn>&>)
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions): lambdas convert implicitly.
    InlineFunction(Fn&& fn) {
        using Stored = std::decay_t<Fn>;
        static_assert(sizeof(Stored) <= Capacity,
                      "Callable is too large for InlineFunction: capture a pointer to the data instead");
        static_assert(alignof(Stored) <= alignof(std::max_align_t), "Callable is over aligned");
        static_assert(std::is_nothrow_move_constructible_v<Stored>, "Callable must be nothrow movable");
        ::new (static_cast<void*>(m_storage)) Stored(std::forward<Fn>(fn));
        m_ops = opsFor<Stored>();
    }

    InlineFunction(const InlineFunction&) = delete;
    InlineFunction& operator=(const InlineFunction&) = delete;

    InlineFunction(InlineFunction&& other) noexcept { moveFrom(other); }
    InlineFunction& operator=(InlineFunction&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    ~InlineFunction() { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept { return m_ops != nullptr; }

    void operator()() {
        VXE_ASSERT(m_ops != nullptr, "Calling an empty InlineFunction");
        m_ops->invoke(m_storage);
    }

    void reset() noexcept {
        if (m_ops != nullptr) {
            m_ops->destroy(m_storage);
            m_ops = nullptr;
        }
    }

private:
    // One table of plain function pointers per stored type, instead of a
    // virtual base class, so the storage holds only the callable itself.
    struct Ops {
        void (*invoke)(std::byte* storage);
        void (*moveTo)(std::byte* from, std::byte* to) noexcept;
        void (*destroy)(std::byte* storage) noexcept;
    };

    template <typename Stored>
    static void invokeStored(std::byte* storage) {
        (*std::launder(reinterpret_cast<Stored*>(storage)))();
    }

    template <typename Stored>
    static void moveStored(std::byte* from, std::byte* to) noexcept {
        Stored* source = std::launder(reinterpret_cast<Stored*>(from));
        ::new (static_cast<void*>(to)) Stored(std::move(*source));
        std::destroy_at(source);
    }

    template <typename Stored>
    static void destroyStored(std::byte* storage) noexcept {
        std::destroy_at(std::launder(reinterpret_cast<Stored*>(storage)));
    }

    // A function local table instead of a static member variable template:
    // MSVC wrongly warns (C4268) that the member template would be zero filled.
    template <typename Stored>
    [[nodiscard]] static const Ops* opsFor() noexcept {
        static constexpr Ops kTable{
            .invoke = &invokeStored<Stored>,
            .moveTo = &moveStored<Stored>,
            .destroy = &destroyStored<Stored>,
        };
        return &kTable;
    }

    void moveFrom(InlineFunction& other) noexcept {
        if (other.m_ops != nullptr) {
            other.m_ops->moveTo(other.m_storage, m_storage);
            m_ops = std::exchange(other.m_ops, nullptr);
        }
    }

    // Uninitialized until a callable is placed in it.
    alignas(std::max_align_t) std::byte m_storage[Capacity]; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const Ops* m_ops = nullptr;
};

} // namespace vxe
