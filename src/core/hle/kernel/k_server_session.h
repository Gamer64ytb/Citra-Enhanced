// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "core/hle/kernel/ipc.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/result.h"

namespace Kernel {

class ClientSession;
class ClientPort;
class KSession;
class SessionRequestHandler;
class KThread;

class KServerSession final : public KSynchronizationObject {
    KERNEL_AUTOOBJECT_TRAITS(KServerSession, KSynchronizationObject);

public:
    ~KServerSession() override;
    explicit KServerSession(KernelSystem& kernel);

    void Destroy() override;

    void Initialize(KSession* parent) {
        m_parent = parent;
    }

    KSession* GetParent() const {
        return m_parent;
    }

    KThread* GetCurrent() {
        return currently_handling;
    }

    std::vector<MappedBufferContext>& GetMappedBufferContext() {
        return mapped_buffer_context;
    }

    void SetHleHandler(std::shared_ptr<SessionRequestHandler>&& hle_handler_) {
        hle_handler = std::move(hle_handler_);
    }

    std::shared_ptr<SessionRequestHandler>& GetHleHandler() {
        return hle_handler;
    }

    void OnClientClosed();
    Result HandleSyncRequest(KThread* thread);

    bool ShouldWait(const KThread* thread) const override;

    void Acquire(KThread* thread) override;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

public:
    std::string m_name;
    KSession* m_parent{};
    std::shared_ptr<SessionRequestHandler> hle_handler;
    std::vector<KThread*> pending_requesting_threads;
    KThread* currently_handling;
    std::vector<MappedBufferContext> mapped_buffer_context;
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KServerSession)
CONSTRUCT_KERNEL_OBJECT(Kernel::KServerSession)
