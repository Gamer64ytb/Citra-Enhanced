// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

KAutoObject* KAutoObject::Create(KAutoObject* obj) {
    obj->m_ref_count = 1;
    return obj;
}

void KAutoObject::RegisterWithKernel() {
    m_kernel.RegisterKernelObject(this);
}

void KAutoObject::UnregisterWithKernel(KernelSystem& kernel, KAutoObject* self) {
    kernel.UnregisterKernelObject(self);
}

template <class Archive>
void KAutoObject::serialize(Archive& ar, const unsigned int) {
    ar& m_name;
    // ar& m_ref_count;
}

SERIALIZE_IMPL(KAutoObject)

} // namespace Kernel
