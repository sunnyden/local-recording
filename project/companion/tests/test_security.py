import pytest

from recorder_companion._espressif import proto
from recorder_companion.errors import ProtocolError
from recorder_companion.security import EspressifSecurity2, validate_security_version


@pytest.mark.parametrize("version", [
    b'{"prov":{"sec_ver":0,"sec_patch_ver":1}}',
    b'{"prov":{"sec_ver":1,"sec_patch_ver":1}}',
    b'{"prov":{"sec_ver":2,"sec_patch_ver":0}}',
    b'{"prov":{"sec_ver":2}}',
    b'{"prov":{"sec_ver":2,"sec_patch_ver":true}}',
])
def test_no_security_downgrade(version):
    with pytest.raises(ProtocolError):
        validate_security_version(version)


def test_real_upstream_srp_initial_frame_is_not_mocked(capsys):
    adapter = EspressifSecurity2("recorder", "not-logged-password")
    payload = adapter.handshake()
    message = proto.session_pb2.SessionData()
    message.ParseFromString(payload)
    assert message.sec_ver == 2
    assert message.sec2.sc0.client_username == b"recorder"
    assert 380 <= len(message.sec2.sc0.client_pubkey) <= 384
    assert len(payload) <= 512
    assert not adapter.ready
    assert "not-logged-password" not in capsys.readouterr().out
    with pytest.raises(ProtocolError):
        adapter.encrypt(b"no plaintext fallback")


def test_real_upstream_rejects_invalid_server_proof(capsys):
    adapter = EspressifSecurity2("recorder", "not-logged-password")
    adapter.handshake()
    response = proto.session_pb2.SessionData(sec_ver=2)
    response.sec2.msg = proto.sec2_pb2.S2Session_Response0
    response.sec2.sr0.device_pubkey = b"\x03"
    response.sec2.sr0.device_salt = b"s" * 16
    proof_frame = adapter.handshake(response.SerializeToString())
    assert proof_frame is not None
    response = proto.session_pb2.SessionData(sec_ver=2)
    response.sec2.msg = proto.sec2_pb2.S2Session_Response1
    response.sec2.sr1.device_proof = b"\x00" * 64
    response.sec2.sr1.device_nonce = b"\x00" * 12
    with pytest.raises(ProtocolError) as error:
        adapter.handshake(response.SerializeToString())
    assert "not-logged-password" not in str(error.value)
    assert not adapter.ready
    assert not capsys.readouterr().out


def test_real_upstream_patch1_cipher_advances_nonce_and_rejects_tampering():
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    adapter = EspressifSecurity2("recorder", "test-password")
    adapter.handshake()
    response = proto.session_pb2.SessionData(sec_ver=2)
    response.sec2.msg = proto.sec2_pb2.S2Session_Response0
    response.sec2.sr0.device_pubkey = b"\x03"
    response.sec2.sr0.device_salt = b"s" * 16
    adapter.handshake(response.SerializeToString())
    # Self-consistency fixture, not a server/hardware interoperability test.
    # Use the expected proof from Espressif's client; no local SRP implementation.
    context = adapter._security.srp6a_ctx
    response = proto.session_pb2.SessionData(sec_ver=2)
    response.sec2.msg = proto.sec2_pb2.S2Session_Response1
    response.sec2.sr1.device_proof = context.H_AMK
    response.sec2.sr1.device_nonce = b"\x00" * 12
    assert adapter.handshake(response.SerializeToString()) is None
    assert adapter.ready
    peer_cipher = AESGCM(context.get_session_key()[:32])
    ciphertext = adapter.encrypt(b"request")
    assert peer_cipher.decrypt(b"\x00" * 12, ciphertext, None) == b"request"
    assert bytes(adapter._security.nonce) == b"\x00" * 11 + b"\x01"
    reply = peer_cipher.encrypt(bytes(adapter._security.nonce), b"reply", None)
    assert adapter.decrypt(reply) == b"reply"
    assert bytes(adapter._security.nonce) == b"\x00" * 11 + b"\x02"
    with pytest.raises(ProtocolError):
        adapter.decrypt(reply)  # Replayed ciphertext authenticates under the wrong nonce.
    assert not adapter.ready
