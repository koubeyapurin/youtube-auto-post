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
import auth
from drive_to_youtube import folder_id as drive_folder_id

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_MODEL = "gemini-2.5-flash"
VEO_MODEL = "veo-2.0-generate-001"
LYRIA_MODEL = "lyria-3-clip-preview"
OUTPUT_FILE = "temp_video.mp4"
RAW_VIDEO    = "temp_raw.mp4"
TEMP_AUDIO   = "temp_audio.mp3"
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

def generate_video(client: genai.Client, prompt: str) -> bytes:
    print("Submitting video generation request to Veo...")
    operation = client.models.generate_videos(
        model=VEO_MODEL,
        prompt=prompt,
        config=types.GenerateVideosConfig(
            number_of_videos=1,
            duration_seconds=8,
            enhance_prompt=True,
        ),
    )

    print("Waiting for video generation (typically 2–5 minutes)...")
    while not operation.done:
        time.sleep(POLL_INTERVAL)
        operation = client.operations.get(operation)
        print("  Still generating...")

    if not operation.response.generated_videos:
        raise RuntimeError("Generation completed but no video was returned")

    video_file = operation.response.generated_videos[0].video
    print("Downloading generated video...")
    return client.files.download(file=video_file)


# ── Lyria: BGM generation ────────────────────────────────────────────────────

def generate_bgm(client: genai.Client, animal: str) -> bytes:
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
            response_mime_type="audio/mp3",
        ),
    )
    return base64.b64decode(response.candidates[0].content.parts[0].inline_data.data)


def compose_with_bgm(video_path: str, audio_path: str, output_path: str) -> None:
    print("Composing video with BGM (FFmpeg)...")
    subprocess.run(
        [
            "ffmpeg", "-y",
            "-i", video_path,
            "-stream_loop", "-1", "-i", audio_path,
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

    client = genai.Client(api_key=GEMINI_API_KEY)

    prompt, animal = generate_prompt(client)
    video_bytes = generate_video(client, prompt)

    with open(RAW_VIDEO, "wb") as f:
        f.write(video_bytes)

    try:
        audio_bytes = generate_bgm(client, animal)
        with open(TEMP_AUDIO, "wb") as f:
            f.write(audio_bytes)
        compose_with_bgm(RAW_VIDEO, TEMP_AUDIO, OUTPUT_FILE)
        os.unlink(TEMP_AUDIO)
    except Exception as e:
        print(f"BGM skipped ({e}) — uploading video without audio.")
        os.replace(RAW_VIDEO, OUTPUT_FILE)
    else:
        os.unlink(RAW_VIDEO)

    size_mb = os.path.getsize(OUTPUT_FILE) / 1024 / 1024
    print(f"Saved locally: {OUTPUT_FILE} ({size_mb:.1f} MB)\n")

    upload_to_drive(OUTPUT_FILE, animal)


if __name__ == "__main__":
    main()
