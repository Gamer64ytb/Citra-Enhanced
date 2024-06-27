// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <boost/serialization/export.hpp>
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/result.h"

namespace Kernel {

class KSession;
class KThread;

class KClientSession final : public KAutoObject {
    KERNEL_AUTOOBJECT_TRAITS(KClientSession, KAutoObject);

public:
    explicit KClientSession(KernelSystem& kernel);
    ~KClientSession() override;

    void Initialize(KSession* parent) {
        // Set member variables.
        m_parent = parent;
    }

    void Destroy() override;

    KSession* GetParent() const {
        return m_parent;
    }

    Result SendSyncRequest(KThread* thread);

    void OnServerClosed();

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

private:
    KSession* m_parent{};
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KClientSession)
CONSTRUCT_KERNEL_OBJECT(Kernel::KClientSession)
