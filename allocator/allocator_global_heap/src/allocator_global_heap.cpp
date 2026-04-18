#include <new>
#include <mutex>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include "../include/allocator_global_heap.h"


allocator_global_heap::allocator_global_heap()
{
}


allocator_global_heap::~allocator_global_heap()
{
}


allocator_global_heap::allocator_global_heap(const allocator_global_heap &other)
{
    (void)other;
}


allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    (void)other;
    return *this;
}


allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
{
    (void)other;
}


allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    (void)other;
    return *this;
}


[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size)
{
     return ::operator new(size);
}


void allocator_global_heap::do_deallocate_sm(void *at)
{
   ::operator delete(at);
}


bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return dynamic_cast<const allocator_global_heap*>(&other) != nullptr;
}