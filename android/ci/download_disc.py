#!/usr/bin/env python3
"""Download a private HTTPS disc URL, safely materializing one ISO from a ZIP.

The URL comes from an environment variable to avoid putting it in shell command
lines or workflow inputs. No downloaded bytes are published as workflow artifacts.

This step used to run for an hour without printing anything, so a dead link, a host that
answers 200 and then trickles or stalls, and a large disc all looked the same. It now:

  * prints a line per progress percentage, after the response headers (so it is visible
    that the server answered and how big the file is), and one when unpacking starts;
  * fails a read that makes no progress for DISC_STALL_SECONDS, with the cause named;
  * stops after DISC_TIME_BUDGET, explained as a budget rather than left looking hung;
  * resumes a partial download it wrote itself in an earlier run of this job, so a second
    attempt over a slow link does not start from zero.

DISC_SHA256 is optional. When it is set, the download must match it before anything
is unpacked. When it is empty, the download's own SHA-256 is printed instead: a
digest is not a secret, so printing it lets the next run pin the link.
"""
import hashlib
import http.client
import os
from pathlib import Path
import re
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

MAX_DOWNLOAD = 10 * 1024**3
MAX_ISO = 5 * 1024**3
BLOCK = 1 << 20

# Defaults chosen so a failure is reported inside the job's timeout rather than at it.
STALL_SECONDS = 90
TIME_BUDGET_SECONDS = 50 * 60
PROGRESS_EVERY = 10          # percent
RESUME_ATTEMPTS = 3


class DiscInputError(ValueError):
    """A problem with what the operator supplied: reported as written."""


class DiscSourceError(RuntimeError):
    """The link or the server failed. The message is built here, never from the exception.

    A network exception's text can contain the signed URL, and so can a redirect's Location
    header, so only what this file writes reaches the log.
    """


class HTTPSRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme.lower() != "https":
            raise DiscSourceError("the download redirected away from HTTPS; refused")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Clock:
    """Where the elapsed time and the stalled seconds come from, so tests can move them."""

    def now(self):
        return time.monotonic()


def human_bytes(n):
    value = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return ("%.0f %s" % (value, unit)) if unit == "B" else ("%.1f %s" % (value, unit))
        value /= 1024.0
    return "%.1f GB" % value


def human_time(seconds):
    minutes, seconds = divmod(int(seconds), 60)
    if minutes >= 60:
        return "%dh %02dm" % (minutes // 60, minutes % 60)
    return "%dm %02ds" % (minutes, seconds)


def length_from_headers(response):
    """The compressed length the server declared, or None. Never raises."""
    try:
        header = response.headers.get("Content-Length")
        if header is not None:
            return int(header)
        # A resumed response carries the total in Content-Range.
        content_range = response.headers.get("Content-Range")
        if content_range:
            match = re.search(r"/(\d+)\s*$", content_range)
            if match:
                return int(match.group(1))
    except (AttributeError, TypeError, ValueError):
        pass
    return None


def check_time_budget(clock, deadline):
    if deadline is not None and clock.now() > deadline:
        raise DiscSourceError(
            "the download did not finish within the time budget (%.0f minutes). "
            "The server is too slow for this job: use a faster link, or a smaller/compressed "
            "image (a ZIP is downloaded as-is and unpacked afterwards)."
            % (TIME_BUDGET_SECONDS / 60.0))


def copy_bounded(src, dst, limit, hasher=None, *, label="download", total_size=None,
                 clock=None, stall=STALL_SECONDS, deadline=None, already=0):
    """Copies src to dst, reporting progress and refusing to wait on a stalled source."""
    clock = clock or Clock()
    total = already
    next_report = PROGRESS_EVERY
    last_logged = clock.now()
    while True:
        check_time_budget(clock, deadline)
        data = src.read(BLOCK)
        if not data:
            return total
        total += len(data)
        if total > limit:
            raise DiscInputError("the input exceeds the size limit (%s)" % human_bytes(limit))
        dst.write(data)
        if hasher:
            hasher.update(data)

        # A read that returns b"" is the end; repeated empty reads are a stall.
        now = clock.now()
        if now - last_logged > stall:
            raise DiscSourceError(
                "the %s made no progress for %.0f seconds (stopped at %s); giving up on this "
                "link" % (label, stall, human_bytes(total)))
        if total_size:
            percent = int(total * 100 / total_size)
            if percent >= next_report:
                print("%s: %d%% (%s of %s)" % (label, percent, human_bytes(total),
                                               human_bytes(total_size)), flush=True)
                while next_report <= percent:
                    next_report += PROGRESS_EVERY
                last_logged = now
        elif now - last_logged >= 15:
            print("%s: %s so far" % (label, human_bytes(total)), flush=True)
            last_logged = now


def open_stream(opener, url, clock, deadline, offset=0):
    """Opens the URL, resuming from offset when the server allows it. Returns (response, offset)."""
    check_time_budget(clock, deadline)
    headers = {'User-Agent': 'SCR-Android-CI'}
    if offset:
        headers['Range'] = 'bytes=%d-' % offset
    request = urllib.request.Request(url, headers=headers)
    # The socket timeout is the stall guard for connection and header waits too: each read has to
    # produce bytes within it, and the loop below enforces the same limit over a sequence of reads.
    return opener.open(request, timeout=STALL_SECONDS)


def download_to(opener, url, download, clock, deadline, tries=RESUME_ATTEMPTS):
    """Downloads url into download, resuming after interruptions that made progress."""
    digest = hashlib.sha256()
    total_size = None
    offset = 0
    last_error = None
    for attempt in range(1, tries + 1):
        partial = download.exists() and download.stat().st_size
        offset = partial if partial else 0
        digest = hashlib.sha256()
        if offset:
            with download.open('rb') as existing:
                while True:
                    chunk = existing.read(BLOCK)
                    if not chunk:
                        break
                    digest.update(chunk)
            print("Resuming the download at %s (attempt %d of %d)"
                  % (human_bytes(offset), attempt, tries), flush=True)
        try:
            with open_stream(opener, url, clock, deadline, offset) as response:
                status = getattr(response, 'status', None) or response.getcode()
                total_size = length_from_headers(response)
                if offset:
                    if status == 206:
                        if total_size is not None:
                            # Content-Range gives the whole size; Content-Length only the rest.
                            content_length = response.headers.get("Content-Length")
                            try:
                                total_size = offset + int(content_length)
                            except (TypeError, ValueError):
                                pass
                    else:
                        # The server ignored the range and is starting over.
                        print("The server sent the whole file again (no resume support).", flush=True)
                        offset = 0
                        digest = hashlib.sha256()
                if total_size:
                    if offset >= total_size:
                        # The leftover file is not a prefix of this: start over rather than append.
                        print("The file received earlier does not match this download; "
                              "starting over.", flush=True)
                        offset = 0
                        digest = hashlib.sha256()
                    print("Downloading %s" % human_bytes(total_size), flush=True)
                    if total_size > MAX_DOWNLOAD:
                        raise DiscInputError("the file is %s, over the %s limit"
                                             % (human_bytes(total_size), human_bytes(MAX_DOWNLOAD)))
                else:
                    print("Downloading (the server did not declare a size)", flush=True)
                mode = 'ab' if offset else 'wb'
                with download.open(mode) as out:
                    # total_size is None when the server declares no length: then progress is
                    # reported per elapsed interval and per bytes received instead of a percentage.
                    copy_bounded(response, out, MAX_DOWNLOAD, digest, label="Download",
                                 total_size=total_size, clock=clock, deadline=deadline,
                                 already=offset)
            return digest
        except (DiscInputError, DiscSourceError):
            raise
        except (urllib.error.URLError, http.client.HTTPException, socket.timeout, OSError) as exc:
            # Only the class and the reason string are used; str(exc) may carry the signed URL.
            reason = _safe_reason(exc)
            if isinstance(exc, urllib.error.HTTPError) and 400 <= exc.code < 500 and exc.code not in (
                    408, 429):
                # 403/404/410 will not change by asking again: say so now instead of after three
                # attempts, which is exactly the kind of wait that looked like a hang.
                raise DiscSourceError("the server answered HTTP %d; the link is not usable "
                                      "(expired, wrong, or not shared)" % exc.code)
            made_progress = download.exists() and download.stat().st_size > 0
            last_error = reason
            if attempt >= tries:
                break
            print("The download was interrupted (%s); attempt %d of %d%s"
                  % (reason, attempt, tries,
                     ", resuming from what was received" if made_progress else ""), flush=True)
            time.sleep(2 * attempt)
    raise DiscSourceError(
        "the download did not complete after %d attempt(s): %s" % (tries, last_error or "unknown"))


def _safe_reason(exc):
    """A short reason that cannot contain the URL.

    HTTPError and URLError stringify their url/reason, so the class name plus the status code
    (which is what matters for a 403/404 link) is all that is reported.
    """
    if isinstance(exc, urllib.error.HTTPError):
        return "the server answered HTTP %s %s" % (exc.code, str(exc.reason or "").split("'")[0][:80])
    if isinstance(exc, urllib.error.URLError):
        inner = exc.reason
        if isinstance(inner, (socket.timeout, TimeoutError)):
            return "no answer from the server within %ds" % STALL_SECONDS
        if isinstance(inner, socket.gaierror):
            return "the host name did not resolve"
        if isinstance(inner, ConnectionError):
            return "the connection was refused or reset"
        return "the connection failed"
    if isinstance(exc, (socket.timeout, TimeoutError)):
        return "no data for %ds" % STALL_SECONDS
    if isinstance(exc, http.client.IncompleteRead):
        return "the connection closed before the file was complete"
    if isinstance(exc, ConnectionError):
        return "the connection was reset"
    return exc.__class__.__name__


def prepare(url, expected_hash, output_dir, *, clock=None, budget=TIME_BUDGET_SECONDS):
    clock = clock or Clock()
    deadline = clock.now() + budget if budget else None
    parts = urllib.parse.urlsplit(url)
    if parts.scheme.lower() != "https":
        raise DiscInputError("Disc URL must use HTTPS")
    if expected_hash and (len(expected_hash) != 64
                          or any(c not in '0123456789abcdefABCDEF' for c in expected_hash)):
        raise DiscInputError("DISC_SHA256 must be a 64-character SHA-256 digest")
    output_dir.mkdir(parents=True, exist_ok=True)
    download = output_dir / "download.bin"
    iso = output_dir / "disc.iso"
    opener = urllib.request.build_opener(HTTPSRedirects())
    print("Fetching from %s%s" % (parts.hostname or "the link",
                                  (" port %d" % parts.port) if parts.port else ""), flush=True)
    digest = download_to(opener, url, download, clock, deadline)
    actual = digest.hexdigest().lower()
    if expected_hash:
        if actual != expected_hash.lower():
            raise DiscInputError("Downloaded file SHA-256 does not match DISC_SHA256")
        print("SHA-256 matches the pinned digest.", flush=True)
    else:
        # Reported, not enforced: the operator can paste it into DISC_SHA256 or the
        # disc_sha256 input so later runs are pinned.
        print("Downloaded file SHA-256: %s (no digest was pinned)" % actual, flush=True)
    if zipfile.is_zipfile(download):
        print("Unpacking the ISO from the ZIP (%s)" % human_bytes(download.stat().st_size), flush=True)
        with zipfile.ZipFile(download) as archive:
            candidates = [x for x in archive.infolist() if not x.is_dir() and x.filename.lower().endswith('.iso')]
            if len(candidates) != 1:
                raise DiscInputError("ZIP must contain exactly one ISO")
            entry = candidates[0]
            if entry.flag_bits & 1 or entry.file_size > MAX_ISO:
                raise DiscInputError("Encrypted or oversized ISO entry")
            print("ISO inside the ZIP: %s" % human_bytes(entry.file_size), flush=True)
            # Never use the archive filename as a filesystem path.
            with archive.open(entry) as src, iso.open('wb') as dst:
                copy_bounded(src, dst, MAX_ISO, label="Unpacking", total_size=entry.file_size,
                             clock=clock, deadline=deadline)
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
    print("Disc image ready: %s" % human_bytes(iso.stat().st_size), flush=True)
    return iso


def main():
    started = time.monotonic()
    try:
        prepare(os.environ.get('DISC_URL', ''), os.environ.get('DISC_SHA256', ''),
                Path(sys.argv[1]))
    except DiscInputError as exc:
        print("Disc input rejected: %s" % exc, file=sys.stderr, flush=True)
        return 1
    except DiscSourceError as exc:
        print("Disc download failed: %s" % exc, file=sys.stderr, flush=True)
        return 1
    except Exception:
        # Anything else could carry URL text in its message, so only the type is reported.
        print("Disc download or ZIP processing failed (details hidden).", file=sys.stderr, flush=True)
        return 1
    print("Disc download and initial validation succeeded in %s."
          % human_time(time.monotonic() - started), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
