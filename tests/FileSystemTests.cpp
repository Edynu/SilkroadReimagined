#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>

#include "TestPk2Writer.h"
#include "sr/assets/FileSystem.h"

using namespace sr::assets;

namespace {

std::string ReadString(const FileSystem& fs, std::string_view path) {
    auto data = fs.Read(path);
    REQUIRE(data.has_value());
    return {reinterpret_cast<const char*>(data->Bytes().data()), data->Size()};
}

std::unique_ptr<Pk2Source> OpenSample() {
    const auto path = sr::test::TempPath("fs_sample.pk2");
    sr::test::WriteSampleArchive(path);
    Pk2Archive archive;
    std::string error;
    REQUIRE_MESSAGE(archive.Open(path, error), error);
    return std::make_unique<Pk2Source>(std::move(archive));
}

} // namespace

TEST_CASE("FileSystem serves files from a mounted archive") {
    FileSystem fs;
    fs.Mount(OpenSample());
    CHECK(fs.Exists("res\\char\\man.bsr"));
    CHECK(ReadString(fs, "Res/Char/Man.bsr") == "man-bsr");
    CHECK_FALSE(fs.Read("nope.txt").has_value());
}

TEST_CASE("An override folder replaces single files and falls back to the archive") {
    const auto overrides = sr::test::TempPath("overrides");
    std::filesystem::remove_all(overrides);
    std::filesystem::create_directories(overrides / "res" / "char");
    {
        std::ofstream os(overrides / "res" / "char" / "man.bsr", std::ios::binary);
        os << "remastered";
    }

    FileSystem fs;
    fs.Mount(OpenSample(), FileSystem::kGamePriority);
    fs.Mount(std::make_unique<DirectorySource>(overrides), FileSystem::kOverridePriority);

    CHECK(ReadString(fs, "res\\char\\man.bsr") == "remastered");
    CHECK(ReadString(fs, "readme.txt") == "hello"); // not overridden
    REQUIRE(fs.Resolve("res\\char\\man.bsr") != nullptr);
    CHECK(fs.Resolve("res\\char\\man.bsr")->Name() == overrides.string());
    CHECK(fs.Resolve("readme.txt")->Name() == "fs_sample.pk2");
}

TEST_CASE("Mount priority does not depend on mount order") {
    const auto overrides = sr::test::TempPath("overrides_order");
    std::filesystem::remove_all(overrides);
    std::filesystem::create_directories(overrides);
    {
        std::ofstream os(overrides / "readme.txt", std::ios::binary);
        os << "override";
    }

    FileSystem fs;
    fs.Mount(std::make_unique<DirectorySource>(overrides), FileSystem::kOverridePriority);
    fs.Mount(OpenSample(), FileSystem::kGamePriority);
    CHECK(ReadString(fs, "readme.txt") == "override");
}

TEST_CASE("DirectorySource refuses paths that escape its root") {
    const auto root = sr::test::TempPath("escape_root");
    std::filesystem::create_directories(root);
    {
        std::ofstream os(sr::test::TempPath("secret.txt"), std::ios::binary);
        os << "secret";
    }
    DirectorySource source(root);
    CHECK_FALSE(source.Contains("..\\secret.txt"));
    CHECK_FALSE(source.Read("..\\secret.txt").has_value());
}
