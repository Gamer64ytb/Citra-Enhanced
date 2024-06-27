// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>
#include <boost/serialization/export.hpp>
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

class KThread;

enum class ArbitrationType : u32 {
    Signal,
    WaitIfLessThan,
    DecrementAndWaitIfLessThan,
    WaitIfLessThanWithTimeout,
    DecrementAndWaitIfLessThanWithTimeout,
};

class KAddressArbiter final : public KAutoObjectWithSlabHeapAndContainer<KAddressArbiter>,
                              public WakeupCallback {
    KERNEL_AUTOOBJECT_TRAITS(KAddressArbiter, KAutoObject);

public:
    explicit KAddressArbiter(KernelSystem& kernel);
    ~KAddressArbiter() override;

    void Initialize(Process* owner);

    uintptr_t GetPostDestroyArgument() const override {
        return reinterpret_cast<uintptr_t>(m_owner);
    }

    static void PostDestroy(uintptr_t arg);

    Process* GetOwner() const override {
        return m_owner;
    }

    Result ArbitrateAddress(KThread* thread, ArbitrationType type, VAddr address, s32 value,
                            u64 nanoseconds);

private:
    void WaitThread(KThread* thread, VAddr wait_address);

    u64 ResumeAllThreads(VAddr address);

    bool ResumeHighestPriorityThread(VAddr address);

    void WakeUp(ThreadWakeupReason reason, KThread* thread,
                KSynchronizationObject* object) override;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

public:
    Process* m_owner{};
    std::string m_name{};
    std::vector<KThread*> m_waiting_threads;
    class Callback;
    std::shared_ptr<Callback> m_timeout_callback;
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KAddressArbiter)
BOOST_CLASS_EXPORT_KEY(Kernel::KAddressArbiter::Callback)
CONSTRUCT_KERNEL_OBJECT(Kernel::KAddressArbiter)
