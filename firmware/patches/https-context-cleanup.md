# HTTPS context allocation failure cleanup

r11-93 replaces only the build input of esp_https_server/src/https_server.c.
tools/patch_https_server.py checks the exact SDK source SHA256 after CRLF/LF normalization, adds TLS session
deletion before the transport-context allocation failure returns, and emits the
patched source and provenance JSON under the build directory. The shared SDK is
never modified. Normal close and handshake-error cleanup remain unchanged.

When changing ESP-IDF, review this branch against upstream and update the pinned
source hash only after the failure-injection test passes; do not bypass the check.
The generated source retains Espressif's Apache-2.0 license header.

The optional CMake flag HEIMAO_SDR_HTTP_TIMING adds handshake timing to a lab
build. The HTTPS server records timings for PCM POSTs with a probe number and
keeps the most recent 32 records for readback after release. Timings use the
device's monotonic clock and the trace contains no PCM.
