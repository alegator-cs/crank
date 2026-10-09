#include "ast_index.hpp"

#include <cstddef>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, lookup_cases = 0, rehash_callbacks = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

using Key = std::pair<std::size_t, std::size_t>;
struct Record {
    Key key;
    bool indexed = false;
};

enum class HashMode { Zero, Sentinel, Distributed };

std::size_t hash_key(const Key& key, HashMode mode) {
    if (mode == HashMode::Zero) return 0;
    if (mode == HashMode::Sentinel) return crank::no_id;
    return (key.first * std::size_t{0x9e3779b9U}) ^
           (key.second * std::size_t{0x85ebca6bU});
}

void compare_to_external_key_oracle(HashMode mode) {
    crank::detail::IdIndex index;
    // Leave this vector unreserved so its own backing storage moves as the
    // independently growing index rehashes. Stored IDs must survive both.
    std::vector<Record> arena;
    std::map<Key, std::size_t> oracle;
    std::size_t arena_capacity_changes = 0;
    std::size_t callbacks = 0;
    const auto hash_of = [&](std::size_t id) {
        ++callbacks;
        require(id != crank::no_id && id < arena.size() && arena[id].indexed,
                "rehash callback receives an actual indexed arena ID");
        return hash_key(arena[id].key, mode);
    };
    const auto lookup = [&](const Key& key) {
        ++lookup_cases;
        return index.find(hash_key(key, mode), [&](std::size_t id) {
            require(id != crank::no_id && id < arena.size() && arena[id].indexed,
                    "equality callback receives an actual indexed arena ID");
            return arena[id].key == key;
        });
    };
    require(index.empty() && index.size() == 0, "new index is empty");
    std::size_t empty_callbacks = 0;
    require(index.find(crank::no_id, [&](std::size_t) {
                ++empty_callbacks;
                return true;
            }) == crank::no_id && empty_callbacks == 0,
            "empty lookup returns the missing sentinel without consulting an arena");

    const auto append = [&](Record record) {
        const auto capacity = arena.capacity();
        arena.push_back(std::move(record));
        arena_capacity_changes += arena.capacity() != capacity;
    };
    const auto insert = [&](Key key) {
        require(!oracle.contains(key), "fixture inserts each key exactly once");
        require(lookup(key) == crank::no_id, "new key is absent before insertion");
        // Dummy arena records are intentionally not indexed. Their IDs are
        // interspersed with valid records, so keys and IDs cannot be conflated.
        append({{crank::no_id, crank::no_id}, false});
        const auto id = arena.size();
        append({key, true});
        require(id != crank::no_id, "fixture never inserts the reserved ID");
        index.insert(hash_key(key, mode), id, hash_of);
        oracle.emplace(key, id);
        require(lookup(key) == id, "new key immediately resolves to its arena ID");
        require(index.size() == oracle.size() && !index.empty(),
                "index cardinality agrees with the independent key map");
    };
    const auto verify_all = [&]() {
        for (const auto& [key, id] : oracle)
            require(lookup(key) == id, "lookup survives arena growth, collisions, and index rehash");
        require(lookup({crank::no_id, 31337}) == crank::no_id,
                "absent key stops safely within a collision cluster");
        require(lookup({777, 888}) == crank::no_id,
                "unrelated absent key remains absent after rehash");
    };

    // The sentinel is reserved for record IDs, not for key fields or hashes.
    // These are valid keys and must remain distinguishable from empty slots.
    insert({crank::no_id, 0});
    insert({0, crank::no_id});
    insert({0, 0});
    verify_all();
    std::size_t next_key = 0;
    for (const std::size_t batch : {1U, 3U, 8U, 17U, 33U, 65U, 129U, 257U}) {
        for (std::size_t item = 0; item < batch; ++item) {
            // Every adjacent pair shares its first component; equality must
            // inspect the complete external key rather than one projected field.
            const Key key{1000 + 97 * (next_key / 2), next_key % 2};
            ++next_key;
            insert(key);
            if (item % 7 == 0) {
                const auto first = oracle.begin();
                require(lookup(first->first) == first->second,
                        "an old key remains findable between batched insertions");
            }
        }
        verify_all();
    }
    require(arena_capacity_changes > 1, "fixture exercised external arena reallocations");
    require(callbacks > 0, "fixture exercised index rehash callbacks");
    rehash_callbacks += callbacks;
    std::cout << (mode == HashMode::Zero ? "identical zero hashes" :
                  mode == HashMode::Sentinel ? "identical sentinel hashes" : "distributed hashes")
              << ": " << oracle.size() << " distinct keys retained across "
              << arena_capacity_changes << " arena capacity changes\n";
}
} // namespace

int main() {
    try {
        compare_to_external_key_oracle(HashMode::Zero);
        compare_to_external_key_oracle(HashMode::Sentinel);
        compare_to_external_key_oracle(HashMode::Distributed);
        std::cout << "ID index: " << checks << " checks passed; " << lookup_cases
                  << " lookups; " << rehash_callbacks << " rehash callbacks.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ID index failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
