#include "files.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cctype>

namespace bh {
namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns false if a %XX escape is truncated or not hex.
bool percentDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') { out += in[i]; continue; }
        if (i + 2 >= in.size()) return false;
        int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    return true;
}

// Rejects anything that could climb out of the root. We do this on the decoded
// path, before touching the filesystem, so a symlink cannot widen the check.
bool escapesRoot(const std::string& path) {
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        std::string seg = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "..") return true;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return path.find('\0') != std::string::npos;
}

} // namespace

std::string mimeFor(const std::string& fsPath) {
    size_t dot = fsPath.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = fsPath.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "html" || ext == "htm")  return "text/html; charset=utf-8";
    if (ext == "css")                   return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs")    return "application/javascript; charset=utf-8";
    if (ext == "json")                  return "application/json";
    if (ext == "txt" || ext == "md")    return "text/plain; charset=utf-8";
    if (ext == "svg")                   return "image/svg+xml";
    if (ext == "png")                   return "image/png";
    if (ext == "jpg" || ext == "jpeg")  return "image/jpeg";
    if (ext == "gif")                   return "image/gif";
    if (ext == "ico")                   return "image/vnd.microsoft.icon";
    if (ext == "pdf")                   return "application/pdf";
    if (ext == "wasm")                  return "application/wasm";
    return "application/octet-stream";
}

std::string httpDate(int64_t epochSeconds) {
    std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm gm{};
    gmtime_r(&t, &gm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &gm);
    return buf;
}

std::string nowDate() { return httpDate(static_cast<int64_t>(std::time(nullptr))); }

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 414: return "URI Too Long";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "Version Not Supported";
        default:  return "Unknown";
    }
}

Resolved resolvePath(const std::string& root, const std::string& path) {
    Resolved r;

    if (path.size() > 8192) { r.status = 414; return r; }
    if (path.empty() || path[0] != '/') { r.status = 403; return r; }

    std::string noQuery = path.substr(0, path.find('?'));
    std::string decoded;
    if (!percentDecode(noQuery, decoded) || escapesRoot(decoded)) {
        r.status = 403;
        return r;
    }

    std::string fs = root;
    if (!fs.empty() && fs.back() == '/') fs.pop_back();
    fs += decoded;

    struct stat st{};
    if (::stat(fs.c_str(), &st) != 0) { r.status = 404; return r; }

    if (S_ISDIR(st.st_mode)) {
        if (fs.back() != '/') fs += '/';
        fs += "index.html";
        if (::stat(fs.c_str(), &st) != 0) { r.status = 404; return r; }
    }
    if (!S_ISREG(st.st_mode)) { r.status = 403; return r; }

    r.fsPath       = fs;
    r.size         = static_cast<uint64_t>(st.st_size);
    r.contentType  = mimeFor(fs);
    r.lastModified = httpDate(static_cast<int64_t>(st.st_mtime));
    return r;
}

bool readFile(const std::string& fsPath, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(fsPath.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + got);
    }
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

} // namespace bh
