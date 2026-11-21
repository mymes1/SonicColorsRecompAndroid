"""Test the Android 64-bit guest memory mapping on a Linux host."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MemoryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler required')
    def test_memory_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / 'test.cpp').write_text('''
#include "runtime/kernel/memory.h"
#include <cassert>
int main() {
    assert(Memory_Init());
    auto* first = g_base;
    assert(Memory_Init() && g_base == first);
    Write32(0x80001000, 0x12345678);
    assert(Read32(0x00001000) == 0x12345678);
    assert(Read32(0xC0001000) == 0x12345678);
    Write32(0xD0000100, 0x87654321);
    assert(Read32(0x10000100) == 0x87654321);
    assert(Read32(0x90000100) == 0x87654321);
    Write32(LOCKED_CACHE_ADDR, 0xaabbccdd);
    assert(Read32(LOCKED_CACHE_ADDR) == 0xaabbccdd);
}
''')
            subprocess.run(['g++', '-std=c++20', '-pthread', '-I', str(ROOT),
                            str(ROOT / 'runtime/platform/android/memory.cpp'), str(tmp / 'test.cpp'),
                            '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)


if __name__ == '__main__': unittest.main()
