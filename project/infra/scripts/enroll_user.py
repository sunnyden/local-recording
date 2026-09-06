"""Explicit operator enrollment; never used by the public proxy or BLE companion."""
import argparse
import asyncio
import json
from pathlib import Path
import re
import sys
import time
from types import SimpleNamespace
from urllib.parse import urlsplit
from uuid import UUID
import webbrowser

import httpx
import jwt

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proxy" / "src"))
from recorder_proxy.auth import AuthError, AuthUnavailable, TokenValidator

AUTHORITY = "https://login.microsoftonline.com/consumers/oauth2/v2.0"
VERIFICATION_URLS = {
    ("microsoft.com", "/devicelogin"),
    ("www.microsoft.com", "/devicelogin"),
    ("www.microsoft.com", "/link"),
}


def verification_uri(value):
    if not isinstance(value, str) or len(value) > 256 or not value.isascii():
        raise ValueError("Invalid Microsoft verification URL.")
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.port is not None or parsed.query or parsed.fragment
            or (parsed.netloc, parsed.path) not in VERIFICATION_URLS):
        raise ValueError("Invalid Microsoft verification URL.")
    return value


async def enroll(identity, open_browser=False):
    client_id = str(UUID(identity["deviceClientId"]))
    audience = str(UUID(identity["apiClientId"]))
    scope = f"api://{audience}/access_as_user"
    async with httpx.AsyncClient(timeout=20, follow_redirects=False, trust_env=False) as http:
        grant_response = await http.post(
            AUTHORITY + "/devicecode",
            data={"client_id": client_id, "scope": scope},
        )
        grant_response.raise_for_status()
        grant = grant_response.json()
        if (not isinstance(grant, dict) or not isinstance(grant.get("user_code"), str)
                or re.fullmatch(r"[A-Za-z0-9-]{1,64}", grant["user_code"]) is None
                or not isinstance(grant.get("device_code"), str)
                or type(grant.get("expires_in")) is not int
                or not 1 <= grant["expires_in"] <= 1800
                or type(grant.get("interval")) is not int
                or not 1 <= grant["interval"] <= 60):
            raise ValueError("Invalid Microsoft device-authorization response.")
        uri = verification_uri(grant.get("verification_uri"))
        print("Operator enrollment only: sign in with the intended PERSONAL Microsoft account.",
              file=sys.stderr)
        print(f"Open {uri} and enter {grant['user_code']}", file=sys.stderr)
        if open_browser and not webbrowser.open(uri, new=2):
            print("Browser did not open; use the displayed address manually.", file=sys.stderr)
        interval = grant["interval"]
        deadline = time.monotonic() + grant["expires_in"]
        while time.monotonic() < deadline:
            await asyncio.sleep(interval)
            if time.monotonic() >= deadline:
                break
            response = await http.post(
                AUTHORITY + "/token",
                data={
                    "client_id": client_id,
                    "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                    "device_code": grant["device_code"],
                },
            )
            payload = response.json()
            if not isinstance(payload, dict):
                raise ValueError("Invalid Microsoft token response.")
            if response.status_code == 400:
                error = payload.get("error")
                if error == "authorization_pending":
                    continue
                if error == "slow_down":
                    interval += 5
                    continue
                raise ValueError("Enrollment was declined, expired, or rejected by Microsoft.")
            response.raise_for_status()
            token = payload.get("access_token")
            if not isinstance(token, str) or not 1 <= len(token) <= 16384:
                raise ValueError("Microsoft did not issue the requested API token.")
            # This untrusted candidate is NEVER output until the full API validator
            # verifies signature, consumer issuer, audience, client and scope below.
            claims = jwt.decode(token, options={"verify_signature": False})
            candidate = str(UUID(claims["oid"]))
            settings = SimpleNamespace(api_audience=audience, client_id=client_id,
                                       allowed_oid=candidate)
            validator = TokenValidator(settings)
            try:
                principal = await validator.validate("Bearer " + token)
            finally:
                await validator.close()
            return {"oid": principal.oid, "api_audience": audience,
                    "instruction": "Use this oid with New-ProxyConfiguration.ps1 -AllowedUserOid."}
        raise ValueError("Enrollment expired. Start a new operator enrollment when ready.")


def main():
    parser = argparse.ArgumentParser(
        description="Explicitly sign in a consumer operator and output its verified API-B oid. "
                    "No tokens are printed, saved, or sent to the proxy."
    )
    parser.add_argument("--identity-state", type=Path,
                        default=Path(__file__).resolve().parents[1] / ".state" / "identity.json")
    parser.add_argument("--open-browser", action="store_true",
                        help="Open only the exact verification URL returned by Microsoft.")
    args = parser.parse_args()
    try:
        identity = json.loads(args.identity_state.read_text(encoding="utf-8"))
        result = asyncio.run(enroll(identity, open_browser=args.open_browser))
    except KeyboardInterrupt:
        print("Enrollment canceled. No user configuration was changed.", file=sys.stderr)
        return 130
    except (httpx.HTTPError, jwt.PyJWTError, AuthError, AuthUnavailable,
            ValueError, KeyError, TypeError, OSError):
        print("Enrollment failed. Check the selected registration and personal account; "
              "no tokens or user configuration were saved.", file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
