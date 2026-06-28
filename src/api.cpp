#include "api.hpp"
#include "http.hpp"
#include "logger.hpp"

namespace nexus {

ChatResult NexusClient::chat(const json& messages, const json& tools) {
    ChatResult out;
    if (cfg_.api_key.empty()) {
        out.error = "API-ключ не задан. Выполни `/config set api_key <КЛЮЧ>` или задай NEXUS_API_KEY.";
        return out;
    }

    // stream=false специально: стримить токены красиво, но потом парсить tool_calls
    // из чанков — это отдельный ад, а нам нужны цельные tool_calls. так что ждём весь ответ
    json payload = {
        {"model", cfg_.model},
        {"messages", messages},
        {"temperature", cfg_.temperature},
        {"stream", false},
    };
    if (tools.is_array() && !tools.empty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";   // пусть модель сама решает, дёргать тулы или нет
    }

    std::vector<std::string> headers = {
        "Authorization: Bearer " + cfg_.api_key,   // Bearer-токен, как у всех OpenAI-совместимых
    };

    std::string url = cfg_.base_url + "/chat/completions";
    Logger::instance().debug("POST " + url + " model=" + cfg_.model);

    auto resp = http::post_json(url, headers, payload.dump());
    if (!resp.error.empty()) { out.error = "сеть: " + resp.error; return out; }

    json body;
    try {
        body = json::parse(resp.body);
    } catch (const std::exception& e) {
        // прилетело не-json — обычно это html-страница ошибки от прокси/клаудфлары.
        // режем до 400 символов, иначе в терминал вывалится вся ёбаная html-портянка
        out.error = "некорректный JSON от API (HTTP " + std::to_string(resp.status) + "): " +
                    resp.body.substr(0, 400);
        return out;
    }

    if (resp.status < 200 || resp.status >= 300) {
        std::string msg = "HTTP " + std::to_string(resp.status);
        if (body.contains("error") && body["error"].contains("message"))
            msg += ": " + body["error"]["message"].get<std::string>();
        out.error = msg;
        return out;
    }

    if (!body.contains("choices") || body["choices"].empty()) {
        out.error = "в ответе нет вариантов (choices)";
        return out;
    }

    out.message = body["choices"][0]["message"];
    if (body.contains("usage")) {
        out.prompt_tokens = body["usage"].value("prompt_tokens", 0);
        out.completion_tokens = body["usage"].value("completion_tokens", 0);
    }
    out.ok = true;
    return out;
}

bool NexusClient::ping() {
    json msgs = json::array({ {{"role","user"},{"content","ping"}} });
    auto r = chat(msgs, json::array());
    return r.ok;
}

}
