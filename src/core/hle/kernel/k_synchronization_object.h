// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <span>
#include <vector>
#include <boost/serialization/access.hpp>
#include <boost/serialization/export.hpp>
#include "core/hle/kernel/k_auto_object.h"

namespace Kernel {

class KThread;

class KSynchronizationObject : public KAutoObject {
    KERNEL_AUTOOBJECT_TRAITS(KSynchronizationObject, KAutoObject);

public:
    explicit KSynchronizationObject(KernelSystem& kernel);
    ~KSynchronizationObject();

    /**
     * Check if the specified thread should wait until the object is available
     * @param thread The thread about which we're deciding.
     * @return True if the current thread should wait due to this object being unavailable
     */
    virtual bool ShouldWait(const KThread* thread) const = 0;

    /// Acquire/lock the object for the specified thread if it is available
    virtual void Acquire(KThread* thread) = 0;

    /**
     * Add a thread to wait on this object
     * @param thread Pointer to thread to add
     */
    virtual void AddWaitingThread(KThread* thread);

    /**
     * Removes a thread from waiting on this object (e.g. if it was resumed already)
     * @param thread Pointer to thread to remove
     */
    virtual void RemoveWaitingThread(KThread* thread);

    /**
     * Wake up all threads waiting on this object that can be awoken, in priority order,
     * and set the synchronization result and output of the thread.
     */
    virtual void WakeupAllWaitingThreads();

    /// Obtains the highest priority thread that is ready to run from this object's waiting list.
    KThread* GetHighestPriorityReadyThread() const;

    /// Get a const reference to the waiting threads list for debug use
    const std::vector<KThread*>& GetWaitingThreads() const;

    /// Sets a callback which is called when the object becomes available
    void SetHLENotifier(std::function<void()> callback);

private:
    /// Threads waiting for this object to become available
    std::vector<KThread*> waiting_threads;

    /// Function to call when this object becomes available
    std::function<void()> hle_notifier;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KSynchronizationObject)
