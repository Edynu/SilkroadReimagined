#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "sr/assets/UiLayout.h"
#include "sr/core/Text.h"

using namespace sr::assets;

namespace {

std::vector<std::byte> Utf16Le(std::u16string_view text) {
    std::vector<std::byte> bytes{std::byte{0xFF}, std::byte{0xFE}};
    for (char16_t c : text) {
        bytes.push_back(static_cast<std::byte>(c & 0xFF));
        bytes.push_back(static_cast<std::byte>(c >> 8));
    }
    return bytes;
}

constexpr const char* kLayout = R"(Interface Text
Section = Login, "0,0,288,140", "0"
{
  GDR_STA_LOGINWINDOW:CIFStatic
  {
    ID=INTEGER,"1"
    Rect=RECT,"0,0,288,140"
    DDJ=STRING,"interface\login\lgn_window.ddj"
    UV_LT=POINT,"0.25,0"
    UV_RB=POINT,"0.75,0.5"
    VAlign=INTEGER,"0"
  }
  GDR_EDIT_ID:CIFEdit
  {
    Rect=RECT,"80, 30, 180f, 20"
    ClientRect=RECT,"3,2,3,2"
    FontColor=COLOR,"255,200,100,50"
    HAlign=INTEGER,"1"
#ifdef EUROPE_SYSTEM
    Text=STRING,"UIIT_EU"
#else
    Text=STRING,"UIIT_KR"
#endif
  }
  GDR_BROKEN:CIFStatic
  {
    Text=STRING,"
  }
}
)";

} // namespace

TEST_CASE("DecodeTextFile handles UTF-16LE, UTF-8 BOM and plain ASCII") {
    const auto utf16 = Utf16Le(u"Login é\U0001F600");
    CHECK(sr::core::DecodeTextFile(utf16) == "Login \xC3\xA9\xF0\x9F\x98\x80");
    const char utf8[] = "\xEF\xBB\xBFok";
    CHECK(sr::core::DecodeTextFile(std::as_bytes(std::span(utf8, 5))) == "ok");
    const char plain[] = "abc";
    CHECK(sr::core::DecodeTextFile(std::as_bytes(std::span(plain, 3))) == "abc");
}

TEST_CASE("DecodeUiLayout reads sections, controls, properties and #ifdef branches") {
    UiLayout layout;
    std::string error;
    REQUIRE_MESSAGE(DecodeUiLayout(kLayout, DecodeUiDefines("EUROPE_SYSTEM // on\n// OFF\n"), layout, error), error);
    REQUIRE(layout.sections.size() == 1);
    CHECK(layout.sections[0].name == "Login");
    CHECK(layout.sections[0].rect.w == 288.0f);
    REQUIRE(layout.sections[0].nodes.size() == 3);

    const UiLayoutNode* window = layout.Find("GDR_STA_LOGINWINDOW");
    REQUIRE(window != nullptr);
    CHECK(window->type == "CIFStatic");
    CHECK(window->id == 1);
    CHECK(window->ddj == "interface\\login\\lgn_window.ddj");

    CHECK(window->uv[0] == doctest::Approx(0.25f));
    CHECK(window->uv[3] == doctest::Approx(0.5f));
    CHECK(window->vAlign == 0);
    const UiLayoutNode* edit = layout.Find("GDR_EDIT_ID");
    CHECK(edit->uv[2] == 1.0f); // default: the whole texture
    CHECK(edit->vAlign == 1);   // default: centred
    REQUIRE(edit != nullptr);
    CHECK(edit->rect.x == 80.0f);
    CHECK(edit->rect.w == 180.0f);
    CHECK(edit->hasClientRect);
    CHECK(edit->clientRect.x == 3.0f);
    CHECK(edit->fontColor.a == 255);
    CHECK(edit->fontColor.r == 200);
    CHECK(edit->fontColor.b == 50);
    CHECK(edit->hAlign == 1);
    CHECK(edit->text == "UIIT_EU");

    CHECK(layout.Find("GDR_BROKEN")->text.empty()); // unterminated quote = empty string
    CHECK(layout.Find("missing") == nullptr);

    REQUIRE(DecodeUiLayout(kLayout, {}, layout, error));
    CHECK(layout.Find("GDR_EDIT_ID")->text == "UIIT_KR");
}

TEST_CASE("DecodeUiLayout rejects unbalanced preprocessor branches") {
    UiLayout layout;
    std::string error;
    CHECK_FALSE(DecodeUiLayout("#ifdef A\n", {}, layout, error));
    CHECK_FALSE(DecodeUiLayout("#endif\n", {}, layout, error));
    CHECK_FALSE(DecodeUiLayout("#ifdef A\n#else\n#else\n#endif\n", {}, layout, error));
}

TEST_CASE("DecodeTextCatalog takes column 1 as key and column 8 as English") {
    const char* text = "// comment\r\n"
                       "1\tUIIT_STT_LOGIN_ID\tko\t\t\t\t\t\tAccount\t\r\n"
                       "1\tUIIT_MULTI\tko\t\t\t\t\t\tFirst\\nSecond\r\n"
                       "continued line\r\n"
                       "0\tUIIT_OFF\tko\r\n";
    TextCatalog catalog;
    std::string error;
    REQUIRE(DecodeTextCatalog(text, catalog, error));
    CHECK(catalog["UIIT_STT_LOGIN_ID"] == "Account");
    CHECK(catalog["UIIT_MULTI"] == "First\nSecond\ncontinued line");
    CHECK(catalog.contains("UIIT_OFF"));
    CHECK(catalog["UIIT_OFF"].empty());
}

TEST_CASE("text catalog: placeholders, comments and repeated keys") {
    const std::string text = "1\tSN_A\t\xEA\xB0\x80\t\t\t\t\t\t0\tFallback A\n"
                             "1\tSN_B\tk\t\t\t\t\t\tFirst\n"
                             "// a comment ends SN_B\n"
                             "1\tSN_B\tk\t\t\t\t\t\tSecond\n"
                             "1\tSN_C\tk\t\t\t\t\t\txxx\n"
                             "1\tSN_D\tk\t\t\t\t\t\t\t\xC3\xA9t\xC3\xA9\n";
    TextCatalog catalog;
    std::string error;
    REQUIRE(DecodeTextCatalog(text, catalog, error));
    CHECK(catalog["SN_A"] == "Fallback A"); // no English: the next column when it is plain English
    CHECK(catalog["SN_D"].empty());          // not when it is not
    CHECK(catalog["SN_B"] == "Second");
    CHECK(catalog["SN_C"].empty());
}
