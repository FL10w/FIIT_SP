#include <allocator_boundary_tags.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <new>
#include <memory_resource>
#include <cstddef>
#include <mutex>

namespace {

// Internal state - stored BEFORE user blocks (EXTRA memory, NOT part of space_size)
struct internal_state_t
{
    std::pmr::memory_resource* parent_allocator;
    allocator_with_fit_mode::fit_mode mode;
    size_t total_size;  // This is USER space size (space_size from constructor)
    mutable std::mutex mtx;
    void* first_block;
};

// Block meta: size_t + 3 void* = 32 bytes on 64-bit (MUST match test expectations)
constexpr size_t BLOCK_META_SIZE = sizeof(size_t) + sizeof(void*) * 3;

inline size_t align_up(size_t val, size_t alignment) noexcept
{
    return (val + alignment - 1) & ~(alignment - 1);
}

inline size_t get_state_size() noexcept
{
    return align_up(sizeof(internal_state_t), alignof(std::max_align_t));
}

inline internal_state_t* get_state(void* tm) noexcept
{
    return reinterpret_cast<internal_state_t*>(tm);
}

inline const internal_state_t* get_state_const(const void* tm) noexcept
{
    return reinterpret_cast<const internal_state_t*>(tm);
}

// User blocks start AFTER internal state
inline void* get_block_start(void* tm) noexcept
{
    return reinterpret_cast<char*>(tm) + get_state_size();
}

inline const void* get_block_start_const(const void* tm) noexcept
{
    return reinterpret_cast<const char*>(tm) + get_state_size();
}

inline size_t read_size(const void* block) noexcept
{
    return *reinterpret_cast<const size_t*>(block);
}

inline void write_size(void* block, size_t size) noexcept
{
    *reinterpret_cast<size_t*>(block) = size;
}

inline void* read_next(const void* block) noexcept
{
    return const_cast<void*>(reinterpret_cast<const void* const*>(block)[1]);
}

inline void write_next(void* block, void* next) noexcept
{
    reinterpret_cast<void**>(block)[1] = next;
}

inline void* read_prev(const void* block) noexcept
{
    return const_cast<void*>(reinterpret_cast<const void* const*>(block)[2]);
}

inline void write_prev(void* block, void* prev) noexcept
{
    reinterpret_cast<void**>(block)[2] = prev;
}

inline bool read_occupied(const void* block) noexcept
{
    return reinterpret_cast<const void* const*>(block)[3] != nullptr;
}

inline void write_occupied(void* block, bool occupied) noexcept
{
    reinterpret_cast<void**>(block)[3] = occupied ? block : nullptr;
}

inline void* to_user(void* block) noexcept
{
    return reinterpret_cast<char*>(block) + BLOCK_META_SIZE;
}

inline void* to_block(void* user) noexcept
{
    return reinterpret_cast<char*>(user) - BLOCK_META_SIZE;
}

} // anonymous namespace

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory != nullptr)
    {
        internal_state_t* state = get_state(_trusted_memory);
        state->mtx.~mutex();
        
        if (state->parent_allocator != nullptr)
        {
            state->parent_allocator->deallocate(_trusted_memory, state->total_size + get_state_size());
        }
        else
        {
            std::pmr::get_default_resource()->deallocate(_trusted_memory, state->total_size + get_state_size());
        }
        _trusted_memory = nullptr;
    }
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags& other)
    : _trusted_memory(nullptr)
{
    const internal_state_t* other_state = get_state_const(other._trusted_memory);
    std::lock_guard<std::mutex> lock(other_state->mtx);
    
    // Allocate EXTRA space for internal state
    size_t total_alloc = other_state->total_size + get_state_size();
    
    if (total_alloc > 0 && other._trusted_memory != nullptr)
    {
        std::pmr::memory_resource* parent = (other_state->parent_allocator != nullptr) 
            ? other_state->parent_allocator 
            : std::pmr::get_default_resource();
        _trusted_memory = parent->allocate(total_alloc);
        if (_trusted_memory == nullptr)
        {
            throw std::bad_alloc();
        }
        
        internal_state_t* new_state = new (_trusted_memory) internal_state_t();
        new_state->parent_allocator = other_state->parent_allocator;
        new_state->mode = other_state->mode;
        new_state->total_size = other_state->total_size;
        
        // Copy ONLY user blocks (skip internal state)
        std::memcpy(get_block_start(_trusted_memory), get_block_start_const(other._trusted_memory), 
                    other_state->total_size);
        
        void* current = get_block_start(_trusted_memory);
        void* prev = nullptr;
        const char* other_start = reinterpret_cast<const char*>(get_block_start_const(other._trusted_memory));
        char* new_start = reinterpret_cast<char*>(get_block_start(_trusted_memory));
        
        while (current != nullptr && reinterpret_cast<char*>(current) < reinterpret_cast<char*>(_trusted_memory) + total_alloc)
        {
            write_prev(current, prev);
            void* next = read_next(current);
            if (next != nullptr)
            {
                size_t offset = reinterpret_cast<char*>(next) - other_start;
                next = new_start + offset;
                write_next(current, next);
            }
            else
            {
                write_next(current, nullptr);
            }
            
            if (read_occupied(current))
            {
                write_occupied(current, true);
            }
            
            prev = current;
            size_t size = read_size(current);
            if (size == 0 || size > other_state->total_size) break;
            current = reinterpret_cast<char*>(current) + size;
        }
        new_state->first_block = get_block_start(_trusted_memory);
    }
}

allocator_boundary_tags& allocator_boundary_tags::operator=(const allocator_boundary_tags& other)
{
    if (this != &other)
    {
        internal_state_t* my_state = get_state(_trusted_memory);
        const internal_state_t* other_state = get_state_const(other._trusted_memory);
        
        std::lock_guard<std::mutex> lock1(my_state->mtx);
        std::lock_guard<std::mutex> lock2(other_state->mtx);
        
        if (_trusted_memory != nullptr)
        {
            my_state->mtx.~mutex();
            if (my_state->parent_allocator != nullptr)
            {
                my_state->parent_allocator->deallocate(_trusted_memory, my_state->total_size + get_state_size());
            }
            else
            {
                std::pmr::get_default_resource()->deallocate(_trusted_memory, my_state->total_size + get_state_size());
            }
            _trusted_memory = nullptr;
        }
        
        size_t total_alloc = other_state->total_size + get_state_size();
        if (total_alloc > 0 && other._trusted_memory != nullptr)
        {
            std::pmr::memory_resource* parent = (other_state->parent_allocator != nullptr) 
                ? other_state->parent_allocator 
                : std::pmr::get_default_resource();
            _trusted_memory = parent->allocate(total_alloc);
            if (_trusted_memory == nullptr)
            {
                throw std::bad_alloc();
            }
            
            internal_state_t* new_state = new (_trusted_memory) internal_state_t();
            new_state->parent_allocator = other_state->parent_allocator;
            new_state->mode = other_state->mode;
            new_state->total_size = other_state->total_size;
            
            std::memcpy(get_block_start(_trusted_memory), get_block_start_const(other._trusted_memory), 
                        other_state->total_size);
            
            void* current = get_block_start(_trusted_memory);
            void* prev = nullptr;
            const char* other_start = reinterpret_cast<const char*>(get_block_start_const(other._trusted_memory));
            char* new_start = reinterpret_cast<char*>(get_block_start(_trusted_memory));
            
            while (current != nullptr && reinterpret_cast<char*>(current) < reinterpret_cast<char*>(_trusted_memory) + total_alloc)
            {
                write_prev(current, prev);
                void* next = read_next(current);
                if (next != nullptr)
                {
                    size_t offset = reinterpret_cast<char*>(next) - other_start;
                    next = new_start + offset;
                    write_next(current, next);
                }
                else
                {
                    write_next(current, nullptr);
                }
                
                if (read_occupied(current))
                {
                    write_occupied(current, true);
                }
                
                prev = current;
                size_t size = read_size(current);
                if (size == 0 || size > other_state->total_size) break;
                current = reinterpret_cast<char*>(current) + size;
            }
            new_state->first_block = get_block_start(_trusted_memory);
        }
    }
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags&& other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags& allocator_boundary_tags::operator=(allocator_boundary_tags&& other) noexcept
{
    if (this != &other)
    {
        if (_trusted_memory != nullptr)
        {
            internal_state_t* state = get_state(_trusted_memory);
            state->mtx.~mutex();
            if (state->parent_allocator != nullptr)
            {
                state->parent_allocator->deallocate(_trusted_memory, state->total_size + get_state_size());
            }
            else
            {
                std::pmr::get_default_resource()->deallocate(_trusted_memory, state->total_size + get_state_size());
            }
        }
        
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    if (space_size == 0)
    {
        throw std::invalid_argument("space_size cannot be zero");
    }
    
    std::pmr::memory_resource* parent = (parent_allocator != nullptr) 
        ? parent_allocator 
        : std::pmr::get_default_resource();
    
    // Allocate EXTRA space for internal state (space_size is for USER blocks only!)
    size_t total_alloc = space_size + get_state_size();
    _trusted_memory = parent->allocate(total_alloc);
    if (_trusted_memory == nullptr)
    {
        throw std::bad_alloc();
    }
    
    internal_state_t* state = new (_trusted_memory) internal_state_t();
    state->parent_allocator = parent;
    state->mode = allocate_fit_mode;
    state->total_size = space_size;  // Store USER space size
    
    void* first_block = get_block_start(_trusted_memory);
    
    // First block spans ALL user space
    write_size(first_block, space_size);
    write_next(first_block, nullptr);
    write_prev(first_block, nullptr);
    write_occupied(first_block, false);
    state->first_block = first_block;
}

[[nodiscard]] void* allocator_boundary_tags::do_allocate_sm(size_t bytes)
{
    internal_state_t* state = get_state(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mtx);
    
    // Keep bytes as-is (tests expect exact sizes, even 0)
    size_t total_needed = bytes + BLOCK_META_SIZE;
    
    void* found_block = nullptr;
    
    switch (state->mode)
    {
        case fit_mode::first_fit:
            found_block = find_first_fit(state->first_block, total_needed);
            break;
        case fit_mode::the_best_fit:
            found_block = find_best_fit(state->first_block, total_needed);
            break;
        case fit_mode::the_worst_fit:
            found_block = find_worst_fit(state->first_block, total_needed);
            break;
        default:
            found_block = find_first_fit(state->first_block, total_needed);
            break;
    }
    
    if (found_block == nullptr)
    {
        throw std::bad_alloc();
    }
    
    size_t block_size = read_size(found_block);
    size_t remaining = block_size - total_needed;
    
    // Split if remaining is enough for metadata + at least 1 byte
    if (remaining >= BLOCK_META_SIZE + 1)
    {
        void* new_block = reinterpret_cast<char*>(found_block) + total_needed;
        
        write_size(new_block, remaining);
        write_next(new_block, read_next(found_block));
        write_prev(new_block, found_block);
        write_occupied(new_block, false);
        
        if (read_next(found_block) != nullptr)
        {
            write_prev(read_next(found_block), new_block);
        }
        
        write_next(found_block, new_block);
        write_size(found_block, total_needed);
    }
    
    write_occupied(found_block, true);
    
    return to_user(found_block);
}

void* allocator_boundary_tags::find_first_fit(void* start, size_t needed_size)
{
    void* current = start;
    while (current != nullptr)
    {
        size_t block_size = read_size(current);
        if (!read_occupied(current) && block_size >= needed_size)
        {
            return current;
        }
        current = read_next(current);
    }
    return nullptr;
}

void* allocator_boundary_tags::find_best_fit(void* start, size_t needed_size)
{
    void* current = start;
    void* best_block = nullptr;
    size_t best_size = SIZE_MAX;
    
    while (current != nullptr)
    {
        size_t block_size = read_size(current);
        if (!read_occupied(current) && block_size >= needed_size && block_size < best_size)
        {
            best_block = current;
            best_size = block_size;
        }
        current = read_next(current);
    }
    return best_block;
}

void* allocator_boundary_tags::find_worst_fit(void* start, size_t needed_size)
{
    void* current = start;
    void* worst_block = nullptr;
    size_t worst_size = 0;
    
    while (current != nullptr)
    {
        size_t block_size = read_size(current);
        if (!read_occupied(current) && block_size >= needed_size && block_size > worst_size)
        {
            worst_block = current;
            worst_size = block_size;
        }
        current = read_next(current);
    }
    return worst_block;
}

void allocator_boundary_tags::do_deallocate_sm(void* at)
{
    if (at == nullptr) return;
    
    internal_state_t* state = get_state(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mtx);
    
    char* mem_start = reinterpret_cast<char*>(get_block_start(_trusted_memory));
    char* mem_end = reinterpret_cast<char*>(get_block_start(_trusted_memory)) + state->total_size;
    char* ptr = reinterpret_cast<char*>(at);
    
    if (ptr < mem_start || ptr >= mem_end)
    {
        throw std::invalid_argument("Pointer does not belong to this allocator");
    }
    
    void* block = to_block(at);
    size_t block_size = read_size(block);
    
    if (block_size == 0 || block_size > state->total_size)
    {
        throw std::invalid_argument("Invalid block size");
    }
    
    write_occupied(block, false);
    
    // Merge with next
    void* next_block = read_next(block);
    if (next_block != nullptr && !read_occupied(next_block))
    {
        size_t next_size = read_size(next_block);
        write_size(block, block_size + next_size);
        write_next(block, read_next(next_block));
        
        if (read_next(block) != nullptr)
        {
            write_prev(read_next(block), block);
        }
    }
    
    // Merge with prev
    void* prev_block = read_prev(block);
    if (prev_block != nullptr && !read_occupied(prev_block))
    {
        size_t prev_size = read_size(prev_block);
        write_size(prev_block, prev_size + read_size(block));
        write_next(prev_block, read_next(block));
        
        if (read_next(prev_block) != nullptr)
        {
            write_prev(read_next(prev_block), prev_block);
        }
    }
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    internal_state_t* state = get_state(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mtx);
    state->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    const internal_state_t* state = get_state_const(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    const void* current = get_block_start_const(_trusted_memory);
    const internal_state_t* state = get_state_const(_trusted_memory);
    const char* mem_end = reinterpret_cast<const char*>(get_block_start_const(_trusted_memory)) + state->total_size;
    
    while (current != nullptr && reinterpret_cast<const char*>(current) < mem_end)
    {
        size_t block_size = read_size(current);
        if (block_size == 0) break;
        
        bool occupied = read_occupied(current);
        
        allocator_test_utils::block_info info;
        info.block_size = block_size;
        info.is_block_occupied = occupied;
        
        result.push_back(info);
        
        current = read_next(current);
    }
    
    return result;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(const_cast<void*>(get_block_start_const(_trusted_memory)));
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator(nullptr);
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return this == &other;
}

// Iterator Implementation

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted)
    : _occupied_ptr(trusted), _occupied(trusted != nullptr ? read_occupied(trusted) : false), _trusted_memory(trusted)
{
}

bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr != nullptr)
    {
        _occupied_ptr = read_next(_occupied_ptr);
        if (_occupied_ptr != nullptr)
        {
            _occupied = read_occupied(_occupied_ptr);
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr != nullptr)
    {
        _occupied_ptr = read_prev(_occupied_ptr);
        if (_occupied_ptr != nullptr)
        {
            _occupied = read_occupied(_occupied_ptr);
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int n)
{
    (void)n;
    boundary_iterator temp = *this;
    ++(*this);
    return temp;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int n)
{
    (void)n;
    boundary_iterator temp = *this;
    --(*this);
    return temp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr != nullptr)
    {
        return read_size(_occupied_ptr);
    }
    return 0;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr != nullptr)
    {
        return to_user(_occupied_ptr);
    }
    return nullptr;
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}