import importlib.util
from pathlib import Path
from types import SimpleNamespace

import httpx
import pytest

source = Path(__file__).resolve().parents[1] / "scripts" / "probe_obo.py"
spec = importlib.util.spec_from_file_location("obo_probe", source)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def test_obo_uses_distinct_assertions_and_tenant_specific_authority(capsys):
    class Identity:
        def get_token(self, scope):
            assert scope == "api://AzureADTokenExchange/.default"
            return SimpleNamespace(token="TEST_RUNTIME_ASSERTION")

    class HTTP:
        def post(self, url, data):
            assert url == f"{probe.LOGIN}/{probe.CONSUMER_TENANT}/oauth2/v2.0/token"
            assert data["assertion"] == "TEST_USER_ASSERTION"
            assert data["client_assertion"] == "TEST_RUNTIME_ASSERTION"
            assert data["requested_token_use"] == "on_behalf_of"
            assert "client_secret" not in data
            return httpx.Response(200, json={"access_token": "TEST_GRAPH_ACCESS"})

    result = probe.obo_request(HTTP(), Identity(), "api-b", "TEST_USER_ASSERTION")
    assert result["access_token"] == "TEST_GRAPH_ACCESS"
    assert capsys.readouterr().out == ""


def test_token_failure_never_prints_provider_description_or_token(capsys):
    response = httpx.Response(400, json={
        "error": "invalid_grant", "error_codes": [50013],
        "error_description": "TEST_SENSITIVE_DETAIL",
        "access_token": "TEST_SENSITIVE_ACCESS",
    })
    with pytest.raises(probe.ProbeFailure):
        probe.token_result(response, "consumer_obo")
    output = capsys.readouterr().out
    assert "50013" in output
    assert "TEST_SENSITIVE" not in output
