// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <boost/serialization/export.hpp>
#include "core/global.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/slab_helpers.h"

namespace Kernel {

enum class ResetType : u32 {
    OneShot,
    Sticky,
    Pulse,
};

class KEvent final : public KAutoObjectWithSlabHeapAndContainer<KEvent, KSynchronizationObject> {
    KERNEL_AUTOOBJECT_TRAITS(KEvent, KSynchronizationObject);

public:
    explicit KEvent(KernelSystem& kernel);
    ~KEvent() override;

    std::string GetName() const {
        return m_name;
    }
    void SetName(const std::string& name) {
        m_name = name;
    }

    void Initialize(Process* owner, ResetType reset_type);

    uintptr_t GetPostDestroyArgument() const override {
        return reinterpret_cast<uintptr_t>(m_owner);
    }

    static void PostDestroy(uintptr_t arg);

    Process* GetOwner() const override {
        return m_owner;
    }

    ResetType GetResetType() const {
        return m_reset_type;
    }

    bool ShouldWait(const KThread* thread) const override;
    void Acquire(KThread* thread) override;

    void WakeupAllWaitingThreads() override;

    void Signal();
    void Clear();

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

private:
    Process* m_owner{};
    ResetType m_reset_type{};
    bool m_signaled{};
    std::string m_name;
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KEvent)
CONSTRUCT_KERNEL_OBJECT(Kernel::KEvent)
