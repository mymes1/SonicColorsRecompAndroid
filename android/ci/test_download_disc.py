import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import download_disc as disc


class FakeOpener:
    def __init__(self, data): self.data = data
    def open(self, request, timeout): return io.BytesIO(self.data)


class DownloadTests(unittest.TestCase):
    def run_data(self, data, digest=None):
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(disc.urllib.request, 'build_opener', return_value=FakeOpener(data)):
                path = disc.prepare('https://example.invalid/game', digest or hashlib.sha256(data).hexdigest(), Path(temp))
                return path.read_bytes()

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

    def test_zip_with_multiple_isos_rejected(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            z.writestr('a.iso', b'SNCP8Pa')
            z.writestr('b.iso', b'SNCP8Pb')
        with self.assertRaises(ValueError): self.run_data(data.getvalue())


if __name__ == '__main__': unittest.main()
