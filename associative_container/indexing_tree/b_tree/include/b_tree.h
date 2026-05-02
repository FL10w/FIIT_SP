#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <iterator>
#include <utility>
#include <stdexcept>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <stack>
#include <pp_allocator.h>
#include <associative_container.h>
#include <not_implemented.h>
#include <initializer_list>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare // EBCO
{
public:

    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:

    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    // region comparators declaration

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const;                                                                            // сравнивает два ключа. Вызывает унаследованный compare::operator()(lhs, rhs)
    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const;                                                       // сравнивает две пары по ключу. Нужно для поиска и сортировки внутри узлов

    // endregion comparators declaration


    struct btree_node
    {
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _keys;                                                         // это гибрид std::vector и std::array. Он хранит элементы внутри самого объекта (без динамического выделения памяти), пока их количество не превышает заданную ёмкость.
        boost::container::static_vector<btree_node*, maximum_keys_in_node + 2> _pointers;
        btree_node() noexcept;
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root;
    size_t _size;

    pp_allocator<value_type> get_allocator() const noexcept;                                                                                     // Возвращает копию аллокатора ассоциативном контейнере
    btree_node* create_node();                                                                                                                   // Выделяет память под новый узел через _allocator и возвращает указатель
    void destroy_node(btree_node* node) noexcept;                                                                                                // Освобождает память одного узла
    void clear_node(btree_node* node) noexcept;                                                                                                  // Рекурсивно уничтожает узел и всё его поддерево        
    bool is_leaf(const btree_node* node) const noexcept;                                                                                         // Проверяет, является ли узел листом (нет дочерних указателей)                                                                         
    size_t find_key_index(const btree_node* node, const tkey& key, bool& found) const;                                                           // Бинарный поиск (на самом деле линейный, так как узел маленький) ключа в узле. Возвращает индекс, куда ключ должен быть вставлен.
    void split_child(btree_node* parent, size_t child_index);                                                                                    // Разделяет переполненного i-го ребёнка на два узла, поднимая медианный ключ в родителя
    void insert_non_full(btree_node* node, tree_data_type&& data, btree_node** out_node, size_t* out_index, bool& inserted);                     // Рекурсивно вставляет данные в неполный узел. Вызывает split_child при обнаружении переполнения у ребёнка
    bool erase_internal(btree_node* node, const tkey& key);                                                                                      // Рекурсивно удаляет ключ из поддерева, выполняя необходимые слияния и перераспределения
    void merge_children(btree_node* node, size_t index);                                                                                         // Сливает i-го и (i+1)-го ребёнка с разделяющим ключом родителя в один узел. Используется при удалении
    tree_data_type get_predecessor(btree_node* node, size_t index);                                                                              // Находит предшественника ключа (самый правый ключ в левом поддереве). Нужен при удалении ключа из внутреннего узла
    tree_data_type get_successor(btree_node* node, size_t index);                                                                                // Находит преемника ключа (самый левый ключ в правом поддереве). Тоже для удаления

public:

    // region constructors declaration

    explicit B_tree(const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    explicit B_tree(pp_allocator<value_type> alloc, const compare& comp = compare());

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(iterator begin, iterator end, const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    // endregion constructors declaration

    // region five declaration

    B_tree(const B_tree& other);

    B_tree(B_tree&& other) noexcept;

    B_tree& operator=(const B_tree& other);

    B_tree& operator=(B_tree&& other) noexcept;

    ~B_tree() noexcept;

    // endregion five declaration

    // region iterators declaration

    class btree_iterator;
    class btree_reverse_iterator;
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    class btree_iterator final
    {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        reference operator*() const noexcept;                                                                                                                                   // получить ссылку на пару ключ-значение
        pointer operator->() const noexcept;                                                                                                                                    // Позволяет обращаться к полям пары через ->.

        self& operator++();                                                                                                                                                     // (Префиксный) Перемещает итератор на следующий по порядку ключей элемент и возвращает ссылку на себя.
        self operator++(int);                                                                                                                                                   // (Постфиксный) То же самое, но возвращает копию итератора до перемещения (старое значение). Параметр int — фиктивный, просто чтобы отличить от префиксного.

        self& operator--();                                                                                                                                                     // (Префиксный) Перемещает итератор на предыдущий по порядку ключей элемент.
        self operator--(int);                                                                                                                                                   // (Постфиксный) Перемещает назад, но возвращает старое значение.

        bool operator==(const self& other) const noexcept;                                                                                                                      // Оба "пустые" (стерты, как end()) — тогда они равны друг другу.  Либо оба непустые, находятся в одном и том же узле и на одной позиции.
        bool operator!=(const self& other) const noexcept;                                                                                                                      // Просто отрицание operator==.

        size_t depth() const noexcept;                                                                                                                                          // Возвращает, на каком уровне дерева находится итератор.
        size_t current_node_keys_count() const noexcept;                                                                                                                        // Сколько ключей в том узле, где сейчас находится итератор.
        bool is_terminate_node() const noexcept;                                                                                                                                // Возвращает true, если итератор "пустой" (как end()). Позволяет проверить, валиден ли итератор.
        size_t index() const noexcept;                                                                                                                                          // Возвращает индекс текущего ключа в массиве _keys узла (начинается с 0).

        explicit btree_iterator(const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(), size_t index = 0);

    };

    class btree_const_iterator final
    {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_iterator;
        friend class btree_const_reverse_iterator;

        btree_const_iterator(const btree_iterator& it) noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path = std::stack<std::pair<btree_node* const*, size_t>>(), size_t index = 0);
    };

    class btree_reverse_iterator final
    {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_reverse_iterator;

        friend class B_tree;
        friend class btree_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        btree_reverse_iterator(const btree_iterator& it) noexcept;
        operator btree_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_reverse_iterator(const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(), size_t index = 0);
    };

    class btree_const_reverse_iterator final
    {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_reverse_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_iterator;

        btree_const_reverse_iterator(const btree_reverse_iterator& it) noexcept;
        operator btree_const_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_reverse_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path = std::stack<std::pair<btree_node* const*, size_t>>(), size_t index = 0);
    };

    friend class btree_iterator;
    friend class btree_const_iterator;
    friend class btree_reverse_iterator;
    friend class btree_const_reverse_iterator;

    // endregion iterators declaration

    // region element access declaration

    /*
     * Returns a reference to the mapped value of the element with specified key. If no such element exists, an exception of type std::out_of_range is thrown.
     */
    tvalue& at(const tkey&);
    const tvalue& at(const tkey&) const;

    /*
     * If key not exists, makes default initialization of value
     */
    tvalue& operator[](const tkey& key);
    tvalue& operator[](tkey&& key);

    // endregion element access declaration
    // region iterator begins declaration

    btree_iterator begin();
    btree_iterator end();

    btree_const_iterator begin() const;
    btree_const_iterator end() const;

    btree_const_iterator cbegin() const;
    btree_const_iterator cend() const;

    btree_reverse_iterator rbegin();
    btree_reverse_iterator rend();

    btree_const_reverse_iterator rbegin() const;
    btree_const_reverse_iterator rend() const;

    btree_const_reverse_iterator crbegin() const;
    btree_const_reverse_iterator crend() const;

    // endregion iterator begins declaration

    // region lookup declaration

    size_t size() const noexcept;
    bool empty() const noexcept;

    /*
     * Returns end() if not exist
     */

    btree_iterator find(const tkey& key);
    btree_const_iterator find(const tkey& key) const;

    btree_iterator lower_bound(const tkey& key);
    btree_const_iterator lower_bound(const tkey& key) const;

    btree_iterator upper_bound(const tkey& key);
    btree_const_iterator upper_bound(const tkey& key) const;

    bool contains(const tkey& key) const;

    // endregion lookup declaration

    // region modifiers declaration

    void clear() noexcept;

    /*
     * Does nothing if key exists, delegates to emplace.
     * Second return value is true, when inserted
     */
    std::pair<btree_iterator, bool> insert(const tree_data_type& data);
    std::pair<btree_iterator, bool> insert(tree_data_type&& data);

    template <typename ...Args>
    std::pair<btree_iterator, bool> emplace(Args&&... args);

    /*
     * Updates value if key exists, delegates to emplace.
     */
    btree_iterator insert_or_assign(const tree_data_type& data);
    btree_iterator insert_or_assign(tree_data_type&& data);

    template <typename ...Args>
    btree_iterator emplace_or_assign(Args&&... args);

    /*
     * Return iterator to node next ro removed or end() if key not exists
     */
    btree_iterator erase(btree_iterator pos);
    btree_iterator erase(btree_const_iterator pos);

    btree_iterator erase(btree_iterator beg, btree_iterator en);
    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en);


    btree_iterator erase(const tkey& key);

    // endregion modifiers declaration
};

template<std::input_iterator iterator, comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare = std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
        std::size_t t = 5, typename U>
B_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>()) -> B_tree<typename std::iterator_traits<iterator>::value_type::first_type, typename std::iterator_traits<iterator>::value_type::second_type, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5, typename U>
B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>()) -> B_tree<tkey, tvalue, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_pairs(const B_tree::tree_data_type &lhs,
                                                     const B_tree::tree_data_type &rhs) const
{
    return compare_keys(lhs.first, rhs.first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_keys(const tkey &lhs, const tkey &rhs) const
{
    return compare::operator()(lhs, rhs);
}


template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_node::btree_node() noexcept
{
    _keys.clear();
    _pointers.clear();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
pp_allocator<typename B_tree<tkey, tvalue, compare, t>::value_type> B_tree<tkey, tvalue, compare, t>::get_allocator() const noexcept
{
    return _allocator;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_node* B_tree<tkey, tvalue, compare, t>::create_node()
{
    return _allocator.template new_object<btree_node>();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::destroy_node(btree_node* node) noexcept
{
    if (node == nullptr) {
        return;
    }
    _allocator.template delete_object(node);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::clear_node(btree_node* node) noexcept
{
    if (node == nullptr) {
        return;
    }
    for (auto child : node->_pointers) {
        clear_node(child);
    }
    destroy_node(node);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::is_leaf(const btree_node* node) const noexcept
{
    return node == nullptr || node->_pointers.empty();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::find_key_index(const btree_node* node, const tkey& key, bool& found) const
{
    size_t left = 0;
    size_t right = node->_keys.size();

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (compare_keys(node->_keys[mid].first, key)) {
            left = mid + 1; // идём вправо
        } else {
            right = mid;    // идём влево
        }
    }

    size_t i = left; // Позиция вставки или найденного элемента
    found = (i < node->_keys.size() &&
             !compare_keys(key, node->_keys[i].first) &&
             !compare_keys(node->_keys[i].first, key));

    return i;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::split_child(btree_node* parent, size_t child_index)
{
    btree_node* full = parent->_pointers[child_index];
    btree_node* right = create_node();
    tree_data_type middle = std::move(full->_keys[t]);
    for (size_t i = t + 1; i < full->_keys.size(); ++i) {
        right->_keys.push_back(std::move(full->_keys[i]));
    }
    full->_keys.erase(full->_keys.begin() + static_cast<ptrdiff_t>(t), full->_keys.end());
    if (!full->_pointers.empty()) {
        for (size_t i = t + 1; i < full->_pointers.size(); ++i) {
            right->_pointers.push_back(full->_pointers[i]);
        }
        full->_pointers.erase(full->_pointers.begin() + static_cast<ptrdiff_t>(t + 1), full->_pointers.end());
    }
    parent->_keys.insert(parent->_keys.begin() + static_cast<ptrdiff_t>(child_index), std::move(middle));
    parent->_pointers.insert(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index + 1), right);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::insert_non_full(btree_node* node, tree_data_type&& data, btree_node** out_node, size_t* out_index, bool& inserted)
{
    bool found = false;
    size_t idx = find_key_index(node, data.first, found);
    if (found) {
        *out_node = node;
        *out_index = idx;
        inserted = false;
        return;
    }
    if (is_leaf(node)) {
        node->_keys.insert(node->_keys.begin() + static_cast<ptrdiff_t>(idx), std::move(data));
        *out_node = node;
        *out_index = idx;
        inserted = true;
        return;
    }
    insert_non_full(node->_pointers[idx], std::move(data), out_node, out_index, inserted);
    if (node->_pointers[idx]->_keys.size() == maximum_keys_in_node + 1) {
        split_child(node, idx);
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::tree_data_type B_tree<tkey, tvalue, compare, t>::get_predecessor(btree_node* node, size_t index)
{
    btree_node* cur = node->_pointers[index];
    while (!is_leaf(cur)) {
        cur = cur->_pointers.back();
    }
    return cur->_keys.back();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::tree_data_type B_tree<tkey, tvalue, compare, t>::get_successor(btree_node* node, size_t index)
{
    btree_node* cur = node->_pointers[index + 1];
    while (!is_leaf(cur)) {
        cur = cur->_pointers.front();
    }
    return cur->_keys.front();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::merge_children(btree_node* node, size_t index)
{
    btree_node* left = node->_pointers[index];
    btree_node* right = node->_pointers[index + 1];
    left->_keys.push_back(std::move(node->_keys[index]));
    for (size_t i = 0; i < right->_keys.size(); ++i) {
        left->_keys.push_back(std::move(right->_keys[i]));
    }
    if (!right->_pointers.empty()) {
        for (size_t i = 0; i < right->_pointers.size(); ++i) {
            left->_pointers.push_back(right->_pointers[i]);
        }
    }
    node->_keys.erase(node->_keys.begin() + static_cast<ptrdiff_t>(index));
    node->_pointers.erase(node->_pointers.begin() + static_cast<ptrdiff_t>(index + 1));
    destroy_node(right);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::erase_internal(btree_node* node, const tkey& key)
{
    bool found = false;
    size_t idx = find_key_index(node, key, found);
    if (found) {
        if (is_leaf(node)) {  // Случай 1: Удаление из листа
            node->_keys.erase(node->_keys.begin() + static_cast<ptrdiff_t>(idx));
            --_size;
            return true;
        }
        btree_node* left = node->_pointers[idx];
        btree_node* right = node->_pointers[idx + 1];
        if (left->_keys.size() >= t) { // Случай 2: Удаление из внутреннего узла (замена)
            tree_data_type pred = get_predecessor(node, idx);
            node->_keys[idx] = pred;
            return erase_internal(left, pred.first);
        }
        if (right->_keys.size() >= t) { // 2.2. Замена преемником (правый ребёнок)
            tree_data_type succ = get_successor(node, idx);
            node->_keys[idx] = succ;
            return erase_internal(right, succ.first);
        }
        merge_children(node, idx); // 2.3. Слияние детей (оба на минимуме):
        return erase_internal(left, key);
    }
    if (is_leaf(node)) { // Случай 3: Ключ не найден, спуск вниз
        return false;
    }
    btree_node* child = node->_pointers[idx];
    if (child->_keys.size() == minimum_keys_in_node) { // 3.2. Заимствование у левого соседа
        if (idx > 0 && node->_pointers[idx - 1]->_keys.size() >= t) {
            btree_node* left = node->_pointers[idx - 1];
            child->_keys.insert(child->_keys.begin(), std::move(node->_keys[idx - 1]));
            node->_keys[idx - 1] = std::move(left->_keys.back());
            left->_keys.pop_back();
            if (!left->_pointers.empty()) {
                child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
                left->_pointers.pop_back();
            }
        } else if (idx + 1 < node->_pointers.size() && node->_pointers[idx + 1]->_keys.size() >= t) { // 3.3. Заимствование у правого соседа:
            btree_node* right = node->_pointers[idx + 1];
            child->_keys.push_back(std::move(node->_keys[idx]));
            node->_keys[idx] = std::move(right->_keys.front());
            right->_keys.erase(right->_keys.begin());
            if (!right->_pointers.empty()) {
                child->_pointers.push_back(right->_pointers.front());
                right->_pointers.erase(right->_pointers.begin());
            }
        } else { // 3.4. Слияние с правым соседом:
            if (idx + 1 < node->_pointers.size()) {
                merge_children(node, idx);
            } else { // 3.5. Слияние с левым соседом:
                merge_children(node, idx - 1);
                child = node->_pointers[idx - 1];
            }
        }
    }
    return erase_internal(child, key); // 3.6. Рекурсивный спуск:
}

// region constructors implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        const compare& cmp,
        pp_allocator<value_type> alloc)
{
    compare::operator=(cmp);
    _allocator = alloc;
    _root = nullptr;
    _size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        pp_allocator<value_type> alloc,\
        const compare& comp)
{
    compare::operator=(comp);
    _allocator = alloc;
    _root = nullptr;
    _size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<input_iterator_for_pair<tkey, tvalue> iterator>
B_tree<tkey, tvalue, compare, t>::B_tree(
        iterator begin,
        iterator end,
        const compare& cmp,
        pp_allocator<value_type> alloc)
{
    compare::operator=(cmp);
    _allocator = alloc;
    _root = nullptr;
    _size = 0;
    for (auto it = begin; it != end; ++it) {
        emplace(it->first, it->second);
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        std::initializer_list<std::pair<tkey, tvalue>> data,
        const compare& cmp,
        pp_allocator<value_type> alloc)
{
    compare::operator=(cmp);
    _allocator = alloc;
    _root = nullptr;
    _size = 0;
    for (auto const& item : data) {
        emplace(item.first, item.second);
    }
}

// endregion constructors implementation

// region five implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::~B_tree() noexcept
{
    clear();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(const B_tree& other)
{
    compare::operator=(static_cast<const compare&>(other));
    _allocator = other._allocator.select_on_container_copy_construction();
    _root = nullptr;
    _size = other._size;
    if (other._root == nullptr) {
        return;
    }
    _root = create_node();
    std::stack<std::pair<const btree_node*, btree_node*>> st;
    st.push(std::make_pair(other._root, _root));
    while (!st.empty()) {
        auto current = st.top();
        st.pop();
        const btree_node* src = current.first;
        btree_node* dst = current.second;
        dst->_keys = src->_keys;
        if (!src->_pointers.empty()) {
            for (size_t i = 0; i < src->_pointers.size(); ++i) {
                dst->_pointers.push_back(nullptr);
                dst->_pointers[i] = create_node();
                st.push(std::make_pair(src->_pointers[i], dst->_pointers[i]));
            }
        }
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(const B_tree& other)
{
    if (this == &other) {
        return *this;
    }
    clear();
    compare::operator=(static_cast<const compare&>(other));
    _allocator = other._allocator;
    _root = nullptr;
    _size = other._size;
    if (other._root == nullptr) {
        return *this;
    }
    _root = create_node();
    std::stack<std::pair<const btree_node*, btree_node*>> st;
    st.push(std::make_pair(other._root, _root));
    while (!st.empty()) {
        auto current = st.top();
        st.pop();
        const btree_node* src = current.first;
        btree_node* dst = current.second;
        dst->_keys = src->_keys;
        if (!src->_pointers.empty()) {
            for (size_t i = 0; i < src->_pointers.size(); ++i) {
                dst->_pointers.push_back(nullptr);
                dst->_pointers[i] = create_node();
                st.push(std::make_pair(src->_pointers[i], dst->_pointers[i]));
            }
        }
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(B_tree&& other) noexcept
{
    compare::operator=(std::move(static_cast<compare&>(other)));
    _allocator = std::move(other._allocator);
    _root = other._root;
    _size = other._size;
    other._root = nullptr;
    other._size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(B_tree&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    clear();
    compare::operator=(std::move(static_cast<compare&>(other)));
    _allocator = std::move(other._allocator);
    _root = other._root;
    _size = other._size;
    other._root = nullptr;
    other._size = 0;
    return *this;
}

// endregion five implementation

// region iterators implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_iterator::btree_iterator(
        const std::stack<std::pair<btree_node**, size_t>>& path, size_t index)
{
    _path = path;
    _index = index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator*() const noexcept                                       
{
    btree_node* node = *(_path.top().first);
    return *reinterpret_cast<value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator->() const noexcept
{
    btree_node* node = *(_path.top().first);
    return reinterpret_cast<value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++()
{
    if (_path.empty()) {
        return *this;
    }
    btree_node* node = *(_path.top().first);
    if (!node->_pointers.empty()) {
        size_t child_idx = _index + 1;
        btree_node** child_ptr = &node->_pointers[child_idx];
        _path.push(std::make_pair(child_ptr, child_idx));
        node = *child_ptr;
        while (!node->_pointers.empty()) {
            btree_node** next_ptr = &node->_pointers[0];
            _path.push(std::make_pair(next_ptr, 0));
            node = *next_ptr;
        }
        _index = 0;
        return *this;
    }
    if (_index + 1 < node->_keys.size()) {
        ++_index;
        return *this;
    }
    while (!_path.empty()) {
        size_t child_index = _path.top().second;
        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }
        btree_node* parent = *(_path.top().first);
        if (child_index < parent->_keys.size()) {
            _index = child_index;
            return *this;
        }
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++(int)
{
    self tmp(*this);
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--()
{
    if (_path.empty()) {
        return *this;
    }
    btree_node* node = *(_path.top().first);
    if (!node->_pointers.empty()) {
        size_t child_idx = _index;
        btree_node** child_ptr = &node->_pointers[child_idx];
        _path.push(std::make_pair(child_ptr, child_idx));
        node = *child_ptr;
        while (!node->_pointers.empty()) {
            size_t last = node->_pointers.size() - 1;
            btree_node** next_ptr = &node->_pointers[last];
            _path.push(std::make_pair(next_ptr, last));
            node = *next_ptr;
        }
        _index = node->_keys.size() - 1;
        return *this;
    }
    if (_index > 0) {
        --_index;
        return *this;
    }
    while (!_path.empty()) {
        size_t child_index = _path.top().second;
        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }
        if (child_index > 0) {
            _index = child_index - 1;
            return *this;
        }
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--(int)
{
    self tmp(*this);
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() || other._path.empty()) {
        return _path.empty() && other._path.empty();
    }
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::depth() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return _path.size() - 1;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::current_node_keys_count() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return (*(_path.top().first))->_keys.size();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::is_terminate_node() const noexcept
{
    return _path.empty();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(
        const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index)
{
    _path = path;
    _index = index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(
        const btree_iterator& it) noexcept
{
    if (it._path.empty()) {
        _index = 0;
        return;
    }
    std::stack<std::pair<btree_node**, size_t>> temp = it._path;
    std::stack<std::pair<btree_node* const*, size_t>> rebuilt;
    std::vector<std::pair<btree_node* const*, size_t>> order;
    while (!temp.empty()) {
        order.push_back(std::make_pair(const_cast<btree_node* const*>(temp.top().first), temp.top().second));
        temp.pop();
    }
    for (auto itv = order.rbegin(); itv != order.rend(); ++itv) {
        rebuilt.push(*itv);
    }
    _path = rebuilt;
    _index = it._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator*() const noexcept
{
    btree_node* node = *(_path.top().first);
    return *reinterpret_cast<const value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator->() const noexcept
{
    btree_node* node = *(_path.top().first);
    return reinterpret_cast<const value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++()
{
    if (_path.empty()) {
        return *this;
    }
    btree_node* node = *(_path.top().first);
    if (!node->_pointers.empty()) {
        size_t child_idx = _index + 1;
        btree_node* const* child_ptr = &node->_pointers[child_idx];
        _path.push(std::make_pair(child_ptr, child_idx));
        node = *child_ptr;
        while (!node->_pointers.empty()) {
            btree_node* const* next_ptr = &node->_pointers[0];
            _path.push(std::make_pair(next_ptr, 0));
            node = *next_ptr;
        }
        _index = 0;
        return *this;
    }
    if (_index + 1 < node->_keys.size()) {
        ++_index;
        return *this;
    }
    while (!_path.empty()) {
        size_t child_index = _path.top().second;
        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }
        btree_node* parent = *(_path.top().first);
        if (child_index < parent->_keys.size()) {
            _index = child_index;
            return *this;
        }
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++(int)
{
    self tmp(*this);
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--()
{
    if (_path.empty()) {
        return *this;
    }
    btree_node* node = *(_path.top().first);
    if (!node->_pointers.empty()) {
        size_t child_idx = _index;
        btree_node* const* child_ptr = &node->_pointers[child_idx];
        _path.push(std::make_pair(child_ptr, child_idx));
        node = *child_ptr;
        while (!node->_pointers.empty()) {
            size_t last = node->_pointers.size() - 1;
            btree_node* const* next_ptr = &node->_pointers[last];
            _path.push(std::make_pair(next_ptr, last));
            node = *next_ptr;
        }
        _index = node->_keys.size() - 1;
        return *this;
    }
    if (_index > 0) {
        --_index;
        return *this;
    }
    while (!_path.empty()) {
        size_t child_index = _path.top().second;
        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }
        if (child_index > 0) {
            _index = child_index - 1;
            return *this;
        }
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--(int)
{
    self tmp(*this);
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() || other._path.empty()) {
        return _path.empty() && other._path.empty();
    }
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::depth() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return _path.size() - 1;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::current_node_keys_count() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return (*(_path.top().first))->_keys.size();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::is_terminate_node() const noexcept
{
    return _path.empty();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(
        const std::stack<std::pair<btree_node**, size_t>>& path, size_t index)
{
    _path = path;
    _index = index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(
        const btree_iterator& it) noexcept
{
    _path = it._path;
    _index = it._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator B_tree<tkey, tvalue, compare, t>::btree_iterator() const noexcept
{
    return btree_iterator(_path, _index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator*() const noexcept
{
    btree_node* node = *(_path.top().first);
    return *reinterpret_cast<value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator->() const noexcept
{
    btree_node* node = *(_path.top().first);
    return reinterpret_cast<value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++()
{
    btree_iterator it = static_cast<btree_iterator>(*this);
    --it;
    _path = it._path;
    _index = it._index;
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++(int)
{
    self tmp(*this);
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--()
{
    btree_iterator it = static_cast<btree_iterator>(*this);
    ++it;
    _path = it._path;
    _index = it._index;
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--(int)
{
    self tmp(*this);
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() || other._path.empty()) {
        return _path.empty() && other._path.empty();
    }
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::depth() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return _path.size() - 1;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::current_node_keys_count() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return (*(_path.top().first))->_keys.size();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::is_terminate_node() const noexcept
{
    return _path.empty();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
        const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index)
{
    _path = path;
    _index = index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
        const btree_reverse_iterator& it) noexcept
{
    if (it._path.empty()) {
        _index = 0;
        return;
    }
    std::stack<std::pair<btree_node**, size_t>> temp = it._path;
    std::stack<std::pair<btree_node* const*, size_t>> rebuilt;
    std::vector<std::pair<btree_node* const*, size_t>> order;
    while (!temp.empty()) {
        order.push_back(std::make_pair(const_cast<btree_node* const*>(temp.top().first), temp.top().second));
        temp.pop();
    }
    for (auto itv = order.rbegin(); itv != order.rend(); ++itv) {
        rebuilt.push(*itv);
    }
    _path = rebuilt;
    _index = it._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator B_tree<tkey, tvalue, compare, t>::btree_const_iterator() const noexcept
{
    return btree_const_iterator(_path, _index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator*() const noexcept
{
    btree_node* node = *(_path.top().first);
    return *reinterpret_cast<const value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator->() const noexcept
{
    btree_node* node = *(_path.top().first);
    return reinterpret_cast<const value_type*>(&node->_keys[_index]);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++()
{
    btree_const_iterator it = static_cast<btree_const_iterator>(*this);
    --it;
    _path = it._path;
    _index = it._index;
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++(int)
{
    self tmp(*this);
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--()
{
    btree_const_iterator it = static_cast<btree_const_iterator>(*this);
    ++it;
    _path = it._path;
    _index = it._index;
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--(int)
{
    self tmp(*this);
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() || other._path.empty()) {
        return _path.empty() && other._path.empty();
    }
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::depth() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return _path.size() - 1;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::current_node_keys_count() const noexcept
{
    if (_path.empty()) {
        return 0;
    }
    return (*(_path.top().first))->_keys.size();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::is_terminate_node() const noexcept
{
    return _path.empty();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::index() const noexcept
{
    return _index;
}

// endregion iterators implementation

// region element access implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key)
{
    auto it = find(key);
    if (it == end()) {
        throw std::out_of_range("B_tree::at");
    }
    return it->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
const tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key) const
{
    auto it = find(key);
    if (it == end()) {
        throw std::out_of_range("B_tree::at");
    }
    return it->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](const tkey& key)
{
    auto result = emplace(key, tvalue());
    return result.first->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](tkey&& key)
{
    auto result = emplace(std::move(key), tvalue());
    return result.first->second;
}

// endregion element access implementation

// region iterator begins implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::begin()
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node**, size_t>> path;
    btree_node** cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (!is_leaf(node)) {
        btree_node** next_ptr = &node->_pointers[0];
        path.push(std::make_pair(next_ptr, 0));
        node = *next_ptr;
    }
    return btree_iterator(path, 0);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::end()
{
    return btree_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::begin() const
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node* const*, size_t>> path;
    btree_node* const* cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (!is_leaf(node)) {
        btree_node* const* next_ptr = &node->_pointers[0];
        path.push(std::make_pair(next_ptr, 0));
        node = *next_ptr;
    }
    return btree_const_iterator(path, 0);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::end() const
{
    return btree_const_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cbegin() const
{
    return begin();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cend() const
{
    return end();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin()
{
    if (_root == nullptr) {
        return rend();
    }
    std::stack<std::pair<btree_node**, size_t>> path;
    btree_node** cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (!is_leaf(node)) {
        size_t last = node->_pointers.size() - 1;
        btree_node** next_ptr = &node->_pointers[last];
        path.push(std::make_pair(next_ptr, last));
        node = *next_ptr;
    }
    return btree_reverse_iterator(path, node->_keys.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend()
{
    return btree_reverse_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin() const
{
    if (_root == nullptr) {
        return rend();
    }
    std::stack<std::pair<btree_node* const*, size_t>> path;
    btree_node* const* cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (!is_leaf(node)) {
        size_t last = node->_pointers.size() - 1;
        btree_node* const* next_ptr = &node->_pointers[last];
        path.push(std::make_pair(next_ptr, last));
        node = *next_ptr;
    }
    return btree_const_reverse_iterator(path, node->_keys.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend() const
{
    return btree_const_reverse_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crbegin() const
{
    return rbegin();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crend() const
{
    return rend();
}

// endregion iterator begins implementation

// region lookup implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::size() const noexcept
{
    return _size;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::empty() const noexcept
{
    return _size == 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::find(const tkey& key)
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node**, size_t>> path;
    btree_node** cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        bool found = false;
        size_t idx = find_key_index(node, key, found);
        if (found) {
            return btree_iterator(path, idx);
        }
        if (is_leaf(node)) {
            return end();
        }
        btree_node** child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::find(const tkey& key) const
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node* const*, size_t>> path;
    btree_node* const* cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        bool found = false;
        size_t idx = find_key_index(node, key, found);
        if (found) {
            return btree_const_iterator(path, idx);
        }
        if (is_leaf(node)) {
            return end();
        }
        btree_node* const* child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(const tkey& key)
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node**, size_t>> path;
    std::stack<std::pair<btree_node**, size_t>> candidate;
    size_t candidate_index = 0;
    bool has_candidate = false;
    btree_node** cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        bool found = false;
        size_t idx = find_key_index(node, key, found);
        if (idx < node->_keys.size()) {
            candidate = path;
            candidate_index = idx;
            has_candidate = true;
        }
        if (is_leaf(node)) {
            break;
        }
        btree_node** child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
    if (!has_candidate) {
        return end();
    }
    return btree_iterator(candidate, candidate_index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(const tkey& key) const
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node* const*, size_t>> path;
    std::stack<std::pair<btree_node* const*, size_t>> candidate;
    size_t candidate_index = 0;
    bool has_candidate = false;
    btree_node* const* cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        bool found = false;
        size_t idx = find_key_index(node, key, found);
        if (idx < node->_keys.size()) {
            candidate = path;
            candidate_index = idx;
            has_candidate = true;
        }
        if (is_leaf(node)) {
            break;
        }
        btree_node* const* child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
    if (!has_candidate) {
        return end();
    }
    return btree_const_iterator(candidate, candidate_index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key)
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node**, size_t>> path;
    std::stack<std::pair<btree_node**, size_t>> candidate;
    size_t candidate_index = 0;
    bool has_candidate = false;
    btree_node** cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        size_t idx = 0;
        while (idx < node->_keys.size() && !compare_keys(key, node->_keys[idx].first)) {
            ++idx;
        }
        if (idx < node->_keys.size()) {
            candidate = path;
            candidate_index = idx;
            has_candidate = true;
        }
        if (is_leaf(node)) {
            break;
        }
        btree_node** child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
    if (!has_candidate) {
        return end();
    }
    return btree_iterator(candidate, candidate_index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key) const
{
    if (_root == nullptr) {
        return end();
    }
    std::stack<std::pair<btree_node* const*, size_t>> path;
    std::stack<std::pair<btree_node* const*, size_t>> candidate;
    size_t candidate_index = 0;
    bool has_candidate = false;
    btree_node* const* cur_ptr = &_root;
    path.push(std::make_pair(cur_ptr, 0));
    btree_node* node = *cur_ptr;
    while (true) {
        size_t idx = 0;
        while (idx < node->_keys.size() && !compare_keys(key, node->_keys[idx].first)) {
            ++idx;
        }
        if (idx < node->_keys.size()) {
            candidate = path;
            candidate_index = idx;
            has_candidate = true;
        }
        if (is_leaf(node)) {
            break;
        }
        btree_node* const* child_ptr = &node->_pointers[idx];
        path.push(std::make_pair(child_ptr, idx));
        node = *child_ptr;
    }
    if (!has_candidate) {
        return end();
    }
    return btree_const_iterator(candidate, candidate_index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::contains(const tkey& key) const
{
    return find(key) != end();
}

// endregion lookup implementation

// region modifiers implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::clear() noexcept
{
    clear_node(_root);
    _root = nullptr;
    _size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::insert(const tree_data_type& data)
{
    tree_data_type copy = data;
    return insert(std::move(copy));
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::insert(tree_data_type&& data)
{
    return emplace(std::move(data.first), std::move(data.second));
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<typename... Args>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::emplace(Args&&... args)
{
    tree_data_type data(std::forward<Args>(args)...);
    tkey key_copy = data.first;
    auto existing = find(key_copy);
    if (existing != end()) {
        return std::make_pair(existing, false);
    }
    if (_root == nullptr) {
        _root = create_node();
        _root->_keys.push_back(std::move(data));
        ++_size;
        std::stack<std::pair<btree_node**, size_t>> path;
        path.push(std::make_pair(&_root, 0));
        return std::make_pair(btree_iterator(path, 0), true);
    }
    btree_node* out_node = nullptr;
    size_t out_index = 0;
    bool inserted = false;
    insert_non_full(_root, std::move(data), &out_node, &out_index, inserted);
    if (_root->_keys.size() == maximum_keys_in_node + 1) {
        btree_node* old_root = _root;
        _root = create_node();
        _root->_pointers.push_back(old_root);
        split_child(_root, 0);
    }
    if (inserted) {
        ++_size;
    }
    return std::make_pair(find(key_copy), inserted);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::insert_or_assign(const tree_data_type& data)
{
    auto result = emplace(data.first, data.second);
    if (!result.second) {
        result.first->second = data.second;
    }
    return result.first;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::insert_or_assign(tree_data_type&& data)
{
    auto result = emplace(data.first, std::move(data.second));
    if (!result.second) {
        result.first->second = std::move(data.second);
    }
    return result.first;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<typename... Args>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::emplace_or_assign(Args&&... args)
{
    tree_data_type data(std::forward<Args>(args)...);
    auto result = emplace(data.first, std::move(data.second));
    if (!result.second) {
        result.first->second = std::move(data.second);
    }
    return result.first;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_iterator pos)
{
    if (pos == end()) {
        return end();
    }
    return erase(pos->first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_const_iterator pos)
{
    if (pos == end()) {
        return end();
    }
    return erase(pos->first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_iterator beg, btree_iterator en)
{
    auto it = beg;
    while (it != en) {
        it = erase(it);
    }
    return it;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_const_iterator beg, btree_const_iterator en)
{
    auto it = beg;
    while (it != en) {
        it = erase(it);
    }
    return it;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(const tkey& key)
{
    if (_root == nullptr) {
        return end();
    }
    bool removed = erase_internal(_root, key);
    if (_root != nullptr && _root->_keys.empty()) {
        if (!_root->_pointers.empty()) {
            btree_node* old_root = _root;
            _root = _root->_pointers[0];
            destroy_node(old_root);
        } else {
            destroy_node(_root);
            _root = nullptr;
        }
    }
    if (!removed) {
        return end();
    }
    if (_root == nullptr) {
        return end();
    }
    return lower_bound(key);
}

// endregion modifiers implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool compare_pairs(const typename B_tree<tkey, tvalue, compare, t>::tree_data_type &lhs,
                   const typename B_tree<tkey, tvalue, compare, t>::tree_data_type &rhs)
{
    return compare()(lhs.first, rhs.first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool compare_keys(const tkey &lhs, const tkey &rhs)
{
    return compare()(lhs, rhs);
}


#endif