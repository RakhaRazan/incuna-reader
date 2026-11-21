#pragma once

#include <functional>

namespace httplib {
class Server;
}

namespace koma {
class CloudClient;
class Database;
class LibraryScanner;
class PortablePaths;
}

namespace koma::api {

struct LibraryRouteContext {
    Database& db;
    LibraryScanner& scanner;
    CloudClient& cloud;
    const PortablePaths& paths;
    std::function<void()> refresh_jumplist;
};

void register_library_routes(httplib::Server& server, LibraryRouteContext ctx);

} // namespace koma::api
