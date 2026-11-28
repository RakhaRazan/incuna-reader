#include "cloud/cloud_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include "covers/cover_manager.hpp"
#include "db/database.hpp"
#include "portable_paths.hpp"

namespace koma {

namespace {

int64_t unix_now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool extract_comic_format(const std::string& filename, std::string& out_format) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return false;
    std::string ext = to_lower_ascii(filename.substr(dot + 1));
    if (ext == "cbz" || ext == "cbr" || ext == "cb7" || ext == "cbt" || ext == "pdf" || ext == "epub") {
        out_format = ext;
        return true;
    }
    return false;
}

std::string sanitize_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char ch : name) {
        if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty() || out == "." || out == "..") {
        return "comic_download";
    }
    return out;
}

std::string url_encode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string url_decode(const std::string& value) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            int hi = hex_val(value[i + 1]);
            int lo = hex_val(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(value[i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string base64url_encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
    } else if (i + 2 == len) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
    }
    return out;
}

std::string random_base64url(size_t num_bytes) {
    std::vector<uint8_t> buf(num_bytes, 0);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Fallback entropy if BCryptGenRandom ever fails
        const uint64_t seed = static_cast<uint64_t>(GetTickCount64()) ^
                              static_cast<uint64_t>(GetCurrentProcessId());
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) ^ (i * 37));
        }
    }
    return base64url_encode(buf.data(), buf.size());
}

std::string sha256_base64url(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t, 32> digest{};
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash,
                               reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                               static_cast<ULONG>(input.size()), 0) == 0 &&
                BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    if (!ok) return input;
    return base64url_encode(digest.data(), digest.size());
}

std::unordered_map<std::string, std::string> parse_query_string(const std::string& qs) {
    std::unordered_map<std::string, std::string> params;
    size_t start = 0;
    while (start < qs.size()) {
        size_t amp = qs.find('&', start);
        if (amp == std::string::npos) amp = qs.size();
        const std::string pair = qs.substr(start, amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                params[url_decode(pair)] = "";
            } else {
                params[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
        }
        start = amp + 1;
    }
    return params;
}

struct ScopedWinHttp {
    HINTERNET h = nullptr;
    ScopedWinHttp() = default;
    explicit ScopedWinHttp(HINTERNET handle) : h(handle) {}
    ~ScopedWinHttp() {
        if (h) WinHttpCloseHandle(h);
    }
    ScopedWinHttp(const ScopedWinHttp&) = delete;
    ScopedWinHttp& operator=(const ScopedWinHttp&) = delete;
    operator HINTERNET() const { return h; }
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string error;
};

bool crack_url_components(const std::wstring& url,
                          std::wstring& out_host,
                          INTERNET_PORT& out_port,
                          std::wstring& out_path_query,
                          bool& out_https) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        return false;
    }
    out_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out_port = uc.nPort;
    if (uc.lpszHostName && uc.dwHostNameLength > 0) {
        out_host.assign(uc.lpszHostName, uc.dwHostNameLength);
    }
    out_path_query.clear();
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) {
        out_path_query.append(uc.lpszUrlPath, uc.dwUrlPathLength);
    } else {
        out_path_query = L"/";
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        out_path_query.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    return !out_host.empty();
}

HttpResponse winhttp_request(const std::wstring& method,
                             const std::string& url_utf8,
                             const std::vector<std::wstring>& headers,
                             const std::string& body) {
    HttpResponse resp;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path_query;
    bool is_https = true;
    const std::wstring url_w = utf8_to_wide(url_utf8);
    if (!crack_url_components(url_w, host, port, path_query, is_https)) {
        resp.error = "Invalid URL: " + url_utf8;
        return resp;
    }

    ScopedWinHttp session(WinHttpOpen(L"IncunaReader/1.1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        resp.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return resp;
    }

    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);

    ScopedWinHttp connect(WinHttpConnect(session.h, host.c_str(), port, 0));
    if (!connect.h) {
        resp.error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ")";
        return resp;
    }

    const DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttp request(WinHttpOpenRequest(connect.h, method.c_str(), path_query.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) {
        resp.error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")";
        return resp;
    }

    for (const auto& hdr : headers) {
        WinHttpAddRequestHeaders(request.h, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    LPVOID body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD body_len = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body_ptr, body_len, body_len, 0)) {
        resp.error = "WinHttpSendRequest failed (" + std::to_string(GetLastError()) + ")";
        return resp;
    }

    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        resp.error = "WinHttpReceiveResponse failed (" + std::to_string(GetLastError()) + ")";
        return resp;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (WinHttpQueryHeaders(request.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        resp.status_code = static_cast<int>(status_code);
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            resp.error = "WinHttpQueryDataAvailable failed (" + std::to_string(GetLastError()) + ")";
            return resp;
        }
        if (avail == 0) break;
        const size_t prev_size = resp.body.size();
        resp.body.resize(prev_size + avail);
        DWORD read_bytes = 0;
        if (!WinHttpReadData(request.h, resp.body.data() + prev_size, avail, &read_bytes)) {
            resp.error = "WinHttpReadData failed (" + std::to_string(GetLastError()) + ")";
            return resp;
        }
        resp.body.resize(prev_size + read_bytes);
    }

    return resp;
}

bool winhttp_download_to_file(const std::string& url_utf8,
                              const std::vector<std::wstring>& headers,
                              const std::filesystem::path& dest_path,
                              const std::atomic<bool>& cancel_flag,
                              int64_t& out_bytes,
                              std::string& out_error) {
    out_bytes = 0;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path_query;
    bool is_https = true;
    const std::wstring url_w = utf8_to_wide(url_utf8);
    if (!crack_url_components(url_w, host, port, path_query, is_https)) {
        out_error = "Invalid download URL";
        return false;
    }

    ScopedWinHttp session(WinHttpOpen(L"IncunaReader/1.1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        out_error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 20000, 60000, 60000);

    ScopedWinHttp connect(WinHttpConnect(session.h, host.c_str(), port, 0));
    if (!connect.h) {
        out_error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    const DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttp request(WinHttpOpenRequest(connect.h, L"GET", path_query.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) {
        out_error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    for (const auto& hdr : headers) {
        WinHttpAddRequestHeaders(request.h, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        out_error = "WinHTTP download request failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(request.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status_code < 200 || status_code >= 300) {
        out_error = "Cloud download returned HTTP " + std::to_string(status_code);
        return false;
    }

    std::error_code ec;
    if (dest_path.has_parent_path()) {
        std::filesystem::create_directories(dest_path.parent_path(), ec);
    }

    std::filesystem::path part_path = dest_path;
    part_path += L".part";

    {
        std::ofstream ofs(part_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            out_error = "Failed to open local destination file for writing";
            return false;
        }

        std::vector<char> buffer(65536);
        for (;;) {
            if (cancel_flag.load()) {
                ofs.close();
                std::filesystem::remove(part_path, ec);
                out_error = "Download cancelled";
                return false;
            }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request.h, &avail)) {
                out_error = "WinHttpQueryDataAvailable failed";
                ofs.close();
                std::filesystem::remove(part_path, ec);
                return false;
            }
            if (avail == 0) break;

            DWORD remaining = avail;
            while (remaining > 0) {
                const DWORD to_read = std::min<DWORD>(remaining, static_cast<DWORD>(buffer.size()));
                DWORD got = 0;
                if (!WinHttpReadData(request.h, buffer.data(), to_read, &got)) {
                    out_error = "WinHttpReadData failed";
                    ofs.close();
                    std::filesystem::remove(part_path, ec);
                    return false;
                }
                if (got == 0) break;
                ofs.write(buffer.data(), static_cast<std::streamsize>(got));
                if (!ofs.good()) {
                    out_error = "Disk write failed while saving cloud file";
                    ofs.close();
                    std::filesystem::remove(part_path, ec);
                    return false;
                }
                out_bytes += static_cast<int64_t>(got);
                remaining -= got;
            }
        }
        ofs.flush();
    }

    if (!MoveFileExW(part_path.c_str(), dest_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::rename(part_path, dest_path, ec);
        if (ec) {
            std::filesystem::remove(part_path, ec);
            out_error = "Failed to finalize downloaded file";
            return false;
        }
    }
    return true;
}

std::string extract_oauth_error_message(const HttpResponse& resp) {
    if (!resp.error.empty()) return resp.error;
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.is_object()) {
            if (j.contains("error_description") && j["error_description"].is_string()) {
                return j["error_description"].get<std::string>();
            }
            if (j.contains("error") && j["error"].is_object() &&
                j["error"].contains("message") && j["error"]["message"].is_string()) {
                return j["error"]["message"].get<std::string>();
            }
            if (j.contains("error") && j["error"].is_string()) {
                return j["error"].get<std::string>();
            }
        }
    } catch (...) {}
    return "HTTP " + std::to_string(resp.status_code);
}

bool dpapi_encrypt_to_base64(const std::string& plaintext, std::string& out_b64) {
    out_b64.clear();
    DATA_BLOB in_blob{};
    in_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in_blob.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out_blob{};
    if (!CryptProtectData(&in_blob, L"IncunaReaderCloudTokens", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return false;
    }

    DWORD b64_len = 0;
    bool ok = false;
    if (CryptBinaryToStringA(out_blob.pbData, out_blob.cbData,
                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             nullptr, &b64_len) && b64_len > 0) {
        std::string buf(b64_len, '\0');
        if (CryptBinaryToStringA(out_blob.pbData, out_blob.cbData,
                                 CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 buf.data(), &b64_len)) {
            while (!buf.empty() && buf.back() == '\0') buf.pop_back();
            out_b64 = std::move(buf);
            ok = true;
        }
    }
    LocalFree(out_blob.pbData);
    return ok;
}

bool dpapi_decrypt_from_base64(const std::string& b64, std::string& out_plaintext) {
    out_plaintext.clear();
    DWORD bin_len = 0;
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()),
                              CRYPT_STRING_BASE64, nullptr, &bin_len, nullptr, nullptr) || bin_len == 0) {
        return false;
    }
    std::vector<BYTE> bin(bin_len);
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()),
                              CRYPT_STRING_BASE64, bin.data(), &bin_len, nullptr, nullptr)) {
        return false;
    }

    DATA_BLOB in_blob{};
    in_blob.pbData = bin.data();
    in_blob.cbData = bin_len;

    DATA_BLOB out_blob{};
    if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return false;
    }
    out_plaintext.assign(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    SecureZeroMemory(out_blob.pbData, out_blob.cbData);
    LocalFree(out_blob.pbData);
    return true;
}

} // namespace

// ============================================================================
// CloudClient
// ============================================================================

CloudClient::CloudClient(Database& db,
                         CoverManager& cover_manager,
                         SettingsManager& settings,
                         std::filesystem::path tokens_path,
                         std::filesystem::path offline_cache_dir)
    : db_(db),
      cover_manager_(cover_manager),
      settings_(settings),
      tokens_path_(std::move(tokens_path)),
      offline_cache_dir_(std::move(offline_cache_dir)) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsa_initialized_ = true;
    }
    load_tokens();
}

CloudClient::~CloudClient() {
    stop();
    if (wsa_initialized_) {
        WSACleanup();
    }
}

void CloudClient::stop() {
    auth_cancel_.store(true);
    import_cancel_.store(true);

    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        if (auth_thread_.joinable()) {
            auth_thread_.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(import_mutex_);
        if (import_thread_.joinable()) {
            import_thread_.join();
        }
    }
}

bool CloudClient::is_valid_provider(const std::string& provider) {
    return provider == "gdrive" || provider == "onedrive";
}

void CloudClient::load_tokens() {
    std::lock_guard<std::mutex> lock(token_mutex_);
    tokens_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(tokens_path_, ec)) return;

    std::ifstream ifs(tokens_path_, std::ios::binary);
    if (!ifs.is_open()) return;

    try {
        nlohmann::json j;
        ifs >> j;
        if (!j.is_object()) return;

        bool migrated_from_plaintext = false;
        if (j.contains("dpapi") && j["dpapi"].is_string()) {
            std::string decrypted;
            if (!dpapi_decrypt_from_base64(j["dpapi"].get<std::string>(), decrypted)) {
                return;
            }
            j = nlohmann::json::parse(decrypted);
            if (!j.is_object()) return;
        } else if (j.contains("gdrive") || j.contains("onedrive")) {
            migrated_from_plaintext = true;
        }

        for (const char* prov : {"gdrive", "onedrive"}) {
            if (!j.contains(prov) || !j[prov].is_object()) continue;
            const auto& obj = j[prov];
            ProviderToken t;
            t.client_id = obj.value("client_id", "");
            t.client_secret = obj.value("client_secret", "");
            t.access_token = obj.value("access_token", "");
            t.refresh_token = obj.value("refresh_token", "");
            t.expires_at = obj.value("expires_at", int64_t{0});
            if (!t.access_token.empty() || !t.refresh_token.empty()) {
                tokens_[prov] = std::move(t);
            }
        }

        // Transparent one-time migration of legacy v1.0.0 plaintext cloud_tokens.json to DPAPI
        if (migrated_from_plaintext && !tokens_.empty()) {
            save_tokens_locked();
        }
    } catch (...) {
        // Ignore corrupt token file
    }
}

bool CloudClient::save_tokens_locked() const {
    std::error_code ec;
    if (tokens_path_.has_parent_path()) {
        std::filesystem::create_directories(tokens_path_.parent_path(), ec);
    }

    nlohmann::json root = nlohmann::json::object();
    for (const auto& kv : tokens_) {
        root[kv.first] = {
            {"client_id", kv.second.client_id},
            {"client_secret", kv.second.client_secret},
            {"access_token", kv.second.access_token},
            {"refresh_token", kv.second.refresh_token},
            {"expires_at", kv.second.expires_at}
        };
    }

    const std::string plaintext = root.dump();
    std::string encrypted_b64;
    if (!dpapi_encrypt_to_base64(plaintext, encrypted_b64)) {
        return false;
    }
    const nlohmann::json envelope{{"dpapi", encrypted_b64}};

    std::filesystem::path tmp_path = tokens_path_;
    tmp_path += L".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs << envelope.dump(2) << "\n";
        ofs.flush();
        if (!ofs.good()) return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), tokens_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::rename(tmp_path, tokens_path_, ec);
        if (ec) return false;
    }
    return true;
}

CloudAuthStartResult CloudClient::start_auth(const std::string& provider,
                                             const std::string& client_id,
                                             const std::string& client_secret) {
    CloudAuthStartResult result;
    result.provider = provider;

    if (!is_valid_provider(provider)) {
        result.error = "Unsupported provider (expected 'gdrive' or 'onedrive')";
        return result;
    }
    if (client_id.empty()) {
        result.error = "Missing 'client_id'";
        return result;
    }
    if (!wsa_initialized_) {
        result.error = "Winsock failed to initialize";
        return result;
    }

    // Cancel any previous pending auth listener cleanly
    auth_cancel_.store(true);
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        if (auth_thread_.joinable()) {
            auth_thread_.join();
        }
        auth_cancel_.store(false);
        auth_error_.clear();
        auth_provider_ = provider;
    }

    // Bind ephemeral 127.0.0.1 loopback socket synchronously so we know redirect_uri now
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        result.error = "Failed to create loopback socket";
        return result;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listen_sock, 4) == SOCKET_ERROR) {
        closesocket(listen_sock);
        result.error = "Failed to bind local OAuth loopback listener on 127.0.0.1";
        return result;
    }

    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(listen_sock, reinterpret_cast<sockaddr*>(&bound), &bound_len) == SOCKET_ERROR) {
        closesocket(listen_sock);
        result.error = "Failed to query loopback listener port";
        return result;
    }

    const uint16_t port = ntohs(bound.sin_port);
    const std::string redirect_uri = "http://127.0.0.1:" + std::to_string(port) + "/callback";
    const std::string state = random_base64url(24);
    const std::string code_verifier = random_base64url(32);
    const std::string code_challenge = sha256_base64url(code_verifier);

    std::string auth_url;
    if (provider == "gdrive") {
        auth_url =
            "https://accounts.google.com/o/oauth2/v2/auth"
            "?client_id=" + url_encode(client_id) +
            "&redirect_uri=" + url_encode(redirect_uri) +
            "&response_type=code"
            "&scope=" + url_encode("https://www.googleapis.com/auth/drive.readonly") +
            "&access_type=offline"
            "&prompt=consent"
            "&state=" + url_encode(state) +
            "&code_challenge=" + url_encode(code_challenge) +
            "&code_challenge_method=S256";
    } else {
        auth_url =
            "https://login.microsoftonline.com/common/oauth2/v2.0/authorize"
            "?client_id=" + url_encode(client_id) +
            "&redirect_uri=" + url_encode(redirect_uri) +
            "&response_type=code"
            "&scope=" + url_encode("Files.Read.All offline_access") +
            "&state=" + url_encode(state) +
            "&code_challenge=" + url_encode(code_challenge) +
            "&code_challenge_method=S256";
    }

    auth_in_progress_.store(true);

    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        auth_thread_ = std::thread(
            [this, listen_sock, provider, client_id, client_secret, redirect_uri, state, code_verifier]() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
                SOCKET client_sock = INVALID_SOCKET;

                while (!auth_cancel_.load() && std::chrono::steady_clock::now() < deadline) {
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET(listen_sock, &read_fds);
                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = 250000; // 250ms poll for responsive cancel/shutdown

                    int ready = select(0, &read_fds, nullptr, nullptr, &tv);
                    if (ready > 0 && FD_ISSET(listen_sock, &read_fds)) {
                        client_sock = accept(listen_sock, nullptr, nullptr);
                        if (client_sock != INVALID_SOCKET) {
                            break;
                        }
                    } else if (ready == SOCKET_ERROR) {
                        break;
                    }
                }

                closesocket(listen_sock);

                if (client_sock == INVALID_SOCKET) {
                    if (!auth_cancel_.load()) {
                        std::lock_guard<std::mutex> lk(auth_mutex_);
                        auth_error_ = "OAuth authorization timed out";
                    }
                    auth_in_progress_.store(false);
                    return;
                }

                // Read HTTP GET request line
                DWORD recv_timeout = 5000;
                setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
                std::string raw_req;
                char buf[4096];
                int n = recv(client_sock, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    raw_req.assign(buf, static_cast<size_t>(n));
                }

                std::string code;
                std::string got_state;
                std::string oauth_err;

                // Parse "GET /callback?code=...&state=... HTTP/1.1"
                const size_t first_sp = raw_req.find(' ');
                if (first_sp != std::string::npos) {
                    const size_t second_sp = raw_req.find(' ', first_sp + 1);
                    if (second_sp != std::string::npos) {
                        const std::string target = raw_req.substr(first_sp + 1, second_sp - first_sp - 1);
                        const size_t qm = target.find('?');
                        if (qm != std::string::npos) {
                            auto params = parse_query_string(target.substr(qm + 1));
                            if (params.count("code")) code = params["code"];
                            if (params.count("state")) got_state = params["state"];
                            if (params.count("error_description")) oauth_err = params["error_description"];
                            else if (params.count("error")) oauth_err = params["error"];
                        }
                    }
                }

                bool exchanged = false;
                std::string final_err;
                if (!oauth_err.empty()) {
                    final_err = oauth_err;
                } else if (got_state != state) {
                    final_err = "OAuth state mismatch";
                } else if (code.empty()) {
                    final_err = "Missing authorization code in callback";
                } else {
                    exchanged = exchange_auth_code(
                        provider, client_id, client_secret, code, redirect_uri, code_verifier, final_err);
                }

                const std::string html = exchanged
                    ? "<!doctype html><html><head><meta charset='utf-8'><title>Incuna Reader</title></head>"
                      "<body style='font-family:sans-serif;background:#111;color:#eee;display:flex;"
                      "align-items:center;justify-content:center;height:100vh;margin:0'>"
                      "<div style='text-align:center'><h2>Connected to Incuna Reader</h2>"
                      "<p>Authorization succeeded. You may close this browser tab.</p></div></body></html>"
                    : "<!doctype html><html><head><meta charset='utf-8'><title>Incuna Reader</title></head>"
                      "<body style='font-family:sans-serif;background:#111;color:#f88;display:flex;"
                      "align-items:center;justify-content:center;height:100vh;margin:0'>"
                      "<div style='text-align:center'><h2>Authorization Failed</h2>"
                      "<p>Please return to Incuna Reader and try again.</p></div></body></html>";

                std::ostringstream http_reply;
                http_reply << "HTTP/1.1 " << (exchanged ? "200 OK" : "400 Bad Request") << "\r\n"
                           << "Content-Type: text/html; charset=utf-8\r\n"
                           << "Content-Length: " << html.size() << "\r\n"
                           << "Connection: close\r\n\r\n"
                           << html;
                const std::string reply_str = http_reply.str();
                send(client_sock, reply_str.data(), static_cast<int>(reply_str.size()), 0);
                closesocket(client_sock);

                {
                    std::lock_guard<std::mutex> lk(auth_mutex_);
                    auth_error_ = exchanged ? "" : final_err;
                }
                auth_in_progress_.store(false);
            });
    }

    // Open the system default browser to the OAuth2 authorization page
    const std::wstring auth_url_w = utf8_to_wide(auth_url);
    ShellExecuteW(nullptr, L"open", auth_url_w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    result.started = true;
    result.auth_url = auth_url;
    result.redirect_uri = redirect_uri;
    return result;
}

bool CloudClient::exchange_auth_code(const std::string& provider,
                                     const std::string& client_id,
                                     const std::string& client_secret,
                                     const std::string& code,
                                     const std::string& redirect_uri,
                                     const std::string& code_verifier,
                                     std::string& out_error) {
    const std::string token_url = (provider == "gdrive")
        ? "https://oauth2.googleapis.com/token"
        : "https://login.microsoftonline.com/common/oauth2/v2.0/token";

    std::string form =
        "grant_type=authorization_code"
        "&client_id=" + url_encode(client_id) +
        "&code=" + url_encode(code) +
        "&redirect_uri=" + url_encode(redirect_uri) +
        "&code_verifier=" + url_encode(code_verifier);
    if (!client_secret.empty()) {
        form += "&client_secret=" + url_encode(client_secret);
    }

    HttpResponse resp = winhttp_request(
        L"POST",
        token_url,
        {L"Content-Type: application/x-www-form-urlencoded\r\n"},
        form);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        out_error = "Token exchange failed: " + extract_oauth_error_message(resp);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp.body);
        const std::string access_token = j.value("access_token", "");
        if (access_token.empty()) {
            out_error = "Token response missing access_token";
            return false;
        }
        const std::string refresh_token = j.value("refresh_token", "");
        const int64_t expires_in = j.value("expires_in", int64_t{3600});

        std::lock_guard<std::mutex> lock(token_mutex_);
        ProviderToken t;
        t.client_id = client_id;
        t.client_secret = client_secret;
        t.access_token = access_token;
        t.refresh_token = refresh_token;
        t.expires_at = unix_now_seconds() + expires_in;
        tokens_[provider] = std::move(t);
        save_tokens_locked();
        return true;
    } catch (const std::exception& ex) {
        out_error = std::string("Failed to parse token JSON: ") + ex.what();
        return false;
    }
}

bool CloudClient::ensure_valid_access_token(const std::string& provider,
                                            std::string& out_token,
                                            std::string& out_error) {
    {
        std::lock_guard<std::mutex> lock(token_mutex_);
        auto it = tokens_.find(provider);
        if (it == tokens_.end() || (it->second.access_token.empty() && it->second.refresh_token.empty())) {
            out_error = "Provider '" + provider + "' is not connected";
            return false;
        }
        const int64_t now = unix_now_seconds();
        if (!it->second.access_token.empty() && (it->second.expires_at == 0 || now + 60 < it->second.expires_at)) {
            out_token = it->second.access_token;
            return true;
        }
    }

    // Per-provider refresh serialization with double-checked locking so concurrent
    // requests never double-refresh and invalidate rotating OneDrive refresh tokens.
    std::mutex& refresh_mtx = (provider == "gdrive") ? gdrive_refresh_mutex_ : onedrive_refresh_mutex_;
    std::lock_guard<std::mutex> refresh_lock(refresh_mtx);

    ProviderToken copy;
    {
        std::lock_guard<std::mutex> lock(token_mutex_);
        auto it = tokens_.find(provider);
        if (it == tokens_.end() || (it->second.access_token.empty() && it->second.refresh_token.empty())) {
            out_error = "Provider '" + provider + "' is not connected";
            return false;
        }
        const int64_t now = unix_now_seconds();
        if (!it->second.access_token.empty() && (it->second.expires_at == 0 || now + 60 < it->second.expires_at)) {
            out_token = it->second.access_token;
            return true;
        }
        copy = it->second;
    }

    if (copy.refresh_token.empty()) {
        out_error = "Access token expired and no refresh token is available; please reconnect";
        return false;
    }

    const std::string token_url = (provider == "gdrive")
        ? "https://oauth2.googleapis.com/token"
        : "https://login.microsoftonline.com/common/oauth2/v2.0/token";

    std::string form =
        "grant_type=refresh_token"
        "&client_id=" + url_encode(copy.client_id) +
        "&refresh_token=" + url_encode(copy.refresh_token);
    if (!copy.client_secret.empty()) {
        form += "&client_secret=" + url_encode(copy.client_secret);
    }
    if (provider == "onedrive") {
        form += "&scope=" + url_encode("Files.Read.All offline_access");
    }

    HttpResponse resp = winhttp_request(
        L"POST",
        token_url,
        {L"Content-Type: application/x-www-form-urlencoded\r\n"},
        form);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        out_error = "Token refresh failed: " + extract_oauth_error_message(resp);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp.body);
        const std::string new_access = j.value("access_token", "");
        if (new_access.empty()) {
            out_error = "Refresh response missing access_token";
            return false;
        }
        const std::string new_refresh = j.value("refresh_token", "");
        const int64_t expires_in = j.value("expires_in", int64_t{3600});

        std::lock_guard<std::mutex> lock(token_mutex_);
        auto& tok = tokens_[provider];
        tok.access_token = new_access;
        if (!new_refresh.empty()) {
            tok.refresh_token = new_refresh;
        }
        tok.expires_at = unix_now_seconds() + expires_in;
        save_tokens_locked();
        out_token = tok.access_token;
        return true;
    } catch (const std::exception& ex) {
        out_error = std::string("Failed to parse refresh JSON: ") + ex.what();
        return false;
    }
}

nlohmann::json CloudClient::get_status() const {
    auto provider_json = [&](const std::string& prov) {
        std::lock_guard<std::mutex> lock(token_mutex_);
        auto it = tokens_.find(prov);
        if (it == tokens_.end() || (it->second.access_token.empty() && it->second.refresh_token.empty())) {
            return nlohmann::json{
                {"connected", false},
                {"client_id", ""},
                {"expires_at", 0},
                {"has_refresh_token", false}
            };
        }
        return nlohmann::json{
            {"connected", true},
            {"client_id", it->second.client_id},
            {"expires_at", it->second.expires_at},
            {"has_refresh_token", !it->second.refresh_token.empty()}
        };
    };

    std::string a_prov;
    std::string a_err;
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        a_prov = auth_provider_;
        a_err = auth_error_;
    }

    std::string i_prov;
    std::string i_cur;
    std::string i_err;
    {
        std::lock_guard<std::mutex> lock(import_mutex_);
        i_prov = import_provider_;
        i_cur = import_current_file_;
        i_err = import_last_error_;
    }

    return nlohmann::json{
        {"gdrive", provider_json("gdrive")},
        {"onedrive", provider_json("onedrive")},
        {"auth_in_progress", auth_in_progress_.load()},
        {"auth_provider", a_prov},
        {"auth_error", a_err},
        {"import", {
            {"running", import_running_.load()},
            {"provider", i_prov},
            {"total", import_total_.load()},
            {"completed", import_completed_.load()},
            {"failed", import_failed_.load()},
            {"current_file", i_cur},
            {"last_error", i_err}
        }}
    };
}

bool CloudClient::disconnect(const std::string& provider) {
    if (!is_valid_provider(provider)) return false;
    std::lock_guard<std::mutex> lock(token_mutex_);
    tokens_.erase(provider);
    return save_tokens_locked();
}

CloudListResult CloudClient::list_folder(const std::string& provider, const std::string& folder_id) {
    CloudListResult res;
    if (!is_valid_provider(provider)) {
        res.http_status = 400;
        res.error = "Unsupported provider (expected 'gdrive' or 'onedrive')";
        return res;
    }

    std::string access_token;
    if (!ensure_valid_access_token(provider, access_token, res.error)) {
        res.http_status = 401;
        return res;
    }

    const std::wstring auth_hdr = L"Authorization: Bearer " + utf8_to_wide(access_token) + L"\r\n";

    constexpr int kMaxPages = 50;
    constexpr size_t kMaxItems = 10000;

    if (provider == "gdrive") {
        const std::string parent = folder_id.empty() ? "root" : folder_id;
        std::string escaped_parent;
        escaped_parent.reserve(parent.size());
        for (char ch : parent) {
            if (ch == '\'' || ch == '\\') escaped_parent.push_back('\\');
            escaped_parent.push_back(ch);
        }
        const std::string q = "'" + escaped_parent + "' in parents and trashed = false";
        const std::string base_url =
            "https://www.googleapis.com/drive/v3/files"
            "?q=" + url_encode(q) +
            "&fields=" + url_encode("nextPageToken,files(id,name,mimeType,size,modifiedTime)") +
            "&pageSize=200"
            "&orderBy=" + url_encode("folder,name");

        std::string page_token;
        for (int page = 0; page < kMaxPages && res.items.size() < kMaxItems; ++page) {
            const std::string url = page_token.empty()
                ? base_url
                : (base_url + "&pageToken=" + url_encode(page_token));

            HttpResponse resp = winhttp_request(L"GET", url, {auth_hdr}, "");
            if (resp.status_code < 200 || resp.status_code >= 300) {
                res.http_status = resp.status_code > 0 ? resp.status_code : 502;
                res.error = "Google Drive list failed: " + extract_oauth_error_message(resp);
                res.ok = false;
                return res;
            }

            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.contains("files") && j["files"].is_array()) {
                    for (const auto& f : j["files"]) {
                        if (res.items.size() >= kMaxItems) break;
                        const std::string id = f.value("id", "");
                        const std::string name = f.value("name", "");
                        const std::string mime = f.value("mimeType", "");
                        if (id.empty() || name.empty()) continue;

                        const bool is_folder = (mime == "application/vnd.google-apps.folder");
                        std::string format;
                        if (!is_folder && !extract_comic_format(name, format)) {
                            continue;
                        }

                        int64_t sz = 0;
                        if (f.contains("size")) {
                            if (f["size"].is_string()) {
                                try { sz = std::stoll(f["size"].get<std::string>()); } catch (...) {}
                            } else if (f["size"].is_number_integer()) {
                                sz = f["size"].get<int64_t>();
                            }
                        }

                        CloudFileEntry entry;
                        entry.id = id;
                        entry.name = name;
                        entry.is_folder = is_folder;
                        entry.size = sz;
                        entry.format = format;
                        entry.modified_at = f.value("modifiedTime", "");
                        res.items.push_back(std::move(entry));
                    }
                }
                page_token = j.value("nextPageToken", "");
                res.ok = true;
                if (page_token.empty()) break;
            } catch (const std::exception& ex) {
                res.http_status = 502;
                res.error = std::string("Failed to parse Google Drive response: ") + ex.what();
                res.ok = false;
                return res;
            }
        }
    } else {
        std::string url;
        if (folder_id.empty() || folder_id == "root") {
            url = "https://graph.microsoft.com/v1.0/me/drive/root/children"
                  "?$select=id,name,size,folder,file,lastModifiedDateTime&$top=200";
        } else {
            url = "https://graph.microsoft.com/v1.0/me/drive/items/" + url_encode(folder_id) +
                  "/children?$select=id,name,size,folder,file,lastModifiedDateTime&$top=200";
        }

        for (int page = 0; page < kMaxPages && !url.empty() && res.items.size() < kMaxItems; ++page) {
            HttpResponse resp = winhttp_request(L"GET", url, {auth_hdr}, "");
            if (resp.status_code < 200 || resp.status_code >= 300) {
                res.http_status = resp.status_code > 0 ? resp.status_code : 502;
                res.error = "OneDrive list failed: " + extract_oauth_error_message(resp);
                res.ok = false;
                return res;
            }

            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.contains("value") && j["value"].is_array()) {
                    for (const auto& f : j["value"]) {
                        if (res.items.size() >= kMaxItems) break;
                        const std::string id = f.value("id", "");
                        const std::string name = f.value("name", "");
                        if (id.empty() || name.empty()) continue;

                        const bool is_folder = f.contains("folder") && f["folder"].is_object();
                        std::string format;
                        if (!is_folder && !extract_comic_format(name, format)) {
                            continue;
                        }

                        CloudFileEntry entry;
                        entry.id = id;
                        entry.name = name;
                        entry.is_folder = is_folder;
                        entry.size = f.value("size", int64_t{0});
                        entry.format = format;
                        entry.modified_at = f.value("lastModifiedDateTime", "");
                        res.items.push_back(std::move(entry));
                    }
                }
                url = j.value("@odata.nextLink", "");
                res.ok = true;
            } catch (const std::exception& ex) {
                res.http_status = 502;
                res.error = std::string("Failed to parse OneDrive response: ") + ex.what();
                res.ok = false;
                return res;
            }
        }
    }

    // Sort folders before files, then case-insensitive name
    std::sort(res.items.begin(), res.items.end(), [](const CloudFileEntry& a, const CloudFileEntry& b) {
        if (a.is_folder != b.is_folder) return a.is_folder > b.is_folder;
        return to_lower_ascii(a.name) < to_lower_ascii(b.name);
    });

    return res;
}

bool CloudClient::download_gdrive_file(const std::string& access_token,
                                       const std::string& file_id,
                                       const std::filesystem::path& dest_path,
                                       int64_t& out_bytes,
                                       std::string& out_error) {
    const std::string url =
        "https://www.googleapis.com/drive/v3/files/" + url_encode(file_id) + "?alt=media";
    const std::wstring auth_hdr = L"Authorization: Bearer " + utf8_to_wide(access_token) + L"\r\n";
    return winhttp_download_to_file(url, {auth_hdr}, dest_path, import_cancel_, out_bytes, out_error);
}

bool CloudClient::download_onedrive_file(const std::string& access_token,
                                         const std::string& item_id,
                                         const std::filesystem::path& dest_path,
                                         int64_t& out_bytes,
                                         std::string& out_error) {
    // First query Graph metadata for pre-authenticated @microsoft.graph.downloadUrl
    // so we can stream directly without cross-host Authorization header issues.
    const std::string meta_url =
        "https://graph.microsoft.com/v1.0/me/drive/items/" + url_encode(item_id) +
        "?$select=id,name,size,@microsoft.graph.downloadUrl";
    const std::wstring auth_hdr = L"Authorization: Bearer " + utf8_to_wide(access_token) + L"\r\n";

    HttpResponse meta = winhttp_request(L"GET", meta_url, {auth_hdr}, "");
    std::string download_url;
    if (meta.status_code >= 200 && meta.status_code < 300) {
        try {
            auto j = nlohmann::json::parse(meta.body);
            if (j.contains("@microsoft.graph.downloadUrl") && j["@microsoft.graph.downloadUrl"].is_string()) {
                download_url = j["@microsoft.graph.downloadUrl"].get<std::string>();
            }
        } catch (...) {}
    }

    if (!download_url.empty()) {
        return winhttp_download_to_file(download_url, {}, dest_path, import_cancel_, out_bytes, out_error);
    }

    // Fallback to /content endpoint
    const std::string fallback_url =
        "https://graph.microsoft.com/v1.0/me/drive/items/" + url_encode(item_id) + "/content";
    return winhttp_download_to_file(fallback_url, {auth_hdr}, dest_path, import_cancel_, out_bytes, out_error);
}

void CloudClient::collect_import_files_recursive(
    const std::string& provider,
    const std::string& folder_id,
    const std::filesystem::path& rel_subdir,
    int depth,
    std::vector<std::pair<CloudFileEntry, std::filesystem::path>>& out_files) {
    if (depth > 8 || import_cancel_.load()) return;

    CloudListResult listed = list_folder(provider, folder_id);
    if (!listed.ok) return;

    for (const auto& entry : listed.items) {
        if (import_cancel_.load()) return;
        if (entry.is_folder) {
            const std::filesystem::path next_sub = rel_subdir / utf8_to_wide(sanitize_filename(entry.name));
            collect_import_files_recursive(provider, entry.id, next_sub, depth + 1, out_files);
        } else {
            out_files.emplace_back(entry, rel_subdir);
        }
    }
}

CloudImportStartResult CloudClient::start_import(const std::string& provider,
                                                 const std::vector<CloudImportItem>& items) {
    CloudImportStartResult res;
    if (!is_valid_provider(provider)) {
        res.error = "Unsupported provider (expected 'gdrive' or 'onedrive')";
        return res;
    }
    if (items.empty()) {
        res.error = "No files or folders selected for import";
        return res;
    }

    std::string token_check;
    std::string token_err;
    if (!ensure_valid_access_token(provider, token_check, token_err)) {
        res.error = token_err;
        return res;
    }

    if (import_running_.exchange(true)) {
        res.error = "A cloud import is already in progress";
        return res;
    }

    import_cancel_.store(false);
    import_total_.store(static_cast<int64_t>(items.size()));
    import_completed_.store(0);
    import_failed_.store(0);

    {
        std::lock_guard<std::mutex> lock(import_mutex_);
        if (import_thread_.joinable()) {
            import_thread_.join();
        }
        import_provider_ = provider;
        import_current_file_.clear();
        import_last_error_.clear();

        import_thread_ = std::thread([this, provider, items]() {
            const std::filesystem::path provider_dir = offline_cache_dir_ / utf8_to_wide(provider);
            std::error_code ec;
            std::filesystem::create_directories(provider_dir, ec);

            // Expand any selected folders into concrete downloadable comic files
            std::vector<std::pair<CloudFileEntry, std::filesystem::path>> queue;
            for (const auto& item : items) {
                if (import_cancel_.load()) break;
                if (item.is_folder) {
                    const std::filesystem::path sub =
                        item.name.empty() ? std::filesystem::path{} : std::filesystem::path(utf8_to_wide(sanitize_filename(item.name)));
                    collect_import_files_recursive(provider, item.id, sub, 0, queue);
                } else {
                    CloudFileEntry fe;
                    fe.id = item.id;
                    fe.name = item.name.empty() ? (item.id + ".cbz") : item.name;
                    fe.is_folder = false;
                    extract_comic_format(fe.name, fe.format);
                    queue.emplace_back(std::move(fe), std::filesystem::path{});
                }
            }

            import_total_.store(static_cast<int64_t>(queue.size()));
            int64_t succeeded_count = 0;

            for (const auto& [entry, rel_sub] : queue) {
                if (import_cancel_.load()) break;

                {
                    std::lock_guard<std::mutex> lk(import_mutex_);
                    import_current_file_ = entry.name;
                }

                std::string access_token;
                std::string err;
                if (!ensure_valid_access_token(provider, access_token, err)) {
                    ++import_failed_;
                    std::lock_guard<std::mutex> lk(import_mutex_);
                    import_last_error_ = err;
                    continue;
                }

                const std::string safe_name = sanitize_filename(entry.name);
                const std::filesystem::path dest_dir = rel_sub.empty() ? provider_dir : (provider_dir / rel_sub);
                std::filesystem::create_directories(dest_dir, ec);
                const std::filesystem::path dest_path = dest_dir / utf8_to_wide(safe_name);

                int64_t downloaded_bytes = 0;
                const bool ok = (provider == "gdrive")
                    ? download_gdrive_file(access_token, entry.id, dest_path, downloaded_bytes, err)
                    : download_onedrive_file(access_token, entry.id, dest_path, downloaded_bytes, err);

                if (!ok) {
                    ++import_failed_;
                    std::lock_guard<std::mutex> lk(import_mutex_);
                    import_last_error_ = err;
                    continue;
                }

                std::string fmt = entry.format;
                if (fmt.empty()) {
                    extract_comic_format(safe_name, fmt);
                    if (fmt.empty()) fmt = "cbz";
                }

                const std::string local_utf8 = wide_to_utf8(dest_path.wstring());
                const std::string folder_utf8 = wide_to_utf8(dest_path.parent_path().wstring());
                const std::string title_utf8 = wide_to_utf8(dest_path.stem().wstring());

                ComicUpsert upsert;
                upsert.path = local_utf8;
                upsert.relative_path = wide_to_utf8((std::filesystem::path(utf8_to_wide(provider)) / rel_sub / utf8_to_wide(safe_name)).wstring());
                upsert.folder_path = folder_utf8;
                upsert.title = title_utf8.empty() ? safe_name : title_utf8;
                upsert.format = fmt;
                upsert.file_size_bytes = downloaded_bytes;

                if (db_.upsert_comics_batch({upsert})) {
                    ComicRecord rec;
                    if (db_.get_comic_by_path(local_utf8, rec)) {
                        db_.update_comic_offline_pin(rec.id, true, local_utf8);
                    }
                    ++succeeded_count;
                    ++import_completed_;
                } else {
                    ++import_failed_;
                    std::lock_guard<std::mutex> lk(import_mutex_);
                    import_last_error_ = db_.last_error();
                }
            }

            if (succeeded_count > 0) {
                auto cfg = settings_.get();
                if (cfg.value("cover_mode", "persistent_pack") == "persistent_pack") {
                    cover_manager_.start_pack_build();
                }
            }

            {
                std::lock_guard<std::mutex> lk(import_mutex_);
                import_current_file_.clear();
            }
            import_running_.store(false);
        });
    }

    res.started = true;
    res.queued = static_cast<int64_t>(items.size());
    return res;
}

} // namespace koma
