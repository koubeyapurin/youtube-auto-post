// Windows socket headers MUST come before anything that pulls in <windows.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#include "youtube_uploader.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

// ── libcurl callbacks ─────────────────────────────────────────────────────────
static size_t yt_write_string(char* p, size_t sz, size_t n, void* ud)
{
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

// Captures the value of the Location: response header
static size_t yt_capture_location(char* buf, size_t sz, size_t n, void* ud)
{
    auto* out = static_cast<std::string*>(ud);
    const std::string hdr(buf, sz * n);

    // case-insensitive prefix check for "location:"
    std::string lower = hdr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.rfind("location:", 0) == 0) {
        const auto colon = hdr.find(": ");
        const auto last  = hdr.find_last_not_of("\r\n \t");
        if (colon != std::string::npos && last > colon + 1)
            *out = hdr.substr(colon + 2, last - (colon + 2) + 1);
    }
    return sz * n;
}

// ── URL-encode a string using libcurl ────────────────────────────────────────
static std::string url_encode(const std::string& s)
{
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* enc = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string result(enc ? enc : s);
    if (enc) curl_free(enc);
    curl_easy_cleanup(c);
    return result;
}

// ── Simple HTTP POST (form or JSON) ─────────────────────────────────────────
static std::string yt_http_post(const std::string& url,
                                 const std::string& body,
                                 curl_slist* headers,
                                 long timeout_sec = 60)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  yt_write_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout_sec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    const CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (res != CURLE_OK)
        throw std::runtime_error(std::string("yt_http_post curl: ") +
                                 curl_easy_strerror(res));
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
YoutubeUploader::YoutubeUploader(const std::string& client_secret_path,
                                  const std::string& token_path)
    : token_path_(token_path)
{
    load_client_secret(client_secret_path);
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::load_client_secret(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open client_secret: " + path);

    auto j = json::parse(f);
    // Supports both "installed" and "web" credential types
    const auto& creds = j.contains("installed") ? j["installed"] : j["web"];
    client_id_     = creds["client_id"].get<std::string>();
    client_secret_ = creds["client_secret"].get<std::string>();
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::load_tokens()
{
    std::ifstream f(token_path_);
    if (!f.is_open()) return;

    const auto j  = json::parse(f);
    access_token_  = j.value("access_token",  "");
    refresh_token_ = j.value("refresh_token", "");
}

void YoutubeUploader::save_tokens()
{
    std::ofstream f(token_path_);
    if (!f.is_open())
        throw std::runtime_error("Cannot write token file: " + token_path_);

    json j = {{"access_token",  access_token_},
              {"refresh_token", refresh_token_}};
    f << j.dump(2);
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::load_or_authorize()
{
    load_tokens();
    if (refresh_token_.empty()) {
        std::cout << "[OAuth] No refresh token found – starting browser auth...\n";
        do_browser_auth();
    } else {
        std::cout << "[OAuth] Refreshing access token...\n";
        refresh_access_token();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Local TCP server: blocks until browser redirects to localhost:kRedirectPort
// ─────────────────────────────────────────────────────────────────────────────
std::string YoutubeUploader::wait_for_oauth_code()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("WSAStartup failed");

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { WSACleanup(); throw std::runtime_error("socket() failed"); }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(kRedirectPort));

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(srv, 1) == SOCKET_ERROR) {
        closesocket(srv); WSACleanup();
        throw std::runtime_error("bind/listen failed on port " +
                                 std::to_string(kRedirectPort));
    }

    std::cout << "[OAuth] Listening on http://localhost:" << kRedirectPort << " ...\n";
    SOCKET cli = accept(srv, nullptr, nullptr);
    closesocket(srv);
    if (cli == INVALID_SOCKET) { WSACleanup(); throw std::runtime_error("accept() failed"); }

    char buf[8192]{};
    recv(cli, buf, sizeof(buf) - 1, 0);
    const std::string req(buf);

    // Extract code from "GET /?code=XXX[&...] HTTP/1.1"
    std::string code;
    const auto pos = req.find("code=");
    if (pos != std::string::npos) {
        const auto end = req.find_first_of(" &\r\n", pos + 5);
        code = req.substr(pos + 5, end - (pos + 5));
    }

    // Respond to browser
    const std::string html =
        "<html><head><meta charset='utf-8'></head><body>"
        "<h2 style='font-family:sans-serif;color:green'>認証完了！</h2>"
        "<p style='font-family:sans-serif'>このウィンドウを閉じてプログラムに戻ってください。</p>"
        "</body></html>";
    const std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(html.size()) + "\r\n"
        "Connection: close\r\n\r\n" + html;
    send(cli, resp.c_str(), static_cast<int>(resp.size()), 0);
    closesocket(cli);
    WSACleanup();
    return code;

#else
    // POSIX
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(kRedirectPort);

    bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(srv, 1);

    std::cout << "[OAuth] Listening on http://localhost:" << kRedirectPort << " ...\n";
    int cli = accept(srv, nullptr, nullptr);
    ::close(srv);

    char buf[8192]{};
    read(cli, buf, sizeof(buf) - 1);
    const std::string req(buf);

    std::string code;
    const auto pos = req.find("code=");
    if (pos != std::string::npos) {
        const auto end = req.find_first_of(" &\r\n", pos + 5);
        code = req.substr(pos + 5, end - (pos + 5));
    }

    const std::string html =
        "<html><head><meta charset='utf-8'></head><body>"
        "<h2>認証完了！</h2><p>このウィンドウを閉じてください。</p></body></html>";
    const std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n" + html;
    write(cli, resp.c_str(), resp.size());
    ::close(cli);
    return code;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::do_browser_auth()
{
    const std::string auth_url =
        "https://accounts.google.com/o/oauth2/auth"
        "?client_id="      + url_encode(client_id_) +
        "&redirect_uri="   + url_encode(kRedirectUri) +
        "&response_type=code"
        "&scope="          + url_encode(kScope) +
        "&access_type=offline"
        "&prompt=consent";

    std::cout << "[OAuth] Opening browser...\n"
              << "[OAuth] If it doesn't open, visit:\n  " << auth_url << "\n\n";

#ifdef _WIN32
    ShellExecuteA(nullptr, "open", auth_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open \"" + auth_url + "\"").c_str());
#else
    std::system(("xdg-open \"" + auth_url + "\"").c_str());
#endif

    const std::string code = wait_for_oauth_code();
    if (code.empty()) throw std::runtime_error("OAuth: no code received from browser");

    std::cout << "[OAuth] Auth code received. Exchanging for tokens...\n";
    exchange_code(code);
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::exchange_code(const std::string& code)
{
    const std::string url  = "https://oauth2.googleapis.com/token";
    const std::string body =
        "grant_type=authorization_code"
        "&code="          + url_encode(code) +
        "&client_id="     + url_encode(client_id_) +
        "&client_secret=" + url_encode(client_secret_) +
        "&redirect_uri="  + url_encode(kRedirectUri);

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs,
           "Content-Type: application/x-www-form-urlencoded");

    const auto resp = json::parse(yt_http_post(url, body, hdrs));
    curl_slist_free_all(hdrs);

    if (resp.contains("error"))
        throw std::runtime_error("Token exchange error: " +
                                 resp["error"].get<std::string>() +
                                 " – " + resp.value("error_description", ""));

    access_token_  = resp["access_token"].get<std::string>();
    if (resp.contains("refresh_token"))
        refresh_token_ = resp["refresh_token"].get<std::string>();

    save_tokens();
    std::cout << "[OAuth] Tokens saved to " << token_path_ << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
void YoutubeUploader::refresh_access_token()
{
    const std::string url  = "https://oauth2.googleapis.com/token";
    const std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + url_encode(refresh_token_) +
        "&client_id="     + url_encode(client_id_) +
        "&client_secret=" + url_encode(client_secret_);

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs,
           "Content-Type: application/x-www-form-urlencoded");

    const auto resp = json::parse(yt_http_post(url, body, hdrs));
    curl_slist_free_all(hdrs);

    if (resp.contains("error"))
        throw std::runtime_error("Token refresh error: " +
                                 resp["error"].get<std::string>() +
                                 " – " + resp.value("error_description", ""));

    access_token_ = resp["access_token"].get<std::string>();
    save_tokens();
    std::cout << "[OAuth] Access token refreshed.\n";
}

// ─────────────────────────────────────────────────────────────────────────────
std::string YoutubeUploader::build_metadata_json(
    const std::string&              title,
    const std::string&              description,
    const std::vector<std::string>& tags,
    const std::string&              scheduled_publish_utc)
{
    json snippet = {
        {"title",       title},
        {"description", description},
        {"tags",        tags},
        {"categoryId",  "15"}   // Pets & Animals
    };

    json status;
    if (!scheduled_publish_utc.empty()) {
        status = {
            {"privacyStatus",            "private"},
            {"publishAt",                scheduled_publish_utc},
            {"selfDeclaredMadeForKids",  false}
        };
    } else {
        status = {
            {"privacyStatus",           "public"},
            {"selfDeclaredMadeForKids", false}
        };
    }

    return json{{"snippet", snippet}, {"status", status}}.dump();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step A: POST metadata → returns the upload session URI
// ─────────────────────────────────────────────────────────────────────────────
std::string YoutubeUploader::start_resumable_upload(
    const std::string& metadata_json,
    long long          file_size)
{
    const std::string url =
        "https://www.googleapis.com/upload/youtube/v3/videos"
        "?uploadType=resumable&part=snippet,status";

    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    std::string body_buf;
    std::string location;

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + access_token_).c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json; charset=UTF-8");
    hdrs = curl_slist_append(hdrs, "X-Upload-Content-Type: video/mp4");
    hdrs = curl_slist_append(hdrs,
           ("X-Upload-Content-Length: " + std::to_string(file_size)).c_str());

    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     metadata_json.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  yt_write_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &body_buf);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, yt_capture_location);
    curl_easy_setopt(c, CURLOPT_HEADERDATA,     &location);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   // must NOT follow; Location IS the upload URI
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        30L);

    const CURLcode res = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (res != CURLE_OK)
        throw std::runtime_error("Resumable init curl: " +
                                 std::string(curl_easy_strerror(res)));
    if (http_code != 200)
        throw std::runtime_error("Resumable init HTTP " +
                                 std::to_string(http_code) + ": " + body_buf);
    if (location.empty())
        throw std::runtime_error("YouTube: no Location header in upload init response");

    return location;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step B: PUT raw bytes → returns YouTube video ID
// ─────────────────────────────────────────────────────────────────────────────
std::string YoutubeUploader::upload_bytes(const std::string& upload_url,
                                           const std::string& video_path,
                                           long long          file_size)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

// suppress MSVC fopen deprecation warning
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
    FILE* fp = fopen(video_path.c_str(), "rb");
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
    if (!fp) {
        curl_easy_cleanup(c);
        throw std::runtime_error("Cannot open video for upload: " + video_path);
    }

    std::string body_buf;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: video/mp4");
    hdrs = curl_slist_append(hdrs, "Expect:");   // disable 100-continue

    curl_easy_setopt(c, CURLOPT_URL,                 upload_url.c_str());
    curl_easy_setopt(c, CURLOPT_UPLOAD,              1L);
    curl_easy_setopt(c, CURLOPT_READDATA,            fp);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE,    static_cast<curl_off_t>(file_size));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,          hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,       yt_write_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,           &body_buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,             600L);  // 10 min for large files
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,      1L);

    const CURLcode res = curl_easy_perform(c);
    fclose(fp);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (res != CURLE_OK)
        throw std::runtime_error("Upload curl: " +
                                 std::string(curl_easy_strerror(res)));
    if (http_code != 200 && http_code != 201)
        throw std::runtime_error("Upload HTTP " +
                                 std::to_string(http_code) + ": " + body_buf);

    const auto resp = json::parse(body_buf);
    if (resp.contains("error"))
        throw std::runtime_error("YouTube upload error: " +
                                 resp["error"]["message"].get<std::string>());

    return resp["id"].get<std::string>();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────
std::string YoutubeUploader::upload(
    const std::string&              video_path,
    const std::string&              title,
    const std::string&              description,
    const std::vector<std::string>& tags,
    const std::string&              scheduled_publish_utc)
{
    // 1. Ensure valid tokens
    load_or_authorize();

    // 2. File size
    std::ifstream f(video_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + video_path);
    const long long file_size = static_cast<long long>(f.tellg());
    f.close();
    std::cout << "[YouTube] File: " << video_path
              << "  (" << file_size / 1024 / 1024 << " MB)\n";

    // 3. Metadata
    const std::string metadata =
        build_metadata_json(title, description, tags, scheduled_publish_utc);

    // 4. Start resumable session
    std::cout << "[YouTube] Initiating resumable upload session...\n";
    const std::string upload_url = start_resumable_upload(metadata, file_size);

    // 5. Upload bytes
    std::cout << "[YouTube] Uploading video data...\n";
    const std::string video_id = upload_bytes(upload_url, video_path, file_size);

    std::cout << "[YouTube] Upload complete!\n"
              << "[YouTube] Video ID  : " << video_id << "\n"
              << "[YouTube] Watch URL : https://www.youtube.com/watch?v="
              << video_id << "\n";
    if (!scheduled_publish_utc.empty())
        std::cout << "[YouTube] Publish at: " << scheduled_publish_utc
                  << " UTC  (JST 22:30)\n";

    return video_id;
}
