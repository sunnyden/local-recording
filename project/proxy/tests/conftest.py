import time

import pytest

from recorder_proxy.auth import Principal
from recorder_proxy.config import Settings


@pytest.fixture
def settings():
    return Settings(
        api_audience="11111111-1111-4111-8111-111111111111",
        client_id="22222222-2222-4222-8222-222222222222",
        allowed_oid="33333333-3333-4333-8333-333333333333",
        managed_identity_client_id="44444444-4444-4444-8444-444444444444",
        endpoint="https://unit-test.services.ai.azure.com",
        model="gpt-realtime-2", profile="native",
    )


@pytest.fixture
def principal(settings):
    return Principal(settings.allowed_oid, int(time.time()) + 60)
