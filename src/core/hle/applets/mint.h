// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/applets/applet.h"

namespace Kernel {
class KSharedMemory;
}

namespace HLE::Applets {

class Mint final : public Applet {
public:
    explicit Mint(Core::System& system, Service::APT::AppletId id, Service::APT::AppletId parent,
                  bool preload, std::weak_ptr<Service::APT::AppletManager> manager)
        : Applet(system, id, parent, preload, std::move(manager)) {}

    Result ReceiveParameterImpl(const Service::APT::MessageParameter& parameter) override;
    Result Start(const Service::APT::MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override;

private:
    /// This SharedMemory will be created when we receive the Request message.
    /// It holds the framebuffer info retrieved by the application with
    /// GSPGPU::ImportDisplayCaptureInfo
    Kernel::KSharedMemory* framebuffer_memory;

    /// Parameter received by the applet on start.
    std::vector<u8> startup_param;
};

} // namespace HLE::Applets
