// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include "core/hle/kernel/k_port.h"
#include "core/hle/result.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KClientSession;
class SessionRequestHandler;
class KPort;
} // namespace Kernel

namespace Service::SM {

class SRV;

constexpr Result ResultServiceNotRegistered(1, ErrorModule::SRV, ErrorSummary::WouldBlock,
                                            ErrorLevel::Temporary); // 0xD0406401
constexpr Result ResultMaxConnectionsReached(2, ErrorModule::SRV, ErrorSummary::WouldBlock,
                                             ErrorLevel::Temporary); // 0xD0406402
constexpr Result ResultInvalidNameSize(5, ErrorModule::SRV, ErrorSummary::WrongArgument,
                                       ErrorLevel::Permanent); // 0xD9006405
constexpr Result ResultAccessDenied(6, ErrorModule::SRV, ErrorSummary::InvalidArgument,
                                    ErrorLevel::Permanent); // 0xD8E06406
constexpr Result ResultNameContainsNul(7, ErrorModule::SRV, ErrorSummary::WrongArgument,
                                       ErrorLevel::Permanent); // 0xD9006407
constexpr Result ResultAlreadyRegistered(ErrorDescription::AlreadyExists, ErrorModule::OS,
                                         ErrorSummary::WrongArgument,
                                         ErrorLevel::Permanent); // 0xD9001BFC
constexpr Result ResultTooManyServices(ErrorDescription::OutOfMemory, ErrorModule::SRV,
                                       ErrorSummary::OutOfResource,
                                       ErrorLevel::Permanent); // 0xD86067F3

class ServiceManager {
public:
    static void InstallInterfaces(Core::System& system);

    explicit ServiceManager(Core::System& system);

    Result RegisterService(Kernel::KServerPort** out_port, std::string name, u32 max_sessions);
    Result RegisterPort(Kernel::KClientPort* port, std::string name);
    Result GetServicePort(Kernel::KClientPort** out_port, const std::string& name);
    Result ConnectToService(Kernel::KClientSession** out_session, const std::string& name);

    // For IPC Recorder
    std::string GetServiceNameByPortId(u32 port) const;

    template <typename T>
    std::shared_ptr<T> GetService(const std::string& service_name) const {
        static_assert(std::is_base_of_v<Kernel::SessionRequestHandler, T>,
                      "Not a base of ServiceFrameworkBase");
        auto service = service_ports.find(service_name);
        if (service == service_ports.end()) {
            LOG_DEBUG(Service, "Can't find service: {}", service_name);
            return nullptr;
        }
        auto& port = service->second->GetParent()->GetServerPort();
        return std::static_pointer_cast<T>(port.GetHleHandler());
    }

private:
    Core::System& system;
    std::weak_ptr<SRV> srv_interface;

    std::unordered_map<std::string, Kernel::KClientPort*> service_ports;
    std::unordered_map<u32, std::string> registered_services_inverse;

    template <class Archive>
    void save(Archive& ar, const u32 file_version) const {
        ar << service_ports;
    }

    template <class Archive>
    void load(Archive& ar, const u32 file_version) {
        ar >> service_ports;
        registered_services_inverse.clear();
        /*for (const auto& [name, port] : service_ports) {
            registered_services_inverse.emplace(port->GetObjectId(), name);
        }*/
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    friend class boost::serialization::access;
};

} // namespace Service::SM
