// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/array.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include "common/archives.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_process.h"

namespace Kernel {

Result KHandleTable::Finalize() {
    // Close and free all entries.
    for (size_t i = 0; i < m_table_size; i++) {
        if (KAutoObject* obj = m_objects[i]; obj != nullptr) {
            obj->Close();
        }
    }

    return ResultSuccess;
}

bool KHandleTable::Remove(Handle handle) {
    // Don't allow removal of a pseudo-handle.
    if (handle == KernelHandle::CurrentProcess || handle == KernelHandle::CurrentThread)
        [[unlikely]] {
        return false;
    }

    // Handles must not have reserved bits set.
    const auto handle_pack = HandlePack(handle);
    if (handle_pack.reserved != 0) [[unlikely]] {
        return false;
    }

    // Find the object and free the entry.
    KAutoObject* obj = nullptr;
    {
        // KScopedLightMutex lk{m_mutex};
        if (this->IsValidHandle(handle)) [[likely]] {
            const auto index = handle_pack.index;

            obj = m_objects[index];
            this->FreeEntry(index);
        } else {
            return false;
        }
    }

    // Close the object.
    obj->Close();
    return true;
}

Result KHandleTable::Add(Handle* out_handle, KAutoObject* obj) {
    // KScopedLightMutex lk{m_mutex};

    // Never exceed our capacity.
    R_UNLESS(m_count < m_table_size, ResultOutOfHandles);

    // Allocate entry, set output handle.
    const auto linear_id = this->AllocateLinearId();
    const auto index = this->AllocateEntry();

    m_entry_infos[index].linear_id = linear_id;
    m_objects[index] = obj;

    obj->Open();

    *out_handle = EncodeHandle(static_cast<u16>(index), linear_id);
    return ResultSuccess;
}

KScopedAutoObject<KAutoObject> KHandleTable::GetObjectForIpc(Handle handle,
                                                             KThread* cur_thread) const {
    // Handle pseudo-handles.
    ASSERT(cur_thread != nullptr);
    if (handle == KernelHandle::CurrentProcess) {
        auto* cur_process = cur_thread->GetOwner();
        ASSERT(cur_process != nullptr);
        return cur_process;
    }
    if (handle == KernelHandle::CurrentThread) {
        return cur_thread;
    }

    return this->GetObjectForIpcWithoutPseudoHandle(handle);
}

template <class Archive>
void KHandleTable::serialize(Archive& ar, const u32 file_version) {
    // ar& m_entry_infos;
    // ar& m_objects;
    ar& m_free_head_index;
    ar& m_table_size;
    ar& m_next_id;
    ar& m_max_count;
    ar& m_next_linear_id;
    ar& m_count;
}

SERIALIZE_IMPL(KHandleTable)

} // namespace Kernel
