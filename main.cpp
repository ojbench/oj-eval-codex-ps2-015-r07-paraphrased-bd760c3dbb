#include <bits/stdc++.h>

using namespace std;

namespace {

constexpr uint32_t kBucketCount = 131072;
constexpr uint32_t kMaxKeys = 100000;
constexpr uint32_t kMaxValues = 100000;
constexpr char kMagic[8] = {'P', 'S', '2', 'D', 'B', '1', '\0', '\0'};
constexpr const char kDbPath[] = "problem015.db";

#pragma pack(push, 1)
struct Header {
    char magic[8];
    uint32_t version;
    uint32_t bucket_count;
    uint32_t max_keys;
    uint32_t max_values;
    uint32_t key_free_top;
    uint32_t value_free_top;
};

struct KeyRecord {
    uint8_t len;
    int32_t root;
    int32_t next;
    char key[64];
};

struct ValueRecord {
    int32_t value;
    int32_t left;
    int32_t right;
    uint32_t priority;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 32);
static_assert(sizeof(KeyRecord) == 73);
static_assert(sizeof(ValueRecord) == 16);

class Database {
public:
    Database() {
        file_ = fopen(kDbPath, "r+b");
        if (!file_) {
            file_ = fopen(kDbPath, "w+b");
            if (!file_) {
                throw runtime_error("cannot open database file");
            }
            init_file();
        } else {
            if (fread(&header_, sizeof(header_), 1, file_) != 1 || memcmp(header_.magic, kMagic, sizeof(kMagic)) != 0) {
                fclose(file_);
                file_ = fopen(kDbPath, "w+b");
                if (!file_) {
                    throw runtime_error("cannot recreate database file");
                }
                init_file();
            } else {
                load_metadata();
            }
        }
    }

    ~Database() {
        if (file_) {
            flush_metadata();
            fclose(file_);
        }
    }

    void insert(const string& key, int value) {
        const uint32_t bucket = hash_key(key);
        int previous = -1;
        const int key_id = find_key(key, bucket, previous);
        if (key_id == -1) {
            const int new_key_id = alloc_key();
            KeyRecord record{};
            record.len = static_cast<uint8_t>(key.size());
            record.root = -1;
            record.next = buckets_[bucket];
            memcpy(record.key, key.data(), key.size());
            write_key(new_key_id, record);
            buckets_[bucket] = new_key_id;
            write_bucket(bucket);

            record.root = alloc_value_node(value);
            write_key(new_key_id, record);
            return;
        }

        KeyRecord record = read_key(key_id);
        const int new_node = alloc_value_node(value);
        auto [left_tree, ge_tree] = split_less(record.root, value);
        auto [equal_tree, right_tree] = split_less_equal(ge_tree, value);
        if (equal_tree != -1) {
            record.root = merge(merge(left_tree, equal_tree), right_tree);
            free_value(new_node);
        } else {
            record.root = merge(merge(left_tree, new_node), right_tree);
        }
        write_key(key_id, record);
    }

    void erase(const string& key, int value) {
        const uint32_t bucket = hash_key(key);
        int previous = -1;
        const int key_id = find_key(key, bucket, previous);
        if (key_id == -1) {
            return;
        }

        KeyRecord record = read_key(key_id);
        auto [less_tree, ge_tree] = split_less(record.root, value);
        auto [equal_tree, greater_tree] = split_less_equal(ge_tree, value);
        if (equal_tree != -1) {
            free_value_tree(equal_tree);
        }
        record.root = merge(less_tree, greater_tree);
        if (record.root == -1) {
            unlink_key(bucket, previous, record.next);
            free_key(key_id);
        } else {
            write_key(key_id, record);
        }
    }

    void find(const string& key) {
        const uint32_t bucket = hash_key(key);
        int previous = -1;
        const int key_id = find_key(key, bucket, previous);
        if (key_id == -1) {
            cout << "null\n";
            return;
        }

        const KeyRecord record = read_key(key_id);
        if (record.root == -1) {
            cout << "null\n";
            return;
        }

        bool first = true;
        inorder_print(record.root, first);
        cout << '\n';
    }

private:
    FILE* file_ = nullptr;
    Header header_{};
    vector<int32_t> buckets_;
    vector<int32_t> key_free_stack_;
    vector<int32_t> value_free_stack_;
    uint32_t rng_ = 2463534242u;

    static constexpr size_t buckets_offset() { return sizeof(Header); }
    static constexpr size_t key_free_offset() { return buckets_offset() + kBucketCount * sizeof(int32_t); }
    static constexpr size_t value_free_offset() { return key_free_offset() + kMaxKeys * sizeof(int32_t); }
    static constexpr size_t key_records_offset() { return value_free_offset() + kMaxValues * sizeof(int32_t); }
    static constexpr size_t value_records_offset() { return key_records_offset() + kMaxKeys * sizeof(KeyRecord); }

    static uint32_t hash_key(const string& key) {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char ch : key) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        return static_cast<uint32_t>(hash & (kBucketCount - 1));
    }

    void init_file() {
        memset(&header_, 0, sizeof(header_));
        memcpy(header_.magic, kMagic, sizeof(kMagic));
        header_.version = 1;
        header_.bucket_count = kBucketCount;
        header_.max_keys = kMaxKeys;
        header_.max_values = kMaxValues;
        header_.key_free_top = kMaxKeys;
        header_.value_free_top = kMaxValues;

        buckets_.assign(kBucketCount, -1);
        key_free_stack_.resize(kMaxKeys);
        value_free_stack_.resize(kMaxValues);
        for (uint32_t i = 0; i < kMaxKeys; ++i) {
            key_free_stack_[i] = static_cast<int32_t>(i);
        }
        for (uint32_t i = 0; i < kMaxValues; ++i) {
            value_free_stack_[i] = static_cast<int32_t>(i);
        }
        flush_metadata();
    }

    void load_metadata() {
        buckets_.resize(kBucketCount);
        key_free_stack_.resize(kMaxKeys);
        value_free_stack_.resize(kMaxValues);

        fseek(file_, static_cast<long>(buckets_offset()), SEEK_SET);
        fread(buckets_.data(), sizeof(int32_t), kBucketCount, file_);
        fread(key_free_stack_.data(), sizeof(int32_t), kMaxKeys, file_);
        fread(value_free_stack_.data(), sizeof(int32_t), kMaxValues, file_);
    }

    void flush_metadata() {
        fseek(file_, 0, SEEK_SET);
        fwrite(&header_, sizeof(header_), 1, file_);
        fwrite(buckets_.data(), sizeof(int32_t), kBucketCount, file_);
        fwrite(key_free_stack_.data(), sizeof(int32_t), kMaxKeys, file_);
        fwrite(value_free_stack_.data(), sizeof(int32_t), kMaxValues, file_);
    }

    void write_bucket(uint32_t bucket) {
        fseek(file_, static_cast<long>(buckets_offset() + bucket * sizeof(int32_t)), SEEK_SET);
        fwrite(&buckets_[bucket], sizeof(int32_t), 1, file_);
    }

    int alloc_key() {
        if (header_.key_free_top == 0) {
            throw runtime_error("key storage exhausted");
        }
        const int32_t id = key_free_stack_[--header_.key_free_top];
        write_header();
        return id;
    }

    void free_key(int id) {
        key_free_stack_[header_.key_free_top++] = id;
        write_header();
        write_key_free_entry(header_.key_free_top - 1);
    }

    int alloc_value_node(int value) {
        if (header_.value_free_top == 0) {
            throw runtime_error("value storage exhausted");
        }
        const int32_t id = value_free_stack_[--header_.value_free_top];
        write_header();
        ValueRecord record{};
        record.value = value;
        record.left = -1;
        record.right = -1;
        record.priority = next_priority();
        write_value(id, record);
        return id;
    }

    void free_value(int id) {
        value_free_stack_[header_.value_free_top++] = id;
        write_header();
        write_value_free_entry(header_.value_free_top - 1);
    }

    void write_header() {
        fseek(file_, 0, SEEK_SET);
        fwrite(&header_, sizeof(header_), 1, file_);
    }

    void write_key_free_entry(uint32_t index) {
        fseek(file_, static_cast<long>(key_free_offset() + index * sizeof(int32_t)), SEEK_SET);
        fwrite(&key_free_stack_[index], sizeof(int32_t), 1, file_);
    }

    void write_value_free_entry(uint32_t index) {
        fseek(file_, static_cast<long>(value_free_offset() + index * sizeof(int32_t)), SEEK_SET);
        fwrite(&value_free_stack_[index], sizeof(int32_t), 1, file_);
    }

    KeyRecord read_key(int id) {
        KeyRecord record{};
        fseek(file_, static_cast<long>(key_records_offset() + static_cast<size_t>(id) * sizeof(KeyRecord)), SEEK_SET);
        fread(&record, sizeof(record), 1, file_);
        return record;
    }

    void write_key(int id, const KeyRecord& record) {
        fseek(file_, static_cast<long>(key_records_offset() + static_cast<size_t>(id) * sizeof(KeyRecord)), SEEK_SET);
        fwrite(&record, sizeof(record), 1, file_);
    }

    ValueRecord read_value(int id) {
        ValueRecord record{};
        fseek(file_, static_cast<long>(value_records_offset() + static_cast<size_t>(id) * sizeof(ValueRecord)), SEEK_SET);
        fread(&record, sizeof(record), 1, file_);
        return record;
    }

    void write_value(int id, const ValueRecord& record) {
        fseek(file_, static_cast<long>(value_records_offset() + static_cast<size_t>(id) * sizeof(ValueRecord)), SEEK_SET);
        fwrite(&record, sizeof(record), 1, file_);
    }

    int find_key(const string& key, uint32_t bucket, int& previous) {
        previous = -1;
        int current = buckets_[bucket];
        while (current != -1) {
            KeyRecord record = read_key(current);
            if (record.len == key.size() && memcmp(record.key, key.data(), record.len) == 0) {
                return current;
            }
            previous = current;
            current = record.next;
        }
        return -1;
    }

    void unlink_key(uint32_t bucket, int previous, int next) {
        if (previous == -1) {
            buckets_[bucket] = next;
            write_bucket(bucket);
            return;
        }
        KeyRecord record = read_key(previous);
        record.next = next;
        write_key(previous, record);
    }

    uint32_t next_priority() {
        uint32_t x = rng_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng_ = x;
        return x;
    }

    pair<int, int> split_less(int root, int value) {
        if (root == -1) {
            return {-1, -1};
        }
        ValueRecord node = read_value(root);
        if (node.value < value) {
            auto [left_subtree, right_subtree] = split_less(node.right, value);
            node.right = left_subtree;
            write_value(root, node);
            return {root, right_subtree};
        }
        auto [left_subtree, right_subtree] = split_less(node.left, value);
        node.left = right_subtree;
        write_value(root, node);
        return {left_subtree, root};
    }

    pair<int, int> split_less_equal(int root, int value) {
        if (root == -1) {
            return {-1, -1};
        }
        ValueRecord node = read_value(root);
        if (node.value <= value) {
            auto [left_subtree, right_subtree] = split_less_equal(node.right, value);
            node.right = left_subtree;
            write_value(root, node);
            return {root, right_subtree};
        }
        auto [left_subtree, right_subtree] = split_less_equal(node.left, value);
        node.left = right_subtree;
        write_value(root, node);
        return {left_subtree, root};
    }

    int merge(int left_root, int right_root) {
        if (left_root == -1) {
            return right_root;
        }
        if (right_root == -1) {
            return left_root;
        }
        ValueRecord left_node = read_value(left_root);
        ValueRecord right_node = read_value(right_root);
        if (left_node.priority < right_node.priority) {
            left_node.right = merge(left_node.right, right_root);
            write_value(left_root, left_node);
            return left_root;
        }
        right_node.left = merge(left_root, right_node.left);
        write_value(right_root, right_node);
        return right_root;
    }

    void free_value_tree(int root) {
        if (root == -1) {
            return;
        }
        ValueRecord node = read_value(root);
        free_value_tree(node.left);
        free_value_tree(node.right);
        free_value(root);
    }

    void inorder_print(int root, bool& first) {
        if (root == -1) {
            return;
        }
        ValueRecord node = read_value(root);
        inorder_print(node.left, first);
        if (!first) {
            cout << ' ';
        }
        first = false;
        cout << node.value;
        inorder_print(node.right, first);
    }
};

} // namespace

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    int n;
    if (!(cin >> n)) {
        return 0;
    }

    Database db;
    string command;
    string key;
    for (int i = 0; i < n; ++i) {
        cin >> command;
        if (command == "insert") {
            int value;
            cin >> key >> value;
            db.insert(key, value);
        } else if (command == "delete") {
            int value;
            cin >> key >> value;
            db.erase(key, value);
        } else if (command == "find") {
            cin >> key;
            db.find(key);
        }
    }
    return 0;
}
