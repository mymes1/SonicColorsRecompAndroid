#pragma once
// Android runtime entry point (see app.cpp). Called from the JNI thread that owns the
// game surface; returns only when the runtime could not start.
#include <string>
#include <vector>

int AndroidMain(std::vector<std::string> args);

// Turns on the Vulkan validation layer when the APK ships it (debug builds and the
// emulator job); read from SCR_ANDROID_VULKAN_VALIDATION at startup.
extern bool g_vkValidation;
