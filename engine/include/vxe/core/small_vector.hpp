#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace vxe {

// A vector that stores its first N elements inside the object itself and only
// allocates heap memory when it grows past N.
//
// Many engine lists are almost always tiny: the up to 3 bindings of an action,
// the up to 4 spans of a LOD cell, the 6 face groups of a chunk. A std::vector
// would allocate for each of them. A SmallVector of the right size never does,
// and keeps the data next to the object that owns it, which is cache friendly.
//
// The interface follows std::vector (push_back, size, begin, ...) so standard
// algorithms and std::back_inserter work. Elements must be nothrow movable,
// because growing moves them and the engine does not handle exceptions.
//
// Unlike std::vector, moving a SmallVector that uses its inline storage moves
// the elements one by one, so pointers into it do not survive a move.
template <typename T, usize N, MemoryTag Tag = MemoryTag::General>
class SmallVector {
    static_assert(N > 0, "Use std::vector when no inline storage is wanted");
    static_assert(std::is_nothrow_move_constructible_v<T>, "SmallVector elements must be nothrow movable");

public:
    // NOLINTBEGIN(readability-identifier-naming): std::vector compatible names.
    using value_type = T;
    using size_type = usize;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVector() noexcept = default;

    SmallVector(std::initializer_list<T> values) {
        reserve(values.size());
        for (const T& value : values) {
            push_back(value);
        }
    }

    SmallVector(const SmallVector& other) {
        reserve(other.m_size);
        std::uninitialized_copy(other.begin(), other.end(), m_data);
        m_size = other.m_size;
    }

    SmallVector(SmallVector&& other) noexcept { takeFrom(std::move(other)); }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            reserve(other.m_size);
            std::uninitialized_copy(other.begin(), other.end(), m_data);
            m_size = other.m_size;
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            releaseHeap();
            takeFrom(std::move(other));
        }
        return *this;
    }

    ~SmallVector() {
        clear();
        releaseHeap();
    }

    [[nodiscard]] usize size() const noexcept { return m_size; }
    [[nodiscard]] usize capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    // True while the elements live in the inline storage (no heap memory used).
    [[nodiscard]] bool isInline() const noexcept { return m_data == inlineData(); }
    [[nodiscard]] static constexpr usize inlineCapacity() noexcept { return N; }

    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] iterator begin() noexcept { return m_data; }
    [[nodiscard]] iterator end() noexcept { return m_data + m_size; }
    [[nodiscard]] const_iterator begin() const noexcept { return m_data; }
    [[nodiscard]] const_iterator end() const noexcept { return m_data + m_size; }

    [[nodiscard]] T& operator[](usize index) noexcept {
        VXE_ASSERT(index < m_size, "SmallVector index out of range");
        return m_data[index];
    }
    [[nodiscard]] const T& operator[](usize index) const noexcept {
        VXE_ASSERT(index < m_size, "SmallVector index out of range");
        return m_data[index];
    }
    [[nodiscard]] T& front() noexcept { return (*this)[0]; }
    [[nodiscard]] const T& front() const noexcept { return (*this)[0]; }
    [[nodiscard]] T& back() noexcept { return (*this)[m_size - 1]; }
    [[nodiscard]] const T& back() const noexcept { return (*this)[m_size - 1]; }

    [[nodiscard]] std::span<T> span() noexcept { return {m_data, m_size}; }
    [[nodiscard]] std::span<const T> span() const noexcept { return {m_data, m_size}; }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size == m_capacity) {
            return growAndEmplace(std::forward<Args>(args)...);
        }
        T* slot = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
        ++m_size;
        return *slot;
    }

    void pop_back() noexcept {
        VXE_ASSERT(m_size > 0, "pop_back on an empty SmallVector");
        --m_size;
        std::destroy_at(m_data + m_size);
    }

    // Destroys all elements. Keeps the capacity, like std::vector.
    void clear() noexcept {
        std::destroy(begin(), end());
        m_size = 0;
    }

    void reserve(usize newCapacity) {
        if (newCapacity > m_capacity) {
            reallocate(newCapacity);
        }
    }

    // Grows with default constructed elements or shrinks by destroying the tail.
    void resize(usize newSize) {
        if (newSize < m_size) {
            std::destroy(m_data + newSize, end());
        } else if (newSize > m_size) {
            reserve(newSize);
            std::uninitialized_value_construct(m_data + m_size, m_data + newSize);
        }
        m_size = newSize;
    }

    // Removes the element at `position` and shifts the rest down by one.
    iterator erase(const_iterator position) {
        VXE_ASSERT(position >= begin() && position < end(), "erase position out of range");
        const auto index = static_cast<usize>(position - begin());
        std::move(m_data + index + 1, end(), m_data + index);
        pop_back();
        return m_data + index;
    }
    // NOLINTEND(readability-identifier-naming)

    [[nodiscard]] friend bool operator==(const SmallVector& lhs, const SmallVector& rhs) {
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

private:
    [[nodiscard]] T* inlineData() noexcept { return reinterpret_cast<T*>(m_inline); }
    [[nodiscard]] const T* inlineData() const noexcept { return reinterpret_cast<const T*>(m_inline); }

    [[nodiscard]] static usize grownCapacity(usize current) noexcept { return current * 2; }

    // Moves the elements into a new heap block of `newCapacity` elements.
    void reallocate(usize newCapacity) {
        T* fresh = static_cast<T*>(memory::allocate(newCapacity * sizeof(T), alignof(T), Tag));
        std::uninitialized_move(begin(), end(), fresh);
        std::destroy(begin(), end());
        releaseHeap();
        m_data = fresh;
        m_capacity = newCapacity;
    }

    // Constructs the new element in the new block before the old elements move,
    // so emplace_back(v[0]) is safe even though v[0] lives in the old block.
    template <typename... Args>
    T& growAndEmplace(Args&&... args) {
        const usize newCapacity = grownCapacity(m_capacity);
        T* fresh = static_cast<T*>(memory::allocate(newCapacity * sizeof(T), alignof(T), Tag));
        T* slot = std::construct_at(fresh + m_size, std::forward<Args>(args)...);
        std::uninitialized_move(begin(), end(), fresh);
        std::destroy(begin(), end());
        releaseHeap();
        m_data = fresh;
        m_capacity = newCapacity;
        ++m_size;
        return *slot;
    }

    // Frees the heap block, if any, and points back at the inline storage.
    // The elements must already be destroyed or moved out.
    void releaseHeap() noexcept {
        if (!isInline()) {
            memory::deallocate(m_data, m_capacity * sizeof(T), alignof(T), Tag);
            m_data = inlineData();
            m_capacity = N;
        }
    }

    // Takes over `other`'s elements. Expects this vector to be empty and inline.
    void takeFrom(SmallVector&& other) noexcept {
        if (other.isInline()) {
            std::uninitialized_move(other.begin(), other.end(), m_data);
            m_size = other.m_size;
            other.clear();
        } else {
            m_data = std::exchange(other.m_data, other.inlineData());
            m_capacity = std::exchange(other.m_capacity, N);
            m_size = std::exchange(other.m_size, 0);
        }
    }

    // Left uninitialized on purpose: elements are constructed into it on demand.
    alignas(T) std::byte m_inline[N * sizeof(T)]; // NOLINT(cppcoreguidelines-pro-type-member-init)
    T* m_data = inlineData();
    usize m_size = 0;
    usize m_capacity = N;
};

} // namespace vxe
