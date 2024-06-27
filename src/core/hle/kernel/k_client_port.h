// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/result.h"

namespace Kernel {

class KClientSession;

class KClientPort final : public KAutoObject {
    KERNEL_AUTOOBJECT_TRAITS(KClientPort, KAutoObject);

public:
    explicit KClientPort(KernelSystem& kernel);
    ~KClientPort() override;

    void Initialize(KPort* parent, s32 max_sessions, std::string name);

    const KPort* GetParent() const {
        return m_parent;
    }
    KPort* GetParent() {
        return m_parent;
    }

    Result CreateSession(KClientSession** out);
    void ConnectionClosed();

private:
    KPort* m_parent{};
    u32 m_max_sessions{};
    u32 m_active_sessions{};
    std::string m_name;

    friend class KernelSystem;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KClientPort)
CONSTRUCT_KERNEL_OBJECT(Kernel::KClientPort)
