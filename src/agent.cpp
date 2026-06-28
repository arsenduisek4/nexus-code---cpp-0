#include "agent.hpp"
#include "logger.hpp"
#include "util.hpp"
#include "ui.hpp"

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

namespace nexus {

Agent::Agent(Config& cfg, History& history, Memory& memory)
    : cfg_(cfg), history_(history), memory_(memory), client_(cfg_) {
    tools_.register_all();
}

std::string Agent::system_prompt() {
    std::string base =
        "Ты — Nexus Code, быстрый автономный кодинг-агент, работающий локально на Linux-машине пользователя. "
        "Ты действуешь через вызовы инструментов: читаешь и изменяешь файлы, выполняешь команды оболочки, "
        "управляешь серверами, работаешь с git и многим другим. Будь кратким и решительным. "
        "Всегда отвечай пользователю на русском языке.\n\n"
        "Правила:\n"
        "- Лучше использовать инструменты, чем догадываться. Читай файлы перед их изменением.\n"
        "- Делай минимальные корректные изменения. При необходимости проверяй через run_bash / run_tests.\n"
        "- Используй memory_add, чтобы запоминать важные факты о проектах пользователя, memory_search — чтобы вспоминать.\n"
        "- Когда задача выполнена, дай короткую сводку о проделанной работе.\n"
        "- Рабочая директория — это текущая директория оболочки пользователя.";

    // режим «разрешено всегда»: явно говорим модели не выпрашивать разрешений текстом
    // («можно я прочитаю/изменю?»), а сразу дёргать нужные инструменты
    if (cfg_.auto_approve)
        base +=
            "\n- Режим «разрешено всегда» ВКЛЮЧЁН: действуй полностью самостоятельно. "
            "НИКОГДА не спрашивай у пользователя разрешения вроде «можно я прочитаю/изменю/запущу?» — "
            "просто сразу вызывай нужные инструменты и делай работу.";

    return base;
}

json Agent::build_messages(const std::string& user_input) {
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", system_prompt()}});

    // подмешиваем топ-3 факта из памяти отдельным system-сообщением, а не в промпт юзера —
    // так модель чётче отделяет «вот что я помню» от «вот что меня спросили»
    auto hits = memory_.search(user_input, 3);
    if (!hits.empty()) {
        std::string mem = "Релевантные факты из долговременной памяти:\n";
        for (auto& h : hits) mem += "- " + h.text + "\n";
        messages.push_back({{"role", "system"}, {"content", mem}});
    }

    // тянем всю историю сессии. да, лимит 1кк — по сути «всё». когда упрёмся в контекст —
    // тут надо будет резать/суммаризировать, но пока на DeepSeek влезает, так что похуй
    for (auto& m : history_.recent_messages(1000000)) messages.push_back(m);
    messages.push_back({{"role", "user"}, {"content", user_input}});
    return messages;
}

bool Agent::confirm(const std::string& summary) {
    if (cfg_.auto_approve) return true;   // «разрешено всегда»: юзер сам разрешил, не доёбываемся
    std::cout << color::yellow << "  [?] выполнить " << color::bold << summary
              << color::reset << color::yellow << " ? [д/Н] " << color::reset << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;   // EOF посреди подтверждения = «нет»
    line = util::to_lower(util::trim(line));
    return line == "y" || line == "yes" || line == "д" || line == "да";
}

void Agent::run_turn(const std::string& user_input) {
    history_.add_message("user", user_input);
    json messages = build_messages(user_input);
    json schemas = cfg_.mode == "chat" ? json::array() : tools_.schemas();

    ToolContext ctx{cfg_, history_, memory_, cfg_.auto_approve};

    for (int iter = 0; cfg_.max_iterations <= 0 || iter < cfg_.max_iterations; ++iter) {
        // chat() блокирующий, поэтому крутим ascii-спиннер в ОТДЕЛЬНОМ потоке,
        // пока ждём ответ. флаг atomic, чтоб без гонок дёрнуть стоп после chat()
        std::atomic<bool> spinning{true};
        std::thread spinner;
        if (ui::is_tty()) {
            spinner = std::thread([&spinning] {
                const char frames[] = {'|', '/', '-', '\\'};   // классический ascii-вертел
                int k = 0;
                while (spinning.load()) {
                    std::cout << color::gray << "  [" << frames[k++ % 4] << "] думаю..."
                              << color::reset << "   \r" << std::flush;
                    std::this_thread::sleep_for(std::chrono::milliseconds(90));
                }
            });
        }
        ChatResult r = client_.chat(messages, schemas);
        spinning.store(false);
        if (spinner.joinable()) spinner.join();
        std::cout << "                  \r";   // затираем строку спиннера хвостом пробелов

        if (!r.ok) {
            std::cout << color::red << "  [x] ошибка: " << r.error << color::reset << "\n";
            Logger::instance().error("chat: " + r.error);
            return;
        }

        json& msg = r.message;
        messages.push_back(msg);

        bool has_calls = msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
                         !msg["tool_calls"].empty();

        std::string content = msg.value("content", "");
        if (!content.empty())
            std::cout << color::cyan << content << color::reset << "\n";

        // нет тул-коллов — значит модель закончила и просто ответила текстом, выходим
        if (!has_calls) {
            history_.add_message("assistant", content);
            return;
        }

        for (auto& call : msg["tool_calls"]) {
            std::string id = call.value("id", "");
            std::string name = call["function"].value("name", "");
            std::string args_str = call["function"].value("arguments", "{}");

            // аргументы прилетают СТРОКОЙ с json внутри (так в OpenAI-совместимом API).
            // если модель насрала кривой json — не падаем, просто едем с пустыми args
            json args = json::object();
            try { args = json::parse(args_str); } catch (...) {}

            const Tool* tool = tools_.find(name);
            std::string short_args = args.dump();
            if (short_args.size() > 120) short_args = short_args.substr(0, 117) + "...";
            // [>] <инструмент>  <аргументы> — ascii-маркер сразу цепляет глаз в потоке
            std::cout << color::magenta << "  [>] " << color::bold << name << color::reset
                      << color::gray << "  " << short_args << color::reset << "\n";

            std::string result;
            if (tool && tool->mutating && !confirm(name)) {
                result = "пользователь отклонил выполнение этого инструмента";
                std::cout << color::yellow << "      (пропущено)" << color::reset << "\n";
            } else {
                result = tools_.call(name, args, ctx);
            }

            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", id},
                {"content", result},
            });

            // результат показываем урезанным и с левым бордюром | — чтоб визуально
            // отделить «вывод тула» от «речи агента». полный результат всё равно ушёл модели
            std::string preview = result.size() > 300 ? result.substr(0, 297) + "..." : result;
            for (auto& pl : util::split(preview, '\n'))
                std::cout << color::gray << "      | " << pl << color::reset << "\n";
        }
    }

    std::cout << color::yellow << "  [!] достигнут лимит итераций" << color::reset << "\n";
    history_.add_message("assistant", "[остановлено: достигнут лимит итераций]");
}

}
