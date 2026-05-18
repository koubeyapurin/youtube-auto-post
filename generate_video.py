import base64
import os
import re
import sys
import json
import time
import subprocess
from datetime import datetime
from google import genai
from google.genai import types
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload
import requests
import auth
from drive_to_youtube import folder_id as drive_folder_id

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
LUMAAI_API_KEY = os.environ.get("LUMAAI_API_KEY", "")
GEMINI_MODEL = "gemini-2.5-flash"
LUMA_MODEL = "dream-machine"
LYRIA_MODEL = "lyria-3-clip-preview"
OUTPUT_FILE = "temp_video.mp4"
RAW_VIDEO   = "temp_raw.mp4"
TEMP_AUDIO  = "temp_audio"  # 拡張子はレスポンスの mime_type から決定
POLL_INTERVAL = 20

DRIVE_TOKEN_FILE = "token_drive.json"
DRIVE_SCOPES = ["https://www.googleapis.com/auth/drive.file"]
DRIVE_FOLDER_NAME = "youtube_queue"


# ── Gemini: prompt generation ────────────────────────────────────────────────

def generate_prompt(client: genai.Client) -> tuple[str, str]:
    """Returns (prompt, animal_name)."""
    print("Generating video prompt with Gemini 2.0 Flash...")
    response = client.models.generate_content(
        model=GEMINI_MODEL,
        contents=(
            "You are a viral video creative director. "
            "Generate exactly one English prompt for an AI video generator. "
            "The video must feature an animal behaving exactly like a human "
            "in a funny, warm, and culturally resonant way for audiences in India "
            "(e.g., bargaining at a street market, watching cricket on TV, "
            "riding a scooter through monsoon rain, eating street food). "
            'Respond in JSON only — no markdown, no code block: '
            '{"prompt": "...", "animal": "single English animal name"}'
        ),
    )
    raw = response.text.strip()
    # strip ```json ... ``` if Gemini adds it anyway
    raw = re.sub(r"^```[a-z]*\n?", "", raw)
    raw = re.sub(r"\n?```$", "", raw)
    data = json.loads(raw)
    prompt = data["prompt"].strip()
    animal = data["animal"].strip().lower().replace(" ", "_")
    print(f"Animal : {animal}")
    print(f"Prompt : {prompt}\n")
    return prompt, animal


# ── Veo: video generation ────────────────────────────────────────────────────

LUMA_API_BASE = "https://api.lumalabs.ai/dream-machine/v1"


def _luma_headers() -> dict:
    return {
        "Authorization": f"Bearer {LUMAAI_API_KEY}",
        "Content-Type": "application/json",
    }


def generate_video(prompt: str) -> bytes:
    print("Submitting video generation request to LUMA...")
    resp = requests.post(
        f"{LUMA_API_BASE}/generations",
        headers=_luma_headers(),
        json={"prompt": prompt, "aspect_ratio": "9:16", "model": LUMA_MODEL},
        timeout=30,
    )
    resp.raise_for_status()
    generation_id = resp.json()["id"]

    print("Waiting for video generation...")
    while True:
        time.sleep(POLL_INTERVAL)
        resp = requests.get(
            f"{LUMA_API_BASE}/generations/{generation_id}",
            headers=_luma_headers(),
            timeout=30,
        )
        resp.raise_for_status()
        data = resp.json()
        state = data["state"]
        print(f"  State: {state}")
        if state == "completed":
            break
        if state == "failed":
            raise RuntimeError(f"LUMA generation failed: {data.get('failure_reason')}")

    print("Downloading generated video...")
    video_url = data["assets"]["video"]
    video_resp = requests.get(video_url, stream=True, timeout=120)
    video_resp.raise_for_status()
    return video_resp.content


# ── Lyria: BGM generation ────────────────────────────────────────────────────

def generate_bgm(client: genai.Client, animal: str) -> tuple[bytes, str]:
    """Returns (audio_bytes, mime_type)."""
    print("Generating BGM with Lyria 3...")
    prompt = (
        f"Upbeat, joyful Indian music with tabla and sitar. "
        f"Energetic and fun, perfect for a viral short video featuring a {animal}. "
        f"30 seconds, no vocals."
    )
    response = client.models.generate_content(
        model=LYRIA_MODEL,
        contents=prompt,
        config=types.GenerateContentConfig(
            response_modalities=["AUDIO"],
        ),
    )
    part = response.candidates[0].content.parts[0]
    return base64.b64decode(part.inline_data.data), part.inline_data.mime_type


def compose_with_bgm(video_path: str, audio_path: str, mime_type: str, output_path: str) -> None:
    print(f"Composing video with BGM (FFmpeg) — audio: {mime_type}...")

    # audio/L16 は raw PCM なので FFmpeg にフォーマットを明示する
    if "L16" in mime_type or "pcm" in mime_type.lower():
        rate = "44100"
        for seg in mime_type.split(";"):
            if "rate=" in seg:
                rate = seg.split("=")[1].strip()
        audio_flags = ["-f", "s16le", "-ar", rate, "-ac", "1",
                       "-stream_loop", "-1", "-i", audio_path]
    else:
        audio_flags = ["-stream_loop", "-1", "-i", audio_path]

    subprocess.run(
        ["ffmpeg", "-y", "-i", video_path] + audio_flags + [
            "-c:v", "copy",
            "-c:a", "aac", "-b:a", "192k",
            "-shortest",
            output_path,
        ],
        check=True,
        capture_output=True,
    )


# ── Google Drive upload ──────────────────────────────────────────────────────

def upload_to_drive(local_path: str, animal: str) -> str:
    """Uploads the video to youtube_queue/<timestamp>_<animal>.mp4 and returns the Drive URL."""
    creds = auth.get_credentials(
        token_env_var="DRIVE_TOKEN_JSON",
        token_file=DRIVE_TOKEN_FILE,
        scopes=DRIVE_SCOPES,
    )
    service = build("drive", "v3", credentials=creds)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{timestamp}_{animal}.mp4"

    fid = drive_folder_id(service, DRIVE_FOLDER_NAME, create=True)

    print(f"Uploading '{filename}' to Drive folder '{DRIVE_FOLDER_NAME}'...")
    media = MediaFileUpload(local_path, mimetype="video/mp4", resumable=True)
    file_meta = service.files().create(
        body={"name": filename, "parents": [fid]},
        media_body=media,
        fields="id, webViewLink",
    ).execute()

    url = file_meta.get("webViewLink", f"https://drive.google.com/file/d/{file_meta['id']}")
    print(f"Drive URL : {url}")
    return url


# ── Entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    if not GEMINI_API_KEY:
        sys.exit(
            "Error: GEMINI_API_KEY is not set.\n"
            "Get your key from https://aistudio.google.com/apikey then run:\n"
            "  $env:GEMINI_API_KEY = 'your-key-here'"
        )
    if not LUMAAI_API_KEY:
        sys.exit(
            "Error: LUMAAI_API_KEY is not set.\n"
            "Get your key from https://lumalabs.ai/dream-machine/api"
        )
    print(f"LUMAAI_API_KEY length: {len(LUMAAI_API_KEY)}, prefix: {LUMAAI_API_KEY[:8]}...")

    gemini_client = genai.Client(api_key=GEMINI_API_KEY)

    prompt, animal = generate_prompt(gemini_client)
    video_bytes = generate_video(prompt)

    with open(RAW_VIDEO, "wb") as f:
        f.write(video_bytes)

    try:
        audio_bytes, mime_type = generate_bgm(gemini_client, animal)
        ext = ".wav" if ("L16" in mime_type or "pcm" in mime_type.lower()) else ".mp3"
        audio_path = TEMP_AUDIO + ext
        with open(audio_path, "wb") as f:
            f.write(audio_bytes)
        compose_with_bgm(RAW_VIDEO, audio_path, mime_type, OUTPUT_FILE)
        os.unlink(audio_path)
        os.unlink(RAW_VIDEO)
    except Exception as e:
        print(f"BGM skipped ({e}) — uploading video without audio.")
        os.replace(RAW_VIDEO, OUTPUT_FILE)

    size_mb = os.path.getsize(OUTPUT_FILE) / 1024 / 1024
    print(f"Saved locally: {OUTPUT_FILE} ({size_mb:.1f} MB)\n")

    upload_to_drive(OUTPUT_FILE, animal)


if __name__ == "__main__":
    main()
