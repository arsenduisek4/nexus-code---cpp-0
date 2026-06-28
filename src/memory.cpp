#include "memory.hpp"
#include "util.hpp"

#include <sqlite3.h>
#include <cmath>
#include <unordered_map>
#include <cctype>
#include <algorithm>

namespace nexus {

// убогий bag-of-words «эмбеддинг»: бьём текст на токены и считаем частоты.
// никаких нейронок, чисто пересечение слов. для «вспомнить факт по ключевику» хватает,
// но семантику (синонимы, перефраз) оно нихуя не ловит — это честный технический долг
static std::unordered_map<std::string, double> vectorize(const std::string& text) {
    std::unordered_map<std::string, double> v;
    std::string token;
    auto flush = [&]() {
        if (token.size() >= 2) v[token] += 1.0;   // >=2 bytes: one Cyrillic char or two ASCII
        token.clear();
    };
    for (unsigned char c : text) {
        bool word = std::isalnum(c) || c >= 0x80;  // keep UTF-8 (Cyrillic etc.) bytes as word chars
        if (word) {
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            token += static_cast<char>(c);
        } else {
            flush();
        }
    }
    flush();
    return v;
}

static double cosine(const std::unordered_map<std::string, double>& a,
                     const std::unordered_map<std::string, double>& b) {
    double dot = 0, na = 0, nb = 0;
    for (auto& [k, va] : a) {
        na += va * va;
        auto it = b.find(k);
        if (it != b.end()) dot += va * it->second;
    }
    for (auto& [k, vb] : b) nb += vb * vb;
    if (na == 0 || nb == 0) return 0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

void Memory::init() {
    char* err = nullptr;
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS memory("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT, ts TEXT);",
        nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

int64_t Memory::add(const std::string& text) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO memory(text,ts) VALUES(?,?);", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = util::now_iso();
    sqlite3_bind_text(st, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db_);
}

std::vector<MemoryHit> Memory::search(const std::string& query, int top_k) {
    auto qv = vectorize(query);
    std::vector<MemoryHit> hits;
    sqlite3_stmt* st = nullptr;
    // да, тащим ВСЮ память в память (lol) и считаем косинус по каждой записи руками.
    // O(n) на каждый поиск — насрать, пока записей сотни. на десятках тысяч надо будет
    // нормальный векторный индекс, но это проблема будущего меня
    sqlite3_prepare_v2(db_, "SELECT id,text FROM memory;", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(st, 0);
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        std::string text = t ? t : "";
        double score = cosine(qv, vectorize(text));
        if (score > 0.0) hits.push_back({id, text, score});
    }
    sqlite3_finalize(st);
    std::sort(hits.begin(), hits.end(),
              [](const MemoryHit& a, const MemoryHit& b) { return a.score > b.score; });
    if ((int)hits.size() > top_k) hits.resize(top_k);
    return hits;
}

std::vector<std::pair<int64_t, std::string>> Memory::list(int limit) {
    std::vector<std::pair<int64_t, std::string>> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id,text FROM memory ORDER BY id DESC LIMIT ?;",
                       -1, &st, nullptr);
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(st, 0);
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        out.emplace_back(id, t ? t : "");
    }
    sqlite3_finalize(st);
    return out;
}

int Memory::count() {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM memory;", -1, &st, nullptr);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

void Memory::clear() {
    char* err = nullptr;
    sqlite3_exec(db_, "DELETE FROM memory;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

}
