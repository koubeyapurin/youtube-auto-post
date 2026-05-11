import os
import sys
import time
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from googleapiclient.http import MediaFileUpload
import auth

SCOPES = ["https://www.googleapis.com/auth/youtube.upload"]
TOKEN_FILE = "token.json"
API_SERVICE_NAME = "youtube"
API_VERSION = "v3"

RETRIABLE_STATUS_CODES = [500, 502, 503, 504]
MAX_RETRIES = 10


def authenticate():
    return auth.get_credentials(
        token_env_var="YOUTUBE_TOKEN_JSON",
        token_file=TOKEN_FILE,
        scopes=SCOPES,
    )


def upload_video(youtube, file_path: str, title: str, description: str = "", tags: list[str] | None = None) -> str:
    body = {
        "snippet": {
            "title": title,
            "description": description,
            "tags": tags or [],
            "categoryId": "22",  # People & Blogs
        },
        "status": {
            "privacyStatus": "private",
        },
    }

    media = MediaFileUpload(file_path, chunksize=-1, resumable=True)
    request = youtube.videos().insert(part=",".join(body.keys()), body=body, media_body=media)

    response = None
    error = None
    retry = 0

    while response is None:
        try:
            print(f"Uploading... {file_path}")
            status, response = request.next_chunk()
            if status:
                print(f"  {int(status.progress() * 100)}% uploaded")
        except HttpError as e:
            if e.resp.status in RETRIABLE_STATUS_CODES:
                error = f"Retriable HTTP error {e.resp.status}: {e.content}"
            else:
                raise
        except Exception as e:
            error = f"Error: {e}"

        if error:
            retry += 1
            if retry > MAX_RETRIES:
                raise Exception(f"Max retries exceeded. Last error: {error}")
            wait = 2 ** retry
            print(f"  {error} — retrying in {wait}s (attempt {retry}/{MAX_RETRIES})")
            time.sleep(wait)
            error = None

    video_id = response["id"]
    print(f"Upload complete: https://www.youtube.com/watch?v={video_id}")
    return video_id


def main():
    video_file = "test.mp4"
    if not os.path.exists(video_file):
        sys.exit(f"Error: {video_file} not found in current directory")

    creds = authenticate()
    youtube = build(API_SERVICE_NAME, API_VERSION, credentials=creds)

    upload_video(
        youtube,
        file_path=video_file,
        title="Test Upload",
        description="Uploaded via YouTube Data API",
        tags=["test", "auto-upload"],
    )


if __name__ == "__main__":
    main()
