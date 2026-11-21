package org.soniccolorsrecomp.android;

import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Shallow identification only. Does not install or fully validate a disc. */
final class DiscInspector {
    private static final long MAX_PREFIX_SCAN = 64L * 1024 * 1024;
    private static final int MAX_ENTRIES = 64;
    private DiscInspector() { }

    static void check(InputStream input, String name) throws IOException {
        if (name.toLowerCase(Locale.ROOT).endsWith(".zip")) {
            try (ZipInputStream zip = new ZipInputStream(input)) {
                long skipped = 0;
                byte[] buffer = new byte[8192];
                for (int entries = 0; entries < MAX_ENTRIES; entries++) {
                    ZipEntry entry = zip.getNextEntry();
                    if (entry == null) break;
                    if (!entry.isDirectory() && entry.getName().toLowerCase(Locale.ROOT).endsWith(".iso")) {
                        checkHeader(zip);
                        return; // Full archive validation belongs to the installer.
                    }
                    int count;
                    while ((count = zip.read(buffer)) != -1) {
                        skipped += count;
                        if (skipped > MAX_PREFIX_SCAN) throw new IOException("ZIP prefix is too large");
                    }
                }
                throw new IOException("No ISO found near the beginning of the ZIP");
            }
        } else {
            checkHeader(input);
        }
    }

    private static void checkHeader(InputStream input) throws IOException {
        byte[] header = new byte[32];
        int count = 0;
        while (count < header.length) {
            int n = input.read(header, count, header.length - count);
            if (n < 0) throw new IOException("Disc header is truncated");
            if (n == 0) throw new IOException("Could not read disc header");
            count += n;
        }
        byte[] expected = {'S', 'N', 'C', 'P', '8', 'P'};
        for (int i = 0; i < expected.length; i++) {
            if (header[i] != expected[i]) throw new IOException("Expected the European SNCP8P disc");
        }
        if ((header[24] & 255) != 0x5d || (header[25] & 255) != 0x1c ||
            (header[26] & 255) != 0x9e || (header[27] & 255) != 0xa3)
            throw new IOException("Not a Wii disc image");
    }
}
