import json
import socket
import threading

import pytest

from app.network_sync import NetworkProfile, load_profile, save_profile, probe_server


def test_default_profile_and_roundtrip(tmp_path):
    path = tmp_path / "Config" / "network.json"
    assert load_profile(path).role == "standalone"
    profile = NetworkProfile(role="client", server_host="192.168.1.20", server_port=9443)
    save_profile(profile, path)
    assert load_profile(path) == profile
    assert json.loads(path.read_text(encoding="utf-8"))["use_tls"] is True


def test_network_modes_require_tls_and_host_for_client():
    with pytest.raises(ValueError):
        NetworkProfile(role="client").validate()
    with pytest.raises(ValueError):
        NetworkProfile(role="server", use_tls=False).validate()
    with pytest.raises(ValueError):
        NetworkProfile(role="client", server_host="pc", server_port=70000).validate()


def test_probe_server_detects_local_listener():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    thread = threading.Thread(target=lambda: listener.accept()[0].close(), daemon=True)
    thread.start()
    try:
        ok, message = probe_server("127.0.0.1", port)
        assert ok and "TCP-порт доступен" in message
    finally:
        listener.close()


def test_probe_server_handles_unavailable_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    ok, message = probe_server("127.0.0.1", port, timeout=0.2)
    assert not ok and "недоступен" in message
