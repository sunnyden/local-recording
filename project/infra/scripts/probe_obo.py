"""Manual Azure-only OBO feasibility probe; tokens remain in process memory."""
import asyncio
import json
import os
import time
from types import SimpleNamespace
from uuid import UUID, uuid4

import httpx
from azure.core.exceptions import AzureError
from azure.identity import ManagedIdentityCredential
from recorder_proxy.auth import AuthError, AuthUnavailable, TokenValidator
from recorder_proxy.config import CONSUMER_TENANT

LOGIN = "https://login.microsoftonline.com"
GRAPH = "https://graph.microsoft.com/v1.0"
SAFE_OAUTH_ERRORS = {
    "invalid_client", "invalid_grant", "invalid_scope", "invalid_request",
    "unauthorized_client", "interaction_required", "consent_required",
    "access_denied", "authorization_declined", "expired_token",
    "unsupported_grant_type", "server_error", "temporarily_unavailable",
}


class ProbeFailure(Exception):
    pass


def report(kind, **fields):
    print(json.dumps({"event": kind, **fields}), flush=True)


def token_result(response, phase):
    body = response.json()
    if not isinstance(body, dict):
        raise ProbeFailure("invalid_token_response")
    if response.status_code != 200 or "access_token" not in body:
        code = body.get("error")
        codes = body.get("error_codes", [])
        report("token_rejected", phase=phase, http_status=response.status_code,
               code=code if code in SAFE_OAUTH_ERRORS else "unrecognized",
               entra_codes=[value for value in codes if type(value) is int][:8])
        raise ProbeFailure(phase + "_rejected")
    token = body["access_token"]
    if not isinstance(token, str) or not 0 < len(token) <= 32768:
        raise ProbeFailure("invalid_access_token")
    return body


def device_grant(http, client_id, api_id):
    response = http.post(
        LOGIN + "/consumers/oauth2/v2.0/devicecode",
        data={"client_id": client_id, "scope": f"api://{api_id}/.default"},
    )
    response.raise_for_status()
    grant = response.json()
    if grant.get("verification_uri") not in (
        "https://www.microsoft.com/link", "https://microsoft.com/devicelogin",
        "https://www.microsoft.com/devicelogin",
    ):
        raise ProbeFailure("invalid_verification_url")
    if (not isinstance(grant.get("user_code"), str)
            or not grant["user_code"].isalnum()
            or not 1 <= len(grant["user_code"]) <= 32
            or type(grant.get("expires_in")) is not int
            or not 1 <= grant["expires_in"] <= 1800
            or type(grant.get("interval")) is not int
            or not 1 <= grant["interval"] <= 60):
        raise ProbeFailure("invalid_device_grant")
    report("sign_in_required", verification_uri=grant["verification_uri"],
           user_code=grant["user_code"], expires_in=grant["expires_in"])
    deadline = time.monotonic() + min(grant["expires_in"], 600)
    interval = grant["interval"]
    while time.monotonic() + interval < deadline:
        time.sleep(interval)
        response = http.post(
            LOGIN + "/consumers/oauth2/v2.0/token",
            data={"client_id": client_id,
                  "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                  "device_code": grant["device_code"]},
        )
        body = response.json()
        if response.status_code == 400 and body.get("error") == "authorization_pending":
            continue
        if response.status_code == 400 and body.get("error") == "slow_down":
            interval += 5
            continue
        return token_result(response, "user_authorization")["access_token"]
    raise ProbeFailure("user_authorization_expired")


async def validate_user(token, api_id, client_id, oid):
    validator = TokenValidator(SimpleNamespace(
        api_audience=api_id, client_id=client_id, allowed_oid=oid,
    ))
    try:
        await validator.validate("Bearer " + token)
    finally:
        await validator.close()


def obo_request(http, identity, api_id, assertion):
    client_assertion = identity.get_token("api://AzureADTokenExchange/.default").token
    response = http.post(
        f"{LOGIN}/{CONSUMER_TENANT}/oauth2/v2.0/token",
        data={
            "client_id": api_id,
            "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
            "requested_token_use": "on_behalf_of",
            "assertion": assertion,
            "scope": "https://graph.microsoft.com/.default",
            "client_assertion_type": "urn:ietf:params:oauth:client-assertion-type:jwt-bearer",
            "client_assertion": client_assertion,
        },
    )
    return token_result(response, "consumer_obo")


def verify_graph(http, token):
    headers = {"Authorization": "Bearer " + token}
    response = http.get(GRAPH + "/me/drive?$select=id", headers=headers)
    response.raise_for_status()
    if not response.json().get("id"):
        raise ProbeFailure("drive_missing")
    response = http.get(GRAPH + "/me/drive/root:/local-recording", headers=headers)
    response.raise_for_status()
    if not isinstance(response.json().get("folder"), dict):
        raise ProbeFailure("recording_folder_missing")
    report("graph_read_succeeded")
    name = ".recorder-obo-probe-" + uuid4().hex + ".txt"
    content = b"Recorder OBO permission probe. No recording or user content.\n"
    item = None
    try:
        response = http.put(
            GRAPH + "/me/drive/root:/local-recording/" + name + ":/content",
            headers={**headers, "Content-Type": "text/plain"}, content=content,
        )
        response.raise_for_status()
        item = response.json()
        if item.get("name") != name or item.get("size") != len(content):
            raise ProbeFailure("probe_upload_mismatch")
        report("graph_write_succeeded")
    finally:
        if item and item.get("id") and item.get("name") == name:
            from urllib.parse import quote
            response = http.delete(
                GRAPH + "/me/drive/items/" + quote(item["id"], safe=""), headers=headers,
            )
            response.raise_for_status()
            report("probe_file_removed")


def main():
    api_id = str(UUID(os.environ["API_B_CLIENT_ID"]))
    client_id = str(UUID(os.environ["PUBLIC_CLIENT_A_ID"]))
    identity_id = str(UUID(os.environ["AZURE_CLIENT_ID"]))
    oid = str(UUID(os.environ["ALLOWED_USER_OID"]))
    with httpx.Client(timeout=30, follow_redirects=False, trust_env=False) as http:
        assertion = device_grant(http, client_id, api_id)
        asyncio.run(validate_user(assertion, api_id, client_id, oid))
        report("user_token_validated")
        with ManagedIdentityCredential(client_id=identity_id) as identity:
            result = obo_request(http, identity, api_id, assertion)
            verify_graph(http, result["access_token"])
            result = obo_request(http, identity, api_id, assertion)
            report("repeated_obo_succeeded")
    report("proof_succeeded")


if __name__ == "__main__":
    try:
        main()
    except ProbeFailure as error:
        report("proof_failed", reason=str(error))
        raise SystemExit(1) from None
    except (httpx.HTTPError, AzureError, AuthError, AuthUnavailable,
            ValueError, KeyError, TypeError):
        report("proof_failed", reason="transport_identity_or_response_error")
        raise SystemExit(1) from None
