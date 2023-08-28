#pragma once

#include <ToolboxModule.h>

class Obfuscator : public ToolboxModule {
public:
    static Obfuscator& Instance()
    {
        static Obfuscator instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Obfuscator"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_USER_SECRET; }

    void Initialize() override;
    void Update(float) override;
    void Terminate() override;
    void DrawSettingsInternal() override;
    void SaveSettings(ToolboxIni* ini) override;
    void LoadSettings(ToolboxIni* ini) override;

    static void Obfuscate(bool obfuscate);
};
