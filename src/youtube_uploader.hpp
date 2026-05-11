#pragma once
#include <string>
#include <vector>

// Handles YouTube OAuth2 flow and resumable video upload.
// First run: opens browser for consent → saves token.json
// Subsequent runs: reads token.json and refreshes the access token automatically.
class YoutubeUploader {
public:
    explicit YoutubeUploader(
        const std::string& client_secret_path = "client_secret.json",
        const std::string& token_path         = "token.json");

    // Returns the YouTube video ID on success.
    // scheduled_publish_utc: ISO 8601 UTC string e.g. "2026-05-08T13:30:00Z"
    // Leave empty to publish immediately as public.
    std::string upload(
        const std::string&              video_path,
        const std::string&              title,
        const std::string&              description,
        const std::vector<std::string>& tags,
        const std::string&              scheduled_publish_utc = "");

private:
    // credentials
    std::string client_id_;
    std::string client_secret_;
    std::string token_path_;
    std::string access_token_;
    std::string refresh_token_;

    static constexpr int         kRedirectPort = 8080;
    static constexpr const char* kRedirectUri  = "http://localhost:8080";
    static constexpr const char* kScope        =
        "https://www.googleapis.com/auth/youtube.upload";

    void load_client_secret(const std::string& path);
    void load_or_authorize();
    void do_browser_auth();
    void exchange_code(const std::string& code);
    void refresh_access_token();
    void save_tokens();
    void load_tokens();

    std::string build_metadata_json(
        const std::string&              title,
        const std::string&              description,
        const std::vector<std::string>& tags,
        const std::string&              scheduled_publish_utc);

    // Step A: POST metadata → returns the upload session URI (from Location header)
    std::string start_resumable_upload(const std::string& metadata_json,
                                       long long file_size);

    // Step B: PUT raw bytes → returns video ID
    std::string upload_bytes(const std::string& upload_url,
                             const std::string& video_path,
                             long long file_size);

    // Blocks until browser redirects to localhost:kRedirectPort, returns auth code
    std::string wait_for_oauth_code();
};
