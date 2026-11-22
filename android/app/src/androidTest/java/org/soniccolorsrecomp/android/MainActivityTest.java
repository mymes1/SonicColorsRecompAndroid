package org.soniccolorsrecomp.android;

import static org.junit.Assert.*;
import static org.junit.Assume.assumeTrue;

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
        if (!BuildConfig.RECOMP_INCLUDED) return;
        assumeTrue("the recompiled-code library is only built for arm64-v8a", RecompCode.available());
        assertEquals("SNCP8P", RecompCode.gameId());
    }

    @Test public void nativeControllerStateIsResetOnDisconnect() {
        // This APK ships arm64-v8a only; an x86_64 emulator has no runtime library to test against.
        assumeTrue("no native runtime for this ABI: " + AndroidInput.loadError(),
                   AndroidInput.loadError() == null);
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
        assumeTrue("no probe library for this ABI: " + NativeProbe.loadError(),
                   NativeProbe.available());
        assertTrue(NativeProbe.checkCpuPrimitives());
    }

    /**
     * The launcher as a player first meets it: the title, the disc picker, the key-file row (a
     * retail ISO cannot be installed without it), a Play button that stays disabled until data is
     * installed, and the settings page. These are the strings MainActivity actually sets; when one
     * of them changes this test should change with it rather than pass on a stale label.
     */
    @Test public void launcherOffersWhatAPlayerNeeds() {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            scenario.onActivity(activity -> {
                View root = activity.getWindow().getDecorView();
                assertNotNull(findText(root, "SONIC COLORS"));
                assertNotNull(findText(root, "Choose your disc image\u2026"));
                assertNotNull(findText(root, "Provide keys.txt (only needed for an encrypted ISO)"));
                TextView keys = findTextStartingWith(root, "Key file:");
                assertNotNull("the key-file state is not shown", keys);
                Button play = (Button) findText(root, "Play");
                assertNotNull(play);
                assertFalse("Play must wait for installed data", play.isEnabled());
                Button settings = (Button) findText(root, "Controller & touch settings");
                assertNotNull(settings);
                settings.performClick();
                root = activity.getWindow().getDecorView();
                assertNotNull(findText(root, "Movement input"));
                assertNotNull(findText(root, "Rearrange touch controls"));
                Button back = (Button) findText(root, "Back");
                assertNotNull(back);
                back.performClick();
                assertNotNull(findText(activity.getWindow().getDecorView(), "Choose your disc image\u2026"));
            });
        }
    }

    private static TextView findTextStartingWith(View view, String prefix) {
        if (view instanceof TextView && ((TextView) view).getText().toString().startsWith(prefix)) {
            return (TextView) view;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                TextView result = findTextStartingWith(group.getChildAt(i), prefix);
                if (result != null) return result;
            }
        }
        return null;
    }
}
