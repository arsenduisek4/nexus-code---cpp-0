#include "history.hpp"
#include "util.hpp"
#include "logger.hpp"

#include <sqlite3.h>
#include <ctime>

namespace nexus {

History::History(const std::string& db_path)
    : db_path_(util::expand_path(db_path)) {
    session_ = std::to_string((long)std::time(nullptr));
}

History::~History() {
    if (db_) sqlite3_close(db_);
}

void History::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        Logger::instance().error(std::string("sqlite: ") + (err ? err : "?"));
        if (err) sqlite3_free(err);
    }
}

bool History::open() {
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        Logger::instance().error("cannot open db: " + db_path_);
        return false;
    }
    // WAL — чтоб чтение не блокировалось записью. для одного процесса это уже перебор,
    // но дёшево и на будущее не помешает, если когда-нибудь прикрутим демон
    exec("PRAGMA journal_mode=WAL;");
    exec("CREATE TABLE IF NOT EXISTS messages("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "session TEXT, role TEXT, content TEXT, ts TEXT);");
    exec("CREATE TABLE IF NOT EXISTS checkpoints("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "label TEXT, snapshot TEXT, created_at TEXT);");
    exec("CREATE TABLE IF NOT EXISTS tasks("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "description TEXT, status TEXT, created_at TEXT);");
    return true;
}

void History::new_session() {
    session_ = std::to_string((long)std::time(nullptr));
}

void History::add_message(const std::string& role, const std::string& content) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO messages(session,role,content,ts) VALUES(?,?,?,?);", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, session_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = util::now_iso();
    sqlite3_bind_text(st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

json History::recent_messages(int limit) {
    json out = json::array();
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT role,content FROM messages WHERE session=? ORDER BY id DESC LIMIT ?;",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, session_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, limit);
    std::vector<json> rows;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* role = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        rows.push_back({{"role", role ? role : ""}, {"content", content ? content : ""}});
    }
    sqlite3_finalize(st);
    // тянем DESC + LIMIT, чтоб взять ПОСЛЕДНИЕ N сообщений, а не первые. но модели нужен
    // хронологический порядок, поэтому разворачиваем обратно через rbegin. да, костыльно
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(*it);
    return out;
}

int History::message_count() {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM messages;", -1, &st, nullptr);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

void History::clear() {
    // тут конкатенация в SQL, а не prepared statement — но session_ это наш же time(),
    // чистые цифры, инъекции взяться неоткуда. для юзерского ввода так делать НЕЛЬЗЯ
    exec("DELETE FROM messages WHERE session='" + session_ + "';");
}

int64_t History::add_checkpoint(const std::string& label, const std::string& snapshot) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO checkpoints(label,snapshot,created_at) VALUES(?,?,?);", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, snapshot.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = util::now_iso();
    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db_);
}

std::vector<Checkpoint> History::list_checkpoints() {
    std::vector<Checkpoint> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id,label,created_at FROM checkpoints ORDER BY id DESC;", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Checkpoint c;
        c.id = sqlite3_column_int64(st, 0);
        c.label = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        c.created_at = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        out.push_back(c);
    }
    sqlite3_finalize(st);
    return out;
}

std::string History::get_checkpoint(int64_t id) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT snapshot FROM checkpoints WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (s) out = s;
    }
    sqlite3_finalize(st);
    return out;
}

int64_t History::add_task(const std::string& description) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO tasks(description,status,created_at) VALUES(?,?,?);", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, "pending", -1, SQLITE_TRANSIENT);
    std::string ts = util::now_iso();
    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db_);
}

std::vector<Task> History::list_tasks() {
    std::vector<Task> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id,description,status,created_at FROM tasks ORDER BY id DESC;", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Task t;
        t.id = sqlite3_column_int64(st, 0);
        t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        t.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        t.created_at = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        out.push_back(t);
    }
    sqlite3_finalize(st);
    return out;
}

bool History::set_task_status(int64_t id, const std::string& status) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "UPDATE tasks SET status=? WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok && sqlite3_changes(db_) > 0;
}

}
