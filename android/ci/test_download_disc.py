import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import download_disc as disc


class FakeResponse(io.BytesIO):
    """A minimal urlopen response: the body, plus the headers the downloader reads."""

    def __init__(self, data, headers=None, status=200):
        super().__init__(data)
        self.headers = headers or {}
        self.status = status

    def getcode(self):
        return self.status

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


class FakeOpener:
    def __init__(self, data, headers=None, response=None):
        self.data = data
        self.headers = headers or {}
        self.response = response
        self.requests = []

    def open(self, request, timeout):
        self.requests.append(request)
        if self.response is not None:
            return self.response
        return FakeResponse(self.data, self.headers)


class Clock:
    """A clock that only moves when a read happens, so a stall and a budget are testable."""

    def __init__(self, step=0.0):
        self.value = 1000.0
        self.step = step

    def now(self):
        self.value += self.step
        return self.value


class DownloadTests(unittest.TestCase):
    def run_data(self, data, digest=None):
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener', return_value=FakeOpener(data)):
                path = disc.prepare('https://example.invalid/game', digest or hashlib.sha256(data).hexdigest(), Path(temp))
                return path.read_bytes()

    def test_iso_is_downloaded_with_progress_reported(self):
        """The step that ran blind for an hour now says what it is doing."""
        import contextlib
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as temp, \
             patch.object(disc.urllib.request, 'build_opener',
                          return_value=FakeOpener(b'SNCP8P' + b'x' * 4096,
                                                  {'Content-Length': '4102'})), \
             contextlib.redirect_stdout(out):
            path = disc.prepare('https://example.invalid/game.iso',
                                hashlib.sha256(b'SNCP8P' + b'x' * 4096).hexdigest(), Path(temp))
            self.assertEqual(path.read_bytes()[:6], b'SNCP8P')
        logged = out.getvalue()
        self.assertIn('Fetching from example.invalid', logged)
        self.assertIn('Downloading', logged)
        self.assertIn('Disc image ready', logged)
        # No URL, no signed query, in anything it printed.
        self.assertNotIn('example.invalid/game', logged)

    def test_a_stalled_transfer_fails_instead_of_hanging(self):
        """This is the failure the user hit: an hour with no progress and no explanation."""

        class Stalling(io.BytesIO):
            def __init__(self):
                super().__init__(b'SNCP8P' + b'x' * 64)
                self.headers = {'Content-Length': '1000000'}
                self.status = 200

            def read(self, n=-1):
                if self.tell() >= 64 + 6:
                    return b''      # a socket that never delivers the rest but never errors
                return super().read(n)

        clock = Clock(step=disc.STALL_SECONDS)   # every read "takes" the whole stall interval
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener',
                              return_value=FakeOpener(None, response=Stalling())):
                with self.assertRaises(disc.DiscSourceError) as raised:
                    disc.prepare('https://example.invalid/game.iso', '', Path(temp), clock=clock)
        self.assertIn('made no progress', str(raised.exception))
        self.assertNotIn('example.invalid', str(raised.exception))

    def test_a_slow_link_is_stopped_by_the_time_budget(self):
        """Each read takes longer than the whole budget, so it must stop after one."""
        clock = Clock(step=2)
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener',
                              return_value=FakeOpener(b'SNCP8P' + b'x' * 64)):
                with self.assertRaises(disc.DiscSourceError) as raised:
                    disc.prepare('https://example.invalid/game.iso', '', Path(temp),
                                 clock=clock, budget=1)
        message = str(raised.exception)
        self.assertIn('time budget', message)
        self.assertNotIn('example.invalid', message)

    def test_a_bad_link_is_named_without_the_url(self):
        import urllib.error
        error = urllib.error.HTTPError('https://example.invalid/secret?token=abc', 403,
                                       'Forbidden', {}, None)
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener') as opener:
                opener.return_value.open.side_effect = error
                with self.assertRaises(disc.DiscSourceError) as raised:
                    disc.prepare('https://example.invalid/secret?token=abc', '', Path(temp))
        message = str(raised.exception)
        self.assertIn('403', message)
        self.assertNotIn('example.invalid', message)
        self.assertNotIn('token=abc', message)

    def test_a_permanent_http_error_is_not_retried(self):
        """A 403/404 will not change by asking again; retrying just looks like another hang."""
        import urllib.error
        error = urllib.error.HTTPError('https://example.invalid/x?sig=abc', 403, 'Forbidden', {}, None)
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener') as opener:
                opener.return_value.open.side_effect = error
                with self.assertRaises(disc.DiscSourceError) as raised:
                    disc.prepare('https://example.invalid/x?sig=abc', '', Path(temp))
            self.assertEqual(opener.return_value.open.call_count, 1)
        self.assertIn('403', str(raised.exception))

    def test_a_partial_download_is_resumed(self):
        data = b'SNCP8P' + b'y' * 1024
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp / 'download.bin').write_bytes(data[:100])
            opener = FakeOpener(None, response=FakeResponse(
                data[100:], {'Content-Range': 'bytes 100-%d/%d' % (len(data) - 1, len(data))},
                status=206))
            with patch.object(disc.urllib.request, 'build_opener', return_value=opener):
                path = disc.prepare('https://example.invalid/game.iso',
                                    hashlib.sha256(data).hexdigest(), temp)
            self.assertEqual(path.read_bytes(), data)
            # It asked for the rest rather than starting over.
            self.assertEqual(opener.requests[0].get_header('Range'), 'bytes=100-')

    def test_a_server_without_resume_support_starts_over_cleanly(self):
        data = b'SNCP8P' + b'z' * 1024
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp / 'download.bin').write_bytes(b'garbage')
            opener = FakeOpener(data, {'Content-Length': str(len(data))})   # 200, no range
            with patch.object(disc.urllib.request, 'build_opener', return_value=opener):
                path = disc.prepare('https://example.invalid/game.iso',
                                    hashlib.sha256(data).hexdigest(), temp)
            self.assertEqual(path.read_bytes(), data)

    def test_a_leftover_file_larger_than_the_remote_one_is_not_appended_to(self):
        data = b'SNCP8P' + b'q' * 100
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp / 'download.bin').write_bytes(b'x' * 5000)     # larger than the remote file
            opener = FakeOpener(data, {'Content-Length': str(len(data))})
            with patch.object(disc.urllib.request, 'build_opener', return_value=opener):
                path = disc.prepare('https://example.invalid/game.iso',
                                    hashlib.sha256(data).hexdigest(), temp)
            self.assertEqual(path.read_bytes(), data)

    def test_iso(self):
        self.assertEqual(self.run_data(b'SNCP8Ptest'), b'SNCP8Ptest')

    def test_zip_with_traversal_name_is_written_to_fixed_path(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            z.writestr('../../game.iso', b'SNCP8Ptest')
        self.assertEqual(self.run_data(data.getvalue()), b'SNCP8Ptest')

    def test_bad_digest(self):
        with self.assertRaises(ValueError): self.run_data(b'SNCP8Ptest', '0' * 64)

    def test_wrong_region(self):
        with self.assertRaises(ValueError): self.run_data(b'SNCE8Ptest')

    def test_unpinned_digest_is_accepted_and_reported(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            z.writestr('game.iso', b'SNCP8Ptest')
        with tempfile.TemporaryDirectory() as temp:
            import contextlib
            out = io.StringIO()
            with patch.object(disc.urllib.request, 'build_opener', return_value=FakeOpener(data.getvalue())), \
                 contextlib.redirect_stdout(out):
                path = disc.prepare('https://example.invalid/game', '', Path(temp))
            self.assertEqual(path.read_bytes(), b'SNCP8Ptest')
            self.assertIn(hashlib.sha256(data.getvalue()).hexdigest(), out.getvalue())

    def test_pinned_digest_still_enforced(self):
        with self.assertRaises(ValueError):
            self.run_data(b'SNCP8Ptest', 'a' * 64)

    def test_zip_with_multiple_isos_rejected(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            z.writestr('a.iso', b'SNCP8Pa')
            z.writestr('b.iso', b'SNCP8Pb')
        with self.assertRaises(ValueError): self.run_data(data.getvalue())


if __name__ == '__main__': unittest.main()
