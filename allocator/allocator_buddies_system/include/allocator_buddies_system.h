#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <allocator_dbg_helper.h>
#include <mutex>
#include <cmath>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <memory_resource>

namespace __detail
{
    constexpr size_t nearest_greater_k_of_2(size_t size) noexcept
    {
        if (size == 0) return 0;
        int ones_counter = 0, index = -1;
        constexpr const size_t o = 1;
        for (int i = sizeof(size_t) * 8 - 1; i >= 0; --i)
        {
            if (size & (o << i))
            {
                if (ones_counter == 0)
                    index = i;
                ++ones_counter;
            }
        }
        return ones_counter <= 1 ? index : index + 1;
    }
}

class allocator_buddies_system final :
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{
private:
    // Метаданные каждого блока (хранятся в начале каждого блока)
    struct block_metadata
    {
        bool occupied : 1;
        unsigned char k : 7; // Степень двойки: размер блока = 2^k
    };

    // Заголовок аллокатора (хранится в начале _trusted_memory)
    struct allocator_header
    {
        std::mutex mtx;
        fit_mode mode;
        std::pmr::memory_resource* parent_resource;
        size_t total_size;      // Полезный размер области (для блоков)
        size_t max_k;           // Максимальная степень двойки
        size_t min_k;           // Минимальная степень двойки
        // Далее следует массив free_lists [max_k + 1]
    };

    void *_trusted_memory;

    // Размеры метаданных
    static constexpr const size_t occupied_block_metadata_size = sizeof(block_metadata) + sizeof(void*);
    static constexpr const size_t free_block_metadata_size = sizeof(block_metadata);
    static constexpr const size_t min_k_val = __detail::nearest_greater_k_of_2(occupied_block_metadata_size);

    // Вспомогательные методы
    allocator_header* get_header() const noexcept {
        return reinterpret_cast<allocator_header*>(_trusted_memory);
    }

    void** get_free_lists() const noexcept {
        return reinterpret_cast<void**>(get_header() + 1);
    }

    void* get_blocks_start() const noexcept {
        auto header = get_header();
        size_t lists_size = (header->max_k + 1) * sizeof(void*);
        size_t header_size = sizeof(allocator_header) + lists_size;
        
        // Выравнивание начала блоков
        uintptr_t addr = reinterpret_cast<uintptr_t>(_trusted_memory) + header_size;
        uintptr_t alignment = sizeof(std::max_align_t);
        uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
        return reinterpret_cast<void*>(aligned_addr);
    }

    size_t get_block_size(unsigned char k) const noexcept {
        return size_t(1) << k;
    }

    void* get_block_addr(void* block_start) const noexcept {
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(block_start) + sizeof(block_metadata));
    }
    
    block_metadata* get_metadata(void* block_user_ptr) const noexcept {
        return reinterpret_cast<block_metadata*>(reinterpret_cast<uintptr_t>(block_user_ptr) - sizeof(block_metadata));
    }

    void* get_buddy(void* ptr, unsigned char k) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base = reinterpret_cast<uintptr_t>(get_blocks_start());
        uintptr_t offset = addr - base;
        uintptr_t buddy_offset = offset ^ (size_t(1) << k);
        return reinterpret_cast<void*>(base + buddy_offset);
    }

    void add_to_free_list(void* block, size_t k);
    void remove_from_free_list(void* block, size_t k);
    void* find_block(size_t needed_k);
    void split_block(void* block, size_t current_k, size_t target_k);
    void merge_block(void* block, size_t k);

public:
    explicit allocator_buddies_system(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator = nullptr,
        allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

    allocator_buddies_system(allocator_buddies_system const &other);
    allocator_buddies_system &operator=(allocator_buddies_system const &other);
    allocator_buddies_system(allocator_buddies_system &&other) noexcept;
    allocator_buddies_system &operator=(allocator_buddies_system &&other) noexcept;
    ~allocator_buddies_system() override;

private:
    [[nodiscard]] void *do_allocate_sm(size_t size) override;
    void do_deallocate_sm(void *at) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
    inline void set_fit_mode(allocator_with_fit_mode::fit_mode mode) override;
    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override;
    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class buddy_iterator
    {
        void* _block; // Указывает на block_metadata
        void* _end;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const buddy_iterator&) const noexcept;
        bool operator!=(const buddy_iterator&) const noexcept;
        buddy_iterator& operator++() & noexcept;
        buddy_iterator operator++(int n);
        size_t size() const noexcept;
        bool occupied() const noexcept;
        void* operator*() const noexcept;
        buddy_iterator();
        buddy_iterator(void* start, void* end);
    };
    friend class buddy_iterator;
    buddy_iterator begin() const noexcept;
    buddy_iterator end() const noexcept;
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H