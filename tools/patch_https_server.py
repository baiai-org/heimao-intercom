"""Build-local ESP-IDF HTTPS cleanup fix. Never edits the shared SDK."""
import argparse
import hashlib
import json
from pathlib import Path

# Canonical LF bytes: Windows checkouts use CRLF, GitHub's Linux runner uses LF.
BASE_SHA256 = 'ae694389d3bfe73314aacbe40e780719b7aad36d9aa5cfd6f61038f7d8533330'

def patched_source(raw):
    normalized = raw.replace(b'\r\n', b'\n')
    if hashlib.sha256(normalized).hexdigest() != BASE_SHA256:
        raise ValueError('Unsupported esp_https_server source; review the cleanup patch before changing SDK')
    text = normalized.decode('utf-8')
    marker = '    if (!transport_ctx) {'
    start = text.index(marker)
    end = text.index('    transport_ctx->tls = tls;', start)
    block = text[start:end]
    needle = '        return ESP_ERR_NO_MEM;'
    if block.count(needle) != 1:
        raise ValueError('Unexpected transport context failure branch')
    block = block.replace(needle, '        /* r11-93: not attached to HTTP yet; reclaim the completed TLS session. */\n'
                         '        esp_tls_server_session_delete(tls);\n' + needle)
    return (text[:start] + block + text[end:]).encode('utf-8')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.source.read_bytes()
    patched = patched_source(raw)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != patched:
        args.output.write_bytes(patched)
    args.output.with_suffix('.patch.json').write_text(json.dumps({
        'patch': 'https-context-oom-cleanup-r11-93', 'sdk_source': str(args.source.resolve()),
        'sdk_sha256': hashlib.sha256(raw).hexdigest(), 'generated_sha256': hashlib.sha256(patched).hexdigest(),
        'sdk_normalized_sha256': hashlib.sha256(raw.replace(b'\r\n', b'\n')).hexdigest(),
        'shared_sdk_modified': False}, indent=2), encoding='utf-8')

if __name__ == '__main__':
    main()
