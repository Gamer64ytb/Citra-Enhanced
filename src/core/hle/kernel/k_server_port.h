// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <boost/serialization/export.hpp>
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_synchronization_object.h"

namespace Kernel {

class KClientPort;
class KServerSession;
class KPort;
class SessionRequestHandler;

class KServerPort final : public KSynchronizationObject {
    KERNEL_AUTOOBJECT_TRAITS(KServerPort, KSynchronizationObject);

public:
    explicit KServerPort(KernelSystem& kernel);
    ~KServerPort() override;

    void Initialize(KPort* parent, std::string name);

    void Destroy() override;

    void EnqueueSession(KServerSession* session);

    KServerSession* AcceptSession();

    bool ShouldWait(const KThread* thread) const override;
    void Acquire(KThread* thread) override;

    void SetHleHandler(std::shared_ptr<SessionRequestHandler> hle_handler_) {
        m_hle_handler = std::move(hle_handler_);
    }

    std::shared_ptr<SessionRequestHandler> GetHleHandler() {
        return m_hle_handler;
    }

private:
    KPort* m_parent{};
    std::string m_name;
    std::vector<KServerSession*> m_pending_sessions;
    std::shared_ptr<SessionRequestHandler> m_hle_handler;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KServerPort)
CONSTRUCT_KERNEL_OBJECT(Kernel::KServerPort)
