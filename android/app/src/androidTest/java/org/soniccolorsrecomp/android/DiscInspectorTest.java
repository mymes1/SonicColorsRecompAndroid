package org.soniccolorsrecomp.android;

import static org.junit.Assert.*;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

@RunWith(AndroidJUnit4.class)
public class DiscInspectorTest {
    private byte[] disc() {
        byte[] header = new byte[32];
        byte[] id = {'S', 'N', 'C', 'P', '8', 'P'};
        System.arraycopy(id, 0, header, 0, id.length);
        header[24] = 0x5d; header[25] = 0x1c;
        header[26] = (byte) 0x9e; header[27] = (byte) 0xa3;
        return header;
    }
    @Test public void identifiesIsoAndZip() throws Exception {
        DiscInspector.check(new ByteArrayInputStream(disc()), "game.iso");
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(data)) {
            out.putNextEntry(new ZipEntry("folder/game.iso"));
            out.write(disc());
            out.closeEntry();
        }
        DiscInspector.check(new ByteArrayInputStream(data.toByteArray()), "game.zip");
    }
    @Test public void rejectsWrongDiscAndTruncation() throws Exception {
        byte[] invalid = disc(); invalid[3] = 'E';
        try { DiscInspector.check(new ByteArrayInputStream(invalid), "game.iso"); fail(); }
        catch (IOException expected) { }
        try { DiscInspector.check(new ByteArrayInputStream(new byte[2]), "game.iso"); fail(); }
        catch (IOException expected) { }
    }
}
