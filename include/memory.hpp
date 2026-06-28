#pragma once

#include "common.hpp"
#include <cstdint>

struct sqlite3;

namespace nexus {

struct MemoryHit {
    int64_t id;
    std::string text;
    double score;
};

class Memory {
public:
    explicit Memory(sqlite3* db) : db_(db) {}

    void init();
    int64_t add(const std::string& text);
    std::vector<MemoryHit> search(const std::string& query, int top_k);
    std::vector<std::pair<int64_t, std::string>> list(int limit);
    int count();
    void clear();

private:
    sqlite3* db_;
};

}
