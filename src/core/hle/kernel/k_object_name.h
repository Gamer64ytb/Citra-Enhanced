// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/intrusive_list.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_slab_heap.h"

namespace Kernel {

class KObjectNameGlobalData;

class KObjectName : public KSlabAllocated<KObjectName>,
                    public Common::IntrusiveListBaseNode<KObjectName> {
public:
    explicit KObjectName(KernelSystem&) {}
    virtual ~KObjectName() = default;

    static constexpr size_t NameLengthMax = 12;
    using List = Common::IntrusiveListBaseTraits<KObjectName>::ListType;

    static Result NewFromName(KernelSystem& kernel, KAutoObject* obj, const char* name);
    static Result Delete(KernelSystem& kernel, KAutoObject* obj, const char* name);

    static KScopedAutoObject<KAutoObject> Find(KernelSystem& kernel, const char* name);

    template <typename Derived>
    static Result Delete(KernelSystem& kernel, const char* name) {
        // Find the object.
        KScopedAutoObject obj = Find(kernel, name);
        R_UNLESS(obj.IsNotNull(), ResultNotFound);

        // Cast the object to the desired type.
        Derived* derived = obj->DynamicCast<Derived*>();
        R_UNLESS(derived != nullptr, ResultNotFound);

        // Check that the object is closed.
        R_UNLESS(derived->IsServerClosed(), ResultInvalidAddressState);

        return Delete(kernel, obj.GetPointerUnsafe(), name);
    }

    template <typename Derived>
        requires(std::derived_from<Derived, KAutoObject>)
    static KScopedAutoObject<Derived> Find(KernelSystem& kernel, const char* name) {
        return Find(kernel, name);
    }

private:
    static KScopedAutoObject<KAutoObject> FindImpl(KernelSystem& kernel, const char* name);

    void Initialize(KAutoObject* obj, const char* name);

    bool MatchesName(const char* name) const;
    KAutoObject* GetObject() const {
        return m_object;
    }

private:
    std::array<char, NameLengthMax> m_name{};
    KAutoObject* m_object{};
};

class KObjectNameGlobalData {
public:
    explicit KObjectNameGlobalData(KernelSystem& kernel);
    ~KObjectNameGlobalData();

    KObjectName::List& GetObjectList() {
        return m_object_list;
    }

private:
    // KMutex m_mutex;
    KObjectName::List m_object_list;
};

} // namespace Kernel
