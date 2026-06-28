#include "http.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <array>
#include <fstream>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

namespace nexus::http {

const char* backend_name() {
#ifdef HAVE_LIBCURL
    return "libcurl";
#else
    return "curl-cli";
#endif
}

#ifdef HAVE_LIBCURL

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static Response do_request(const std::string& url,
                           const std::vector<std::string>& headers,
                           const std::string* body,
                           long timeout_sec) {
    Response r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl init failed"; return r; }

    struct curl_slist* hdr = nullptr;
    for (const auto& h : headers) hdr = curl_slist_append(hdr, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nexus Code/3.0");
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->size());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) r.error = curl_easy_strerror(res);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);

    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return r;
}

#else

// фоллбэк-режим без libcurl: дёргаем системный curl через popen.
// уродливо, но работает везде, где есть curl в PATH — а он есть почти везде
static std::string run_capture(const std::string& cmd, int& code) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { code = -1; return out; }
    std::array<char, 8192> buf{};
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
    code = pclose(p);
    return out;
}

static Response do_request(const std::string& url,
                           const std::vector<std::string>& headers,
                           const std::string* body,
                           long timeout_sec) {
    Response r;
    std::string body_file;
    std::string cmd = "curl -sS --max-time " + std::to_string(timeout_sec) +
                      " -w '\\n__HTTP_STATUS__%{http_code}' -L";

    for (const auto& h : headers)
        cmd += " -H " + util::shell_escape(h);

    if (body) {
        // тело пишем во временный файл и шлём через @file, а НЕ через -d "...".
        // иначе огромный json с кавычками порвёт командную строку нахуй и словим инъекцию.
        // имя файла = pid + адрес указателя, чтоб два запроса не затёрли друг друга
        body_file = "/tmp/nexus_req_" + std::to_string(::getpid()) + "_" +
                    std::to_string((uintptr_t)body % 100000) + ".json";
        std::ofstream(body_file, std::ios::binary) << *body;
        cmd += " --data-binary @" + util::shell_escape(body_file);
    }
    cmd += " " + util::shell_escape(url) + " 2>/tmp/nexus_curl_err";

    int code = 0;
    std::string raw = run_capture(cmd, code);
    if (!body_file.empty()) std::remove(body_file.c_str());

    // curl по -w дописывает в конец строку __HTTP_STATUS__<код>. ищем её с конца (rfind),
    // тк сам ответ тоже может содержать эту подстроку — мало ли что вернёт сервер
    auto marker = raw.rfind("__HTTP_STATUS__");
    if (marker != std::string::npos) {
        r.status = std::strtol(raw.c_str() + marker + 15, nullptr, 10);
        r.body = raw.substr(0, marker);
        if (!r.body.empty() && r.body.back() == '\n') r.body.pop_back();
    } else {
        r.body = raw;
    }

    if (code != 0 && r.status == 0) {
        bool ok = false;
        std::string err = util::read_file_raw("/tmp/nexus_curl_err", ok);
        r.error = ok && !err.empty() ? util::trim(err) : "curl failed";
    }
    return r;
}

#endif

Response post_json(const std::string& url,
                   const std::vector<std::string>& headers,
                   const std::string& body,
                   long timeout_sec) {
    std::vector<std::string> h = headers;
    h.push_back("Content-Type: application/json");
    return do_request(url, h, &body, timeout_sec);
}

Response get(const std::string& url,
             const std::vector<std::string>& headers,
             long timeout_sec) {
    return do_request(url, headers, nullptr, timeout_sec);
}

bool download(const std::string& url, const std::string& dest, long timeout_sec) {
#ifdef HAVE_LIBCURL
    auto r = get(url, {}, timeout_sec);
    if (!r.ok()) return false;
    return util::write_file_raw(dest, r.body);
#else
    std::string cmd = "curl -sS -L --max-time " + std::to_string(timeout_sec) +
                      " -o " + util::shell_escape(util::expand_path(dest)) +
                      " " + util::shell_escape(url);
    return std::system(cmd.c_str()) == 0;
#endif
}

}
