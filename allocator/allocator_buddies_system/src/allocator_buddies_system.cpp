#include <not_implemented.h>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <ctime>
#include "../include/allocator_buddies_system.h"

// --- Iterator Implementation ---

allocator_buddies_system::buddy_iterator::buddy_iterator() : _block(nullptr), _end(nullptr) {}

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start, void* end) 
    : _block(start), _end(end) {}

bool allocator_buddies_system::buddy_iterator::operator==(const buddy_iterator &other) const noexcept {
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const buddy_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator &allocator_buddies_system::buddy_iterator::operator++() & noexcept {
    if (_block < _end) {
        auto meta = reinterpret_cast<allocator_buddies_system::block_metadata*>(_block);
        size_t size = size_t(1) << meta->k;
        _block = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(_block) + size);
    }
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int n) {
    (void)n;
    buddy_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept {
    if (_block >= _end) return 0;
    auto meta = reinterpret_cast<allocator_buddies_system::block_metadata*>(_block);
    return size_t(1) << meta->k;
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept {
    if (_block >= _end) return false;
    auto meta = reinterpret_cast<allocator_buddies_system::block_metadata*>(_block);
    return meta->occupied;
}

void *allocator_buddies_system::buddy_iterator::operator*() const noexcept {
    if (_block >= _end) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(_block) + sizeof(allocator_buddies_system::block_metadata));
}

// --- Allocator Implementation ---

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory) {
        auto header = get_header();
        // Явный вызов деструктора мьютекса
        header->mtx.~mutex();
        
        auto parent = header->parent_resource;
        if (parent) {
            // Вычисляем полный размер выделенной памяти
            size_t lists_size = (header->max_k + 1) * sizeof(void*);
            size_t header_raw_size = sizeof(allocator_header) + lists_size;
            size_t alignment = sizeof(std::max_align_t);
            size_t header_size = (header_raw_size + alignment - 1) & ~(alignment - 1);
            size_t total_memory_size = header_size + header->total_size;
            
            parent->deallocate(_trusted_memory, total_memory_size, alignment);
        }
    }
}

allocator_buddies_system::allocator_buddies_system(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (!parent_allocator) {
        parent_allocator = std::pmr::get_default_resource();
    }

    // Проверка минимального размера
    size_t min_size = size_t(1) << min_k_val;
    if (space_size < min_size) {
        throw std::logic_error("Space size too small");
    }

    // Вычисляем max_k для space_size (округляем до степени двойки)
    size_t k = __detail::nearest_greater_k_of_2(space_size);
    size_t aligned_space_size = size_t(1) << k;
    
    size_t max_k = k;
    size_t min_k = min_k_val;

    // Расчет размера заголовка
    size_t lists_size = (max_k + 1) * sizeof(void*);
    size_t header_raw_size = sizeof(allocator_header) + lists_size;
    
    // Выравнивание начала блоков
    size_t alignment = sizeof(std::max_align_t);
    size_t header_size = (header_raw_size + alignment - 1) & ~(alignment - 1);
    
    size_t total_memory_size = header_size + aligned_space_size;

    // Выделение памяти
    _trusted_memory = parent_allocator->allocate(total_memory_size, alignment);
    if (!_trusted_memory) {
        throw std::bad_alloc();
    }

    // Инициализация заголовка (Placement New)
    allocator_header* header = new (_trusted_memory) allocator_header;
    header->mode = allocate_fit_mode;
    header->parent_resource = parent_allocator;
    header->total_size = aligned_space_size;
    header->max_k = max_k;
    header->min_k = min_k;
    // std::mutex конструируется автоматически при placement new

    // Инициализация free lists
    std::fill(get_free_lists(), get_free_lists() + max_k + 1, nullptr);

    // Добавляем один большой блок в free list
    void* first_block = get_blocks_start();
    block_metadata* meta = reinterpret_cast<block_metadata*>(first_block);
    meta->occupied = false;
    meta->k = static_cast<unsigned char>(max_k);
    
    add_to_free_list(first_block, max_k);
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &other)
{
    (void)other;
    throw std::logic_error("Copy constructor not supported for allocator");
}

allocator_buddies_system &allocator_buddies_system::operator=(const allocator_buddies_system &other)
{
    (void)other;
    throw std::logic_error("Copy assignment not supported for allocator");
}

allocator_buddies_system::allocator_buddies_system(allocator_buddies_system &&other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(allocator_buddies_system &&other) noexcept
{
    if (this != &other) {
        if (_trusted_memory) {
            this->~allocator_buddies_system();
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

void allocator_buddies_system::add_to_free_list(void* block, size_t k) {
    // В свободном блоке после метаданных храним указатель на следующий
    void** next_ptr = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(block) + sizeof(block_metadata));
    *next_ptr = get_free_lists()[k];
    get_free_lists()[k] = block;
}

void allocator_buddies_system::remove_from_free_list(void* block, size_t k) {
    void** head_ptr = &get_free_lists()[k];
    while (*head_ptr) {
        if (*head_ptr == block) {
            void** next_ptr = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(block) + sizeof(block_metadata));
            *head_ptr = *next_ptr;
            return;
        }
        void** next_ptr = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(*head_ptr) + sizeof(block_metadata));
        head_ptr = next_ptr;
    }
}

void* allocator_buddies_system::find_block(size_t needed_k) {
    auto header = get_header();
    fit_mode mode = header->mode;
    
    size_t start_k = needed_k;
    size_t end_k = header->max_k;
    int step = 1;

    if (mode == fit_mode::the_worst_fit) {
        // Ищем самый большой подходящий блок
        start_k = header->max_k;
        end_k = needed_k;
        step = -1;
    }
    // first_fit и best_fit для Buddy System идентичны (ищем наименьшую степень >= needed_k)
    
    for (size_t k = start_k; ; k += step) {
        if (get_free_lists()[k] != nullptr) {
            return get_free_lists()[k];
        }
        if (k == end_k) break;
    }
    return nullptr;
}

void allocator_buddies_system::split_block(void* block, size_t current_k, size_t target_k) {
    while (current_k > target_k) {
        size_t next_k = current_k - 1;
        size_t half_size = size_t(1) << next_k;

        // Удаляем текущий блок из списка current_k
        remove_from_free_list(block, current_k);

        block_metadata* meta = reinterpret_cast<block_metadata*>(block);
        meta->k = static_cast<unsigned char>(next_k);
        
        // Второй половинке (buddy)
        void* buddy = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(block) + half_size);
        block_metadata* buddy_meta = reinterpret_cast<block_metadata*>(buddy);
        buddy_meta->occupied = false;
        buddy_meta->k = static_cast<unsigned char>(next_k);

        // Добавляем buddy в free list
        add_to_free_list(buddy, next_k);

        // Продолжаем делить первую половину
        current_k = next_k;
    }
}

void allocator_buddies_system::merge_block(void* block, size_t k) {
    auto header = get_header();
    while (k < header->max_k) {
        void* buddy = get_buddy(block, static_cast<unsigned char>(k));
        block_metadata* buddy_meta = reinterpret_cast<block_metadata*>(buddy);

        // Проверяем границы памяти
        uintptr_t start_blocks = reinterpret_cast<uintptr_t>(get_blocks_start());
        uintptr_t end_blocks = start_blocks + header->total_size;
        uintptr_t buddy_addr = reinterpret_cast<uintptr_t>(buddy);
        
        if (buddy_addr < start_blocks || buddy_addr >= end_blocks) break;

        if (!buddy_meta->occupied && buddy_meta->k == k) {
            // Можно сливать
            remove_from_free_list(buddy, k);
            
            // Новый адрес - минимальный из block и buddy
            void* new_block = (buddy < block) ? buddy : block;
            block_metadata* new_meta = reinterpret_cast<block_metadata*>(new_block);
            new_meta->k = static_cast<unsigned char>(k + 1);
            
            block = new_block;
            k++;
        } else {
            break;
        }
    }
    // Добавляем результирующий блок в free list
    add_to_free_list(block, k);
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(get_header()->mtx);

    if (size == 0) size = 1;

    // Размер блока должен вместить метаданные и запрошенный размер
    size_t total_needed = size + occupied_block_metadata_size;
    
    // Находим k
    size_t k = __detail::nearest_greater_k_of_2(total_needed);
    auto header = get_header();
    if (k < header->min_k) k = header->min_k;

    if (k > header->max_k) {
        throw std::bad_alloc();
    }

    void* block = find_block(k);

    if (!block) {
        throw std::bad_alloc();
    }

    // Если нашли блок большего размера, делим его
    block_metadata* meta = reinterpret_cast<block_metadata*>(block);
    size_t block_k = meta->k;

    if (block_k > k) {
        split_block(block, block_k, k);
    } else {
        // Удаляем из free list
        remove_from_free_list(block, k);
    }

    // Помечаем как занятый
    meta = reinterpret_cast<block_metadata*>(block);
    meta->occupied = true;

    return get_block_addr(block);
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    if (!at) return;
    
    std::lock_guard<std::mutex> lock(get_header()->mtx);

    auto header = get_header();
    uintptr_t addr = reinterpret_cast<uintptr_t>(at);
    uintptr_t start_blocks = reinterpret_cast<uintptr_t>(get_blocks_start());
    uintptr_t end_blocks = start_blocks + header->total_size;

    // Проверка принадлежности
    if (addr < start_blocks || addr >= end_blocks) {
        throw std::logic_error("Pointer does not belong to this allocator");
    }

    block_metadata* meta = get_metadata(at);
    
    if (!meta->occupied) {
        throw std::logic_error("Double free or invalid pointer");
    }

    size_t k = meta->k;
    void* block = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(at) - sizeof(block_metadata));
    
    meta->occupied = false;

    merge_block(block, k);
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

inline void allocator_buddies_system::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(get_header()->mtx);
    get_header()->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    auto header = const_cast<allocator_buddies_system*>(this)->get_header();
    std::lock_guard<std::mutex> lock(header->mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> info;
    auto header = get_header();
    void* current = get_blocks_start();
    void* end = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(current) + header->total_size);

    while (current < end) {
        block_metadata* meta = reinterpret_cast<block_metadata*>(current);
        size_t size = size_t(1) << meta->k;
        info.push_back({ .block_size = size, .is_block_occupied = meta->occupied });
        current = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(current) + size);
    }
    return info;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept {
    auto header = get_header();
    void* end = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(get_blocks_start()) + header->total_size);
    return buddy_iterator(get_blocks_start(), end);
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept {
    auto header = get_header();
    void* end = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(get_blocks_start()) + header->total_size);
    return buddy_iterator(end, end);
}