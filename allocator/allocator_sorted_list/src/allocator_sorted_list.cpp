#include <not_implemented.h>
#include "../include/allocator_sorted_list.h"
#include <cstring>
#include <new>
#include <algorithm>
#include <iostream>

// ============================================================================
// Внутренние структуры (размещаются в trusted_memory)
// ============================================================================

struct BlockHeader {
    size_t size;           // Полный размер блока (включая заголовок)
    bool is_free;          // Статус: свободен или занят
    BlockHeader* next;     // Следующий блок в списке
    BlockHeader* prev;     // Предыдущий блок в списке (для слияния)
    
    void* get_data_ptr() {
        return reinterpret_cast<char*>(this) + sizeof(BlockHeader);
    }
    
    static BlockHeader* from_data_ptr(void* ptr) {
        return reinterpret_cast<BlockHeader*>(
            reinterpret_cast<char*>(ptr) - sizeof(BlockHeader)
        );
    }
};

struct AllocatorMetadata {
    std::pmr::memory_resource* parent_allocator;
    allocator_with_fit_mode::fit_mode fit_mode;
    size_t total_size;
    std::mutex mtx;
    BlockHeader* first_block;
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

static AllocatorMetadata* get_metadata(void* trusted_memory) {
    return reinterpret_cast<AllocatorMetadata*>(trusted_memory);
}

static size_t align_size(size_t size) {
    constexpr size_t alignment = alignof(std::max_align_t);
    return (size + alignment - 1) & ~(alignment - 1);
}

// ============================================================================
// Конструкторы / Деструктор
// ============================================================================

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource();
    }
    
    size_t metadata_aligned = align_size(sizeof(AllocatorMetadata));
    size_t block_header_aligned = align_size(sizeof(BlockHeader));
    size_t total_needed = metadata_aligned + block_header_aligned + space_size;
    
    _trusted_memory = parent_allocator->allocate(total_needed);
    if (!_trusted_memory) {
        throw std::bad_alloc();
    }
    
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    meta->parent_allocator = parent_allocator;
    meta->fit_mode = allocate_fit_mode;
    meta->total_size = total_needed;
    meta->first_block = nullptr;
    
    new (&meta->mtx) std::mutex();
    
    char* block_start = reinterpret_cast<char*>(_trusted_memory) + metadata_aligned;
    BlockHeader* first_block = reinterpret_cast<BlockHeader*>(block_start);
    first_block->size = block_header_aligned + space_size;
    first_block->is_free = true;
    first_block->next = nullptr;
    first_block->prev = nullptr;
    meta->first_block = first_block;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    throw not_implemented("allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)", 
                         "Copy constructor not supported for allocator");
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    throw not_implemented("allocator_sorted_list::operator=(const allocator_sorted_list &other)", 
                         "Copy assignment not supported for allocator");
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this != &other) {
        if (_trusted_memory) {
            AllocatorMetadata* meta = get_metadata(_trusted_memory);
            meta->parent_allocator->deallocate(_trusted_memory, meta->total_size);
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory) {
        AllocatorMetadata* meta = get_metadata(_trusted_memory);
        meta->mtx.~mutex();
        meta->parent_allocator->deallocate(_trusted_memory, meta->total_size);
    }
}

// ============================================================================
// Основные методы выделения/освобождения
// ============================================================================

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size)
{
    if (size == 0) {
        throw std::bad_alloc();
    }
    
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    
    size_t aligned_size = align_size(size);
    size_t block_header_aligned = align_size(sizeof(BlockHeader));
    size_t required_block_size = block_header_aligned + aligned_size;
    
    BlockHeader* selected_block = nullptr;
    BlockHeader* current = meta->first_block;
    
    while (current) {
        if (current->is_free && current->size >= required_block_size) {
            if (selected_block == nullptr) {
                selected_block = current;
            } else {
                switch (meta->fit_mode) {
                    case allocator_with_fit_mode::fit_mode::first_fit:
                        break;
                    case allocator_with_fit_mode::fit_mode::the_best_fit:
                        if (current->size < selected_block->size) {
                            selected_block = current;
                        }
                        break;
                    case allocator_with_fit_mode::fit_mode::the_worst_fit:
                        if (current->size > selected_block->size) {
                            selected_block = current;
                        }
                        break;
                }
            }
            
            if (meta->fit_mode == allocator_with_fit_mode::fit_mode::first_fit) {
                break;
            }
        }
        current = current->next;
    }
    
    if (!selected_block) {
        throw std::bad_alloc();
    }
    
    size_t remaining = selected_block->size - required_block_size;
    size_t min_block_size = block_header_aligned + align_size(1);
    
    if (remaining >= min_block_size) {
        char* new_block_addr = reinterpret_cast<char*>(selected_block) + required_block_size;
        BlockHeader* new_block = reinterpret_cast<BlockHeader*>(new_block_addr);
        new_block->size = remaining;
        new_block->is_free = true;
        new_block->next = selected_block->next;
        new_block->prev = selected_block;
        
        if (selected_block->next) {
            selected_block->next->prev = new_block;
        }
        selected_block->next = new_block;
        selected_block->size = required_block_size;
    }
    
    selected_block->is_free = false;
    return selected_block->get_data_ptr();
}

void allocator_sorted_list::do_deallocate_sm(void *at)
{
    if (!at) {
        return;
    }
    
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    
    BlockHeader* block = BlockHeader::from_data_ptr(at);
    
    char* memory_start = reinterpret_cast<char*>(_trusted_memory) + align_size(sizeof(AllocatorMetadata));
    char* memory_end = reinterpret_cast<char*>(_trusted_memory) + meta->total_size;
    char* block_addr = reinterpret_cast<char*>(block);
    
    if (block_addr < memory_start || block_addr >= memory_end) {
        throw std::logic_error("Block does not belong to this allocator");
    }
    
    block->is_free = true;
    
    // Слияние со следующим блоком
    if (block->next && block->next->is_free) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    
    // Слияние с предыдущим блоком
    if (block->prev && block->prev->is_free) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return this == &other;
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->fit_mode = mode;
}

// ============================================================================
// Методы для тестов (get_blocks_info)
// ============================================================================

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    const AllocatorMetadata* meta = get_metadata(_trusted_memory);
    BlockHeader* current = const_cast<AllocatorMetadata*>(meta)->first_block;
    
    while (current) {
        allocator_test_utils::block_info info;
        info.is_block_occupied = !current->is_free;
        info.block_size = current->size - align_size(sizeof(BlockHeader));
        result.push_back(info);
        current = current->next;
    }
    
    return result;
}

// ============================================================================
// Итераторы
// ============================================================================

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    BlockHeader* current = meta->first_block;
    
    while (current && !current->is_free) {
        current = current->next;
    }
    
    return sorted_free_iterator(current ? current->get_data_ptr() : nullptr);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator(nullptr);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    AllocatorMetadata* meta = get_metadata(_trusted_memory);
    void* first_ptr = meta->first_block ? meta->first_block->get_data_ptr() : nullptr;
    return sorted_iterator(first_ptr);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator(nullptr);
}

// ============================================================================
// sorted_free_iterator implementation
// ============================================================================

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted)
    : _free_ptr(trusted) {}

bool allocator_sorted_list::sorted_free_iterator::operator==(
    const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(
    const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return _free_ptr != other._free_ptr;
}

allocator_sorted_list::sorted_free_iterator& 
allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr) {
        BlockHeader* current = BlockHeader::from_data_ptr(_free_ptr);
        BlockHeader* next = current->next;
        
        while (next && !next->is_free) {
            next = next->next;
        }
        
        _free_ptr = next ? next->get_data_ptr() : nullptr;
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator 
allocator_sorted_list::sorted_free_iterator::operator++(int n)
{
    sorted_free_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    if (_free_ptr) {
        BlockHeader* block = BlockHeader::from_data_ptr(_free_ptr);
        return block->size - align_size(sizeof(BlockHeader));
    }
    return 0;
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr;
}

// ============================================================================
// sorted_iterator implementation
// ============================================================================

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted)
    : _free_ptr(trusted), _current_ptr(trusted), _trusted_memory(trusted) {}

bool allocator_sorted_list::sorted_iterator::operator==(
    const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(
    const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return _current_ptr != other._current_ptr;
}

allocator_sorted_list::sorted_iterator& 
allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr) {
        BlockHeader* current = BlockHeader::from_data_ptr(_current_ptr);
        if (current->next) {
            _current_ptr = current->next->get_data_ptr();
        } else {
            _current_ptr = nullptr;
        }
    }
    return *this;
}

allocator_sorted_list::sorted_iterator 
allocator_sorted_list::sorted_iterator::operator++(int n)
{
    sorted_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    if (_current_ptr) {
        BlockHeader* block = BlockHeader::from_data_ptr(_current_ptr);
        return block->size - align_size(sizeof(BlockHeader));
    }
    return 0;
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    if (_current_ptr) {
        BlockHeader* block = BlockHeader::from_data_ptr(_current_ptr);
        return !block->is_free;
    }
    return false;
}