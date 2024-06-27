// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <boost/serialization/export.hpp>
#include "core/global.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

class KThread;

class KMutex final : public KAutoObjectWithSlabHeapAndContainer<KMutex, KSynchronizationObject> {
    KERNEL_AUTOOBJECT_TRAITS(KMutex, KSynchronizationObject);

public:
    explicit KMutex(KernelSystem& kernel);
    ~KMutex() override;

    void Initialize(Process* owner, bool initial_locked);

    uintptr_t GetPostDestroyArgument() const override {
        return reinterpret_cast<uintptr_t>(m_owner);
    }

    static void PostDestroy(uintptr_t arg);

    Process* GetOwner() const override {
        return m_owner;
    }

    u32 GetPriority() const {
        return m_priority;
    }

    bool ShouldWait(const KThread* thread) const override;
    void Acquire(KThread* thread) override;

    void AddWaitingThread(KThread* thread) override;
    void RemoveWaitingThread(KThread* thread) override;

    /**
     * Elevate the mutex priority to the best priority
     * among the priorities of all its waiting threads.
     */
    void UpdatePriority();

    /**
     * Attempts to release the mutex from the specified thread.
     * @param thread Thread that wants to release the mutex.
     * @returns The result code of the operation.
     */
    Result Release(KThread* thread);

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

public:
    Process* m_owner{};
    int m_lock_count{};
    u32 m_priority{};
    KThread* m_holding_thread{};
};

/**
 * Releases all the mutexes held by the specified thread
 * @param thread Thread that is holding the mutexes
 */
void ReleaseThreadMutexes(KThread* thread);

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KMutex)
CONSTRUCT_KERNEL_OBJECT(Kernel::KMutex)
