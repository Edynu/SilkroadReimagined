#pragma once

// The create screens (C5e): the original's "Character
// Customize" for the race the pedestal picked (Europe from pscharactercreate_europe.txt, China from
// pscharactercreatechina.txt): a slowly turning view of the race's town behind the dressed
// character, the customize window (name and its check, sex, figure, height, volume, armour and
// weapon), the explain window, rotate and zoom, then confirm and create over the title session.
// It returns to the character select either way.
//
// Automated runs: --sex woman, --look F,H,V,W,A (figure, height, volume, weapon, armour slider
// values), --create-name NAME (types it, confirms and creates), --exit-on-create (ends the program
// with 0 when the character is created, 1 on a refusal), --create-clock S (the town's turn S seconds on).

#include <memory>
#include <string>

#include "frame/GameUi.h"
#include "screens/Screen.h"
#include "screens/WorldScreen.h"
#include "sr/assets/IntroScript.h"
#include "sr/ui/Context.h"
#include "title/ScreenFade.h"
#include "title/TitleShared.h"
#include "title/UiKit.h"

namespace sr::client {

class CreateScreen final : public Screen {
public:
    const char* Name() const override { return "create"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    // The sliders, in the explain window's topic order.
    enum Slider { kFigure, kHeight, kVolume, kWeapon, kArmour, kSliders };
    void HandleEvents(ScreenContext& context);
    void UpdateScene(ScreenContext& context, FrameSnapshot& snapshot);
    void Dress(ScreenContext& context);
    void DrawUnder(float width, float height); // the bars, the caption, the back image
    void Draw(ScreenContext& context, float height);
    void DrawCustom(float x, float y);
    int DrawSlider(Slider which, const assets::UiLayoutNode& node, float wx, float wy, int value, int min, int max, bool enabled);
    void DrawExplain(float x, float y);
    void Leave();
    // The figure and items the sliders mean now (0: none).
    struct Choice {
        std::uint32_t model = 0, chest = 0, legs = 0, shoes = 0, weapon = 0, shield = 0;
    };
    Choice Chosen() const;
    void SendCreate();
    // The range of a slider now (Europe's armour depends on the weapon).
    std::pair<int, int> Range(Slider which) const;
    // The armour set index (1 heavy, 2 light, 3 clothes; 0 none) the armour slider's value means.
    int ArmourSet() const;
    int WeaponClass() const; // the chosen weapon's fourth type id (0: none)
    std::string ExplainName() const;
    std::string ExplainText() const;

    std::shared_ptr<TitleShared> shared_;
    std::shared_ptr<CharacterData> tables_;
    std::unique_ptr<UiTextures> textures_;
    std::unique_ptr<UiKit> kit_;
    ui::Context ui_;
    FontAtlas atlas_;
    const assets::UiLayout* layout_ = nullptr;
    bool europe_ = true;

    // The scene: the town's turning view, the character in front of it.
    std::unique_ptr<WorldScreen> world_;
    std::vector<assets::CameraKey> view_; // the character window's camera track (zoom in and out)
    float viewTime_ = 0.0f;
    bool zoomed_ = false;
    double clock_ = 0.0; // added to the clock the town turns by (--create-clock)
    float angle_ = 0.0f, spin_ = 0.0f; // the character's turn (degrees) and its speed (degrees / s)
    bool lookDirty_ = true;
    bool shown_ = false; // the town and the character were in once: no black after that
    ScreenFade screenFade_; // in from black over 0.5 s when shown; out before the lobby
    bool leaving_ = false;
    float height_ = 1.0f; // the dressed height factor

    // The choices (slider values as the original's: 0 none where it has one).
    bool male_ = true;
    int value_[kSliders] = {1, 2, 2, 0, 0};
    int topic_ = kFigure; // what the explain window tells
    std::string name_;

    // Confirming and creating.
    bool warning_ = false; // the confirm window (GDR_STA_WCREATE) is open
    bool busy_ = false;
    std::string message_;
    std::string autoName_;
    bool autoConfirm_ = false; // the automated run presses the warning's Create
    bool exitOnCreate_ = false;
};

} // namespace sr::client
