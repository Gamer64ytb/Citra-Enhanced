// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "core/hle/kernel/k_auto_object_container.h"

namespace Kernel {

void KAutoObjectWithListContainer::Register(KAutoObject* obj) {
    // KScopedLightMutex lk{m_mutex};
    m_object_list.push_back(*obj);
}

void KAutoObjectWithListContainer::Unregister(KAutoObject* obj) {
    // KScopedLightMutex lk{m_mutex};
    for (auto it = m_object_list.begin(); it != m_object_list.end(); it++) {
        if (std::addressof(*it) == obj) {
            m_object_list.erase(it);
            return;
        }
    }
}

size_t KAutoObjectWithListContainer::GetOwnedCount(Process* owner) {
    // KScopedLightMutex lk{m_mutex};
    return std::count_if(m_object_list.begin(), m_object_list.end(),
                         [&](const auto& obj) { return obj.GetOwner() == owner; });
}

} // namespace Kernel
