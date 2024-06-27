// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

#include "common/assert.h"

namespace Common {
namespace detail {
template <typename T, size_t Size, size_t Align>
struct TypedStorageImpl {
    alignas(Align) u8 storage_[Size];
};
} // namespace detail

template <typename T>
using TypedStorage = detail::TypedStorageImpl<T, sizeof(T), alignof(T)>;

template <typename T>
static constexpr T* GetPointer(TypedStorage<T>& ts) {
    return static_cast<T*>(static_cast<void*>(std::addressof(ts.storage_)));
}

template <typename T>
static constexpr const T* GetPointer(const TypedStorage<T>& ts) {
    return static_cast<const T*>(static_cast<const void*>(std::addressof(ts.storage_)));
}

namespace impl {

template <size_t MaxDepth>
struct OffsetOfUnionHolder {
    template <typename ParentType, typename MemberType, size_t Offset>
    union UnionImpl {
        using PaddingMember = char;
        static constexpr size_t GetOffset() {
            return Offset;
        }

#pragma pack(push, 1)
        struct {
            PaddingMember padding[Offset];
            MemberType members[(sizeof(ParentType) / sizeof(MemberType)) + 1];
        } data;
#pragma pack(pop)
        UnionImpl<ParentType, MemberType, Offset + 1> next_union;
    };

    template <typename ParentType, typename MemberType>
    union UnionImpl<ParentType, MemberType, 0> {
        static constexpr size_t GetOffset() {
            return 0;
        }

        struct {
            MemberType members[(sizeof(ParentType) / sizeof(MemberType)) + 1];
        } data;
        UnionImpl<ParentType, MemberType, 1> next_union;
    };

    template <typename ParentType, typename MemberType>
    union UnionImpl<ParentType, MemberType, MaxDepth> {};
};

template <typename ParentType, typename MemberType>
struct OffsetOfCalculator {
    using UnionHolder =
        typename OffsetOfUnionHolder<sizeof(MemberType)>::template UnionImpl<ParentType, MemberType,
                                                                             0>;
    union Union {
        char c{};
        UnionHolder first_union;
        TypedStorage<ParentType> parent;

        constexpr Union() : c() {}
    };
    static constexpr Union U = {};

    static constexpr const MemberType* GetNextAddress(const MemberType* start,
                                                      const MemberType* target) {
        while (start < target) {
            start++;
        }
        return start;
    }

    static constexpr std::ptrdiff_t GetDifference(const MemberType* start,
                                                  const MemberType* target) {
        return (target - start) * sizeof(MemberType);
    }

    template <typename CurUnion>
    static constexpr std::ptrdiff_t OffsetOfImpl(MemberType ParentType::*member,
                                                 CurUnion& cur_union) {
        constexpr size_t Offset = CurUnion::GetOffset();
        const auto target = std::addressof(GetPointer(U.parent)->*member);
        const auto start = std::addressof(cur_union.data.members[0]);
        const auto next = GetNextAddress(start, target);

        if (next != target) {
            if constexpr (Offset < sizeof(MemberType) - 1) {
                return OffsetOfImpl(member, cur_union.next_union);
            } else {
                UNREACHABLE();
            }
        }

        return static_cast<ptrdiff_t>(static_cast<size_t>(next - start) * sizeof(MemberType) +
                                      Offset);
    }

    static constexpr std::ptrdiff_t OffsetOf(MemberType ParentType::*member) {
        return OffsetOfImpl(member, U.first_union);
    }
};

template <typename T>
struct GetMemberPointerTraits;

template <typename P, typename M>
struct GetMemberPointerTraits<M P::*> {
    using Parent = P;
    using Member = M;
};

template <auto MemberPtr>
using GetParentType = typename GetMemberPointerTraits<decltype(MemberPtr)>::Parent;

template <auto MemberPtr>
using GetMemberType = typename GetMemberPointerTraits<decltype(MemberPtr)>::Member;

template <auto MemberPtr, typename RealParentType = GetParentType<MemberPtr>>
constexpr std::ptrdiff_t OffsetOf() {
    using DeducedParentType = GetParentType<MemberPtr>;
    using MemberType = GetMemberType<MemberPtr>;
    static_assert(std::is_base_of<DeducedParentType, RealParentType>::value ||
                  std::is_same<RealParentType, DeducedParentType>::value);

    return OffsetOfCalculator<RealParentType, MemberType>::OffsetOf(MemberPtr);
};

} // namespace impl

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType& GetParentReference(impl::GetMemberType<MemberPtr>* member) {
    std::ptrdiff_t Offset = impl::OffsetOf<MemberPtr, RealParentType>();
    return *static_cast<RealParentType*>(
        static_cast<void*>(static_cast<uint8_t*>(static_cast<void*>(member)) - Offset));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType const& GetParentReference(impl::GetMemberType<MemberPtr> const* member) {
    std::ptrdiff_t Offset = impl::OffsetOf<MemberPtr, RealParentType>();
    return *static_cast<const RealParentType*>(static_cast<const void*>(
        static_cast<const uint8_t*>(static_cast<const void*>(member)) - Offset));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType* GetParentPointer(impl::GetMemberType<MemberPtr>* member) {
    return std::addressof(GetParentReference<MemberPtr, RealParentType>(member));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType const* GetParentPointer(impl::GetMemberType<MemberPtr> const* member) {
    return std::addressof(GetParentReference<MemberPtr, RealParentType>(member));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType& GetParentReference(impl::GetMemberType<MemberPtr>& member) {
    return GetParentReference<MemberPtr, RealParentType>(std::addressof(member));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType const& GetParentReference(impl::GetMemberType<MemberPtr> const& member) {
    return GetParentReference<MemberPtr, RealParentType>(std::addressof(member));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType* GetParentPointer(impl::GetMemberType<MemberPtr>& member) {
    return std::addressof(GetParentReference<MemberPtr, RealParentType>(member));
}

template <auto MemberPtr, typename RealParentType = impl::GetParentType<MemberPtr>>
constexpr RealParentType const* GetParentPointer(impl::GetMemberType<MemberPtr> const& member) {
    return std::addressof(GetParentReference<MemberPtr, RealParentType>(member));
}

} // namespace Common
