package org.soniccolorsrecomp.android;

import static org.junit.Assert.*;

import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class MainActivityTest {
    private static TextView findText(View view, String text) {
        if (view instanceof TextView && text.contentEquals(((TextView) view).getText())) {
            return (TextView) view;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                TextView result = findText(group.getChildAt(i), text);
                if (result != null) return result;
            }
        }
        return null;
    }

    @Test public void optionalRecompLibraryMetadataLoads() {
        if (BuildConfig.RECOMP_INCLUDED) assertEquals("SNCP8P", RecompCode.gameId());
    }

    @Test public void nativeControllerStateIsResetOnDisconnect() {
        AndroidInput.reset();
        assertEquals(-1, AndroidInput.readButtonsForTest());
        AndroidInput.setConnected(true);
        AndroidInput.setButton(0x1000, true);
        assertEquals(0x1000, AndroidInput.readButtonsForTest());
        AndroidInput.setConnected(false);
        AndroidInput.setConnected(true);
        assertEquals(0, AndroidInput.readButtonsForTest());
        AndroidInput.reset();
    }

    @Test public void nativeCpuPrimitivesWork() {
        assertTrue(NativeProbe.checkCpuPrimitives());
    }

    @Test public void launcherAndControlEditorAreAvailable() {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            scenario.onActivity(activity -> {
                View root = activity.getWindow().getDecorView();
                assertNotNull(findText(root, "Choose Wii ISO or ZIP"));
                Button play = (Button) findText(root, "Play (runtime unavailable)");
                assertNotNull(play);
                assertFalse(play.isEnabled());
                Button arrange = (Button) findText(root, "Arrange touch controls");
                assertNotNull(arrange);
                arrange.performClick();
                root = activity.getWindow().getDecorView();
                assertNotNull(findText(root, "Stick"));
                assertNotNull(findText(root, "A"));
                Button done = (Button) findText(root, "Done");
                assertNotNull(done);
                done.performClick();
                assertNotNull(findText(activity.getWindow().getDecorView(), "Choose Wii ISO or ZIP"));
            });
        }
    }
}
