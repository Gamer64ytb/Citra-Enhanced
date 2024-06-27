// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once
// #pragma clang optimize off

#include <atomic>

#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

namespace impl {

class KSlabHeapImpl {
    CITRA_NON_COPYABLE(KSlabHeapImpl);
    CITRA_NON_MOVEABLE(KSlabHeapImpl);

public:
    struct Node {
        Node* next{};
    };

public:
    constexpr KSlabHeapImpl() = default;

    void Initialize() {
        ASSERT(m_head == nullptr);
    }

    Node* GetHead() const {
        return m_head;
    }

    void* Allocate() {
        Node* ret = m_head;
        if (ret != nullptr) [[likely]] {
            m_head = ret->next;
        }
        return ret;
    }

    void Free(void* obj) {
        Node* node = static_cast<Node*>(obj);
        node->next = m_head;
        m_head = node;
    }

private:
    std::atomic<Node*> m_head{};
};

} // namespace impl

class KSlabHeapBase : protected impl::KSlabHeapImpl {
    CITRA_NON_COPYABLE(KSlabHeapBase);
    CITRA_NON_MOVEABLE(KSlabHeapBase);

private:
    size_t m_obj_size{};
    uintptr_t m_peak{};
    uintptr_t m_start{};
    uintptr_t m_end{};

private:
    void UpdatePeakImpl(uintptr_t obj) {
        const uintptr_t alloc_peak = obj + this->GetObjectSize();
        uintptr_t cur_peak = m_peak;
        do {
            if (alloc_peak <= cur_peak) {
                break;
            }
        } while (!Common::AtomicCompareAndSwap(
            reinterpret_cast<volatile u64*>(std::addressof(m_peak)), static_cast<u64>(alloc_peak),
            static_cast<u64>(cur_peak), reinterpret_cast<u64&>(cur_peak)));
    }

public:
    constexpr KSlabHeapBase() = default;

    bool Contains(uintptr_t address) const {
        return m_start <= address && address < m_end;
    }

    void Initialize(size_t obj_size, void* memory, size_t memory_size) {
        // Ensure we don't initialize a slab using null memory.
        ASSERT(memory != nullptr);

        // Set our object size.
        m_obj_size = obj_size;

        // Initialize the base allocator.
        KSlabHeapImpl::Initialize();

        // Set our tracking variables.
        const size_t num_obj = (memory_size / obj_size);
        m_start = reinterpret_cast<uintptr_t>(memory);
        m_end = m_start + num_obj * obj_size;
        m_peak = m_start;

        // Free the objects.
        u8* cur = reinterpret_cast<u8*>(m_end);

        for (size_t i = 0; i < num_obj; i++) {
            cur -= obj_size;
            KSlabHeapImpl::Free(cur);
        }
    }

    size_t GetSlabHeapSize() const {
        return (m_end - m_start) / this->GetObjectSize();
    }

    size_t GetObjectSize() const {
        return m_obj_size;
    }

    void* Allocate() {
        void* obj = KSlabHeapImpl::Allocate();
        return obj;
    }

    void Free(void* obj) {
        // Don't allow freeing an object that wasn't allocated from this heap.
        const bool contained = this->Contains(reinterpret_cast<uintptr_t>(obj));
        ASSERT(contained);
        KSlabHeapImpl::Free(obj);
    }

    size_t GetObjectIndex(const void* obj) const {
        return (reinterpret_cast<uintptr_t>(obj) - m_start) / this->GetObjectSize();
    }

    size_t GetPeakIndex() const {
        return this->GetObjectIndex(reinterpret_cast<const void*>(m_peak));
    }

    uintptr_t GetSlabHeapAddress() const {
        return m_start;
    }

    size_t GetNumRemaining() const {
        // Only calculate the number of remaining objects under debug configuration.
        return 0;
    }
};

template <typename T>
class KSlabHeap final : public KSlabHeapBase {
private:
    using BaseHeap = KSlabHeapBase;

public:
    constexpr KSlabHeap() = default;

    void Initialize(void* memory, size_t memory_size) {
        BaseHeap::Initialize(sizeof(T), memory, memory_size);
    }

    T* Allocate() {
        T* obj = static_cast<T*>(BaseHeap::Allocate());

        if (obj != nullptr) [[likely]] {
            std::construct_at(obj);
        }
        return obj;
    }

    T* Allocate(KernelSystem& kernel) {
        T* obj = static_cast<T*>(BaseHeap::Allocate());

        if (obj != nullptr) [[likely]] {
            std::construct_at(obj, kernel);
        }
        return obj;
    }

    void Free(T* obj) {
        BaseHeap::Free(obj);
    }

    size_t GetObjectIndex(const T* obj) const {
        return BaseHeap::GetObjectIndex(obj);
    }
};

template <class Derived>
class KSlabAllocated {
public:
    constexpr KSlabAllocated() = default;

    size_t GetSlabIndex(KernelSystem& kernel) const {
        return kernel.SlabHeap<Derived>().GetIndex(static_cast<const Derived*>(this));
    }

public:
    static void InitializeSlabHeap(KernelSystem& kernel, void* memory, size_t memory_size) {
        kernel.SlabHeap<Derived>().Initialize(memory, memory_size);
    }

    static Derived* Allocate(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().Allocate(kernel);
    }

    static void Free(KernelSystem& kernel, Derived* obj) {
        kernel.SlabHeap<Derived>().Free(obj);
    }

    static size_t GetObjectSize(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().GetObjectSize();
    }

    static size_t GetSlabHeapSize(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapSize();
    }

    static size_t GetPeakIndex(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().GetPeakIndex();
    }

    static uintptr_t GetSlabHeapAddress(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapAddress();
    }

    static size_t GetNumRemaining(KernelSystem& kernel) {
        return kernel.SlabHeap<Derived>().GetNumRemaining();
    }
};

} // namespace Kernel
