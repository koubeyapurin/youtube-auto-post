"""
Credential loader — two modes:
  Local dev  : reads/writes token files on disk, opens browser for first grant
  CI          : reads token JSON from env vars; after any refresh, writes the
                updated token back to the GitHub Secret via the REST API so the
                next run still has a valid refresh token.

Required GitHub Secrets (CI):
  GH_PAT                    Fine-grained PAT with Actions > Secrets: Read & Write
  GOOGLE_CLIENT_SECRET_JSON client_secret.json content
  YOUTUBE_TOKEN_JSON        token.json content   (for upload.py)
  DRIVE_TOKEN_JSON          token_drive.json content (for generate_video.py)
  GEMINI_API_KEY            plain API key string
"""

import base64
import json
import os
import warnings

import requests
from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from nacl import encoding
from nacl import public as nacl_public

CLIENT_SECRETS_FILE = "client_secret.json"
_IS_CI = bool(os.environ.get("CI"))


# ── GitHub Secrets API ───────────────────────────────────────────────────────

def _encrypt_for_github(public_key_b64: str, plaintext: str) -> str:
    """Encrypt `plaintext` with the repo's NaCl public key (Base64-encoded)."""
    pk = nacl_public.PublicKey(public_key_b64.encode(), encoding.Base64Encoder())
    encrypted = nacl_public.SealedBox(pk).encrypt(plaintext.encode())
    return base64.b64encode(encrypted).decode()


def _update_github_secret(secret_name: str, value: str) -> None:
    """
    Overwrite a GitHub Actions secret with `value`.
    Silently skips if GH_PAT or GITHUB_REPOSITORY is not set.
    """
    pat = os.environ.get("GH_PAT")
    repo = os.environ.get("GITHUB_REPOSITORY")   # auto-set by Actions: "owner/repo"
    if not pat or not repo:
        warnings.warn(
            f"Token for '{secret_name}' was refreshed but GH_PAT / "
            "GITHUB_REPOSITORY is not set — secret not updated. "
            "It will expire in ~1 hour.",
            stacklevel=2,
        )
        return

    headers = {
        "Authorization": f"Bearer {pat}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }

    r = requests.get(
        f"https://api.github.com/repos/{repo}/actions/secrets/public-key",
        headers=headers,
        timeout=10,
    )
    r.raise_for_status()
    key_data = r.json()

    encrypted = _encrypt_for_github(key_data["key"], value)

    r = requests.put(
        f"https://api.github.com/repos/{repo}/actions/secrets/{secret_name}",
        headers=headers,
        json={"encrypted_value": encrypted, "key_id": key_data["key_id"]},
        timeout=10,
    )
    r.raise_for_status()
    print(f"  GitHub Secret '{secret_name}' updated via API")


# ── OAuth client config ──────────────────────────────────────────────────────

def _client_config() -> dict:
    raw = os.environ.get("GOOGLE_CLIENT_SECRET_JSON")
    if raw:
        return json.loads(raw)
    with open(CLIENT_SECRETS_FILE) as f:
        return json.load(f)


# ── Public interface ─────────────────────────────────────────────────────────

def get_credentials(
    *,
    token_env_var: str,
    token_file: str,
    scopes: list[str],
) -> Credentials:
    """
    Return valid OAuth2 credentials.

    Resolution order
    ────────────────
    1. Env var <token_env_var>       → CI mode (GitHub Actions)
    2. Local file <token_file>       → developer machine
    3. Interactive browser OAuth     → local only; raises RuntimeError in CI

    Token refresh behaviour
    ───────────────────────
    - Local : refreshed token is written back to <token_file>
    - CI    : refreshed token is written back to the GitHub Secret via REST API
              (requires GH_PAT secret with Secrets:write permission)
    """
    creds: Credentials | None = None
    came_from_env = False

    raw_env = os.environ.get(token_env_var)
    if raw_env:
        creds = Credentials.from_authorized_user_info(json.loads(raw_env), scopes)
        came_from_env = True
    else:
        try:
            creds = Credentials.from_authorized_user_file(token_file, scopes)
        except (FileNotFoundError, ValueError):
            pass

    if creds and creds.valid:
        return creds

    if creds and creds.expired and creds.refresh_token:
        creds.refresh(Request())
        if came_from_env:
            # CI: push refreshed token back to GitHub Secrets so next run works
            _update_github_secret(token_env_var, creds.to_json())
        else:
            # Local: persist to disk
            with open(token_file, "w") as f:
                f.write(creds.to_json())
        return creds

    # No usable token at all — need a fresh browser grant
    if _IS_CI:
        raise RuntimeError(
            f"No valid credentials found in CI.\n"
            f"  → Generate the token locally:  python -c \""
            f"import auth; auth.get_credentials("
            f"token_env_var='', token_file='{token_file}', scopes={scopes})\"\n"
            f"  → Copy the contents of '{token_file}' into GitHub Secret: {token_env_var}"
        )

    flow = InstalledAppFlow.from_client_config(_client_config(), scopes)
    creds = flow.run_local_server(port=0)
    with open(token_file, "w") as f:
        f.write(creds.to_json())
    return creds
