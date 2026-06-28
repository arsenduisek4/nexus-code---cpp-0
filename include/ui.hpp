#pragma once

// весь красивый вывод живёт тут. header-only, чтоб не лезть в CMakeLists лишний раз —
// все функции inline, новый .cpp не нужен
#include "common.hpp"

#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unistd.h>   // isatty / usleep — для анимации и детекта терминала

namespace nexus::ui {

// рисуем анимацию только в настоящем терминале. если вывод в пайп/файл — нахуй анимацию,
// иначе в логах будет каша из \033[ escape-ов
inline bool is_tty() { return isatty(STDOUT_FILENO) != 0; }

// прячем имя бэкенда из всего, что показываем юзеру: наружу мы Nexus, и точка.
// АПИ при этом всё равно стучится на deepseek — это чисто косметика для вывода,
// сам cfg.model/base_url не трогаем, иначе запросы отвалятся нахуй
inline std::string brand(std::string s) {
    const char* needles[] = {"deepseek", "DeepSeek", "Deepseek", "DEEPSEEK"};
    for (const char* n : needles) {
        size_t len = std::string(n).size();
        size_t pos;
        while ((pos = s.find(n)) != std::string::npos) s.replace(pos, len, "nexus");
    }
    return s;
}

// ---- truecolor (24-bit). если терминал не умеет — он просто проигнорит, не упадёт ----
inline std::string fg(int r, int g, int b) {
    return "\033[38;2;" + std::to_string(r) + ";" +
           std::to_string(g) + ";" + std::to_string(b) + "m";
}

// видимая ширина строки в колонках: пропускаем ANSI-эскейпы и считаем
// только лид-байты UTF-8 (continuation-байты 10xxxxxx не считаем).
// кириллица в utf-8 это 2 байта = 1 колонка, иначе рамки разъезжаются нахуй
inline size_t vwidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {                       // ESC — начало ANSI-последовательности
            i++;
            if (i < s.size() && s[i] == '[') {
                i++;
                while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') ||
                                         (s[i] >= 'a' && s[i] <= 'z'))) i++;
                if (i < s.size()) i++;         // съедаем финальную букву (m, K, ...)
            }
            continue;
        }
        if ((c & 0xC0) != 0x80) w++;           // не continuation-байт — значит новый символ
        i++;
    }
    return w;
}

// n раз повторить горизонтальную линию ─ (она 3-байтовая, наивный std::string(n,'─') не прокатит)
inline std::string hline(size_t n) {
    std::string out;
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) out += "─";
    return out;
}

// аккуратная рамка с заголовком в стиле современных агентных CLI:
//   ╭─ Заголовок ───────────╮
//   │ строка                │
//   ╰───────────────────────╯
inline void box(const std::string& title,
                const std::vector<std::string>& lines,
                const std::string& clr = color::cyan,
                std::ostream& os = std::cout) {
    size_t cw = vwidth(title) + 1;            // минимальная ширина — по заголовку
    for (const auto& l : lines) cw = std::max(cw, vwidth(l));
    size_t inner = cw + 2;                     // по одному пробелу слева и справа

    size_t tv = vwidth(title);
    size_t dash = inner > tv + 3 ? inner - tv - 3 : 0;  // сколько ─ после заголовка
    os << clr << "╭─ " << color::reset << color::bold << title << color::reset
       << clr << " " << hline(dash) << "╮" << color::reset << "\n";

    for (const auto& l : lines) {
        size_t pad = cw > vwidth(l) ? cw - vwidth(l) : 0;
        os << clr << "│ " << color::reset << l << std::string(pad, ' ')
           << clr << " │" << color::reset << "\n";
    }
    os << clr << "╰" << hline(inner) << "╯" << color::reset << "\n";
}

// горизонтальная линейка на всю «логическую» ширину
inline void rule(size_t width = 60, const std::string& clr = color::gray,
                 std::ostream& os = std::cout) {
    os << clr << hline(width) << color::reset << "\n";
}

// разбить многострочный текст на строки, выкинув ведущие пустые
// (raw-литерал баннера начинается с \n, не хотим лишний отступ сверху)
inline std::vector<std::string> rows_of(const std::string& text) {
    std::vector<std::string> rows;
    std::string cur;
    std::istringstream ss(text);
    while (std::getline(ss, cur, '\n')) rows.push_back(cur);
    while (!rows.empty() && rows.front().empty()) rows.erase(rows.begin());
    return rows;
}

// центрировать строку в поле шириной width (по видимой ширине, ANSI не считаем)
inline std::string center(const std::string& s, size_t width) {
    size_t v = vwidth(s);
    if (v >= width) return s;
    return std::string((width - v) / 2, ' ') + s;
}

// печать многострочного текста с вертикальным градиентом цвета.
// для баннера: сверху один цвет, снизу другой, между — плавный переход
inline void gradient(const std::string& text,
                     int r1, int g1, int b1,
                     int r2, int g2, int b2,
                     std::ostream& os = std::cout) {
    auto rows = rows_of(text);
    size_t n = rows.size();
    for (size_t i = 0; i < n; ++i) {
        double t = n > 1 ? double(i) / double(n - 1) : 0.0;
        os << fg(int(r1 + (r2 - r1) * t),
                 int(g1 + (g2 - g1) * t),
                 int(b1 + (b2 - b1) * t)) << rows[i] << color::reset << "\n";
    }
}

// АНИМИРОВАННЫЙ баннер: сначала кот «рисуется» сверху вниз построчно,
// потом по нему один раз пробегает яркий блик. в не-tty просто печатаем статикой
inline void animate_banner(const std::string& text,
                           int r1, int g1, int b1,
                           int r2, int g2, int b2,
                           std::ostream& os = std::cout) {
    auto rows = rows_of(text);
    size_t n = rows.size();
    if (n == 0) return;

    auto grad = [&](size_t i) {
        double t = n > 1 ? double(i) / double(n - 1) : 0.0;
        return fg(int(r1 + (r2 - r1) * t),
                  int(g1 + (g2 - g1) * t),
                  int(b1 + (b2 - b1) * t));
    };

    if (!is_tty()) {                       // пайп/файл — без анимации
        for (size_t i = 0; i < n; ++i) os << grad(i) << rows[i] << color::reset << "\n";
        return;
    }

    os << "\033[?25l";                     // прячем курсор, чтоб не мигал во время рисовки
    // фаза 1 — рисуем кота построчно
    for (size_t i = 0; i < n; ++i) {
        os << grad(i) << rows[i] << color::reset << "\n" << std::flush;
        usleep(11000);
    }
    // фаза 2 — блик: яркая полоса бежит сверху вниз (один проход)
    for (size_t w = 0; w < n + 3; ++w) {
        os << "\033[" << n << "A";         // курсор наверх кота
        for (size_t i = 0; i < n; ++i) {
            int dist = (int)i - (int)w;
            std::string clr = grad(i);
            if (dist == 0)            clr = fg(245, 245, 255);   // ядро блика — почти белое
            else if (dist == -1 || dist == 1) clr = fg(195, 205, 255);
            os << clr << rows[i] << color::reset << "\033[K\n";
        }
        os << std::flush;
        usleep(15000);
    }
    os << "\033[?25h";                      // возвращаем курсор
}

} // namespace nexus::ui
