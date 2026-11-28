#pragma once

#include <functional>

namespace httplib {
class Server;
}

namespace koma {
class CloudClient;
class CoverManager;
class Database;
class LibraryScanner;
class PortablePaths;
class ReaderSession;
}

namespace koma::api {

struct LibraryRouteContext {
    Database& db;
    LibraryScanner& scanner;
    CloudClient& cloud;
    CoverManager& cover_manager;
    ReaderSession& reader_session;
    const PortablePaths& paths;
    std::function<void()> refresh_jumplist;
};

void register_library_routes(httplib::Server& server, LibraryRouteContext ctx);

} // namespace koma::api
