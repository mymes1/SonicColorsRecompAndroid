#!/usr/bin/env python3
"""Download a private HTTPS disc URL, safely materializing one ISO from a ZIP.

The URL comes from an environment variable to avoid putting it in shell command
lines or workflow inputs. No downloaded bytes are published as workflow artifacts.

DISC_SHA256 is optional. When it is set, the download must match it before anything
is unpacked. When it is empty, the download's own SHA-256 is printed instead: a
digest is not a secret, so printing it lets the next run pin the link.
"""
import hashlib
import os
from pathlib import Path
import sys
import urllib.parse
import urllib.request
import zipfile

MAX_DOWNLOAD = 10 * 1024**3
MAX_ISO = 5 * 1024**3
BLOCK = 1024 * 1024


class DiscInputError(ValueError):
    pass


class HTTPSRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme.lower() != "https":
            raise DiscInputError("Non-HTTPS redirect rejected")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def copy_bounded(src, dst, limit, hasher=None):
    total = 0
    while True:
        data = src.read(BLOCK)
        if not data:
            return total
        total += len(data)
        if total > limit:
            raise DiscInputError("Input exceeds size limit")
        dst.write(data)
        if hasher:
            hasher.update(data)


def prepare(url, expected_hash, output_dir):
    if urllib.parse.urlsplit(url).scheme.lower() != "https":
        raise DiscInputError("Disc URL must use HTTPS")
    if expected_hash and (len(expected_hash) != 64
                          or any(c not in '0123456789abcdefABCDEF' for c in expected_hash)):
        raise DiscInputError("DISC_SHA256 must be a 64-character SHA-256 digest")
    output_dir.mkdir(parents=True, exist_ok=True)
    download = output_dir / "download.bin"
    iso = output_dir / "disc.iso"
    opener = urllib.request.build_opener(HTTPSRedirects())
    digest = hashlib.sha256()
    with opener.open(urllib.request.Request(url, headers={'User-Agent': 'SCR-Android-CI'}), timeout=60) as response:
        with download.open('wb') as out:
            copy_bounded(response, out, MAX_DOWNLOAD, digest)
    actual = digest.hexdigest().lower()
    if expected_hash:
        if actual != expected_hash.lower():
            raise DiscInputError("Downloaded file SHA-256 does not match DISC_SHA256")
    else:
        # Reported, not enforced: the operator can paste it into DISC_SHA256 or the
        # disc_sha256 input so later runs are pinned.
        print(f"Downloaded file SHA-256: {actual} (no digest was pinned)")
    if zipfile.is_zipfile(download):
        with zipfile.ZipFile(download) as archive:
            candidates = [x for x in archive.infolist() if not x.is_dir() and x.filename.lower().endswith('.iso')]
            if len(candidates) != 1:
                raise DiscInputError("ZIP must contain exactly one ISO")
            entry = candidates[0]
            if entry.flag_bits & 1 or entry.file_size > MAX_ISO:
                raise DiscInputError("Encrypted or oversized ISO entry")
            # Never use the archive filename as a filesystem path.
            with archive.open(entry) as src, iso.open('wb') as dst:
                copy_bounded(src, dst, MAX_ISO)
        download.unlink()
    else:
        if download.stat().st_size > MAX_ISO:
            raise DiscInputError("ISO exceeds size limit")
        download.rename(iso)
    if iso.stat().st_size < 6:
        raise DiscInputError("Disc image is too small")
    with iso.open('rb') as src:
        if src.read(6) != b'SNCP8P':
            raise DiscInputError("Disc is not the supported SNCP8P European Wii release")
    return iso


def main():
    try:
        prepare(os.environ.get('DISC_URL', ''), os.environ.get('DISC_SHA256', ''), Path(sys.argv[1]))
    except Exception as exc:
        # Network exceptions can contain signed URLs: do not print exception details.
        if isinstance(exc, DiscInputError):
            print(str(exc), file=sys.stderr)
        else:
            print("Disc download or ZIP processing failed (URL details hidden).", file=sys.stderr)
        return 1
    print("Disc download and initial validation succeeded.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
