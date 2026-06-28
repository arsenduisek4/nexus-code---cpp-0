#include "banner.hpp"
#include "common.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "history.hpp"
#include "memory.hpp"
#include "agent.hpp"
#include "commands.hpp"
#include "http.hpp"
#include "util.hpp"
#include "ui.hpp"

#include <iostream>
#include <string>

using namespace nexus;

static LogLevel parse_level(const std::string& s) {
    if (s == "debug") return LogLevel::Debug;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;   // всё что не распознали — Info, и не еби мозги
}

static void print_usage() {
    std::cout <<
        "Nexus Code v3.0 (C++ Edition) — автономный кодинг-агент\n\n"
        "Использование:\n"
        "  nexus                     запустить интерактивную сессию\n"
        "  nexus -p \"<запрос>\"        выполнить один запрос и выйти\n"
        "  nexus \"<запрос>\"           то же, что и -p\n\n"
        "Опции:\n"
        "  --model <имя>       выбрать модель для этого запуска\n"
        "  --auto, --allow     режим «разрешено всегда»: действия без подтверждения\n"
        "  --chat              режим чата (без инструментов)\n"
        "  --config            показать текущие настройки и выйти\n"
        "  -h, --help          показать эту справку\n"
        "  -v, --version       показать версию\n";
}

int main(int argc, char** argv) {
    Config cfg = Config::load();
    Logger::instance().init(cfg.log_path(), parse_level(cfg.log_level));

    std::string one_shot;
    bool want_config = false;
    // парсер аргументов на коленке. argp/getopt — оверкилл для пяти флагов,
    // так что просто гоняем по argv руками
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        if (a == "-v" || a == "--version") { std::cout << "Nexus Code 3.0.0\n"; return 0; }
        if (a == "--config") { want_config = true; }
        else if (a == "--auto" || a == "--allow" || a == "--yes") { cfg.auto_approve = true; }
        else if (a == "--chat") { cfg.mode = "chat"; }
        else if (a == "--model" && i + 1 < argc) { cfg.model = argv[++i]; }
        else if (a == "-p" && i + 1 < argc) { one_shot = argv[++i]; }
        // всё что без дефиса — это куски запроса, склеиваем через пробел.
        // чтоб `nexus почини мне сборку` работало без кавычек, как у людей
        else if (!a.empty() && a[0] != '-') { one_shot = one_shot.empty() ? a : one_shot + " " + a; }
    }

    History history(cfg.db_path());
    if (!history.open()) {
        // без бд жить не можем — тут вся память и история, так что валимся сразу
        std::cerr << "критическая ошибка: не удалось открыть базу данных: " << cfg.db_path() << "\n";
        return 1;
    }
    Memory memory(history.raw());
    memory.init();

    Agent agent(cfg, history, memory);

    if (want_config) {
        // конфиг показываем в той же рамке, что и стартовый хедер — единый стиль
        ui::box("настройки", {
            "модель        " + ui::brand(cfg.model),
            "base_url      " + ui::brand(cfg.base_url),
            "режим         " + cfg.mode,
            // ключ НИКОГДА не печатаем целиком, только факт наличия — спалить токен в логах это позор
            std::string("api-ключ      ") + (cfg.api_key.empty() ? "(не задан)" : "задан"),
            std::string("http-бэкенд   ") + http::backend_name(),
            "инструментов  " + std::to_string(agent.tools().size()),
            "папка данных  " + util::expand_path(cfg.data_dir),
        }, color::cyan);
        return 0;
    }

    if (!one_shot.empty()) {
        // one-shot режим: отработали запрос и съебались, без интерактива
        agent.run_turn(one_shot);
        return 0;
    }

    // \033[2J\033[3J\033[H — чистим экран И скроллбэк, потом курсор домой.
    // именно [3J, иначе старый мусор остаётся в истории терминала и выглядит как говно
    std::cout << "\033[2J\033[3J\033[H";

    // кот рисуется построчно + по нему пробегает блик. градиент cyan → фиолет
    ui::animate_banner(kBanner, 90, 220, 255, 170, 110, 255);

    // тег-строка по центру под котом (ширина кота ~65 колонок)
    const size_t W = 65;
    std::cout << "\n"
              << color::bold << ui::fg(150, 165, 255)
              << ui::center("N E X U S   C O D E", W) << color::reset << "\n"
              << color::gray
              << ui::center("автономный кодинг-агент  ·  v3.0", W) << color::reset << "\n\n";

    // статус-строки в рамке: сразу видно модель/режим/бэкенд, не надо лезть в /config
    ui::box("сессия", {
        "модель        " + std::string(color::cyan) + ui::brand(cfg.model) + color::reset,
        "режим         " + cfg.mode,
        "инструменты   " + std::to_string(agent.tools().size()) +
            std::string(color::gray) + "  ·  http=" + http::backend_name() + color::reset,
        std::string(color::gray) + "/help - список команд" + color::reset,
    }, color::magenta);

    if (cfg.api_key.empty())
        std::cout << color::red << "  [!] API-ключ не задан - "
                  << color::reset << "выполни "
                  << color::bold << "/config set api_key <КЛЮЧ>" << color::reset << "\n";

    // главный цикл REPL: читаем строку, сначала пробуем как команду, иначе — в агента
    std::string line;
    while (true) {
        // двухстрочный промпт ╭ / ╰─> — рамка box-drawing, стрелка ascii
        std::cout << color::magenta << "\n╭─" << color::reset << "\n"
                  << color::magenta << "╰─> " << color::reset << std::flush;
        if (!std::getline(std::cin, line)) break;   // Ctrl+D / EOF — выходим по-тихому
        line = util::trim(line);
        if (line.empty()) continue;

        CommandOutcome oc = handle_command(agent, line);
        if (oc.quit) break;
        if (oc.handled) continue;   // была команда типа /help — в агента не тащим

        agent.run_turn(line);
    }

    // ascii-котик на прощание вместо эмодзи
    std::cout << color::gray << "\n   /\\_/\\\n  ( -.- )   до встречи\n   > ^ <\n" << color::reset;
    return 0;
}
