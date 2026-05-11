// YouTube Auto-Post Pipeline
// Step 1: Text idea (Gemini)  →  Step 2: Video (Veo 3.1 Lite)
// Step 3: Music (Lyria 3)     →  Step 4: Compose (FFmpeg)  →  Step 5: YouTube upload
#include "youtube_uploader.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using json = nlohmann::json;

// ── libcurl write callbacks ────────────────────────────────────────────────────
static size_t write_string_cb(char* ptr, size_t sz, size_t n, void* ud)
{
    static_cast<std::string*>(ud)->append(ptr, sz * n);
    return sz * n;
}

static size_t write_file_cb(char* ptr, size_t sz, size_t n, void* ud)
{
    static_cast<std::ofstream*>(ud)->write(ptr, sz * n);
    return sz * n;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────
// All requests authenticate via the x-goog-api-key header.

static std::string http_post(const std::string& url,
                              const std::string& api_key,
                              const std::string& body,
                              long timeout_sec = 60)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    std::string buf;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("x-goog-api-key: " + api_key).c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  write_string_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout_sec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (res != CURLE_OK)
        throw std::runtime_error(std::string("POST curl error: ") + curl_easy_strerror(res));
    return buf;
}

static std::string http_get(const std::string& url, const std::string& api_key,
                             long timeout_sec = 30)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    std::string buf;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("x-goog-api-key: " + api_key).c_str());

    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  write_string_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout_sec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (res != CURLE_OK)
        throw std::runtime_error(std::string("GET curl error: ") + curl_easy_strerror(res));
    return buf;
}

static void http_download(const std::string& url, const std::string& api_key,
                           const std::string& filepath)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Cannot open for writing: " + filepath);

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("x-goog-api-key: " + api_key).c_str());

    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  write_file_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &file);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        180L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    file.close();
    if (res != CURLE_OK)
        throw std::runtime_error(std::string("Download curl error: ") + curl_easy_strerror(res));
}

// ── Base64 decode (RFC 4648, no external deps) ────────────────────────────────
static std::string base64_decode(const std::string& in)
{
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ── Helper: throw on API error field ─────────────────────────────────────────
static void check_api_error(const json& j, const std::string& context)
{
    if (j.contains("error"))
        throw std::runtime_error(context + ": " +
                                 j["error"]["message"].get<std::string>());
}

// ─────────────────────────────────────────────────────────────────────────────
// 1.  TEXT GENERATION  (Gemini 2.0 Flash)
// ─────────────────────────────────────────────────────────────────────────────
std::string gemini_generate_text(const std::string& api_key,
                                  const std::string& prompt,
                                  const std::string& model = "gemini-2.0-flash")
{
    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/models/" +
        model + ":generateContent";

    json body = {
        {"contents", {{{"parts", {{{"text", prompt}}}}}}}
    };

    auto resp = json::parse(http_post(url, api_key, body.dump()));
    check_api_error(resp, "Gemini text");
    return resp["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.  VIDEO GENERATION  (Veo 3.1 Lite)
//     Uses a Long-Running Operation (LRO): submit → poll until done → download
// ─────────────────────────────────────────────────────────────────────────────
void veo_generate_video(const std::string& api_key,
                         const std::string& prompt,
                         const std::string& output_path = "generated_video.mp4")
{
    const std::string model   = "veo-3.1-lite-generate-preview";
    const std::string submit_url =
        "https://generativelanguage.googleapis.com/v1beta/models/" +
        model + ":predictLongRunning";

    json body = {
        {"instances", {{{"prompt", prompt}}}},
        {"parameters", {
            {"aspectRatio",     "9:16"},
            {"durationSeconds", "8"}
        }}
    };

    // ── Submit job ───────────────────────────────────────────────────────────
    std::cout << "[Veo] Submitting video generation job...\n";
    auto submit_resp = json::parse(http_post(submit_url, api_key, body.dump(), 60));
    check_api_error(submit_resp, "Veo submit");

    const std::string op_name = submit_resp["name"].get<std::string>();
    std::cout << "[Veo] Operation: " << op_name << "\n";

    // ── Poll until done (max 10 min, every 10 s) ─────────────────────────────
    const std::string poll_url =
        "https://generativelanguage.googleapis.com/v1beta/" + op_name;

    for (int attempt = 1; attempt <= 60; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::cout << "[Veo] Polling " << attempt << "/60...\n";

        auto poll = json::parse(http_get(poll_url, api_key));
        check_api_error(poll, "Veo poll");

        if (poll.value("done", false)) {
            const auto& samples =
                poll["response"]["generateVideoResponse"]["generatedSamples"];
            const std::string video_uri =
                samples[0]["video"]["uri"].get<std::string>();

            std::cout << "[Veo] Generation complete. Downloading video...\n";
            http_download(video_uri, api_key, output_path);
            std::cout << "[Veo] Saved: " << output_path << "\n";
            return;
        }
    }
    throw std::runtime_error("Veo: timed out (10 min) waiting for video generation");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.  MUSIC GENERATION  (Lyria 3 Clip)
//     Single-turn: audio returned as base64-encoded MP3 inline data
// ─────────────────────────────────────────────────────────────────────────────
void lyria_generate_music(const std::string& api_key,
                           const std::string& prompt,
                           const std::string& output_path = "generated_audio.mp3")
{
    // lyria-3-clip-preview  →  ~30 s clip, MP3 by default
    const std::string model = "lyria-3-clip-preview";
    const std::string url   =
        "https://generativelanguage.googleapis.com/v1beta/models/" +
        model + ":generateContent";

    json body = {
        {"contents", {{{"parts", {{{"text", prompt}}}}}}} ,
        {"generationConfig", {
            {"responseModalities", {"AUDIO"}},
            {"responseMimeType",   "audio/mp3"}
        }}
    };

    std::cout << "[Lyria] Generating music...\n";
    auto resp = json::parse(http_post(url, api_key, body.dump(), 120));
    check_api_error(resp, "Lyria");

    // Find the inlineData part that contains the audio bytes
    for (const auto& part : resp["candidates"][0]["content"]["parts"]) {
        if (!part.contains("inlineData")) continue;

        const std::string b64   = part["inlineData"]["data"].get<std::string>();
        const std::string bytes = base64_decode(b64);

        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open())
            throw std::runtime_error("Cannot write: " + output_path);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();

        std::cout << "[Lyria] Saved: " << output_path
                  << "  (" << bytes.size() << " bytes)\n";
        return;
    }
    throw std::runtime_error("Lyria: no audio inlineData in response");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4.  FFMPEG COMPOSITION
//     - Pads/scales video to exactly 720×1280 (9:16)
//     - Loops audio to fill video duration, then trims to video length
// ─────────────────────────────────────────────────────────────────────────────
void ffmpeg_compose(const std::string& video_path,
                    const std::string& audio_path,
                    const std::string& output_path = "final_output.mp4")
{
    // scale to 720 wide (keep AR), then pad height to 1280 with black bars
    // -stream_loop -1  →  loop audio indefinitely
    // -shortest        →  stop output when the shortest stream ends (= video)
    const std::string cmd =
        "ffmpeg -y"
        " -i \""            + video_path + "\""
        " -stream_loop -1"
        " -i \""            + audio_path + "\""
        " -vf \"scale=720:-2,pad=720:1280:0:(oh-ih)/2:black\""
        " -c:v libx264 -preset fast -crf 23"
        " -c:a aac -b:a 192k"
        " -shortest"
        " \""               + output_path + "\"";

    std::cout << "[FFmpeg] Command:\n  " << cmd << "\n\n";

    int ret = std::system(cmd.c_str());
    if (ret != 0)
        throw std::runtime_error("FFmpeg exited with code " + std::to_string(ret));

    std::cout << "[FFmpeg] Saved: " << output_path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.  SCHEDULED PUBLISH TIME  (JST 22:30 → UTC 13:30)
//     Returns the next occurrence as an ISO 8601 UTC string.
// ─────────────────────────────────────────────────────────────────────────────
static std::string next_jst_2230_utc()
{
    // JST is UTC+9, so JST 22:30 == UTC 13:30
    std::time_t now = std::time(nullptr);

    struct tm utc_now{};
#ifdef _WIN32
    gmtime_s(&utc_now, &now);
#else
    gmtime_r(&now, &utc_now);
#endif

    struct tm candidate = utc_now;
    candidate.tm_hour = 13;
    candidate.tm_min  = 30;
    candidate.tm_sec  = 0;

    // Convert back to time_t treating the struct as UTC
    std::time_t target_t;
#ifdef _WIN32
    target_t = _mkgmtime(&candidate);
#else
    target_t = timegm(&candidate);
#endif

    // Already past today's 13:30 UTC → schedule for tomorrow
    if (target_t <= now) {
        target_t += 86400;
#ifdef _WIN32
        gmtime_s(&candidate, &target_t);
#else
        gmtime_r(&target_t, &candidate);
#endif
    }

    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &candidate);
    return std::string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN  –  Full pipeline
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    const char* key_env = std::getenv("GEMINI_API_KEY");
    if (!key_env || std::string(key_env).empty()) {
        std::cerr << "Error: GEMINI_API_KEY environment variable is not set.\n"
                  << "  PowerShell: $env:GEMINI_API_KEY = \"YOUR_KEY\"\n";
        return 1;
    }
    const std::string api_key(key_env);

    try {
        // ── Step 1: Generate video idea ───────────────────────────────────────
        std::cout << "========================================\n"
                  << " Step 1: Generate video idea\n"
                  << "========================================\n";

        const std::string idea_prompt =
            "インドでバズりそうな動物の動画ネタを1つ考えて、"
            "英語で20語以内の動画生成プロンプトとして出力してください。"
            "プロンプトのみ出力し、説明や記号は不要です。";

        std::string video_prompt = gemini_generate_text(api_key, idea_prompt);

        // strip leading/trailing whitespace
        auto trim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                    [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(),
                    [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
        };
        trim(video_prompt);

        std::cout << "Video prompt: \"" << video_prompt << "\"\n\n";

        // ── Step 2: Generate video ────────────────────────────────────────────
        std::cout << "========================================\n"
                  << " Step 2: Video generation (Veo 3.1 Lite)\n"
                  << "========================================\n";

        veo_generate_video(api_key, video_prompt, "generated_video.mp4");

        // ── Step 3: Generate music ────────────────────────────────────────────
        std::cout << "\n========================================\n"
                  << " Step 3: Music generation (Lyria 3 Clip)\n"
                  << "========================================\n";

        const std::string music_prompt =
            "Upbeat Indian music with tabla and sitar, energetic and joyful, "
            "suitable for a viral animal video short reel, 30 seconds";

        lyria_generate_music(api_key, music_prompt, "generated_audio.mp3");

        // ── Step 4: Compose ───────────────────────────────────────────────────
        std::cout << "\n========================================\n"
                  << " Step 4: Compose with FFmpeg\n"
                  << "========================================\n";

        ffmpeg_compose("generated_video.mp4", "generated_audio.mp3", "final_output.mp4");

        // ── Step 5: Upload to YouTube ─────────────────────────────────────────
        std::cout << "\n========================================\n"
                  << " Step 5: YouTube upload (scheduled JST 22:30)\n"
                  << "========================================\n";

        // Generate a punchy Japanese title from the English video_prompt
        const std::string title_prompt =
            "次の英語の動画プロンプトをもとに、YouTubeショート動画の日本語タイトルを1行で作成してください。\n"
            "バズりやすく感情を刺激するタイトルにしてください（30文字以内）。"
            "タイトルのみ出力し、説明や記号は不要です。\n"
            "プロンプト: " + video_prompt;

        std::string yt_title = gemini_generate_text(api_key, title_prompt);
        trim(yt_title);
        // Append #Shorts so YouTube classifies it as a Short
        if (yt_title.find("#Shorts") == std::string::npos)
            yt_title += " #Shorts";

        const std::string description =
            "AIが生成したインドでバズる動物のショート動画\n\n"
            "Generated with Veo 3.1 Lite (video) × Lyria 3 (music)\n\n"
            "#Shorts #India #AI #Animals #Viral #かわいい";

        const std::vector<std::string> tags =
            {"Shorts", "India", "AI", "Animals", "Viral", "Cute",
             "インド", "動物", "バズ", "AIショート"};

        const std::string publish_utc = next_jst_2230_utc();
        std::cout << "Title    : " << yt_title    << "\n"
                  << "Publish  : " << publish_utc << " UTC  (JST 22:30)\n\n";

        YoutubeUploader uploader("client_secret.json", "token.json");
        const std::string video_id =
            uploader.upload("final_output.mp4", yt_title, description,
                            tags, publish_utc);

        std::cout << "\n========================================\n"
                  << " Pipeline complete!\n"
                  << " Video ID : " << video_id << "\n"
                  << " Watch    : https://www.youtube.com/watch?v=" << video_id << "\n"
                  << " Publish  : " << publish_utc << " UTC\n"
                  << "========================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
