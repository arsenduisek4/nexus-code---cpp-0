#pragma once

#include "common.hpp"
#include <cstdint>

struct sqlite3;

namespace nexus {

struct Checkpoint {
    int64_t id;
    std::string label;
    std::string created_at;
};

struct Task {
    int64_t id;
    std::string description;
    std::string status;
    std::string created_at;
};

class History {
public:
    explicit History(const std::string& db_path);
    ~History();

    bool open();
    const std::string& session() const { return session_; }
    void new_session();

    void add_message(const std::string& role, const std::string& content);
    json recent_messages(int limit);
    int  message_count();
    void clear();

    int64_t add_checkpoint(const std::string& label, const std::string& snapshot);
    std::vector<Checkpoint> list_checkpoints();
    std::string get_checkpoint(int64_t id);

    int64_t add_task(const std::string& description);
    std::vector<Task> list_tasks();
    bool set_task_status(int64_t id, const std::string& status);

    sqlite3* raw() { return db_; }

private:
    void exec(const std::string& sql);
    std::string db_path_;
    std::string session_;
    sqlite3* db_ = nullptr;
};

}
