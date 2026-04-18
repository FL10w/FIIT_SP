#include <not_implemented.h>
#include "../include/allocator_red_black_tree.h"
#include <new>
#include <mutex>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <functional>
#include <cstdint>

// ============================================================================
// Константы для выравнивания
// ============================================================================
constexpr size_t ALIGNMENT = alignof(std::max_align_t);

// ============================================================================
// Используем константы из заголовочного файла
// occupied_block_metadata_size = 25 (1 + 24)
// free_block_metadata_size = 41 (1 + 40)
// ============================================================================

// ============================================================================
// Смещения метаданных в _trusted_memory
// ============================================================================
constexpr size_t OFFSET_PARENT_RESOURCE = 0;
constexpr size_t OFFSET_FIT_MODE = sizeof(void*);
constexpr size_t OFFSET_TOTAL_SIZE = OFFSET_FIT_MODE + sizeof(size_t);
constexpr size_t OFFSET_MUTEX = OFFSET_TOTAL_SIZE + sizeof(size_t);
constexpr size_t OFFSET_RB_ROOT = OFFSET_MUTEX + sizeof(std::mutex);

// ============================================================================
// Смещения в заголовках блоков (согласно заголовочному файлу)
// block_data = 1 byte (bit fields: 4 bits occupied, 4 bits color)
// ============================================================================
constexpr size_t OFFSET_BLOCK_DATA = 0;
constexpr size_t OFFSET_BLOCK_SIZE = sizeof(allocator_red_black_tree::block_data); // 1 byte
constexpr size_t OFFSET_BLOCK_PREV = OFFSET_BLOCK_SIZE + sizeof(size_t); // 1 + 8 = 9
constexpr size_t OFFSET_BLOCK_NEXT_OCC = OFFSET_BLOCK_PREV + sizeof(void*); // 9 + 8 = 17
constexpr size_t OFFSET_BLOCK_PARENT = OFFSET_BLOCK_PREV + sizeof(void*); // 9 + 8 = 17
constexpr size_t OFFSET_BLOCK_LEFT = OFFSET_BLOCK_PARENT + sizeof(void*); // 17 + 8 = 25
constexpr size_t OFFSET_BLOCK_RIGHT = OFFSET_BLOCK_LEFT + sizeof(void*); // 25 + 8 = 33

// ============================================================================
// Helper-функции для доступа к метаданным аллокатора
// ============================================================================
static std::pmr::memory_resource** get_parent_resource_ptr(void* trusted) {
    return reinterpret_cast<std::pmr::memory_resource**>(
        reinterpret_cast<char*>(trusted) + OFFSET_PARENT_RESOURCE);
}

static size_t* get_fit_mode_ptr(void* trusted) {
    return reinterpret_cast<size_t*>(
        reinterpret_cast<char*>(trusted) + OFFSET_FIT_MODE);
}

static size_t* get_total_size_ptr(void* trusted) {
    return reinterpret_cast<size_t*>(
        reinterpret_cast<char*>(trusted) + OFFSET_TOTAL_SIZE);
}

static std::mutex* get_mutex_ptr(void* trusted) {
    return reinterpret_cast<std::mutex*>(
        reinterpret_cast<char*>(trusted) + OFFSET_MUTEX);
}

static void** get_rb_root_ptr(void* trusted) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(trusted) + OFFSET_RB_ROOT);
}

// ============================================================================
// Helper-функции для доступа к заголовкам блоков
// ============================================================================
static unsigned char* get_block_data_byte(void* block) {
    return reinterpret_cast<unsigned char*>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_DATA);
}

static size_t* get_block_size_ptr(void* block) {
    return reinterpret_cast<size_t*>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_SIZE);
}

static void** get_block_prev_ptr(void* block) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_PREV);
}

static void** get_block_next_ptr(void* block) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_NEXT_OCC);
}

static void** get_block_parent_ptr(void* block) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_PARENT);
}

static void** get_block_left_ptr(void* block) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_LEFT);
}

static void** get_block_right_ptr(void* block) {
    return reinterpret_cast<void**>(
        reinterpret_cast<char*>(block) + OFFSET_BLOCK_RIGHT);
}

// ============================================================================
// Функции работы с блоками
// ============================================================================
static bool is_occupied(void* block) {
    unsigned char byte = *get_block_data_byte(block);
    return (byte & 0x0F) != 0;
}

static void set_occupied(void* block, bool occ) {
    unsigned char* ptr = get_block_data_byte(block);
    unsigned char color = (*ptr & 0xF0);
    *ptr = color | (occ ? 0x01 : 0x00);
}

static unsigned char get_color_byte(void* block) {
    unsigned char byte = *get_block_data_byte(block);
    return (byte & 0xF0) >> 4;
}

static void set_color_byte(void* block, unsigned char color) {
    unsigned char* ptr = get_block_data_byte(block);
    unsigned char occ = (*ptr & 0x0F);
    *ptr = occ | ((color & 0x01) << 4);
}

static size_t get_block_size(void* block) {
    return *get_block_size_ptr(block);
}

static void set_block_size(void* block, size_t size) {
    *get_block_size_ptr(block) = size;
}

// Вычисляем смещение до пользовательских данных с учётом выравнивания
static size_t get_header_offset(bool occupied) {
    size_t header_size = occupied ? 
        allocator_red_black_tree::occupied_block_metadata_size : 
        allocator_red_black_tree::free_block_metadata_size;
    // Выравниваем смещение до ALIGNMENT
    return (header_size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static void* get_user_ptr(void* block) {
    size_t offset = get_header_offset(is_occupied(block));
    return reinterpret_cast<char*>(block) + offset;
}

// Для поиска блока по пользовательскому указателю используем максимальное смещение
static constexpr size_t MAX_HEADER_OFFSET = 48; // 41 + 7 padding max

static void* get_block_ptr_from_user(void* user) {
    // Проверяем возможные смещения (25->32, 41->48 с выравниванием)
    char* cand32 = reinterpret_cast<char*>(user) - 32;
    char* cand48 = reinterpret_cast<char*>(user) - 48;
    
    // Проверяем哪个 является валидным заголовком
    if (is_occupied(cand32)) {
        return cand32;
    }
    return cand48;
}

static void* get_next_block(void* block, void* trusted_end) {
    size_t offset = get_header_offset(is_occupied(block));
    size_t size = get_block_size(block);
    size_t total = offset + size;
    // Выравниваем общий размер до ALIGNMENT
    total = (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    
    char* next = reinterpret_cast<char*>(block) + total;
    if (next >= trusted_end) return nullptr;
    return next;
}

// ============================================================================
// Функции Красно-Чёрного Дерева
// ============================================================================
static void* rb_parent(void* node) {
    if (!node || is_occupied(node)) return nullptr;
    return *get_block_parent_ptr(node);
}

static void set_rb_parent(void* node, void* parent) {
    if (!node || is_occupied(node)) return;
    *get_block_parent_ptr(node) = parent;
}

static void* rb_left(void* node) {
    if (!node || is_occupied(node)) return nullptr;
    return *get_block_left_ptr(node);
}

static void set_rb_left(void* node, void* left) {
    if (!node || is_occupied(node)) return;
    *get_block_left_ptr(node) = left;
}

static void* rb_right(void* node) {
    if (!node || is_occupied(node)) return nullptr;
    return *get_block_right_ptr(node);
}

static void set_rb_right(void* node, void* right) {
    if (!node || is_occupied(node)) return;
    *get_block_right_ptr(node) = right;
}

static void rb_rotate_left(void* trusted, void* x) {
    void* y = rb_right(x);
    set_rb_right(x, rb_left(y));
    if (rb_left(y)) set_rb_parent(rb_left(y), x);
    set_rb_parent(y, rb_parent(x));
    if (!rb_parent(x)) {
        *get_rb_root_ptr(trusted) = y;
    } else if (x == rb_left(rb_parent(x))) {
        set_rb_left(rb_parent(x), y);
    } else {
        set_rb_right(rb_parent(x), y);
    }
    set_rb_left(y, x);
    set_rb_parent(x, y);
}

static void rb_rotate_right(void* trusted, void* x) {
    void* y = rb_left(x);
    set_rb_left(x, rb_right(y));
    if (rb_right(y)) set_rb_parent(rb_right(y), x);
    set_rb_parent(y, rb_parent(x));
    if (!rb_parent(x)) {
        *get_rb_root_ptr(trusted) = y;
    } else if (x == rb_right(rb_parent(x))) {
        set_rb_right(rb_parent(x), y);
    } else {
        set_rb_left(rb_parent(x), y);
    }
    set_rb_right(y, x);
    set_rb_parent(x, y);
}

static void rb_insert_fixup(void* trusted, void* z) {
    while (true) {
        void* p = rb_parent(z);
        if (!p) {
            set_color_byte(z, 1); // BLACK
            break;
        }
        if (get_color_byte(p) == 1) break; // BLACK
        
        void* g = rb_parent(p);
        if (!g) break;
        
        void* u = (p == rb_left(g)) ? rb_right(g) : rb_left(g);
        
        if (u && get_color_byte(u) == 0) { // RED
            set_color_byte(p, 1); // BLACK
            set_color_byte(u, 1); // BLACK
            set_color_byte(g, 0); // RED
            z = g;
        } else {
            if (p == rb_left(g)) {
                if (z == rb_right(p)) {
                    z = p;
                    rb_rotate_left(trusted, z);
                    p = rb_parent(z);
                    g = rb_parent(p);
                }
                set_color_byte(p, 1); // BLACK
                set_color_byte(g, 0); // RED
                rb_rotate_right(trusted, g);
            } else {
                if (z == rb_left(p)) {
                    z = p;
                    rb_rotate_right(trusted, z);
                    p = rb_parent(z);
                    g = rb_parent(p);
                }
                set_color_byte(p, 1); // BLACK
                set_color_byte(g, 0); // RED
                rb_rotate_left(trusted, g);
            }
            break;
        }
    }
}

static void rb_insert(void* trusted, void* z) {
    set_rb_left(z, nullptr);
    set_rb_right(z, nullptr);
    set_color_byte(z, 0); // RED
    
    void* y = nullptr;
    void* x = *get_rb_root_ptr(trusted);
    
    while (x) {
        y = x;
        if (z < x) {
            x = rb_left(x);
        } else {
            x = rb_right(x);
        }
    }
    
    set_rb_parent(z, y);
    if (!y) {
        *get_rb_root_ptr(trusted) = z;
    } else if (z < y) {
        set_rb_left(y, z);
    } else {
        set_rb_right(y, z);
    }
    
    rb_insert_fixup(trusted, z);
}

static void rb_transplant(void* trusted, void* u, void* v) {
    if (!rb_parent(u)) {
        *get_rb_root_ptr(trusted) = v;
    } else if (u == rb_left(rb_parent(u))) {
        set_rb_left(rb_parent(u), v);
    } else {
        set_rb_right(rb_parent(u), v);
    }
    if (v) set_rb_parent(v, rb_parent(u));
}

static void* rb_minimum(void* node) {
    while (rb_left(node)) node = rb_left(node);
    return node;
}

static void rb_delete_fixup(void* trusted, void* x) {
    while (x != *get_rb_root_ptr(trusted) && 
           (!x || get_color_byte(x) == 1)) {
        if (x == rb_left(rb_parent(x))) {
            void* w = rb_right(rb_parent(x));
            if (get_color_byte(w) == 0) {
                set_color_byte(w, 1);
                set_color_byte(rb_parent(x), 0);
                rb_rotate_left(trusted, rb_parent(x));
                w = rb_right(rb_parent(x));
            }
            if ((!rb_left(w) || get_color_byte(rb_left(w)) == 1) &&
                (!rb_right(w) || get_color_byte(rb_right(w)) == 1)) {
                set_color_byte(w, 0);
                x = rb_parent(x);
            } else {
                if (!rb_right(w) || get_color_byte(rb_right(w)) == 1) {
                    if (rb_left(w)) set_color_byte(rb_left(w), 1);
                    set_color_byte(w, 0);
                    rb_rotate_right(trusted, w);
                    w = rb_right(rb_parent(x));
                }
                set_color_byte(w, get_color_byte(rb_parent(x)));
                set_color_byte(rb_parent(x), 1);
                if (rb_right(w)) set_color_byte(rb_right(w), 1);
                rb_rotate_left(trusted, rb_parent(x));
                x = *get_rb_root_ptr(trusted);
            }
        } else {
            void* w = rb_left(rb_parent(x));
            if (get_color_byte(w) == 0) {
                set_color_byte(w, 1);
                set_color_byte(rb_parent(x), 0);
                rb_rotate_right(trusted, rb_parent(x));
                w = rb_left(rb_parent(x));
            }
            if ((!rb_right(w) || get_color_byte(rb_right(w)) == 1) &&
                (!rb_left(w) || get_color_byte(rb_left(w)) == 1)) {
                set_color_byte(w, 0);
                x = rb_parent(x);
            } else {
                if (!rb_left(w) || get_color_byte(rb_left(w)) == 1) {
                    if (rb_right(w)) set_color_byte(rb_right(w), 1);
                    set_color_byte(w, 0);
                    rb_rotate_left(trusted, w);
                    w = rb_left(rb_parent(x));
                }
                set_color_byte(w, get_color_byte(rb_parent(x)));
                set_color_byte(rb_parent(x), 1);
                if (rb_left(w)) set_color_byte(rb_left(w), 1);
                rb_rotate_right(trusted, rb_parent(x));
                x = *get_rb_root_ptr(trusted);
            }
        }
    }
    if (x) set_color_byte(x, 1);
}

static void rb_delete(void* trusted, void* z) {
    void* y = z;
    unsigned char y_original_color = get_color_byte(y);
    void* x = nullptr;
    
    if (!rb_left(z)) {
        x = rb_right(z);
        rb_transplant(trusted, z, rb_right(z));
    } else if (!rb_right(z)) {
        x = rb_left(z);
        rb_transplant(trusted, z, rb_left(z));
    } else {
        y = rb_minimum(rb_right(z));
        y_original_color = get_color_byte(y);
        x = rb_right(y);
        if (rb_parent(y) == z) {
            if (x) set_rb_parent(x, y);
        } else {
            rb_transplant(trusted, y, rb_right(y));
            set_rb_right(y, rb_right(z));
            set_rb_parent(rb_right(y), y);
        }
        rb_transplant(trusted, z, y);
        set_rb_left(y, rb_left(z));
        set_rb_parent(rb_left(y), y);
        set_color_byte(y, get_color_byte(z));
    }
    
    if (y_original_color == 1) {
        rb_delete_fixup(trusted, x);
    }
}

static void* rb_find_fit(void* trusted, size_t size, size_t mode) {
    void* root = *get_rb_root_ptr(trusted);
    if (!root) return nullptr;
    
    void* best = nullptr;
    
    std::function<void(void*)> traverse = [&](void* node) {
        if (!node) return;
        
        traverse(rb_left(node));
        
        if (get_block_size(node) >= size) {
            if (mode == 0) { // first_fit
                if (!best) best = node;
            } else if (mode == 1) { // the_best_fit
                if (!best || get_block_size(node) < get_block_size(best)) {
                    best = node;
                } else if (get_block_size(node) == get_block_size(best) && node < best) {
                    best = node;
                }
            } else if (mode == 2) { // the_worst_fit
                if (!best || get_block_size(node) > get_block_size(best)) {
                    best = node;
                } else if (get_block_size(node) == get_block_size(best) && node < best) {
                    best = node;
                }
            }
        }
        
        traverse(rb_right(node));
    };
    
    traverse(root);
    return best;
}

// ============================================================================
// Реализация методов класса allocator_red_black_tree
// ============================================================================

allocator_red_black_tree::~allocator_red_black_tree()
{
    if (_trusted_memory) {
        std::mutex* mtx = get_mutex_ptr(_trusted_memory);
        mtx->~mutex();
        
        size_t total_size = *get_total_size_ptr(_trusted_memory);
        std::pmr::memory_resource* parent = *get_parent_resource_ptr(_trusted_memory);
        
        if (parent) {
            parent->deallocate(_trusted_memory, total_size, ALIGNMENT);
        } else {
            ::operator delete(_trusted_memory);
        }
        _trusted_memory = nullptr;
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    allocator_red_black_tree &&other) noexcept
{
    if (other._trusted_memory) {
        std::lock_guard<std::mutex> lock(*get_mutex_ptr(other._trusted_memory));
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    } else {
        _trusted_memory = nullptr;
    }
}

allocator_red_black_tree &allocator_red_black_tree::operator=(
    allocator_red_black_tree &&other) noexcept
{
    if (this != &other) {
        this->~allocator_red_black_tree();
        if (other._trusted_memory) {
            std::lock_guard<std::mutex> lock(*get_mutex_ptr(other._trusted_memory));
            _trusted_memory = other._trusted_memory;
            other._trusted_memory = nullptr;
        } else {
            _trusted_memory = nullptr;
        }
    }
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator) {
        _trusted_memory = parent_allocator->allocate(space_size, ALIGNMENT);
    } else {
        _trusted_memory = ::operator new(space_size);
    }
    
    // Инициализируем метаданные нулями
    std::memset(_trusted_memory, 0, allocator_metadata_size);
    
    *get_parent_resource_ptr(_trusted_memory) = parent_allocator;
    *get_fit_mode_ptr(_trusted_memory) = static_cast<size_t>(allocate_fit_mode);
    *get_total_size_ptr(_trusted_memory) = space_size;
    
    new (get_mutex_ptr(_trusted_memory)) std::mutex();
    *get_rb_root_ptr(_trusted_memory) = nullptr;
    
    // Вычисляем начало блоков с выравниванием
    char* metadata_end = reinterpret_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* blocks_start = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(metadata_end) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    
    size_t metadata_size_used = blocks_start - reinterpret_cast<char*>(_trusted_memory);
    size_t available_for_blocks = space_size - metadata_size_used;
    
    // Создаём первый свободный блок
    if (available_for_blocks >= free_block_metadata_size + ALIGNMENT) {
        void* first_block = blocks_start;
        set_occupied(first_block, false);
        set_color_byte(first_block, 1); // BLACK
        set_block_size(first_block, available_for_blocks - get_header_offset(false));
        *get_block_prev_ptr(first_block) = nullptr;
        *get_block_parent_ptr(first_block) = nullptr;
        *get_block_left_ptr(first_block) = nullptr;
        *get_block_right_ptr(first_block) = nullptr;
        
        rb_insert(_trusted_memory, first_block);
    }
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other)
{
    if (!other._trusted_memory) {
        _trusted_memory = nullptr;
        return;
    }
    
    size_t space_size = *get_total_size_ptr(other._trusted_memory);
    std::pmr::memory_resource* parent = *get_parent_resource_ptr(other._trusted_memory);
    
    if (parent) {
        _trusted_memory = parent->allocate(space_size, ALIGNMENT);
    } else {
        _trusted_memory = ::operator new(space_size);
    }
    
    std::memcpy(_trusted_memory, other._trusted_memory, allocator_metadata_size);
    get_mutex_ptr(_trusted_memory)->~mutex();
    new (get_mutex_ptr(_trusted_memory)) std::mutex();
    *get_rb_root_ptr(_trusted_memory) = nullptr;
    
    char* other_end = reinterpret_cast<char*>(other._trusted_memory) + space_size;
    char* other_metadata_end = reinterpret_cast<char*>(other._trusted_memory) + allocator_metadata_size;
    char* other_blocks_start = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(other_metadata_end) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    
    void* curr = other_blocks_start;
    void* prev_copy = nullptr;
    
    while (curr < other_end) {
        size_t size = get_block_size(curr);
        size_t offset = get_header_offset(is_occupied(curr));
        size_t total_block_size = offset + size;
        total_block_size = (total_block_size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        
        ptrdiff_t diff = reinterpret_cast<char*>(_trusted_memory) - reinterpret_cast<char*>(other._trusted_memory);
        void* copy_block = reinterpret_cast<char*>(curr) + diff;
        
        size_t header_size = is_occupied(curr) ? 
            occupied_block_metadata_size : free_block_metadata_size;
        std::memcpy(copy_block, curr, header_size);
        
        *get_block_prev_ptr(copy_block) = prev_copy;
        
        if (is_occupied(copy_block)) {
            void* next = reinterpret_cast<char*>(curr) + total_block_size;
            if (next < other_end) {
                *get_block_next_ptr(copy_block) = reinterpret_cast<char*>(next) + diff;
            } else {
                *get_block_next_ptr(copy_block) = nullptr;
            }
        }
        
        if (!is_occupied(copy_block)) {
            *get_block_parent_ptr(copy_block) = nullptr;
            *get_block_left_ptr(copy_block) = nullptr;
            *get_block_right_ptr(copy_block) = nullptr;
            rb_insert(_trusted_memory, copy_block);
        }
        
        prev_copy = copy_block;
        curr = reinterpret_cast<char*>(curr) + total_block_size;
    }
}

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other)
{
    if (this != &other) {
        this->~allocator_red_black_tree();
        new (this) allocator_red_black_tree(other);
    }
    return *this;
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(*get_mutex_ptr(_trusted_memory));
    
    // Выравниваем запрошенный размер
    size_t aligned_size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    size_t mode = *get_fit_mode_ptr(_trusted_memory);
    void* block = rb_find_fit(_trusted_memory, aligned_size, mode);
    
    if (!block) {
        throw std::bad_alloc();
    }
    
    rb_delete(_trusted_memory, block);
    size_t block_size = get_block_size(block);
    size_t header_offset = get_header_offset(false); // Был свободным
    
    // Проверяем, можно ли разделить блок
    size_t min_remainder = get_header_offset(false) + ALIGNMENT;
    if (block_size >= aligned_size + min_remainder) {
        // Разделяем блок
        set_block_size(block, aligned_size);
        
        char* remainder = reinterpret_cast<char*>(block) + header_offset + aligned_size;
        // Выравниваем начало остатка
        remainder = reinterpret_cast<char*>(
            (reinterpret_cast<uintptr_t>(remainder) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
        
        char* original_end = reinterpret_cast<char*>(block) + header_offset + block_size;
        size_t remainder_size = original_end - remainder - get_header_offset(false);
        
        if (remainder_size >= ALIGNMENT) {
            set_occupied(remainder, false);
            set_color_byte(remainder, 1); // BLACK
            set_block_size(remainder, remainder_size);
            *get_block_prev_ptr(remainder) = block;
            
            void* end = reinterpret_cast<char*>(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
            void* next = get_next_block(remainder, end);
            if (next) {
                *get_block_prev_ptr(next) = remainder;
            }
            
            *get_block_parent_ptr(remainder) = nullptr;
            *get_block_left_ptr(remainder) = nullptr;
            *get_block_right_ptr(remainder) = nullptr;
            rb_insert(_trusted_memory, remainder);
        }
    }
    
    // Помечаем как занятый
    set_occupied(block, true);
    void* end = reinterpret_cast<char*>(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
    void* next = get_next_block(block, end);
    *get_block_next_ptr(block) = next;
    
    return get_user_ptr(block);
}

void allocator_red_black_tree::do_deallocate_sm(void *at)
{
    if (!at) return;
    
    std::lock_guard<std::mutex> lock(*get_mutex_ptr(_trusted_memory));
    
    char* start = reinterpret_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* end = reinterpret_cast<char*>(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
    char* ptr = reinterpret_cast<char*>(at);
    
    if (ptr < start || ptr >= end) {
        throw std::runtime_error("Pointer out of range");
    }
    
    void* block = get_block_ptr_from_user(at);
    
    if (reinterpret_cast<char*>(block) < start) {
        throw std::runtime_error("Invalid block pointer");
    }
    
    if (!is_occupied(block)) {
        throw std::runtime_error("Block is not occupied");
    }
    
    // Помечаем как свободный
    set_occupied(block, false);
    set_color_byte(block, 1); // BLACK
    
    // Объединяем со следующим
    void* next = *get_block_next_ptr(block);
    if (next && !is_occupied(next)) {
        rb_delete(_trusted_memory, next);
        size_t offset1 = get_header_offset(false);
        size_t offset2 = get_header_offset(false);
        size_t fp1 = offset1 + get_block_size(block);
        size_t fp2 = offset2 + get_block_size(next);
        fp1 = (fp1 + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        fp2 = (fp2 + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        set_block_size(block, fp1 + fp2 - offset1);
        
        void* next_next = get_next_block(next, end);
        if (next_next) {
            *get_block_prev_ptr(next_next) = block;
        }
    }
    
    // Объединяем с предыдущим
    void* prev = *get_block_prev_ptr(block);
    if (prev && !is_occupied(prev)) {
        rb_delete(_trusted_memory, prev);
        size_t offset1 = get_header_offset(false);
        size_t offset2 = get_header_offset(false);
        size_t fp1 = offset1 + get_block_size(prev);
        size_t fp2 = offset2 + get_block_size(block);
        fp1 = (fp1 + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        fp2 = (fp2 + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        set_block_size(prev, fp1 + fp2 - offset1);
        
        void* next_next = get_next_block(block, end);
        if (next_next) {
            *get_block_prev_ptr(next_next) = prev;
        }
        block = prev;
    }
    
    // Вставляем в дерево
    *get_block_parent_ptr(block) = nullptr;
    *get_block_left_ptr(block) = nullptr;
    *get_block_right_ptr(block) = nullptr;
    rb_insert(_trusted_memory, block);
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(*get_mutex_ptr(_trusted_memory));
    *get_fit_mode_ptr(_trusted_memory) = static_cast<size_t>(mode);
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    std::lock_guard<std::mutex> lock(*get_mutex_ptr(const_cast<void*>(_trusted_memory)));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> info;
    
    char* start = reinterpret_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* end = reinterpret_cast<char*>(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
    
    char* blocks_start = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(start) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    
    void* curr = blocks_start;
    while (curr < end) {
        allocator_test_utils::block_info bi;
        bi.is_block_occupied = is_occupied(curr);
        bi.block_size = get_block_size(curr);
        info.push_back(bi);
        
        size_t offset = get_header_offset(bi.is_block_occupied);
        size_t total = offset + bi.block_size;
        total = (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        curr = reinterpret_cast<char*>(curr) + total;
    }
    
    return info;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    char* start = reinterpret_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* blocks_start = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(start) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    return rb_iterator(blocks_start);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    char* end = reinterpret_cast<char*>(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
    return rb_iterator(end);
}

// ============================================================================
// Реализация rb_iterator
// ============================================================================
allocator_red_black_tree::rb_iterator::rb_iterator() : _block_ptr(nullptr), _trusted(nullptr) {}

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted) : _block_ptr(trusted), _trusted(trusted) {}

bool allocator_red_black_tree::rb_iterator::operator==(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return _block_ptr == other._block_ptr;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    if (!_block_ptr) return *this;
    
    bool occ = is_occupied(_block_ptr);
    size_t offset = get_header_offset(occ);
    size_t size = get_block_size(_block_ptr);
    size_t total = offset + size;
    total = (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    
    _block_ptr = reinterpret_cast<char*>(_block_ptr) + total;
    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int n)
{
    rb_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept
{
    if (!_block_ptr) return 0;
    return get_block_size(_block_ptr);
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    return get_user_ptr(_block_ptr);
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    if (!_block_ptr) return false;
    return is_occupied(_block_ptr);
}