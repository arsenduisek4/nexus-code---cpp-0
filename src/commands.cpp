#include "commands.hpp"
#include "agent.hpp"
#include "http.hpp"
#include "util.hpp"
#include "ui.hpp"

#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nexus {

namespace {

// есть ли в системе бинарь (первое слово командной строки)? проверяем заранее,
// чтобы НЕ дёргать popen на отсутствующую утилиту и не словить SIGPIPE при записи
bool cmd_available(const std::string& cmdline) {
    std::string bin = cmdline.substr(0, cmdline.find(' '));
    return std::system(("command -v " + bin + " >/dev/null 2>&1").c_str()) == 0;
}

// кладём текст в системный буфер обмена. перебираем известные утилиты по очереди:
// wl-clipboard (Wayland), затем xclip / xsel (X11). true — если хоть одна сработала
bool clipboard_copy(const std::string& text) {
    const char* tools[] = {
        "wl-copy",
        "xclip -selection clipboard",
        "xsel --clipboard --input",
    };
    for (const char* t : tools) {
        if (!cmd_available(t)) continue;
        FILE* p = popen(t, "w");
        if (!p) continue;
        fwrite(text.data(), 1, text.size(), p);
        if (pclose(p) == 0) return true;
    }
    return false;
}

// читаем системный буфер обмена. ok=false — если ни одной утилиты не нашлось
std::string clipboard_paste(bool& ok) {
    const char* tools[] = {
        "wl-paste --no-newline",
        "xclip -selection clipboard -o",
        "xsel --clipboard --output",
    };
    for (const char* t : tools) {
        if (!cmd_available(t)) continue;
        FILE* p = popen(t, "r");
        if (!p) continue;
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        if (pclose(p) == 0) { ok = true; return out; }
    }
    ok = false;
    return "";
}

// ручной многострочный ввод — режим вставления. собираем строки как есть,
// БЕЗ обработки (markdown, отступы, кавычки сохраняются дословно), пока не встретим
// строку-маркер конца: одна точка «.» или «END»/«КОНЕЦ». пустые строки внутри блока
// сохраняются — это важно для вставки кода и diff'ов
std::string read_multiline() {
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(std::cin, l)) {
        std::string t = util::trim(l);
        if (t == "." || util::to_lower(t) == "end" || t == "КОНЕЦ") break;
        lines.push_back(l);
    }
    return util::join(lines, "\n");
}

// последний текстовый ответ ассистента из истории сессии — это и копируем по /copy
std::string last_assistant_reply(Agent& a) {
    json msgs = a.history().recent_messages(1000);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->value("role", "") == "assistant") {
            std::string c = it->value("content", "");
            if (!util::trim(c).empty()) return c;
        }
    }
    return "";
}

void print_help() {
    // всю справку в одну рамку — так она выглядит как цельный блок, а не как простыня echo.
    // c — цвет команды, чтоб глаз цеплялся за /команду, а не за описание
    auto c = [](const char* cmd) {
        return std::string(color::cyan) + cmd + color::reset;
    };
    ui::box("команды Nexus Code", {
        c("/help") + "                  эта справка",
        c("/config [show|set к з]") + " показать или изменить настройки",
        c("/model <имя>") + "           сменить модель",
        c("/mode <agent|chat>") + "     сменить режим",
        c("/auto") + "                  вкл/выкл авто-подтверждение изменяющих действий",
        c("/allow [on|off]") + "        режим «разрешено всегда» — ИИ не спрашивает подтверждений",
        c("/copy") + "                  скопировать последний ответ в буфер обмена",
        c("/paste [инструкция]") + "    вставить текст из буфера (или вручную) и отдать агенту",
        c("/insert [инструкция]") + "   режим вставления: многострочный ввод до строки «.»",
        c("/tools") + "                 список доступных инструментов",
        c("/stats") + "                 статистика сессии",
        c("/memory [search q]") + "     показать или искать в долговременной памяти",
        c("/forget") + "                очистить долговременную память",
        c("/plan <цель>") + "           попросить агента составить план",
        c("/execute") + "               выполнить составленный план",
        c("/search <шаблон>") + "       поиск по проекту",
        c("/run") + "                   определить и запустить проект",
        c("/deploy [цель]") + "         задеплоить проект",
        c("/checkpoint [метка]") + "    сделать снимок диалога",
        c("/checkpoints") + "           список снимков",
        c("/undo") + "                  восстановить последний снимок",
        c("/rollback <id>") + "         восстановить конкретный снимок",
        c("/clear") + "                 очистить историю текущей сессии и экран",
        c("/exit") + "                  выход",
    }, color::magenta);
}

// показать превью собранного блока и отдать его агенту. instruction — необязательная
// строка-инструкция, которая встаёт перед вставленным текстом. возвращает false и
// печатает «пусто», если вставлять нечего — чтобы вызывающий просто вышел
bool show_and_send(Agent& a, const std::string& instruction, const std::string& content) {
    if (util::trim(content).empty()) {
        std::cout << "пусто — нечего вставлять\n";
        return false;
    }
    // превью вставленного, чтоб юзер видел что именно ушло агенту
    std::string preview = content.size() > 200 ? content.substr(0, 197) + "..." : content;
    std::cout << color::gray << "  вставлено " << content.size() << " символов:" << color::reset << "\n";
    for (auto& pl : util::split(preview, '\n'))
        std::cout << color::gray << "  | " << pl << color::reset << "\n";
    // instruction — необязательная инструкция перед вставленным блоком
    a.run_turn(instruction.empty() ? content : instruction + "\n\n" + content);
    return true;
}

std::string call_tool(Agent& a, const std::string& name, const json& args) {
    ToolContext ctx{a.config(), a.history(), a.memory(), a.config().auto_approve};
    return a.tools().call(name, args, ctx);
}

} // namespace

CommandOutcome handle_command(Agent& agent, const std::string& input) {
    // не с / в начале — это не команда, а обычный запрос юзера, отдаём агенту
    if (input.empty() || input[0] != '/') return {false, false};

    std::istringstream ss(input.substr(1));
    std::string cmd;
    ss >> cmd;
    std::string rest;
    std::getline(ss, rest);
    rest = util::trim(rest);
    cmd = util::to_lower(cmd);

    auto& cfg = agent.config();

    if (cmd == "exit" || cmd == "quit") return {true, true};

    if (cmd == "help") { print_help(); return {true, false}; }

    if (cmd == "config") {
        std::istringstream cs(rest);
        std::string sub; cs >> sub;
        if (sub == "set") {
            std::string key, val; cs >> key; std::getline(cs, val); val = util::trim(val);
            if (key == "api_key") cfg.api_key = val;
            else if (key == "model") cfg.model = val;
            else if (key == "base_url") cfg.base_url = val;
            else if (key == "mode") cfg.mode = val;
            else if (key == "temperature") try { cfg.temperature = std::stod(val); } catch (...) {}
            else if (key == "max_iterations") try { cfg.max_iterations = std::stoi(val); } catch (...) {}
            else { std::cout << "неизвестный параметр: " << key << "\n"; return {true, false}; }
            cfg.save();
            std::cout << color::green << "задано: " << key << color::reset << "\n";
        } else {
            // brand() прячет «deepseek» из вывода — наружу мы Nexus
            std::cout << "модель=" << ui::brand(cfg.model) << "  режим=" << cfg.mode
                      << "  temp=" << cfg.temperature
                      << "  лимит_итераций=" << (cfg.max_iterations <= 0 ? "без лимита" : std::to_string(cfg.max_iterations))
                      << "  авто=" << (cfg.auto_approve ? "вкл" : "выкл") << "\n"
                      << "base_url=" << ui::brand(cfg.base_url)
                      << "  api-ключ=" << (cfg.api_key.empty() ? "(не задан)" : "задан") << "\n";
        }
        return {true, false};
    }

    if (cmd == "model") {
        if (rest.empty()) { std::cout << "текущая модель: " << ui::brand(cfg.model) << "\n"; }
        else { cfg.model = rest; cfg.save(); std::cout << color::green << "модель → " << ui::brand(rest) << color::reset << "\n"; }
        return {true, false};
    }

    if (cmd == "mode") {
        if (rest == "agent" || rest == "chat" || rest == "auto") {
            cfg.mode = rest; cfg.save();
            std::cout << color::green << "режим → " << rest << color::reset << "\n";
        } else std::cout << "использование: /mode <agent|chat|auto>\n";
        return {true, false};
    }

    if (cmd == "auto") {
        cfg.auto_approve = !cfg.auto_approve; cfg.save();
        std::cout << "авто-подтверждение " << (cfg.auto_approve ? "ВКЛ" : "ВЫКЛ") << "\n";
        return {true, false};
    }

    if (cmd == "allow" || cmd == "yolo") {
        // режим «разрешено всегда»: переключатель сохраняется в конфиг и держится
        // между запусками. явный on/off, чтобы не угадывать состояние как у /auto
        std::string r = util::to_lower(rest);
        bool on = !(r == "off" || r == "выкл" || r == "0" || r == "нет");
        cfg.auto_approve = on; cfg.save();
        if (on)
            std::cout << color::green << "режим «разрешено всегда» ВКЛ — "
                      << "выполняю действия без вопросов" << color::reset << "\n";
        else
            std::cout << color::yellow << "режим «разрешено всегда» ВЫКЛ — "
                      << "снова спрашиваю подтверждение изменяющих действий" << color::reset << "\n";
        return {true, false};
    }

    if (cmd == "copy") {
        std::string reply = last_assistant_reply(agent);
        if (reply.empty()) {
            std::cout << "нечего копировать — агент ещё не отвечал в этой сессии\n";
            return {true, false};
        }
        if (clipboard_copy(reply))
            std::cout << color::green << "последний ответ скопирован в буфер обмена ("
                      << reply.size() << " символов)" << color::reset << "\n";
        else
            std::cout << color::yellow << "не нашёл утилиту буфера обмена — "
                      << "установи xclip, xsel или wl-clipboard" << color::reset << "\n";
        return {true, false};
    }

    if (cmd == "paste") {
        // сначала пробуем системный буфер обмена (Ctrl+C из редактора/браузера).
        // если утилиты буфера нет — переходим в ручной многострочный режим
        bool ok = false;
        std::string content = clipboard_paste(ok);
        if (!ok) {
            std::cout << color::gray
                      << "буфер обмена недоступен. вставьте текст и завершите строкой с одной точкой «.»:"
                      << color::reset << "\n";
            content = read_multiline();
        }
        show_and_send(agent, rest, content);
        return {true, false};
    }

    if (cmd == "insert" || cmd == "вставка") {
        // режим вставления: всегда ручной многострочный ввод, БЕЗ обращения к буферу.
        // нужен когда буфер обмена не тот (ssh, tmux без интеграции) или текст набирают
        // руками. построчный ввод склеивается в одно сообщение — терминал не дробит
        // вставку на отдельные промпты, как было бы в обычном цикле REPL
        std::cout << color::gray
                  << "режим вставления — вставьте или введите текст; "
                  << "завершите строкой с одной точкой «.» (или END):"
                  << color::reset << "\n";
        std::string content = read_multiline();
        show_and_send(agent, rest, content);
        return {true, false};
    }

    if (cmd == "tools") {
        // инструменты сгруппированы по категориям — по одной строке на категорию в рамке
        auto cats = agent.tools().by_category();
        std::vector<std::string> lines;
        for (auto& [cat, names] : cats)
            lines.push_back(std::string(color::cyan) + cat + color::reset + "  " +
                            std::string(color::gray) + util::join(names, ", ") + color::reset);
        ui::box(std::to_string(agent.tools().size()) + " инструментов", lines, color::magenta);
        return {true, false};
    }

    if (cmd == "stats") {
        ui::box("статистика сессии", {
            "модель              " + ui::brand(cfg.model),
            "режим               " + cfg.mode,
            std::string("http-бэкенд         ") + http::backend_name(),
            "инструментов        " + std::to_string(agent.tools().size()),
            "сообщений (всего)   " + std::to_string(agent.history().message_count()),
            "записей в памяти    " + std::to_string(agent.memory().count()),
        }, color::cyan);
        return {true, false};
    }

    if (cmd == "memory") {
        std::istringstream ms(rest);
        std::string sub; ms >> sub; std::string q; std::getline(ms, q); q = util::trim(q);
        if (sub == "search" && !q.empty())
            std::cout << call_tool(agent, "memory_search", {{"query", q}}) << "\n";
        else
            std::cout << call_tool(agent, "memory_list", json::object()) << "\n";
        return {true, false};
    }

    if (cmd == "forget") {
        agent.memory().clear();
        std::cout << color::green << "память очищена" << color::reset << "\n";
        return {true, false};
    }

    if (cmd == "plan") {
        if (rest.empty()) { std::cout << "использование: /plan <цель>\n"; return {true, false}; }
        agent.run_turn("Составь краткий пронумерованный пошаговый план достижения этой цели. "
                       "Пока не выполняй его. Цель: " + rest);
        return {true, false};
    }

    if (cmd == "execute") {
        agent.run_turn("Выполни предложенный тобой план по шагам, используя инструменты.");
        return {true, false};
    }

    if (cmd == "search") {
        if (rest.empty()) { std::cout << "использование: /search <шаблон>\n"; return {true, false}; }
        std::cout << call_tool(agent, "search_project", {{"pattern", rest}}) << "\n";
        return {true, false};
    }

    if (cmd == "run") {
        // тупая эвристика «угадай проект по файлу-маркеру». порядок важен: package.json
        // проверяем первым, тк node-проекты часто тащат за собой и питон, и cmake
        std::string c;
        if (util::file_exists("package.json")) c = "npm start";
        else if (util::file_exists("main.py")) c = "python3 main.py";
        else if (util::file_exists("Cargo.toml")) c = "cargo run";
        else if (util::file_exists("CMakeLists.txt")) c = "cmake -B build && cmake --build build";
        else { std::cout << "не удалось определить, как запустить этот проект\n"; return {true, false}; }
        std::cout << color::gray << "$ " << c << color::reset << "\n";
        std::cout << call_tool(agent, "run_bash", {{"command", c}, {"timeout", 120}}) << "\n";
        return {true, false};
    }

    if (cmd == "deploy") {
        agent.run_turn("Задеплой этот проект" + (rest.empty() ? "." : " на " + rest + "."));
        return {true, false};
    }

    if (cmd == "checkpoint") {
        std::string label = rest.empty() ? util::now_iso() : rest;
        std::string snap = agent.history().recent_messages(1000).dump();
        auto id = agent.history().add_checkpoint(label, snap);
        std::cout << color::green << "снимок #" << id << " (" << label << ")" << color::reset << "\n";
        return {true, false};
    }

    if (cmd == "checkpoints") {
        auto cps = agent.history().list_checkpoints();
        if (cps.empty()) std::cout << "снимков нет\n";
        for (auto& c : cps)
            std::cout << "#" << c.id << "  " << c.created_at << "  " << c.label << "\n";
        return {true, false};
    }

    if (cmd == "undo" || cmd == "rollback") {
        int64_t id = 0;
        if (cmd == "rollback") { try { id = std::stoll(rest); } catch (...) {} }
        else {
            auto cps = agent.history().list_checkpoints();
            if (!cps.empty()) id = cps.front().id;
        }
        if (id == 0) { std::cout << "нет снимка для восстановления\n"; return {true, false}; }
        std::string snap = agent.history().get_checkpoint(id);
        if (snap.empty()) { std::cout << "снимок не найден\n"; return {true, false}; }
        // сносим текущую историю и заливаем из снимка. да, это деструктивно и без бэкапа —
        // если снимок битый, текущую сессию уже не вернуть. но это /undo, тут так и задумано
        agent.history().clear();
        try {
            for (auto& m : json::parse(snap))
                agent.history().add_message(m.value("role", "user"), m.value("content", ""));
        } catch (...) {}
        std::cout << color::green << "восстановлен снимок #" << id << color::reset << "\n";
        return {true, false};
    }

    if (cmd == "clear") {
        agent.history().clear();
        std::cout << "\033[2J\033[3J\033[H";
        std::cout << color::green << "история сессии и экран очищены" << color::reset << "\n";
        return {true, false};
    }

    std::cout << "неизвестная команда: /" << cmd << "  (см. /help)\n";
    return {true, false};
}

}
