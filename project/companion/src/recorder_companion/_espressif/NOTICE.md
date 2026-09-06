# Vendored Espressif implementation

The Security 2 SRP6a/AES-GCM implementation is **Espressif's implementation**,
not a local crypto implementation. All upstream copyright/SPDX headers are
preserved. See the accompanying Apache-2.0 `LICENSE`.

## Source pins

The [Component Registry network_provisioning 1.2.4 metadata](https://components.espressif.com/components/espressif/network_provisioning/versions/1.2.4)
identifies the repository and commit below. ESP-IDF 6.1 removed the old in-tree
Wi-Fi provisioning tool, so do not install a guessed `esp-network-provisioning`
package or use an older Security 1 example.

* Repository: <https://github.com/espressif/idf-extra-components>
* Commit: `2de4980640bbe3d2d69473d7251640039e185b92`
* Component: `network_provisioning`, release `1.2.4`
* `security\{security.py,security2.py,srp6a.py}` and
  `utils\{__init__.py,convenience.py}` originate under
  `network_provisioning/tool/esp_prov/`.
* `proto\network_{constants,config}_pb2.py` originate under
  `network_provisioning/python/`.

Protocomm generated messages and license:

* Repository: <https://github.com/espressif/esp-idf>
* Tag: `v6.1`
* Commit: `fff9895c82d744c7237be8847347bdd1b07c6643`
* `proto\{constants,sec0,sec1,sec2,session}_pb2.py` originate under
  `components/protocomm/python/`.
* `LICENSE` originates at the repository root.

Only import paths were changed in upstream files: `proto` / `utils` became
package-relative imports, as did the generated protobuf sibling imports.
The three package initializers outside `utils` are local packaging glue.
No SRP, key derivation, nonce or AES implementation changes were made.
The `sec0`/`sec1` **message definitions** are necessary imports of Espressif's
`session_pb2`; no Security 0 or Security 1 implementation or fallback is shipped.

The enclosing adapter always constructs `Security2(1, username, password, False)`.
Patch 1 increments the AES-GCM nonce for each encryption/decryption; patch 0
is deliberately refused. Upstream verbose logging is always off.
The local adapter additionally checks handshake types, status, lengths and
readiness before application payloads are permitted.

The upstream BLE implementation (`tool/esp_prov/transport/ble_cli.py` at the same
component pin) documents the transport pattern reused in `ble.py`: discover
user-description descriptors, write the complete characteristic with response,
then read the characteristic. The local transport adds explicit bounds,
timeouts, WinRT uncached reads and fail-closed reconnect behavior. No upstream
console/plaintext fallback is reused.

## Original file SHA-256 values

These hashes are of the original downloads **before** import relocation.
To update, fetch the exact repositories/commits above, compare these hashes,
apply only the documented import relocation, and rerun companion tests.
Do not silently download a moving branch during installation.

```text
CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30 LICENSE
24077AD3A8830C9035DC5CA22B9AAFD74218BCFBCF5AE0BA4CBFD2446B9199A9 proto\constants_pb2.py
661034B2A3AE032A439CC066CBB46E5BA4C5FCF1CE4891AC66779D9C534DEDD9 proto\network_config_pb2.py
C943B8DAAEA27DE97A5DAD44A36775520058F1DE3B6819EE18B27A6EF62427DB proto\network_constants_pb2.py
87232AEBC640E012B6C3B5184EAA10296C9679890D985C9AD9067B5F1AA22E39 proto\sec0_pb2.py
93BE151ECF6890B4464AACA77CA892693997C49D2308258A61D202A44FC2FDA8 proto\sec1_pb2.py
51BDBBEFABC8A948F7307F9B682E32FBFCAAA1B8BEE34E16CB2FC1D937230379 proto\sec2_pb2.py
8130C80BE0DB49C86D6897F9F48AB2CB7FB676D4886D15E549BB908324AA767D proto\session_pb2.py
37A88D266D730154B0B914E51B68E0E50D2565A192CEDF76AC57F2743F79C58E security\security.py
BDC90F36A754A9A767AA7298BD697B7ED0DDF42777BDA159D13ED646A7C39445 security\security2.py
6074AB48D653669B6DA24525CF7B7D6AC6AB4B88E4274AE69C2DF8597C2A0834 security\srp6a.py
E88FDF01F49DF6317AF44A84B1328B75909037D0965E7F2936EDB48CFEAC1CB1 utils\__init__.py
CA6C639A8285BFA2C757D2B3D4879B25C63C8A1FDC71A395479836686C2C667E utils\convenience.py
```
