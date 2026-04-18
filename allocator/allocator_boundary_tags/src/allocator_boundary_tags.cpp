#include <not_implemented.h>
#include "../include/allocator_boundary_tags.h"
#include <memory>
#include <new>
#include <stdexcept>

constexpr size_t parent_off = 0;
constexpr size_t mode_off =
    ((parent_off + sizeof(std::pmr::memory_resource*) + alignof(allocator_with_fit_mode::fit_mode) - 1) /
     alignof(allocator_with_fit_mode::fit_mode)) *
    alignof(allocator_with_fit_mode::fit_mode);
constexpr size_t managed_off =
    ((mode_off + sizeof(allocator_with_fit_mode::fit_mode) + alignof(size_t) - 1) / alignof(size_t)) *
    alignof(size_t);
constexpr size_t mutex_off =
    ((managed_off + sizeof(size_t) + alignof(std::mutex) - 1) / alignof(std::mutex)) * alignof(std::mutex);
constexpr size_t first_occ_off =
    ((mutex_off + sizeof(std::mutex) + alignof(void*) - 1) / alignof(void*)) * alignof(void*);
constexpr size_t kAllocatorMetadataSize = first_occ_off + sizeof(void*);
constexpr size_t kOccupiedBlockMetadataSize = sizeof(size_t) + sizeof(void*) + sizeof(void*) + sizeof(void*);

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* parent = *reinterpret_cast<std::pmr::memory_resource**>(base + parent_off);
    const size_t managed = *reinterpret_cast<size_t*>(base + managed_off);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::destroy_at(mtx);
    parent->deallocate(_trusted_memory, kAllocatorMetadataSize + managed, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags &&other) noexcept:
    _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr)
    {
        return;
    }
    char* other_base = reinterpret_cast<char*>(other._trusted_memory);
    auto* other_mtx = reinterpret_cast<std::mutex*>(other_base + mutex_off);
    std::lock_guard<std::mutex> lock(*other_mtx);
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    std::mutex* this_mtx = nullptr;
    if (_trusted_memory != nullptr)
    {
        char* this_base = reinterpret_cast<char*>(_trusted_memory);
        this_mtx = reinterpret_cast<std::mutex*>(this_base + mutex_off);
    }
    std::mutex* other_mtx = nullptr;
    if (other._trusted_memory != nullptr)
    {
        char* other_base = reinterpret_cast<char*>(other._trusted_memory);
        other_mtx = reinterpret_cast<std::mutex*>(other_base + mutex_off);
    }
    void* old_memory = nullptr;
    if (this_mtx != nullptr && other_mtx != nullptr && this_mtx != other_mtx)
    {
        std::scoped_lock lock(*this_mtx, *other_mtx);
        old_memory = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else if (this_mtx != nullptr)
    {
        std::lock_guard<std::mutex> lock(*this_mtx);
        old_memory = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else if (other_mtx != nullptr)
    {
        std::lock_guard<std::mutex> lock(*other_mtx);
        old_memory = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else
    {
        old_memory = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    if (old_memory != nullptr)
    {
        char* old_base = reinterpret_cast<char*>(old_memory);
        auto* parent = *reinterpret_cast<std::pmr::memory_resource**>(old_base + parent_off);
        const size_t managed = *reinterpret_cast<size_t*>(old_base + managed_off);
        auto* mtx = reinterpret_cast<std::mutex*>(old_base + mutex_off);
        std::destroy_at(mtx);
        parent->deallocate(old_memory, kAllocatorMetadataSize + managed, alignof(std::max_align_t));
    }
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }
    const size_t total_size = kAllocatorMetadataSize + space_size;
    _trusted_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));
    char* base = reinterpret_cast<char*>(_trusted_memory);
    *reinterpret_cast<std::pmr::memory_resource**>(base + parent_off) = parent_allocator;
    *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off) = allocate_fit_mode;
    *reinterpret_cast<size_t*>(base + managed_off) = space_size;
    new (base + mutex_off) std::mutex();
    *reinterpret_cast<void**>(base + first_occ_off) = nullptr;
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    const size_t needed = kOccupiedBlockMetadataSize + size;
    const auto mode = *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off);
    void* head = *reinterpret_cast<void**>(base + first_occ_off);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    if (needed > *reinterpret_cast<size_t*>(base + managed_off))
    {
        throw std::bad_alloc();
    }
    void* best_prev = nullptr;
    void* best_next = nullptr;
    char* best_start = nullptr;
    size_t best_gap = 0;
    if (mode == allocator_with_fit_mode::fit_mode::first_fit)
    {
        void* prev = nullptr;
        void* cur = head;
        char* cursor = region_begin;
        while (cur != nullptr)
        {
            char* cur_start = reinterpret_cast<char*>(cur);
            const size_t gap = static_cast<size_t>(cur_start - cursor);
            if (gap >= needed)
            {
                best_start = cursor;
                best_prev = prev;
                best_next = cur;
                best_gap = gap;
                break;
            }
            cursor = (reinterpret_cast<char*>(cur) + kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(cur)));
            prev = cur;
            cur = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
        }
        if (best_start == nullptr)
        {
            const size_t gap = static_cast<size_t>(region_end - cursor);
            if (gap >= needed)
            {
                best_start = cursor;
                best_prev = prev;
                best_next = nullptr;
                best_gap = gap;
            }
        }
    }
    else
    {
        void* prev = nullptr;
        void* cur = head;
        char* cursor = region_begin;
        while (cur != nullptr)
        {
            char* cur_start = reinterpret_cast<char*>(cur);
            if (cur_start >= cursor)
            {
                const size_t gap = static_cast<size_t>(cur_start - cursor);
                if (gap >= needed)
                {
                    if (best_start == nullptr)
                    {
                        best_start = cursor;
                        best_prev = prev;
                        best_next = cur;
                        best_gap = gap;
                    }
                    else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit && gap < best_gap)
                    {
                        best_start = cursor;
                        best_prev = prev;
                        best_next = cur;
                        best_gap = gap;
                    }
                    else if (mode == allocator_with_fit_mode::fit_mode::the_worst_fit && gap > best_gap)
                    {
                        best_start = cursor;
                        best_prev = prev;
                        best_next = cur;
                        best_gap = gap;
                    }
                }
            }
            cursor = (reinterpret_cast<char*>(cur) + kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(cur)));
            prev = cur;
            cur = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
        }
        if (region_end >= cursor)
        {
            const size_t gap = static_cast<size_t>(region_end - cursor);
            if (gap >= needed)
            {
                if (best_start == nullptr)
                {
                    best_start = cursor;
                    best_prev = prev;
                    best_next = nullptr;
                    best_gap = gap;
                }
                else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit && gap < best_gap)
                {
                    best_start = cursor;
                    best_prev = prev;
                    best_next = nullptr;
                    best_gap = gap;
                }
                else if (mode == allocator_with_fit_mode::fit_mode::the_worst_fit && gap > best_gap)
                {
                    best_start = cursor;
                    best_prev = prev;
                    best_next = nullptr;
                    best_gap = gap;
                }
            }
        }
    }
    if (best_start == nullptr)
    {
        throw std::bad_alloc();
    }
    void* block = best_start;
    size_t payload_size = size;
    if (best_gap > needed && best_gap - needed < kOccupiedBlockMetadataSize)
    {
        payload_size = best_gap - kOccupiedBlockMetadataSize;
    }
    *reinterpret_cast<size_t*>(reinterpret_cast<char*>(block)) = payload_size;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t)) = best_prev;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*)) = best_next;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*) * 2) = _trusted_memory;
    if (best_prev != nullptr)
    {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(best_prev) + sizeof(size_t) + sizeof(void*)) = block;
    }
    else
    {
        *reinterpret_cast<void**>(base + first_occ_off) = block;
    }
    if (best_next != nullptr)
    {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(best_next) + sizeof(size_t)) = block;
    }
    return (reinterpret_cast<char*>(block) + kOccupiedBlockMetadataSize);
}

void allocator_boundary_tags::do_deallocate_sm(
    void *at)
{
    if (at == nullptr)
    {
        return;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    char* payload = reinterpret_cast<char*>(at);
    if (payload < region_begin + kOccupiedBlockMetadataSize || payload >= region_end)
    {
        throw std::invalid_argument("");
    }
    void* block = payload - kOccupiedBlockMetadataSize;
    if (*reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*) * 2) != _trusted_memory)
    {
        throw std::invalid_argument("");
    }
    void* prev = *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t));
    void* next = *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*));
    if (prev != nullptr)
    {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(prev) + sizeof(size_t) + sizeof(void*)) = next;
    }
    else
    {
        *reinterpret_cast<void**>(base + first_occ_off) = next;
    }
    if (next != nullptr)
    {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(next) + sizeof(size_t)) = prev;
    }
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*) * 2) = nullptr;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t)) = nullptr;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + sizeof(size_t) + sizeof(void*)) = nullptr;
    *reinterpret_cast<size_t*>(reinterpret_cast<char*>(block)) = 0;
}

inline void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    if (_trusted_memory == nullptr)
    {
        return {};
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    return get_blocks_info_inner();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return boundary_iterator();
    }
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    if (_trusted_memory == nullptr)
    {
        return result;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    void* cur_occ = *reinterpret_cast<void**>(base + first_occ_off);
    char* cursor = region_begin;
    while (cursor < region_end)
    {
        if (cur_occ != nullptr && reinterpret_cast<char*>(cur_occ) == cursor)
        {
            const size_t bytes = kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(cur_occ));
            result.push_back({bytes, true});
            cursor += bytes;
            cur_occ = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur_occ) + sizeof(size_t) + sizeof(void*));
            continue;
        }
        char* free_end = region_end;
        if (cur_occ != nullptr)
        {
            free_end = reinterpret_cast<char*>(cur_occ);
        }
        if (free_end <= cursor)
        {
            break;
        }
        const size_t free_bytes = static_cast<size_t>(free_end - cursor);
        result.push_back({free_bytes, false});
        cursor = free_end;
    }
    return result;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

bool allocator_boundary_tags::boundary_iterator::operator==(
        const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
        const allocator_boundary_tags::boundary_iterator & other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_trusted_memory == nullptr || _occupied_ptr == nullptr)
    {
        return *this;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    char* cur_start = reinterpret_cast<char*>(_occupied_ptr);
    if (_occupied)
    {
        char* next_start = const_cast<char*>((reinterpret_cast<char*>(_occupied_ptr) + kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(_occupied_ptr))));
        if (next_start >= region_end)
        {
            _occupied_ptr = nullptr;
            _occupied = false;
            return *this;
        }
        void* cur = *reinterpret_cast<void**>(base + first_occ_off);
        while (cur != nullptr && cur != _occupied_ptr)
        {
            cur = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
        }
        void* occ_next = cur == nullptr ? nullptr : *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
        if (occ_next != nullptr && reinterpret_cast<char*>(occ_next) == next_start)
        {
            _occupied_ptr = occ_next;
            _occupied = true;
            return *this;
        }
        _occupied_ptr = next_start;
        _occupied = false;
        return *this;
    }
    void* cur = *reinterpret_cast<void**>(base + first_occ_off);
    while (cur != nullptr && reinterpret_cast<char*>(cur) <= cur_start)
    {
        cur = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
    }
    if (cur == nullptr)
    {
        _occupied_ptr = nullptr;
        _occupied = false;
        return *this;
    }
    _occupied_ptr = cur;
    _occupied = true;
    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_trusted_memory == nullptr)
    {
        return *this;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    void* head = *reinterpret_cast<void**>(base + first_occ_off);
    if (_occupied_ptr == nullptr)
    {
        if (region_begin >= region_end)
        {
            return *this;
        }
        if (head == nullptr)
        {
            _occupied_ptr = region_begin;
            _occupied = false;
            return *this;
        }
        void* tail = head;
        while (*reinterpret_cast<void**>(reinterpret_cast<char*>(tail) + sizeof(size_t) + sizeof(void*)) != nullptr)
        {
            tail = *reinterpret_cast<void**>(reinterpret_cast<char*>(tail) + sizeof(size_t) + sizeof(void*));
        }
        char* tail_end = const_cast<char*>((reinterpret_cast<char*>(tail) + kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(tail))));
        if (tail_end < region_end)
        {
            _occupied_ptr = tail_end;
            _occupied = false;
            return *this;
        }
        _occupied_ptr = tail;
        _occupied = true;
        return *this;
    }
    char* cur_start = reinterpret_cast<char*>(_occupied_ptr);
    if (cur_start <= region_begin)
    {
        return *this;
    }
    if (_occupied)
    {
        void* prev_occ = *reinterpret_cast<void**>(reinterpret_cast<char*>(_occupied_ptr) + sizeof(size_t));
        if (prev_occ == nullptr)
        {
            if (cur_start > region_begin)
            {
                _occupied_ptr = region_begin;
                _occupied = false;
            }
            return *this;
        }
        char* prev_end = const_cast<char*>((reinterpret_cast<char*>(prev_occ) + kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(prev_occ))));
        if (prev_end == cur_start)
        {
            _occupied_ptr = prev_occ;
            _occupied = true;
            return *this;
        }
        _occupied_ptr = prev_end;
        _occupied = false;
        return *this;
    }
    void* cur = head;
    void* prev_occ = nullptr;
    while (cur != nullptr && reinterpret_cast<char*>(cur) < cur_start)
    {
        prev_occ = cur;
        cur = *reinterpret_cast<void**>(reinterpret_cast<char*>(cur) + sizeof(size_t) + sizeof(void*));
    }
    if (prev_occ == nullptr)
    {
        _occupied_ptr = region_begin;
        _occupied = false;
        return *this;
    }
    _occupied_ptr = prev_occ;
    _occupied = true;
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int n)
{
    (void)n;
    boundary_iterator tmp(*this);
    ++(*this);
    return tmp;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int n)
{
    (void)n;
    boundary_iterator tmp(*this);
    --(*this);
    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_trusted_memory == nullptr || _occupied_ptr == nullptr)
    {
        return 0;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    if (_occupied)
    {
        return kOccupiedBlockMetadataSize + *reinterpret_cast<size_t*>(reinterpret_cast<char*>(_occupied_ptr));
    }
    char* cur = reinterpret_cast<char*>(_occupied_ptr);
    void* occ = *reinterpret_cast<void**>(base + first_occ_off);
    while (occ != nullptr && reinterpret_cast<char*>(occ) <= cur)
    {
        occ = *reinterpret_cast<void**>(reinterpret_cast<char*>(occ) + sizeof(size_t) + sizeof(void*));
    }
    char* end = occ == nullptr ? region_end : reinterpret_cast<char*>(occ);
    if (end <= cur)
    {
        return 0;
    }
    return static_cast<size_t>(end - cur);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied_ptr != nullptr && _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr == nullptr)
    {
        return nullptr;
    }
    if (_occupied)
    {
        return reinterpret_cast<char*>(_occupied_ptr) + kOccupiedBlockMetadataSize;
    }
    return _occupied_ptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
{
    _occupied_ptr = nullptr;
    _occupied = false;
    _trusted_memory = nullptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted)
{
    _trusted_memory = trusted;
    if (trusted == nullptr)
    {
        _occupied_ptr = nullptr;
        _occupied = false;
        return;
    }
    char* base = reinterpret_cast<char*>(trusted);
    char* region_begin = base + kAllocatorMetadataSize;
    char* region_end = region_begin + *reinterpret_cast<size_t*>(base + managed_off);
    if (region_begin >= region_end)
    {
        _occupied_ptr = nullptr;
        _occupied = false;
        return;
    }
    void* head = *reinterpret_cast<void**>(base + first_occ_off);
    if (head == nullptr)
    {
        _occupied_ptr = region_begin;
        _occupied = false;
        return;
    }
    if (reinterpret_cast<char*>(head) == region_begin)
    {
        _occupied_ptr = head;
        _occupied = true;
        return;
    }
    _occupied_ptr = region_begin;
    _occupied = false;
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}