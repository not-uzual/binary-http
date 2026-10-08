// files.hpp -- mapping a binary-http :path onto a file under the document root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bh {

struct Resolved {
    int         status = 200;   // 200, 403, 404, 414 or 500
    std::string fsPath;         // valid when status == 200
    std::string contentType;
    std::string lastModified;   // RFC 1123, GMT
    uint64_t    size = 0;
};

// Percent-decodes `path`, strips any ?query, rejects traversal, appends
// index.html for directories, and stats the result.
Resolved resolvePath(const std::string& root, const std::string& path);

bool readFile(const std::string& fsPath, std::vector<uint8_t>& out);

std::string mimeFor(const std::string& fsPath);
std::string httpDate(int64_t epochSeconds);
std::string nowDate();
const char* statusText(int status);

} // namespace bh
