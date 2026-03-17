#include <new>
#include <mutex>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include "../include/allocator_global_heap.h"


allocator_global_heap::allocator_global_heap()
{
    // инициализация
}


allocator_global_heap::~allocator_global_heap()
{
    // освобождение
}


allocator_global_heap::allocator_global_heap(const allocator_global_heap &other)
{
    // Аллокатор не хранит состояния, которое нужно копировать
    // Каждый экземпляр имеет свой mutex
    (void)other;
}


allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this == std::addressof(other))
    {
        return *this;
    }
    (void)other;
    return *this;
}


allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
{
    // Аллокатор не хранит состояния, которое нужно перемещать
    (void)other;
}


allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this == std::addressof(other))
    {
        return *this;
    }
    (void)other;
    return *this;
}


[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    
    // Обработка выделения 0 байт
    if (size == 0)
    {
        size = 1;
    }
    
    // Проверка на переполнение
    if (size > std::numeric_limits<size_t>::max() - size_t_size)
    {
        throw std::bad_alloc();
    }
    
    void *allocated_memory = nullptr;
    
    try
    {
        allocated_memory = ::operator new(size);
        if (allocated_memory == nullptr)
        {
            throw std::bad_alloc();
        }
    }
    catch (const std::bad_alloc &)
    {
        throw;
    }
    catch (...)
    {
        if (allocated_memory != nullptr)
        {
            ::operator delete(allocated_memory);
            allocated_memory = nullptr;
        }
        throw std::bad_alloc();
    }
    
    return allocated_memory;
}


void allocator_global_heap::do_deallocate_sm(void *at)
{
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    
    if (at == nullptr)
    {
        return;
    }
    
    try
    {
        ::operator delete(at);
    }
    catch (...)
    {
        // Не выбрасываем исключения из deallocate
    }
}


bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return typeid(*this) == typeid(other);
}