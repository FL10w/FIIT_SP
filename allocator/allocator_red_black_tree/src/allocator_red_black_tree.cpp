#include "../include/allocator_red_black_tree.h"
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

constexpr size_t parent_off = 0;
constexpr size_t mode_off = parent_off + sizeof(std::pmr::memory_resource*);
constexpr size_t managed_off = mode_off + sizeof(allocator_with_fit_mode::fit_mode);
constexpr size_t mutex_off = managed_off + sizeof(size_t);
constexpr size_t root_off = mutex_off + sizeof(std::mutex);

constexpr size_t bd_off = 0;
constexpr size_t bd_sz = sizeof(unsigned char);
constexpr size_t s0_off = bd_off + bd_sz;
constexpr size_t s1_off = s0_off + sizeof(void*);
constexpr size_t s2_off = s1_off + sizeof(void*);
constexpr size_t s3_off = s2_off + sizeof(void*);
constexpr size_t s4_off = s3_off + sizeof(void*);

constexpr size_t occ_md = bd_sz + 3 * sizeof(void*);
constexpr size_t free_md = bd_sz + 5 * sizeof(void*);
constexpr size_t md_delta = free_md - occ_md;

constexpr unsigned char c_red = 0;
constexpr unsigned char c_black = 1;

static void *lp(const char *p, size_t off)
{
    void *v = nullptr;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

static void sp(char *p, size_t off, void *v)
{
    std::memcpy(p + off, &v, sizeof(v));
}

static size_t ls(const char *p, size_t off)
{
    size_t v = 0;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

static void ss(char *p, size_t off, size_t v)
{
    std::memcpy(p + off, &v, sizeof(v));
}

static unsigned char lb(const char *p)
{
    unsigned char v = 0;
    std::memcpy(&v, p + bd_off, sizeof(v));
    return v;
}

static void sb(char *p, unsigned char v)
{
    std::memcpy(p + bd_off, &v, sizeof(v));
}

static bool occ(const char *b)
{
    return (lb(b) & 0x0F) != 0;
}

static unsigned char color_of(const char *b)
{
    if (b == nullptr)
    {
        return c_black;
    }
    return static_cast<unsigned char>((lb(b) >> 4) & 0x0F);
}

static void set_color(char *b, unsigned char c)
{
    if (b == nullptr)
    {
        return;
    }
    unsigned char v = lb(b);
    v = static_cast<unsigned char>((v & 0x0F) | ((c & 0x0F) << 4));
    sb(b, v);
}

static size_t free_sz(const char *b)
{
    return ls(b, s3_off);
}

static size_t occ_sz(const char *b)
{
    return ls(b, s1_off);
}

static size_t blk_sz(const char *b)
{
    return occ(b) ? occ_sz(b) : free_sz(b);
}

static size_t user_sz(const char *b)
{
    if (!occ(b))
    {
        return free_sz(b);
    }
    const size_t raw = occ_sz(b);
    return raw >= md_delta ? (raw - md_delta) : 0;
}

static char *root(char *trusted)
{
    return reinterpret_cast<char*>(lp(trusted, root_off));
}

static void set_root(char *trusted, char *node)
{
    sp(trusted, root_off, node);
}

static bool less_node(char *a, char *b)
{
    const size_t as = free_sz(a);
    const size_t bs = free_sz(b);
    if (as != bs)
    {
        return as < bs;
    }
    return a < b;
}

static char *next_blk(char *b)
{
    return b + free_md + blk_sz(b);
}

static void mark_occ(char *b, void *owner, size_t payload_plus_delta, char *prev)
{
    sb(b, static_cast<unsigned char>(1 | (c_black << 4)));
    sp(b, s0_off, owner);
    ss(b, s1_off, payload_plus_delta);
    sp(b, s2_off, prev);
}

static void mark_free(char *b, size_t payload, char *prev)
{
    sb(b, static_cast<unsigned char>(c_red << 4));
    sp(b, s0_off, nullptr);
    sp(b, s1_off, nullptr);
    sp(b, s2_off, nullptr);
    ss(b, s3_off, payload);
    sp(b, s4_off, prev);
}

static void left_rotate(char *trusted, char *x)
{
    char *y = reinterpret_cast<char*>(lp(x, s2_off));
    char *yl = reinterpret_cast<char*>(lp(y, s1_off));
    sp(x, s2_off, yl);
    if (yl != nullptr)
    {
        sp(yl, s0_off, x);
    }
    char *xp = reinterpret_cast<char*>(lp(x, s0_off));
    sp(y, s0_off, xp);
    if (xp == nullptr)
    {
        set_root(trusted, y);
    }
    else if (x == reinterpret_cast<char*>(lp(xp, s1_off)))
    {
        sp(xp, s1_off, y);
    }
    else
    {
        sp(xp, s2_off, y);
    }
    sp(y, s1_off, x);
    sp(x, s0_off, y);
}

static void right_rotate(char *trusted, char *y)
{
    char *x = reinterpret_cast<char*>(lp(y, s1_off));
    char *xr = reinterpret_cast<char*>(lp(x, s2_off));
    sp(y, s1_off, xr);
    if (xr != nullptr)
    {
        sp(xr, s0_off, y);
    }
    char *yp = reinterpret_cast<char*>(lp(y, s0_off));
    sp(x, s0_off, yp);
    if (yp == nullptr)
    {
        set_root(trusted, x);
    }
    else if (y == reinterpret_cast<char*>(lp(yp, s2_off)))
    {
        sp(yp, s2_off, x);
    }
    else
    {
        sp(yp, s1_off, x);
    }
    sp(x, s2_off, y);
    sp(y, s0_off, x);
}

static void rb_insert_fixup(char *trusted, char *z)
{
    while (z != nullptr)
    {
        char *p = reinterpret_cast<char*>(lp(z, s0_off));
        if (p == nullptr || color_of(p) != c_red)
        {
            break;
        }
        char *g = reinterpret_cast<char*>(lp(p, s0_off));
        if (p == reinterpret_cast<char*>(lp(g, s1_off)))
        {
            char *u = reinterpret_cast<char*>(lp(g, s2_off));
            if (color_of(u) == c_red)
            {
                set_color(p, c_black);
                set_color(u, c_black);
                set_color(g, c_red);
                z = g;
            }
            else
            {
                if (z == reinterpret_cast<char*>(lp(p, s2_off)))
                {
                    z = p;
                    left_rotate(trusted, z);
                    p = reinterpret_cast<char*>(lp(z, s0_off));
                    g = reinterpret_cast<char*>(lp(p, s0_off));
                }
                set_color(p, c_black);
                set_color(g, c_red);
                right_rotate(trusted, g);
            }
        }
        else
        {
            char *u = reinterpret_cast<char*>(lp(g, s1_off));
            if (color_of(u) == c_red)
            {
                set_color(p, c_black);
                set_color(u, c_black);
                set_color(g, c_red);
                z = g;
            }
            else
            {
                if (z == reinterpret_cast<char*>(lp(p, s1_off)))
                {
                    z = p;
                    right_rotate(trusted, z);
                    p = reinterpret_cast<char*>(lp(z, s0_off));
                    g = reinterpret_cast<char*>(lp(p, s0_off));
                }
                set_color(p, c_black);
                set_color(g, c_red);
                left_rotate(trusted, g);
            }
        }
    }
    set_color(root(trusted), c_black);
}

static void rb_insert(char *trusted, char *node)
{
    sp(node, s0_off, nullptr);
    sp(node, s1_off, nullptr);
    sp(node, s2_off, nullptr);
    set_color(node, c_red);

    char *y = nullptr;
    char *x = root(trusted);
    while (x != nullptr)
    {
        y = x;
        x = less_node(node, x) ? reinterpret_cast<char*>(lp(x, s1_off)) : reinterpret_cast<char*>(lp(x, s2_off));
    }
    sp(node, s0_off, y);
    if (y == nullptr)
    {
        set_root(trusted, node);
    }
    else if (less_node(node, y))
    {
        sp(y, s1_off, node);
    }
    else
    {
        sp(y, s2_off, node);
    }
    rb_insert_fixup(trusted, node);
}

static char *rb_min(char *node)
{
    while (node != nullptr && reinterpret_cast<char*>(lp(node, s1_off)) != nullptr)
    {
        node = reinterpret_cast<char*>(lp(node, s1_off));
    }
    return node;
}

static void rb_transplant(char *trusted, char *u, char *v)
{
    char *up = reinterpret_cast<char*>(lp(u, s0_off));
    if (up == nullptr)
    {
        set_root(trusted, v);
    }
    else if (u == reinterpret_cast<char*>(lp(up, s1_off)))
    {
        sp(up, s1_off, v);
    }
    else
    {
        sp(up, s2_off, v);
    }
    if (v != nullptr)
    {
        sp(v, s0_off, up);
    }
}

static void rb_delete_fixup(char *trusted, char *x, char *xp)
{
    while (x != root(trusted) && color_of(x) == c_black)
    {
        if (xp == nullptr)
        {
            break;
        }
        if (x == reinterpret_cast<char*>(lp(xp, s1_off)))
        {
            char *w = reinterpret_cast<char*>(lp(xp, s2_off));
            if (color_of(w) == c_red)
            {
                set_color(w, c_black);
                set_color(xp, c_red);
                left_rotate(trusted, xp);
                w = reinterpret_cast<char*>(lp(xp, s2_off));
            }
            char *wl = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s1_off));
            char *wr = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s2_off));
            if (color_of(wl) == c_black && color_of(wr) == c_black)
            {
                set_color(w, c_red);
                x = xp;
                xp = reinterpret_cast<char*>(lp(xp, s0_off));
            }
            else
            {
                if (color_of(wr) == c_black)
                {
                    set_color(wl, c_black);
                    set_color(w, c_red);
                    right_rotate(trusted, w);
                    w = reinterpret_cast<char*>(lp(xp, s2_off));
                    wl = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s1_off));
                    wr = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s2_off));
                }
                set_color(w, color_of(xp));
                set_color(xp, c_black);
                set_color(wr, c_black);
                left_rotate(trusted, xp);
                x = root(trusted);
                xp = nullptr;
            }
        }
        else
        {
            char *w = reinterpret_cast<char*>(lp(xp, s1_off));
            if (color_of(w) == c_red)
            {
                set_color(w, c_black);
                set_color(xp, c_red);
                right_rotate(trusted, xp);
                w = reinterpret_cast<char*>(lp(xp, s1_off));
            }
            char *wl = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s1_off));
            char *wr = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s2_off));
            if (color_of(wr) == c_black && color_of(wl) == c_black)
            {
                set_color(w, c_red);
                x = xp;
                xp = reinterpret_cast<char*>(lp(xp, s0_off));
            }
            else
            {
                if (color_of(wl) == c_black)
                {
                    set_color(wr, c_black);
                    set_color(w, c_red);
                    left_rotate(trusted, w);
                    w = reinterpret_cast<char*>(lp(xp, s1_off));
                    wl = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s1_off));
                    wr = (w == nullptr) ? nullptr : reinterpret_cast<char*>(lp(w, s2_off));
                }
                set_color(w, color_of(xp));
                set_color(xp, c_black);
                set_color(wl, c_black);
                right_rotate(trusted, xp);
                x = root(trusted);
                xp = nullptr;
            }
        }
    }
    set_color(x, c_black);
}

static void rb_delete(char *trusted, char *z)
{
    char *y = z;
    unsigned char y_color = color_of(y);
    char *x = nullptr;
    char *xp = nullptr;

    if (reinterpret_cast<char*>(lp(z, s1_off)) == nullptr)
    {
        x = reinterpret_cast<char*>(lp(z, s2_off));
        xp = reinterpret_cast<char*>(lp(z, s0_off));
        rb_transplant(trusted, z, reinterpret_cast<char*>(lp(z, s2_off)));
    }
    else if (reinterpret_cast<char*>(lp(z, s2_off)) == nullptr)
    {
        x = reinterpret_cast<char*>(lp(z, s1_off));
        xp = reinterpret_cast<char*>(lp(z, s0_off));
        rb_transplant(trusted, z, reinterpret_cast<char*>(lp(z, s1_off)));
    }
    else
    {
        y = rb_min(reinterpret_cast<char*>(lp(z, s2_off)));
        y_color = color_of(y);
        x = reinterpret_cast<char*>(lp(y, s2_off));
        if (reinterpret_cast<char*>(lp(y, s0_off)) == z)
        {
            xp = y;
            if (x != nullptr)
            {
                sp(x, s0_off, y);
            }
        }
        else
        {
            xp = reinterpret_cast<char*>(lp(y, s0_off));
            rb_transplant(trusted, y, reinterpret_cast<char*>(lp(y, s2_off)));
            sp(y, s2_off, reinterpret_cast<char*>(lp(z, s2_off)));
            sp(reinterpret_cast<char*>(lp(y, s2_off)), s0_off, y);
        }
        rb_transplant(trusted, z, y);
        sp(y, s1_off, reinterpret_cast<char*>(lp(z, s1_off)));
        sp(reinterpret_cast<char*>(lp(y, s1_off)), s0_off, y);
        set_color(y, color_of(z));
    }

    if (y_color == c_black)
    {
        rb_delete_fixup(trusted, x, xp);
    }
}

allocator_red_black_tree::~allocator_red_black_tree()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    char *base = reinterpret_cast<char*>(_trusted_memory);
    auto *parent = reinterpret_cast<std::pmr::memory_resource*>(lp(base, parent_off));
    const size_t managed = ls(base, managed_off);
    auto *mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::destroy_at(mtx);
    parent->deallocate(_trusted_memory, allocator_metadata_size + free_block_metadata_size + managed);
    _trusted_memory = nullptr;
}

allocator_red_black_tree::allocator_red_black_tree(allocator_red_black_tree &&other) noexcept:
    _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr)
    {
        return;
    }
    char *ob = reinterpret_cast<char*>(other._trusted_memory);
    auto *om = reinterpret_cast<std::mutex*>(ob + mutex_off);
    std::lock_guard<std::mutex> lock(*om);
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(allocator_red_black_tree &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    std::mutex *tm = nullptr;
    if (_trusted_memory != nullptr)
    {
        tm = reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(_trusted_memory) + mutex_off);
    }
    std::mutex *om = nullptr;
    if (other._trusted_memory != nullptr)
    {
        om = reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(other._trusted_memory) + mutex_off);
    }
    void *old = nullptr;
    if (tm != nullptr && om != nullptr && tm != om)
    {
        std::scoped_lock lock(*tm, *om);
        old = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else if (tm != nullptr)
    {
        std::lock_guard<std::mutex> lock(*tm);
        old = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else if (om != nullptr)
    {
        std::lock_guard<std::mutex> lock(*om);
        old = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    else
    {
        old = _trusted_memory;
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    if (old != nullptr)
    {
        char *b = reinterpret_cast<char*>(old);
        auto *parent = reinterpret_cast<std::pmr::memory_resource*>(lp(b, parent_off));
        size_t managed = ls(b, managed_off);
        auto *m = reinterpret_cast<std::mutex*>(b + mutex_off);
        std::destroy_at(m);
        parent->deallocate(old, allocator_metadata_size + free_block_metadata_size + managed);
    }
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }
    _trusted_memory = parent_allocator->allocate(allocator_metadata_size + free_block_metadata_size + space_size);
    char *base = reinterpret_cast<char*>(_trusted_memory);
    sp(base, parent_off, parent_allocator);
    std::memcpy(base + mode_off, &allocate_fit_mode, sizeof(allocate_fit_mode));
    ss(base, managed_off, space_size);
    new (base + mutex_off) std::mutex();

    char *first = base + allocator_metadata_size;
    mark_free(first, space_size, nullptr);
    set_color(first, c_black);
    set_root(base, first);
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other)
{
    if (other._trusted_memory == nullptr)
    {
        _trusted_memory = nullptr;
        return;
    }
    char *ob = reinterpret_cast<char*>(other._trusted_memory);
    auto *om = reinterpret_cast<std::mutex*>(ob + mutex_off);
    std::lock_guard<std::mutex> lock(*om);

    auto *parent = reinterpret_cast<std::pmr::memory_resource*>(lp(ob, parent_off));
    auto mode = *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(ob + mode_off);
    size_t managed = ls(ob, managed_off);
    if (parent == nullptr)
    {
        parent = std::pmr::get_default_resource();
    }
    _trusted_memory = parent->allocate(allocator_metadata_size + free_block_metadata_size + managed);
    char *base = reinterpret_cast<char*>(_trusted_memory);
    sp(base, parent_off, parent);
    std::memcpy(base + mode_off, &mode, sizeof(mode));
    ss(base, managed_off, managed);
    new (base + mutex_off) std::mutex();

    char *first = base + allocator_metadata_size;
    mark_free(first, managed, nullptr);
    set_color(first, c_black);
    set_root(base, first);
}

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other)
{
    if (this == &other)
    {
        return *this;
    }
    allocator_red_black_tree tmp(other);
    *this = std::move(tmp);
    return *this;
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_red_black_tree*>(&other) != nullptr;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size)
{
    if (size == 0)
    {
        size = 1;
    }
    char *base = reinterpret_cast<char*>(_trusted_memory);
    auto *mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);

    char *chosen = nullptr;
    auto mode = *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off);

    std::vector<char*> st;
    char *cur = root(base);
    while (cur != nullptr || !st.empty())
    {
        while (cur != nullptr)
        {
            st.push_back(cur);
            cur = reinterpret_cast<char*>(lp(cur, s1_off));
        }
        cur = st.back();
        st.pop_back();

        size_t cs = free_sz(cur);
        if (cs >= size)
        {
            if (chosen == nullptr)
            {
                chosen = cur;
                if (mode == allocator_with_fit_mode::fit_mode::first_fit)
                {
                    break;
                }
            }
            else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit)
            {
                if (cs < free_sz(chosen))
                {
                    chosen = cur;
                }
            }
            else if (mode == allocator_with_fit_mode::fit_mode::the_worst_fit)
            {
                if (cs > free_sz(chosen))
                {
                    chosen = cur;
                }
            }
        }

        cur = reinterpret_cast<char*>(lp(cur, s2_off));
    }

    if (chosen == nullptr)
    {
        throw std::bad_alloc();
    }

    char *prev = reinterpret_cast<char*>(occ(chosen) ? lp(chosen, s2_off) : lp(chosen, s4_off));
    char *next = next_blk(chosen);
    size_t fs = free_sz(chosen);

    rb_delete(base, chosen);

    size_t remaining = fs >= size ? (fs - size) : 0;
    if (remaining >= free_block_metadata_size + 1)
    {
        char *nf = chosen + free_md + size;
        size_t nf_payload = remaining - free_md;
        mark_free(nf, nf_payload, chosen);
        if (next < base + allocator_metadata_size + free_md + ls(base, managed_off))
        {
            if (occ(next))
            {
                sp(next, s2_off, nf);
            }
            else
            {
                sp(next, s4_off, nf);
            }
        }
        rb_insert(base, nf);
        mark_occ(chosen, _trusted_memory, size + md_delta, prev);
    }
    else
    {
        if (next < base + allocator_metadata_size + free_md + ls(base, managed_off))
        {
            if (occ(next))
            {
                sp(next, s2_off, chosen);
            }
            else
            {
                sp(next, s4_off, chosen);
            }
        }
        mark_occ(chosen, _trusted_memory, fs + md_delta, prev);
    }

    return chosen + occ_md;
}

void allocator_red_black_tree::do_deallocate_sm(void *at)
{
    if (at == nullptr)
    {
        return;
    }
    char *base = reinterpret_cast<char*>(_trusted_memory);
    auto *mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);

    char *begin = base + allocator_metadata_size;
    char *end = begin + free_md + ls(base, managed_off);
    char *payload = reinterpret_cast<char*>(at);

    if (payload < begin + occ_md || payload >= end)
    {
        throw std::invalid_argument("");
    }

    char *block = payload - occ_md;
    if (!occ(block))
    {
        throw std::invalid_argument("");
    }
    if (lp(block, s0_off) != _trusted_memory)
    {
        throw std::invalid_argument("");
    }

    size_t cur = occ_sz(block);
    if (cur < md_delta)
    {
        throw std::invalid_argument("");
    }
    cur -= md_delta;
    char *prev = reinterpret_cast<char*>(lp(block, s2_off));
    mark_free(block, cur, prev);

    char *next = next_blk(block);
    if (next < end && !occ(next))
    {
        rb_delete(base, next);
        cur += free_md + free_sz(next);
        ss(block, s3_off, cur);
        char *nn = next_blk(next);
        if (nn < end)
        {
            if (occ(nn))
            {
                sp(nn, s2_off, block);
            }
            else
            {
                sp(nn, s4_off, block);
            }
        }
    }

    if (prev != nullptr && !occ(prev))
    {
        rb_delete(base, prev);
        size_t merged = free_sz(prev) + free_md + free_sz(block);
        ss(prev, s3_off, merged);
        char *after = next_blk(prev);
        if (after < end)
        {
            if (occ(after))
            {
                sp(after, s2_off, prev);
            }
            else
            {
                sp(after, s4_off, prev);
            }
        }
        block = prev;
    }

    rb_insert(base, block);
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    char *base = reinterpret_cast<char*>(_trusted_memory);
    auto *mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(base + mode_off) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    if (_trusted_memory == nullptr)
    {
        return {};
    }
    char *base = reinterpret_cast<char*>(_trusted_memory);
    auto *mtx = reinterpret_cast<std::mutex*>(base + mutex_off);
    std::lock_guard<std::mutex> lock(*mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> out;
    if (_trusted_memory == nullptr)
    {
        return out;
    }
    char *base = reinterpret_cast<char*>(_trusted_memory);
    char *b = base + allocator_metadata_size;
    char *end = b + free_md + ls(base, managed_off);
    while (b < end)
    {
        out.push_back({user_sz(b), occ(b)});
        b = next_blk(b);
    }
    return out;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return rb_iterator();
    }
    return rb_iterator(_trusted_memory);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    return rb_iterator();
}

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
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return *this;
    }
    char *base = reinterpret_cast<char*>(_trusted);
    char *end = base + allocator_metadata_size + free_md + ls(base, managed_off);
    char *next = reinterpret_cast<char*>(_block_ptr) + free_md + blk_sz(reinterpret_cast<char*>(_block_ptr));
    _block_ptr = (next >= end) ? nullptr : next;
    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int n)
{
    (void)n;
    rb_iterator tmp(*this);
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept
{
    if (_block_ptr == nullptr)
    {
        return 0;
    }
    return user_sz(reinterpret_cast<char*>(_block_ptr));
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    if (_block_ptr == nullptr)
    {
        return nullptr;
    }
    return occ(reinterpret_cast<char*>(_block_ptr)) ? reinterpret_cast<char*>(_block_ptr) + occ_md : reinterpret_cast<char*>(_block_ptr) + free_md;
}

allocator_red_black_tree::rb_iterator::rb_iterator()
{
    _block_ptr = nullptr;
    _trusted = nullptr;
}

allocator_red_black_tree::rb_iterator::rb_iterator(void *trusted)
{
    _trusted = trusted;
    _block_ptr = (trusted == nullptr) ? nullptr : reinterpret_cast<char*>(trusted) + allocator_metadata_size;
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    if (_block_ptr == nullptr)
    {
        return false;
    }
    return occ(reinterpret_cast<char*>(_block_ptr));
}