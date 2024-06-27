// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "core/global.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

class ResourceLimit;

class KSemaphore final
    : public KAutoObjectWithSlabHeapAndContainer<KSemaphore, KSynchronizationObject> {
    KERNEL_AUTOOBJECT_TRAITS(KSemaphore, KSynchronizationObject);

public:
    explicit KSemaphore(KernelSystem& kernel);
    ~KSemaphore() override;

    void Initialize(Process* owner, s32 initial_count, s32 max_count, std::string name);

    uintptr_t GetPostDestroyArgument() const override {
        return reinterpret_cast<uintptr_t>(m_owner);
    }

    static void PostDestroy(uintptr_t arg);

    Process* GetOwner() const override {
        return m_owner;
    }

    bool ShouldWait(const KThread* thread) const override;
    void Acquire(KThread* thread) override;

    s32 GetAvailableCount() const {
        return m_available_count;
    }

    s32 GetMaxCount() const {
        return m_max_count;
    }

    Result Release(s32* out_count, s32 release_count);

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

private:
    Process* m_owner{};
    s32 m_max_count{};
    s32 m_available_count{};
    std::string m_name;
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KSemaphore)
CONSTRUCT_KERNEL_OBJECT(Kernel::KSemaphore)
