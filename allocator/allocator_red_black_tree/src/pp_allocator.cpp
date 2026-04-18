#include <pp_allocator.h>
#include <cstddef>
#include <new>

// Реализация методов smart_mem_resource для создания vtable
void smart_mem_resource::do_deallocate(void* p, size_t bytes, size_t alignment)
{
    do_deallocate_sm(p);
}

void* smart_mem_resource::do_allocate(size_t bytes, size_t alignment)
{
    return do_allocate_sm(bytes);
}