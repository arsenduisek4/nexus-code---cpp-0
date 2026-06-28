#pragma once

#include <string>
#include <vector>

namespace nexus::http {

struct Response {
    long status = 0;
    std::string body;
    std::string error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

Response post_json(const std::string& url,
                   const std::vector<std::string>& headers,
                   const std::string& body,
                   long timeout_sec = 120);

Response get(const std::string& url,
             const std::vector<std::string>& headers = {},
             long timeout_sec = 60);

bool download(const std::string& url, const std::string& dest, long timeout_sec = 300);

const char* backend_name();

}
