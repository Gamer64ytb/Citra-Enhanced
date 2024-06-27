// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/intrusive_list.h"
#include "core/hle/kernel/k_slab_heap.h"

namespace Kernel {

class KernelSystem;

class KLinkedListNode : public Common::IntrusiveListBaseNode<KLinkedListNode>,
                        public KSlabAllocated<KLinkedListNode> {

public:
    explicit KLinkedListNode(KernelSystem&) {}
    KLinkedListNode() = default;

    void Initialize(void* it) {
        m_item = it;
    }

    void* GetItem() const {
        return m_item;
    }

private:
    void* m_item = nullptr;
};

template <typename T>
class KLinkedList : private Common::IntrusiveListBaseTraits<KLinkedListNode>::ListType {
private:
    using BaseList = Common::IntrusiveListBaseTraits<KLinkedListNode>::ListType;

public:
    template <bool Const>
    class Iterator;

    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using reference = value_type&;
    using const_reference = const value_type&;
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    template <bool Const>
    class Iterator {
    private:
        using BaseIterator = BaseList::iterator;
        friend class KLinkedList;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = typename KLinkedList::value_type;
        using difference_type = typename KLinkedList::difference_type;
        using pointer = std::conditional_t<Const, KLinkedList::const_pointer, KLinkedList::pointer>;
        using reference =
            std::conditional_t<Const, KLinkedList::const_reference, KLinkedList::reference>;

    public:
        explicit Iterator(BaseIterator it) : m_base_it(it) {}

        pointer GetItem() const {
            return static_cast<pointer>(m_base_it->GetItem());
        }

        bool operator==(const Iterator& rhs) const {
            return m_base_it == rhs.m_base_it;
        }

        bool operator!=(const Iterator& rhs) const {
            return !(*this == rhs);
        }

        pointer operator->() const {
            return this->GetItem();
        }

        reference operator*() const {
            return *this->GetItem();
        }

        Iterator& operator++() {
            ++m_base_it;
            return *this;
        }

        Iterator& operator--() {
            --m_base_it;
            return *this;
        }

        Iterator operator++(int) {
            const Iterator it{*this};
            ++(*this);
            return it;
        }

        Iterator operator--(int) {
            const Iterator it{*this};
            --(*this);
            return it;
        }

        operator Iterator<true>() const {
            return Iterator<true>(m_base_it);
        }

    private:
        BaseIterator m_base_it;
    };

public:
    constexpr KLinkedList(KernelSystem& kernel_) : BaseList(), kernel{kernel_} {}

    ~KLinkedList() {
        // Erase all elements.
        for (auto it = begin(); it != end(); it = erase(it)) {
        }

        // Ensure we succeeded.
        ASSERT(this->empty());
    }

    // Iterator accessors.
    iterator begin() {
        return iterator(BaseList::begin());
    }

    const_iterator begin() const {
        return const_iterator(BaseList::begin());
    }

    iterator end() {
        return iterator(BaseList::end());
    }

    const_iterator end() const {
        return const_iterator(BaseList::end());
    }

    const_iterator cbegin() const {
        return this->begin();
    }

    const_iterator cend() const {
        return this->end();
    }

    reverse_iterator rbegin() {
        return reverse_iterator(this->end());
    }

    const_reverse_iterator rbegin() const {
        return const_reverse_iterator(this->end());
    }

    reverse_iterator rend() {
        return reverse_iterator(this->begin());
    }

    const_reverse_iterator rend() const {
        return const_reverse_iterator(this->begin());
    }

    const_reverse_iterator crbegin() const {
        return this->rbegin();
    }

    const_reverse_iterator crend() const {
        return this->rend();
    }

    // Content management.
    using BaseList::empty;
    using BaseList::size;

    reference back() {
        return *(--this->end());
    }

    const_reference back() const {
        return *(--this->end());
    }

    reference front() {
        return *this->begin();
    }

    const_reference front() const {
        return *this->begin();
    }

    iterator insert(const_iterator pos, reference ref) {
        KLinkedListNode* new_node = KLinkedListNode::Allocate(kernel);
        ASSERT(new_node != nullptr);
        new_node->Initialize(std::addressof(ref));
        return iterator(BaseList::insert(pos.m_base_it, *new_node));
    }

    void push_back(reference ref) {
        this->insert(this->end(), ref);
    }

    void push_front(reference ref) {
        this->insert(this->begin(), ref);
    }

    void pop_back() {
        this->erase(--this->end());
    }

    void pop_front() {
        this->erase(this->begin());
    }

    iterator erase(const iterator pos) {
        KLinkedListNode* freed_node = std::addressof(*pos.m_base_it);
        iterator ret = iterator(BaseList::erase(pos.m_base_it));
        KLinkedListNode::Free(kernel, freed_node);

        return ret;
    }

private:
    KernelSystem& kernel;
};

} // namespace Kernel
