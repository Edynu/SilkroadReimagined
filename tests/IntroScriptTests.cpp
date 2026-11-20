#include <doctest/doctest.h>

#include "sr/assets/IntroScript.h"
#include "sr/assets/Terrain.h"

using namespace sr::assets;

TEST_CASE("DecodeIntroCamera reads S_CameraInsert keys and ignores other lines") {
    const char* text = "0 S_Fade 1 2\r\n"
                       "1 S_CameraInsert 0 78 105 100 50 200 0.5 3.14 0 10\r\n"
                       "2\tS_CameraInsert\t1\t78\t105\t200\t60\t300\t0.25f\t-3.0\t0.1\t10\r\n";
    std::vector<CameraKey> keys;
    std::string error;
    REQUIRE_MESSAGE(DecodeIntroCamera(text, keys, error), error);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].sectorX == 78);
    CHECK(keys[0].sectorY == 105);
    CHECK(keys[0].position[2] == 200.0f);
    CHECK(keys[0].rotation[1] == doctest::Approx(3.14f));
    CHECK(keys[1].rotation[0] == doctest::Approx(0.25f));
    CHECK(keys[1].distance == 10.0f);

    CHECK_FALSE(DecodeIntroCamera("x S_CameraInsert 0 1 2\n", keys, error));
    CHECK_FALSE(DecodeIntroCamera("nothing here\n", keys, error));
    CHECK_FALSE(DecodeIntroCamera("1 S_CameraInsert 1 0 0 0 0 0 0 0 0 1\n2 S_CameraInsert 1 0 0 0 0 0 0 0 0 1\n", keys,
                                  error));
}

TEST_CASE("SampleCameraTrack interpolates through the keys") {
    std::vector<CameraKey> keys(4);
    for (int i = 0; i < 4; ++i) {
        keys[static_cast<std::size_t>(i)].time = static_cast<float>(i);
        keys[static_cast<std::size_t>(i)].position[0] = static_cast<float>(i) * 100.0f;
        keys[static_cast<std::size_t>(i)].distance = 10.0f;
    }
    CHECK(SampleCameraTrack(keys, -1.0f).position[0] == 0.0f);
    CHECK(SampleCameraTrack(keys, 1.0f).position[0] == doctest::Approx(100.0f)); // passes through keys
    CHECK(SampleCameraTrack(keys, 1.5f).position[0] == doctest::Approx(150.0f)); // evenly spaced keys: a straight line
    CHECK(SampleCameraTrack(keys, 9.0f).position[0] == 300.0f);
    CHECK(SampleCameraTrack(keys, 0.5f).distance == doctest::Approx(10.0f));
}

TEST_CASE("SampleCameraTrack turns the short way and joins sectors") {
    std::vector<CameraKey> keys(2);
    keys[0].rotation[1] = 3.0f;
    keys[1].time = 1.0f;
    keys[1].rotation[1] = -3.0f; // 0.28 rad further, across +-pi
    const float yaw = SampleCameraTrack(keys, 0.5f).rotation[1];
    CHECK(std::abs(std::abs(yaw) - 3.1416f) < 0.05f);

    keys[0].sectorX = 78;
    keys[0].position[0] = kRegionSize - 10.0f;
    keys[1].sectorX = 79;
    keys[1].position[0] = 10.0f; // 20 units east of key 0
    const CameraKey mid = SampleCameraTrack(keys, 0.5f);
    CHECK(mid.sectorX == 78);
    CHECK(mid.position[0] == doctest::Approx(kRegionSize));
}
