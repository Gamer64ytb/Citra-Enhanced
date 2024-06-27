// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <boost/serialization/access.hpp>

#include "common/assert.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/global.h"

namespace Kernel {

class KernelSystem;
class Process;

using Handle = u32;

constexpr u32 DefaultStackSize = 0x4000;

enum class ClassTokenType : u32 {
    KAutoObject = 0,
    KSynchronizationObject = 1,
    KSemaphore = 27,
    KEvent = 31,
    KTimer = 53,
    KMutex = 57,
    Debug = 77,
    KServerPort = 85,
    DmaObject = 89,
    KClientPort = 101,
    CodeSet = 104,
    KSession = 112,
    KThread = 141,
    KServerSession = 149,
    KAddressArbiter = 152,
    KClientSession = 165,
    KPort = 168,
    KSharedMemory = 176,
    Process = 197,
    KResourceLimit = 200,
};
DECLARE_ENUM_FLAG_OPERATORS(ClassTokenType)

#define KERNEL_AUTOOBJECT_TRAITS_IMPL(CLASS, BASE_CLASS, ATTRIBUTE)                                \
private:                                                                                           \
    static constexpr inline const char* const TypeName = #CLASS;                                   \
    static constexpr inline auto ClassToken = ClassTokenType::CLASS;                               \
                                                                                                   \
public:                                                                                            \
    CITRA_NON_COPYABLE(CLASS);                                                                     \
    CITRA_NON_MOVEABLE(CLASS);                                                                     \
                                                                                                   \
    using BaseClass = BASE_CLASS;                                                                  \
    static constexpr TypeObj GetStaticTypeObj() { return TypeObj(TypeName, ClassToken); }          \
    static constexpr const char* GetStaticTypeName() { return TypeName; }                          \
    virtual TypeObj GetTypeObj() ATTRIBUTE { return GetStaticTypeObj(); }                          \
    virtual const char* GetTypeName() ATTRIBUTE { return GetStaticTypeName(); }                    \
                                                                                                   \
private:                                                                                           \
    constexpr bool operator!=(const TypeObj& rhs)

#define KERNEL_AUTOOBJECT_TRAITS(CLASS, BASE_CLASS)                                                \
    KERNEL_AUTOOBJECT_TRAITS_IMPL(CLASS, BASE_CLASS, const override)

class KAutoObject {
protected:
    class TypeObj {
    public:
        constexpr explicit TypeObj(const char* n, ClassTokenType tok)
            : m_name(n), m_class_token(tok) {}

        constexpr const char* GetName() const {
            return m_name;
        }
        constexpr ClassTokenType GetClassToken() const {
            return m_class_token;
        }

        constexpr bool operator==(const TypeObj& rhs) const {
            return this->GetClassToken() == rhs.GetClassToken();
        }

        constexpr bool operator!=(const TypeObj& rhs) const {
            return this->GetClassToken() != rhs.GetClassToken();
        }

        constexpr bool IsDerivedFrom(const TypeObj& rhs) const {
            return (this->GetClassToken() | rhs.GetClassToken()) == this->GetClassToken();
        }

    private:
        const char* m_name;
        ClassTokenType m_class_token;
    };

private:
    KERNEL_AUTOOBJECT_TRAITS_IMPL(KAutoObject, KAutoObject, const);

public:
    explicit KAutoObject(KernelSystem& kernel) : m_kernel(kernel) {
        RegisterWithKernel();
    }
    virtual ~KAutoObject() = default;

    static KAutoObject* Create(KAutoObject* ptr);

    // Destroy is responsible for destroying the auto object's resources when ref_count hits zero.
    virtual void Destroy() {
        UNIMPLEMENTED();
    }

    // Finalize is responsible for cleaning up resource, but does not destroy the object.
    virtual void Finalize() {}

    virtual Process* GetOwner() const {
        return nullptr;
    }

    u32 GetReferenceCount() const {
        return m_ref_count.load();
    }

    bool IsDerivedFrom(const TypeObj& rhs) const {
        return this->GetTypeObj().IsDerivedFrom(rhs);
    }

    bool IsDerivedFrom(const KAutoObject& rhs) const {
        return this->IsDerivedFrom(rhs.GetTypeObj());
    }

    template <typename Derived>
    Derived DynamicCast() {
        static_assert(std::is_pointer_v<Derived>);
        using DerivedType = std::remove_pointer_t<Derived>;

        if (this->IsDerivedFrom(DerivedType::GetStaticTypeObj())) {
            return static_cast<Derived>(this);
        } else {
            return nullptr;
        }
    }

    template <typename Derived>
    const Derived DynamicCast() const {
        static_assert(std::is_pointer_v<Derived>);
        using DerivedType = std::remove_pointer_t<Derived>;

        if (this->IsDerivedFrom(DerivedType::GetStaticTypeObj())) {
            return static_cast<Derived>(this);
        } else {
            return nullptr;
        }
    }

    bool Open() {
        // Atomically increment the reference count, only if it's positive.
        u32 cur_ref_count = m_ref_count.load(std::memory_order_acquire);
        do {
            if (cur_ref_count == 0) {
                return false;
            }
            ASSERT(cur_ref_count < cur_ref_count + 1);
        } while (!m_ref_count.compare_exchange_weak(cur_ref_count, cur_ref_count + 1,
                                                    std::memory_order_relaxed));

        return true;
    }

    void Close() {
        // Atomically decrement the reference count, not allowing it to become negative.
        u32 cur_ref_count = m_ref_count.load(std::memory_order_acquire);
        do {
            ASSERT(cur_ref_count > 0);
        } while (!m_ref_count.compare_exchange_weak(cur_ref_count, cur_ref_count - 1,
                                                    std::memory_order_acq_rel));

        // If ref count hits zero, destroy the object.
        if (cur_ref_count - 1 == 0) {
            KernelSystem& kernel = m_kernel;
            this->Destroy();
            KAutoObject::UnregisterWithKernel(kernel, this);
        }
    }

private:
    void RegisterWithKernel();
    static void UnregisterWithKernel(KernelSystem& kernel, KAutoObject* self);

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);

protected:
    KernelSystem& m_kernel;
    std::string m_name{};

private:
    std::atomic<u32> m_ref_count{};
};

template <typename T>
class KScopedAutoObject {
public:
    CITRA_NON_COPYABLE(KScopedAutoObject);

    constexpr KScopedAutoObject() = default;

    constexpr KScopedAutoObject(T* o) : m_obj(o) {
        if (m_obj != nullptr) {
            m_obj->Open();
        }
    }

    ~KScopedAutoObject() {
        if (m_obj != nullptr) {
            m_obj->Close();
        }
        m_obj = nullptr;
    }

    template <typename U>
        requires(std::derived_from<T, U> || std::derived_from<U, T>)
    constexpr KScopedAutoObject(KScopedAutoObject<U>&& rhs) {
        if constexpr (std::derived_from<U, T>) {
            // Upcast.
            m_obj = rhs.m_obj;
            rhs.m_obj = nullptr;
        } else {
            // Downcast.
            T* derived = nullptr;
            if (rhs.m_obj != nullptr) {
                derived = rhs.m_obj->template DynamicCast<T*>();
                if (derived == nullptr) {
                    rhs.m_obj->Close();
                }
            }

            m_obj = derived;
            rhs.m_obj = nullptr;
        }
    }

    constexpr KScopedAutoObject<T>& operator=(KScopedAutoObject<T>&& rhs) {
        rhs.Swap(*this);
        return *this;
    }

    constexpr T* operator->() {
        return m_obj;
    }
    constexpr T& operator*() {
        return *m_obj;
    }

    constexpr void Reset(T* o) {
        KScopedAutoObject(o).Swap(*this);
    }

    constexpr T* GetPointerUnsafe() {
        return m_obj;
    }

    constexpr T* GetPointerUnsafe() const {
        return m_obj;
    }

    constexpr T* ReleasePointerUnsafe() {
        T* ret = m_obj;
        m_obj = nullptr;
        return ret;
    }

    constexpr bool IsNull() const {
        return m_obj == nullptr;
    }
    constexpr bool IsNotNull() const {
        return m_obj != nullptr;
    }

private:
    template <typename U>
    friend class KScopedAutoObject;

private:
    T* m_obj{};

private:
    constexpr void Swap(KScopedAutoObject& rhs) noexcept {
        std::swap(m_obj, rhs.m_obj);
    }
};

} // namespace Kernel

#define CONSTRUCT_KERNEL_OBJECT(T)                                                                 \
    namespace boost::serialization {                                                               \
    template <class Archive>                                                                       \
    void load_construct_data(Archive& ar, T* t, const unsigned int file_version) {                 \
        ::new (t) T(Core::Global<Kernel::KernelSystem>());                                         \
    }                                                                                              \
    }
