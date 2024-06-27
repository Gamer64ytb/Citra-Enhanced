// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"

namespace Kernel {

class KScopedResourceReservation {
public:
    explicit KScopedResourceReservation(KResourceLimit* l, ResourceLimitType type, s32 amount = 1)
        : m_limit(l), m_amount(amount), m_type(type) {
        if (m_limit) {
            m_succeeded = m_limit->Reserve(m_type, m_amount);
        } else {
            m_succeeded = true;
        }
    }

    explicit KScopedResourceReservation(const Process* p, ResourceLimitType type, s32 amount = 1)
        : KScopedResourceReservation(p->resource_limit, type, amount) {}

    ~KScopedResourceReservation() noexcept {
        if (m_limit && m_succeeded) {
            // Resource was not committed, release the reservation.
            m_limit->Release(m_type, m_amount);
        }
    }

    /// Commit the resource reservation, destruction of this object does not release the resource
    void Commit() {
        m_limit = nullptr;
    }

    bool Succeeded() const {
        return m_succeeded;
    }

private:
    KResourceLimit* m_limit{};
    s32 m_amount{};
    ResourceLimitType m_type{};
    bool m_succeeded{};
};

} // namespace Kernel
