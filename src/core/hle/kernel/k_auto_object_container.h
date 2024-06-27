// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_funcs.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_linked_list.h"

namespace Kernel {

class KernelSystem;
class Process;

class KAutoObjectWithListContainer {
public:
    CITRA_NON_COPYABLE(KAutoObjectWithListContainer);
    CITRA_NON_MOVEABLE(KAutoObjectWithListContainer);

    using ListType = KLinkedList<KAutoObject>;

    KAutoObjectWithListContainer(KernelSystem& kernel) : m_object_list(kernel) {}

    void Initialize() {}
    void Finalize() {}

    void Register(KAutoObject* obj);
    void Unregister(KAutoObject* obj);
    size_t GetOwnedCount(Process* owner);

private:
    // KLightMutex m_mutex;
    ListType m_object_list;
};

} // namespace Kernel
