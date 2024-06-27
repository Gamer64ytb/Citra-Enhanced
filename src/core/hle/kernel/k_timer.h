// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include "common/common_types.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/slab_helpers.h"

namespace Core {
class Timing;
}

namespace Kernel {

class KTimer;

class TimerManager {
public:
    explicit TimerManager(Core::Timing& timing);
    ~TimerManager();

    u64 GetNextCallbackId() {
        return +m_next_timer_callback_id;
    }

    Core::TimingEventType* GetEventType() {
        return m_timer_callback_event_type;
    }

    void Register(u64 callback_id, KTimer* timer) {
        m_timer_callback_table[callback_id] = timer;
    }

    void Unregister(u64 callback_id) {
        m_timer_callback_table.erase(callback_id);
    }

private:
    void TimerCallback(u64 callback_id, s64 cycles_late);

private:
    [[maybe_unused]] Core::Timing& m_timing;
    Core::TimingEventType* m_timer_callback_event_type{};
    u64 m_next_timer_callback_id{};
    std::unordered_map<u64, KTimer*> m_timer_callback_table;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        ar& m_next_timer_callback_id;
        ar& m_timer_callback_table;
    }
};

class ResourceLimit;
enum class ResetType : u32;

class KTimer final : public KAutoObjectWithSlabHeapAndContainer<KTimer, KSynchronizationObject> {
    KERNEL_AUTOOBJECT_TRAITS(KTimer, KSynchronizationObject);

public:
    explicit KTimer(KernelSystem& kernel);
    ~KTimer() override;

    void Initialize(Process* owner, ResetType reset_type);

    void Finalize() override;

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

    u64 GetInitialDelay() const {
        return m_initial_delay;
    }

    u64 GetIntervalDelay() const {
        return m_interval_delay;
    }

    void Set(s64 initial, s64 interval);
    void Signal(s64 cycles_late);

    void Cancel();
    void Clear();

    void WakeupAllWaitingThreads() override;
    bool ShouldWait(const KThread* thread) const override;
    void Acquire(KThread* thread) override;

private:
    TimerManager& m_timer_manager;
    Process* m_owner{};
    ResetType m_reset_type{};
    u64 m_initial_delay{};
    u64 m_interval_delay{};
    bool m_signaled{};
    u64 m_callback_id{};

    friend class KernelSystem;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        ar& boost::serialization::base_object<KSynchronizationObject>(*this);
        ar& m_owner;
        ar& m_reset_type;
        ar& m_initial_delay;
        ar& m_interval_delay;
        ar& m_signaled;
        ar& m_callback_id;
    }
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KTimer)
CONSTRUCT_KERNEL_OBJECT(Kernel::KTimer)
