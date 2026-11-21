// Settings::Set and Get, used by the remote game commands.

#include "TestRunner.hpp"
#include "utils/Settings.h"

TEST_CASE(SettingsSetFromText)
{
    Settings settings;
    CHECK(settings.Set("CaptureAtDate", "true") && settings.captureAtDate);
    CHECK(settings.Set("capturemonth", "7") && settings.captureMonth == 7);  // any case
    CHECK(settings.Get("CaptureMonth") == "7");
    // Out of range or not a number: logged, the field stays.
    CHECK(settings.Set("CaptureMonth", "13") && settings.captureMonth == 7);
    CHECK(settings.Set("MaxRenderZoom", "x") && settings.maxRenderZoom == 4);
    CHECK(settings.Set("RenderRotation2", "false") && !settings.renderRotation[1]);
    CHECK(!settings.Set("NoSuchKey", "1") && settings.Get("NoSuchKey").empty());
}
