#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "TestPk2Writer.h"
#include "sr/assets/Pk2Archive.h"

using sr::assets::Pk2Archive;

namespace {

std::string AsString(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

TEST_CASE("Pk2Archive indexes nested folders and chained blocks") {
    const auto path = sr::test::TempPath("sample.pk2");
    sr::test::WriteSampleArchive(path);

    Pk2Archive archive;
    std::string error;
    REQUIRE_MESSAGE(archive.Open(path, error), error);

    CHECK(archive.KeyVerifiedByHeader());
    CHECK(archive.Files().size() == 4);
    CHECK(archive.FolderCount() == 2); // Res, Res\Char ("." and ".." are skipped)

    const auto* man = archive.Find("res/char/man.bsr");
    REQUIRE(man != nullptr);
    CHECK(man->path == "Res\\Char\\Man.BSR");
    CHECK(AsString(archive.Read(*man)) == "man-bsr");

    REQUIRE(archive.Find("ROOT2.TXT") != nullptr);
    CHECK(AsString(archive.Read(*archive.Find("ROOT2.TXT"))) == "root2");
    CHECK(AsString(archive.Read(*archive.Find("\\Res\\Extra.dat"))) == "extra");
    CHECK(archive.Find("res\\char") == nullptr); // folders are not files
    CHECK(archive.Find("missing.txt") == nullptr);
}

TEST_CASE("Pk2Archive rejects a wrong key") {
    const auto path = sr::test::TempPath("sample_wrong_key.pk2");
    sr::test::WriteSampleArchive(path);

    Pk2Archive archive;
    std::string error;
    CHECK_FALSE(archive.Open(path, error, "000000"));
    CHECK(error.find("key") != std::string::npos);
}

TEST_CASE("Pk2Archive opens archives made with another key") {
    const auto path = sr::test::TempPath("sample_other_key.pk2");
    sr::test::WriteSampleArchive(path, "abcdef");

    Pk2Archive archive;
    std::string error;
    REQUIRE_MESSAGE(archive.Open(path, error, "abcdef"), error);
    CHECK(archive.Files().size() == 4);
}

TEST_CASE("Pk2Archive rejects files that are not archives") {
    const auto path = sr::test::TempPath("not_an_archive.pk2");
    {
        std::ofstream os(path, std::ios::binary);
        os << std::string(4096, 'x');
    }
    Pk2Archive archive;
    std::string error;
    CHECK_FALSE(archive.Open(path, error));
    CHECK_FALSE(archive.Open(sr::test::TempPath("does_not_exist.pk2"), error));
}
