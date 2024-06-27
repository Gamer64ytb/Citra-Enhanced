// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "common/common_types.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/result.h"

namespace Kernel {

enum KernelHandle : Handle {
    CurrentThread = 0xFFFF8000,
    CurrentProcess = 0xFFFF8001,
};

class KHandleTable {
    CITRA_NON_COPYABLE(KHandleTable);
    CITRA_NON_MOVEABLE(KHandleTable);

public:
    static constexpr size_t MaxTableSize = 1024;

public:
    explicit KHandleTable(KernelSystem& kernel) : m_kernel(kernel) {}

    Result Initialize(s32 size) {
        // KScopedLightMutex lk{m_mutex};

        // Initialize all fields.
        m_max_count = 0;
        m_table_size = static_cast<s16>((size <= 0) ? MaxTableSize : size);
        m_next_linear_id = MinLinearId;
        m_count = 0;
        m_free_head_index = -1;

        // Create the arrays
        m_objects.resize(m_table_size);
        m_entry_infos.resize(m_table_size);

        // Free all entries.
        for (s32 i = 0; i < static_cast<s32>(m_table_size); ++i) {
            m_objects[i] = nullptr;
            m_entry_infos[i].next_free_index = static_cast<s16>(i - 1);
            m_free_head_index = i;
        }

        return ResultSuccess;
    }

    size_t GetTableSize() const {
        return m_table_size;
    }
    size_t GetCount() const {
        return m_count;
    }
    size_t GetMaxCount() const {
        return m_max_count;
    }

    Result Finalize();
    bool Remove(Handle handle);
    Result Add(Handle* out_handle, KAutoObject* obj);

    template <typename T = KAutoObject>
    KScopedAutoObject<T> GetObjectWithoutPseudoHandle(Handle handle) const {
        // KScopedLightMutex lk{m_mutex};

        if constexpr (std::is_same_v<T, KAutoObject>) {
            return this->GetObjectImpl(handle);
        } else {
            if (auto* obj = this->GetObjectImpl(handle); obj != nullptr) [[likely]] {
                return obj->DynamicCast<T*>();
            } else {
                return nullptr;
            }
        }
    }

    template <typename T = KAutoObject>
    KScopedAutoObject<T> GetObject(Handle handle) const {
        // Handle pseudo-handles.
        if constexpr (std::derived_from<Process, T>) {
            if (handle == KernelHandle::CurrentProcess) {
                auto* const cur_process = m_kernel.GetCurrentProcess();
                ASSERT(cur_process != nullptr);
                return cur_process;
            }
        } else if constexpr (std::derived_from<KThread, T>) {
            if (handle == KernelHandle::CurrentThread) {
                auto* const cur_thread = m_kernel.GetCurrentThreadManager().GetCurrentThread();
                ASSERT(cur_thread != nullptr);
                return cur_thread;
            }
        }

        return this->template GetObjectWithoutPseudoHandle<T>(handle);
    }

    KScopedAutoObject<KAutoObject> GetObjectForIpcWithoutPseudoHandle(Handle handle) const {
        return this->GetObjectImpl(handle);
    }

    KScopedAutoObject<KAutoObject> GetObjectForIpc(Handle handle, KThread* cur_thread) const;

    template <typename T>
    bool GetMultipleObjects(T** out, const Handle* handles, size_t num_handles) const {
        // Try to convert and open all the handles.
        size_t num_opened;
        {
            // KScopedLightMutex lk{m_mutex};
            for (num_opened = 0; num_opened < num_handles; num_opened++) {
                // Get the current handle.
                const auto cur_handle = handles[num_opened];

                // Get the object for the current handle.
                KAutoObject* cur_object = this->GetObjectImpl(cur_handle);
                if (cur_object == nullptr) [[unlikely]] {
                    break;
                }

                // Cast the current object to the desired type.
                T* cur_t = cur_object->DynamicCast<T*>();
                if (cur_t == nullptr) [[unlikely]] {
                    break;
                }

                // Open a reference to the current object.
                cur_t->Open();
                out[num_opened] = cur_t;
            }
        }

        // If we converted every object, succeed.
        if (num_opened == num_handles) [[likely]] {
            return true;
        }

        // If we didn't convert entry object, close the ones we opened.
        for (size_t i = 0; i < num_opened; i++) {
            out[i]->Close();
        }

        return false;
    }

private:
    s32 AllocateEntry() {
        ASSERT(m_count < m_table_size);

        const auto index = m_free_head_index;

        m_free_head_index = m_entry_infos[index].GetNextFreeIndex();

        m_max_count = std::max(m_max_count, ++m_count);

        return index;
    }

    void FreeEntry(s32 index) {
        ASSERT(m_count > 0);

        m_objects[index] = nullptr;
        m_entry_infos[index].next_free_index = static_cast<s16>(m_free_head_index);

        m_free_head_index = index;

        --m_count;
    }

    u16 AllocateLinearId() {
        const u16 id = m_next_linear_id++;
        if (m_next_linear_id > MaxLinearId) {
            m_next_linear_id = MinLinearId;
        }
        return id;
    }

    bool IsValidHandle(Handle handle) const {
        // Unpack the handle.
        const auto handle_pack = HandlePack(handle);
        const auto raw_value = handle_pack.raw;
        const auto index = handle_pack.index;
        const auto linear_id = handle_pack.linear_id;
        const auto reserved = handle_pack.reserved;
        ASSERT(reserved == 0);

        // Validate our indexing information.
        if (raw_value == 0) [[unlikely]] {
            return false;
        }
        if (linear_id == 0) [[unlikely]] {
            return false;
        }
        if (index >= m_table_size) [[unlikely]] {
            return false;
        }

        // Check that there's an object, and our serial id is correct.
        if (m_objects[index] == nullptr) [[unlikely]] {
            return false;
        }
        if (m_entry_infos[index].GetLinearId() != linear_id) [[unlikely]] {
            return false;
        }

        return true;
    }

    KAutoObject* GetObjectImpl(Handle handle) const {
        // Handles must not have reserved bits set.
        const auto handle_pack = HandlePack(handle);
        if (handle_pack.reserved != 0) [[unlikely]] {
            return nullptr;
        }

        if (this->IsValidHandle(handle)) [[likely]] {
            return m_objects[handle_pack.index];
        } else {
            return nullptr;
        }
    }

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

private:
    union HandlePack {
        constexpr HandlePack() = default;
        constexpr HandlePack(Handle handle) : raw{static_cast<u32>(handle)} {}

        u32 raw{};
        BitField<0, 15, u32> index;
        BitField<15, 15, u32> linear_id;
        BitField<30, 2, u32> reserved;
    };

    static constexpr Handle EncodeHandle(u16 index, u16 linear_id) {
        HandlePack handle{};
        handle.index.Assign(index);
        handle.linear_id.Assign(linear_id);
        handle.reserved.Assign(0);
        return handle.raw;
    }

private:
    static constexpr u16 MinLinearId = 1;
    static constexpr u16 MaxLinearId = 0x7FFF;

    union EntryInfo {
        u16 linear_id;
        s16 next_free_index;

        constexpr u16 GetLinearId() const {
            return linear_id;
        }
        constexpr s32 GetNextFreeIndex() const {
            return next_free_index;
        }

        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar& linear_id;
            ar& next_free_index;
        }
    };

private:
    KernelSystem& m_kernel;
    std::vector<EntryInfo> m_entry_infos{};
    std::vector<KAutoObject*> m_objects{};
    s32 m_free_head_index{};
    u16 m_table_size{};
    u16 m_next_id{};
    u16 m_max_count{};
    u16 m_next_linear_id{};
    u16 m_count{};
    // KLightMutex mutex;
};

} // namespace Kernel
