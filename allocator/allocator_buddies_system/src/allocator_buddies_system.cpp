#include <cstddef>
#include "../include/allocator_buddies_system.h"
#include <memory>
#include <stdexcept>

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* parent = *reinterpret_cast<std::pmr::memory_resource**>(base + parent_off);
    const unsigned char max_power = *reinterpret_cast<unsigned char*>(base + power_off);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::destroy_at(mtx);
    const size_t managed = static_cast<size_t>(1) << max_power;
    parent->deallocate(_trusted_memory, allocator_metadata_size + managed);
    _trusted_memory = nullptr;
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system &&other) noexcept:
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

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system &&other) noexcept
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
        const unsigned char max_power = *reinterpret_cast<unsigned char*>(old_base + power_off);
        auto* mtx = reinterpret_cast<std::mutex*>(old_base + mutex_off);
        std::destroy_at(mtx);
        const size_t managed = static_cast<size_t>(1) << max_power;
        parent->deallocate(old_memory, allocator_metadata_size + managed);
    }
    return *this;
}

allocator_buddies_system::allocator_buddies_system(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }
    size_t normalized = 1;
    while (normalized < space_size)
    {
        normalized *= 2;
    }
    if (normalized < (static_cast<size_t>(1) << min_k))
    {
        throw std::logic_error("too small memory region");
    }
    _trusted_memory = parent_allocator->allocate(allocator_metadata_size + normalized);
    char* base = reinterpret_cast<char*>(_trusted_memory);
    *reinterpret_cast<std::pmr::memory_resource**>(base + parent_off) = parent_allocator;
    *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off) = allocate_fit_mode;
    *reinterpret_cast<unsigned char*>(base + power_off) = static_cast<unsigned char>(__detail::nearest_greater_k_of_2(normalized));
    new (base + mutex_off) std::mutex();
    char* first_block = base + allocator_metadata_size;
    auto* root_md = reinterpret_cast<block_metadata*>(first_block);
    root_md->occupied = false;
    root_md->size = *reinterpret_cast<unsigned char*>(base + power_off);
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    if (size == 0)
    {
        size = 1;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    const auto mode = *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off);
    const unsigned char max_k = *reinterpret_cast<unsigned char*>(base + power_off);
    char* region_begin = base + allocator_metadata_size;
    char* region_end = region_begin + (static_cast<size_t>(1) << max_k);
    const size_t required_total = size + occupied_block_metadata_size;
    size_t req_k = __detail::nearest_greater_k_of_2(required_total == 0 ? 1 : required_total);
    if (req_k < min_k)
    {
        req_k = min_k;
    }
    if (req_k > max_k)
    {
        throw std::bad_alloc();
    }
    char* chosen = nullptr;
    unsigned char chosen_k = 0;
    char* cursor = region_begin;
    while (cursor < region_end)
    {
        auto* md = reinterpret_cast<block_metadata*>(cursor);
        const unsigned char cur_k = md->size;
        const size_t cur_sz = static_cast<size_t>(1) << cur_k;
        if (!md->occupied && cur_k >= req_k)
        {
            if (chosen == nullptr)
            {
                chosen = cursor;
                chosen_k = cur_k;
                if (mode == allocator_with_fit_mode::fit_mode::first_fit)
                {
                    break;
                }
            }
            else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit)
            {
                if (cur_k < chosen_k)
                {
                    chosen = cursor;
                    chosen_k = cur_k;
                }
            }
            else if (mode == allocator_with_fit_mode::fit_mode::the_worst_fit)
            {
                if (cur_k > chosen_k)
                {
                    chosen = cursor;
                    chosen_k = cur_k;
                }
            }
        }
        cursor += cur_sz;
    }
    if (chosen == nullptr)
    {
        throw std::bad_alloc();
    }
    while (chosen_k > req_k)
    {
        const unsigned char split_k = static_cast<unsigned char>(chosen_k - 1);
        const size_t half = static_cast<size_t>(1) << split_k;
        auto* left = reinterpret_cast<block_metadata*>(chosen);
        left->occupied = false;
        left->size = split_k;
        char* right_ptr = chosen + half;
        auto* right = reinterpret_cast<block_metadata*>(right_ptr);
        right->occupied = false;
        right->size = split_k;
        chosen_k = split_k;
    }
    auto* md = reinterpret_cast<block_metadata*>(chosen);
    md->occupied = true;
    return chosen + occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    if (at == nullptr)
    {
        return;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    const unsigned char max_k = *reinterpret_cast<unsigned char*>(base + power_off);
    char* region_begin = base + allocator_metadata_size;
    char* region_end = region_begin + (static_cast<size_t>(1) << max_k);
    char* payload = reinterpret_cast<char*>(at);
    if (payload < region_begin + occupied_block_metadata_size || payload >= region_end)
    {
        throw std::invalid_argument("");
    }
    char* block = payload - occupied_block_metadata_size;
    block_metadata* md = nullptr;
    bool found = false;
    char* cursor = region_begin;
    while (cursor < region_end)
    {
        auto* cur_md = reinterpret_cast<block_metadata*>(cursor);
        const size_t cur_block_size = static_cast<size_t>(1) << cur_md->size;
        if (cursor == block)
        {
            md = cur_md;
            found = true;
            break;
        }
        cursor += cur_block_size;
    }
    if (!found || md == nullptr)
    {
        throw std::invalid_argument("");
    }
    const size_t rel = static_cast<size_t>(block - region_begin);
    const size_t block_size = static_cast<size_t>(1) << md->size;
    if (rel % block_size != 0)
    {
        throw std::invalid_argument("");
    }
    if (!md->occupied)
    {
        throw std::invalid_argument("");
    }
    md->occupied = false;
    unsigned char cur_k = md->size;
    size_t merge_rel = rel;
    while (cur_k < max_k)
    {
        const size_t cur_sz = static_cast<size_t>(1) << cur_k;
        const size_t buddy_rel = merge_rel ^ cur_sz;
        char* buddy_ptr = region_begin + buddy_rel;
        auto* buddy_md = reinterpret_cast<block_metadata*>(buddy_ptr);
        if (buddy_md->occupied || buddy_md->size != cur_k)
        {
            break;
        }
        if (buddy_rel < merge_rel)
        {
            merge_rel = buddy_rel;
            block = buddy_ptr;
            md = buddy_md;
        }
        cur_k = static_cast<unsigned char>(cur_k + 1);
        md = reinterpret_cast<block_metadata*>(region_begin + merge_rel);
        md->occupied = false;
        md->size = cur_k;
    }
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

inline void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    char* base = reinterpret_cast<char*>(_trusted_memory);
    auto* mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
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

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    if (_trusted_memory == nullptr)
    {
        return result;
    }
    char* base = reinterpret_cast<char*>(_trusted_memory);
    const unsigned char max_k = *reinterpret_cast<unsigned char*>(base + power_off);
    char* region_begin = base + allocator_metadata_size;
    char* region_end = region_begin + (static_cast<size_t>(1) << max_k);
    char* cursor = region_begin;
    while (cursor < region_end)
    {
        auto* md = reinterpret_cast<block_metadata*>(cursor);
        const size_t block_size = static_cast<size_t>(1) << md->size;
        result.push_back({block_size, md->occupied});
        cursor += block_size;
    }
    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return buddy_iterator();
    }
    return buddy_iterator(reinterpret_cast<char*>(_trusted_memory) + allocator_metadata_size);
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator();
}

bool allocator_buddies_system::buddy_iterator::operator==(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator &allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr)
    {
        return *this;
    }
    auto* md = reinterpret_cast<block_metadata*>(_block);
    const size_t block_size = static_cast<size_t>(1) << md->size;
    _block = reinterpret_cast<char*>(_block) + block_size;
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int n)
{
    (void)n;
    buddy_iterator tmp(*this);
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    if (_block == nullptr)
    {
        return 0;
    }
    auto* md = reinterpret_cast<block_metadata*>(_block);
    return static_cast<size_t>(1) << md->size;
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    if (_block == nullptr)
    {
        return false;
    }
    auto* md = reinterpret_cast<block_metadata*>(_block);
    return md->occupied;
}

void *allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    if (_block == nullptr)
    {
        return nullptr;
    }
    auto* md = reinterpret_cast<block_metadata*>(_block);
    if (md->occupied)
    {
        return reinterpret_cast<char*>(_block) + allocator_buddies_system::occupied_block_metadata_size;
    }
    return _block;
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void *start)
{
    _block = start;
}

allocator_buddies_system::buddy_iterator::buddy_iterator()
{
    _block = nullptr;
}