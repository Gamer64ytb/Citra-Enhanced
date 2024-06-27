// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "core/core.h"
#include "core/hle/kernel/k_object_name.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/result.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/sm/srv.h"

namespace Service::SM {

static constexpr size_t MaxRegisteredServices = 160;

static Result ValidateServiceName(const std::string& name) {
    R_UNLESS(name.size() > 0 && name.size() <= 8, ResultInvalidNameSize);
    R_UNLESS(name.find('\0') == std::string::npos, ResultNameContainsNul);
    return ResultSuccess;
}

ServiceManager::ServiceManager(Core::System& system) : system(system) {}

void ServiceManager::InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    ASSERT(service_manager.srv_interface.expired());

    auto srv = std::make_shared<SRV>(system);
    srv->InstallAsNamedPort(service_manager, system.Kernel());
    service_manager.srv_interface = srv;
}

Result ServiceManager::RegisterService(Kernel::KServerPort** out_port, std::string name,
                                       u32 max_sessions) {
    // Validate service name.
    R_TRY(ValidateServiceName(name));
    R_UNLESS(service_ports.size() < MaxRegisteredServices, ResultTooManyServices);

    // Create the port
    auto& kernel = system.Kernel();
    auto* port = Kernel::KPort::Create(kernel);

    // Initialize and register the port.
    port->Initialize(max_sessions, name);
    Kernel::KPort::Register(kernel, port);
    Kernel::KObjectName::NewFromName(kernel, &port->GetClientPort(), name.c_str());

    // Register port.
    service_ports.emplace(name, &port->GetClientPort());
    *out_port = std::addressof(port->GetServerPort());
    return ResultSuccess;
}

Result ServiceManager::RegisterPort(Kernel::KClientPort* port, std::string name) {
    // Validate port name.
    R_TRY(ValidateServiceName(name));
    R_UNLESS(service_ports.find(name) == service_ports.end(), ResultAlreadyRegistered);
    R_UNLESS(service_ports.size() < MaxRegisteredServices, ResultTooManyServices);

    // Register port.
    service_ports.emplace(name, port);
    return ResultSuccess;
}

Result ServiceManager::GetServicePort(Kernel::KClientPort** out_port, const std::string& name) {
    // Validate service name.
    R_TRY(ValidateServiceName(name));

    // Get the port.
    auto it = service_ports.find(name);
    R_UNLESS(it != service_ports.end(), ResultServiceNotRegistered);

    // Return it.
    *out_port = it->second;
    return ResultSuccess;
}

Result ServiceManager::ConnectToService(Kernel::KClientSession** out_session,
                                        const std::string& name) {
    Kernel::KClientPort* client_port;
    R_TRY(this->GetServicePort(std::addressof(client_port), name));
    return client_port->CreateSession(out_session);
}

std::string ServiceManager::GetServiceNameByPortId(u32 port) const {
    if (registered_services_inverse.count(port)) {
        return registered_services_inverse.at(port);
    }

    return "";
}

} // namespace Service::SM
