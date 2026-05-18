"""
Drive → YouTube パイプライン

1. Google Drive の youtube_queue フォルダから最古の動画を1件取得
2. ローカルに一時保存
3. YouTube に非公開でアップロード
4. Drive ファイルを youtube_posted フォルダへ移動
"""

import os
import sys
from googleapiclient.discovery import build
from googleapiclient.http import MediaIoBaseDownload
import auth
import upload as yt

DRIVE_SCOPES    = ["https://www.googleapis.com/auth/drive.file"]
YOUTUBE_SCOPES  = ["https://www.googleapis.com/auth/youtube.upload"]
DRIVE_TOKEN_FILE   = "token_drive.json"
YOUTUBE_TOKEN_FILE = "token.json"
QUEUE_FOLDER    = "youtube_queue"
POSTED_FOLDER   = "youtube_posted"
LOCAL_VIDEO     = "temp_video.mp4"


# ── Drive helpers ────────────────────────────────────────────────────────────

def _drive_service():
    creds = auth.get_credentials(
        token_env_var="DRIVE_TOKEN_JSON",
        token_file=DRIVE_TOKEN_FILE,
        scopes=DRIVE_SCOPES,
    )
    return build("drive", "v3", credentials=creds)


def folder_id(service, name: str, create: bool = False) -> str | None:
    res = service.files().list(
        q=f"name='{name}' and mimeType='application/vnd.google-apps.folder' and trashed=false",
        fields="files(id)",
        pageSize=1,
    ).execute().get("files", [])
    if res:
        return res[0]["id"]
    if not create:
        return None
    folder = service.files().create(
        body={"name": name, "mimeType": "application/vnd.google-apps.folder"},
        fields="id",
    ).execute()
    print(f"Drive: created folder '{name}'")
    return folder["id"]


def fetch_oldest_video(service) -> dict | None:
    """youtube_queue 内の最古の動画メタデータを返す。なければ None。"""
    fid = folder_id(service, QUEUE_FOLDER)
    if not fid:
        return None
    files = service.files().list(
        q=f"'{fid}' in parents and mimeType='video/mp4' and trashed=false",
        orderBy="createdTime",
        fields="files(id, name, createdTime)",
        pageSize=1,
    ).execute().get("files", [])
    return files[0] if files else None


def download_video(service, file_id: str, dest: str) -> None:
    req = service.files().get_media(fileId=file_id)
    with open(dest, "wb") as fh:
        dl = MediaIoBaseDownload(fh, req, chunksize=8 * 1024 * 1024)
        done = False
        while not done:
            status, done = dl.next_chunk()
            if status:
                print(f"  Download {int(status.progress() * 100)}%")


def move_to_posted(service, file_id: str) -> None:
    """youtube_queue から youtube_posted にファイルを移動する。"""
    posted_id = folder_id(service, POSTED_FOLDER, create=True)
    meta = service.files().get(fileId=file_id, fields="parents").execute()
    old_parents = ",".join(meta.get("parents", []))
    service.files().update(
        fileId=file_id,
        addParents=posted_id,
        removeParents=old_parents,
        fields="id",
    ).execute()
    print(f"Drive: moved to '{POSTED_FOLDER}'")


# ── YouTube title ────────────────────────────────────────────────────────────

def title_from_filename(filename: str) -> str:
    """
    'YYYYMMDD_HHMMSS_animal.mp4' → 'Animal | Auto Post YYYY-MM-DD'
    それ以外はそのまま返す。
    """
    stem = filename.removesuffix(".mp4")
    parts = stem.split("_", 2)
    if len(parts) == 3 and parts[0].isdigit():
        date = parts[0]
        animal = parts[2].replace("_", " ").title()
        return f"{animal} | Auto Post {date[:4]}-{date[4:6]}-{date[6:]} #Shorts"
    return stem


# ── Entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    drive = _drive_service()

    video = fetch_oldest_video(drive)
    if not video:
        print("Queue is empty — nothing to upload.")
        sys.exit(0)

    print(f"Queued: {video['name']}  (created {video['createdTime']})")
    download_video(drive, video["id"], LOCAL_VIDEO)
    print(f"Downloaded → {LOCAL_VIDEO} ({os.path.getsize(LOCAL_VIDEO) / 1e6:.1f} MB)")

    title = title_from_filename(video["name"])
    print(f"YouTube title: {title}")

    youtube_creds = auth.get_credentials(
        token_env_var="YOUTUBE_TOKEN_JSON",
        token_file=YOUTUBE_TOKEN_FILE,
        scopes=YOUTUBE_SCOPES,
    )
    youtube = build("youtube", "v3", credentials=youtube_creds)
    yt.upload_video(youtube, LOCAL_VIDEO, title=title, tags=["auto", "shorts"])

    move_to_posted(drive, video["id"])
    print("Done.")


if __name__ == "__main__":
    main()
