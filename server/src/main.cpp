// bserve -- a binary-http static file server.
//
//   ./bserve ./www 9000
//
// Accepts TCP connections, reads binary request frames, maps :path to a file
// under the document root, and replies with status, headers and the bytes.
// The connection stays open.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "connection.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: bserve [-v] [-x] <document-root> <port>\n"
        "\n"
        "  <document-root>  directory served as /\n"
        "  <port>           TCP port to listen on\n"
        "\n"
        "  -v   hexdump every frame sent and received\n"
        "  -x   emit an unregistered frame type (0x7F) before each response,\n"
        "       to exercise the client's unknown-frame skip path (SPEC \xc2\xa7""7)\n"
        "  -h   this message\n");
}

} // namespace

int main(int argc, char** argv) {
    // A client that closes mid-response must not take the server with it.
    ::signal(SIGPIPE, SIG_IGN);

    bh::Options opt;
    std::string root;
    std::string portArg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v" || a == "--verbose")      opt.verbose = true;
        else if (a == "-x" || a == "--probe")   opt.sendUnknown = true;
        else if (a == "-h" || a == "--help")    { usage(); return 0; }
        else if (!a.empty() && a[0] == '-')     { usage(); return 2; }
        else if (root.empty())                  root = a;
        else if (portArg.empty())               portArg = a;
        else                                    { usage(); return 2; }
    }
    if (root.empty() || portArg.empty()) { usage(); return 2; }

    char* end = nullptr;
    long port = std::strtol(portArg.c_str(), &end, 10);
    if (!end || *end != '\0' || port < 1 || port > 65535) {
        std::fprintf(stderr, "bserve: bad port '%s'\n", portArg.c_str());
        return 2;
    }
    opt.root = root;
    opt.port = static_cast<int>(port);

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::perror("bserve: socket");
        return 1;
    }

    int one = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(opt.port));

    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::fprintf(stderr, "bserve: bind :%d: %s\n", opt.port, std::strerror(errno));
        ::close(listenFd);
        return 1;
    }
    if (::listen(listenFd, 64) != 0) {
        std::perror("bserve: listen");
        ::close(listenFd);
        return 1;
    }

    std::fprintf(stderr, "bserve: binary-http on port %d, root '%s'%s\n",
                 opt.port, opt.root.c_str(), opt.sendUnknown ? " [probe]" : "");

    for (;;) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof peer;
        int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("bserve: accept");
            break;
        }

        // Nagle would hold a small RESPONSE frame back waiting for the DATA
        // frame behind it; we already batch deliberately.
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        std::string label = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        // One thread per connection: connections are long-lived by design, so
        // the thread lives exactly as long as the thing it is serving.
        std::thread(bh::serveConnection, fd, opt, label).detach();
    }

    ::close(listenFd);
    return 0;
}
