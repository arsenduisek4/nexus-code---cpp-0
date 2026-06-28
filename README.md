# Nexus Code v3.0 — C++ Edition

Быстрый автономный кодинг-агент на C++17. Один бинарник, мгновенный запуск,
80 встроенных инструментов и 20 слэш-команд (всего 100 функций). Полностью
русскоязычный интерфейс.

## Возможности

- **Function calling** в формате OpenAI (`api.cpp`).
- **Самодостаточная сборка** — SQLite (amalgamation) и nlohmann/json вшиты в
  `third_party/`. Никаких `-dev` пакетов ставить не нужно.
- **Авто-выбор HTTP-бэкенда** — `libcurl`, если есть его dev-заголовки, иначе
  откат на CLI `curl(1)`. Оба варианта работают сразу.
- **Векторная память** — локально, мешок слов + косинусное сходство, без
  внешних сервисов (понимает UTF-8/кириллицу).
- **История в SQLite** — сессии, снимки (checkpoints), задачи.
- **80 инструментов**: файлы, поиск, система, серверы, сеть, git, деплой, код,
  планирование, безопасность, утилиты.

## Требования

- CMake ≥ 3.16, компилятор C++17 (g++/clang++)
- `curl` в PATH (используется как HTTP-бэкенд, если нет dev-заголовков libcurl)

## Сборка и установка

```bash
./install.sh                 # собирает и ставит в ~/.local/bin
# или вручную:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Настройка

Ключ берётся из (в порядке приоритета): переменной окружения `NEXUS_API_KEY`
или файла `~/.nexuscode/config.json`.

```bash
export NEXUS_API_KEY=sk-...
# или:
nexus
› /config set api_key sk-...
```

Ключи в `~/.nexuscode/config.json`: `api_key`, `base_url`, `model`, `mode`,
`temperature`, `max_iterations` (0 = без лимита), `auto_approve`, `log_level`.

## Запуск

```bash
nexus                       # интерактивная сессия (REPL)
nexus -p "<запрос>"         # один запрос и выход
nexus --auto "<запрос>"     # без подтверждений на изменяющие действия
nexus --config              # показать текущие настройки
```

### Слэш-команды

`/help /config /model /mode /auto /tools /stats /memory /forget /plan /execute
/search /run /deploy /checkpoint /checkpoints /undo /rollback /clear /exit`

## Архитектура

| Файл | Назначение |
|------|------------|
| `main.cpp` | точка входа, разбор аргументов, REPL |
| `agent.cpp` | цикл «модель + вызовы инструментов» |
| `api.cpp` | клиент chat completions |
| `http.cpp` | HTTP-бэкенд (libcurl или curl CLI) |
| `tools.cpp` | реализация 80 инструментов + реестр |
| `memory.cpp` | локальная векторная память |
| `history.cpp` | SQLite: сессии, снимки, задачи |
| `commands.cpp` | обработка слэш-команд |
| `config.cpp` | загрузка/сохранение JSON-конфига |
| `logger.cpp` | логирование в файл |
| `util.cpp` | общие хелперы |

Данные хранятся в `~/.nexuscode/` (`config.json`, `nexus_cpp.db`, `logs/agent.log`).
