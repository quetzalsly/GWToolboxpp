#include "stdafx.h"

#include <GWCA/Utilities/MemoryPatcher.h>
#include <GWCA/Utilities/Scanner.h>

#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Managers/QuestMgr.h>

#include <GWCA/GameEntities/Map.h>

#include <GWCA/Packets/StoC.h>

#include <Widgets/WorldMapWidget.h>

#include "Defines.h"
#include <GWCA/Managers/MapMgr.h>
#include <ImGuiAddons.h>
#include <GWCA/GameEntities/Quest.h>

#include <Constants/EncStrings.h>
#include <Modules/GwDatTextureModule.h>
#include <GWCA/Context/MapContext.h>
#include <GWCA/Constants/Maps.h>

namespace {
    ImRect show_all_rect;
    ImRect hard_mode_rect;
    ImRect place_marker_rect;
    ImRect remove_marker_rect;

    bool showing_all_outposts = false;

    bool drawn = false;

    GW::MemoryPatcher view_all_outposts_patch;
    GW::MemoryPatcher view_all_carto_areas_patch;

    uint32_t __cdecl GetCartographyFlagsForArea(uint32_t , uint32_t , uint32_t , uint32_t ) {
        return 0xffffffff;
    }

    bool world_map_clicking = false;
    GW::Vec2f world_map_click_pos;

    GW::Constants::QuestID custom_quest_id = (GW::Constants::QuestID)0x0000fdd;
    GW::Quest custom_quest_marker;
    GW::Vec2f custom_quest_marker_world_pos;

    bool WorldMapToGamePos(GW::Vec2f& world_map_pos, GW::GamePos* game_map_pos);

    ImRect GetMapWorldMapBounds(GW::AreaInfo* map) {

        auto bounds = &map->icon_start_x;
        if (*bounds == 0)
            bounds = &map->icon_start_x_dupe;

        // NB: Even though area info holds map bounds as uints, the world map uses signed floats anyway - a cast should be fine here.
        return ImRect(
            { static_cast<float> (bounds[0]), static_cast<float>(bounds[1]) }, 
            { static_cast<float> (bounds[2]), static_cast<float>(bounds[3]) }
        );
    }

    bool MapContainsWorldPos(GW::Constants::MapID map_id, const GW::Vec2f& world_map_pos) {
        const auto map = GW::Map::GetMapInfo(map_id);
        if (!(map && map->GetIsOnWorldMap()))
            return false;
        const auto map_bounds = GetMapWorldMapBounds(map);
        return map_bounds.Contains(world_map_pos);
    }

    GW::Constants::MapID GetMapIdForLocation(const GW::Vec2f& world_map_pos) {
        auto map_id = GW::Map::GetMapID();
        if (MapContainsWorldPos(map_id, world_map_pos))
            return map_id;
        for (size_t i = 1; i < static_cast<size_t>(GW::Constants::MapID::Count); i++) {
            map_id = static_cast<GW::Constants::MapID>(i);
            if (MapContainsWorldPos(map_id, world_map_pos))
                return map_id;
        }
        return GW::Constants::MapID::None;
    }

    void EmulateSelectedQuest(const GW::Quest* quest) {
        if (!quest) return;
        if (!GW::GameThread::IsInGameThread()) {
            GW::GameThread::Enqueue([quest]() {
                EmulateSelectedQuest(quest);
                });
            return;
        }
        struct QuestSetActive : GW::Packet::StoC::PacketBase {
            GW::Constants::QuestID quest_id;
            GW::GamePos marker;
            GW::Constants::MapID map_id;
        };
        QuestSetActive packet;
        packet.header = GAME_SMSG_QUEST_ADD_MARKER;
        packet.quest_id = quest->quest_id;
        packet.map_id = quest->map_to;
        GW::StoC::EmulatePacket(&packet);
    }

    void SetCustomQuestMarker(const GW::Vec2f world_pos) {
        custom_quest_marker_world_pos = world_pos;
        if (!GW::GameThread::IsInGameThread()) {
            GW::GameThread::Enqueue([pos_cpy = world_pos]() {
                SetCustomQuestMarker(pos_cpy);
                });
            return;
        }
        if (GW::QuestMgr::GetQuest(custom_quest_id)) {
            struct QuestRemovePacket : GW::Packet::StoC::PacketBase {
                GW::Constants::QuestID quest_id = custom_quest_id;
            };
            QuestRemovePacket quest_remove_packet;
            quest_remove_packet.header = GAME_SMSG_QUEST_REMOVE;
            GW::StoC::EmulatePacket(&quest_remove_packet);
            memset(&custom_quest_marker, 0, sizeof(custom_quest_marker));
        }
        if (custom_quest_marker_world_pos.x == 0 && custom_quest_marker_world_pos.y == 0)
            return;

        const auto map_id = GetMapIdForLocation(custom_quest_marker_world_pos);

        GW::Packet::StoC::QuestAdd quest_add_packet;
        quest_add_packet.quest_id = custom_quest_id;
        wcscpy(quest_add_packet.location, GW::EncStrings::MapRegion::Kryta);
        wcscpy(quest_add_packet.name, GW::EncStrings::MapRegion::Kryta);
        wcscpy(quest_add_packet.npc, GW::EncStrings::MapRegion::Kryta);
        quest_add_packet.map_from = map_id;
        quest_add_packet.map_to = map_id;
        GW::StoC::EmulatePacket(&quest_add_packet);

        struct QuestDescriptionPacket : GW::Packet::StoC::PacketBase {
            GW::Constants::QuestID quest_id = custom_quest_id;
            wchar_t reward[128] = { 0 };
            wchar_t objective[128] = { 0 };
        };
        QuestDescriptionPacket quest_description_packet;
        quest_description_packet.header = GAME_SMSG_QUEST_DESCRIPTION;
        wcscpy(quest_description_packet.reward, GW::EncStrings::MapRegion::Kryta);
        wcscpy(quest_description_packet.objective, GW::EncStrings::MapRegion::Kryta);
        GW::StoC::EmulatePacket(&quest_description_packet);

        struct QuestMarkerPacket : GW::Packet::StoC::PacketBase {
            GW::Constants::QuestID quest_id = custom_quest_id;
            GW::GamePos marker;
            GW::Constants::MapID map_id;
        };
        QuestMarkerPacket quest_marker_packet;
        quest_marker_packet.header = GAME_SMSG_QUEST_UPDATE_MARKER;
        quest_marker_packet.marker = { INFINITY, INFINITY };
        quest_marker_packet.map_id = map_id;
        if(map_id == GW::Map::GetMapID())
            WorldMapToGamePos(custom_quest_marker_world_pos, &quest_marker_packet.marker);
        GW::StoC::EmulatePacket(&quest_marker_packet);

        const auto quest = GW::QuestMgr::GetQuest(custom_quest_id);
        ASSERT(quest);
        custom_quest_marker = *quest;
        Log::Flash("Quest %d", custom_quest_id);
    }

    bool WorldMapContextMenu(void*) {
        if (!GW::Map::GetWorldMapContext())
            return false;
        const auto c = ImGui::GetCurrentContext();
        auto viewport_offset = c->CurrentViewport->Pos;
        viewport_offset.x *= -1;
        viewport_offset.y *= -1;



        ImGui::Text("%.2f, %.2f", world_map_click_pos.x, world_map_click_pos.y);
        GW::GamePos game_pos;
        if (WorldMapToGamePos(world_map_click_pos, &game_pos)) {
            ImGui::Text("%.2f, %.2f", game_pos.x, game_pos.y);
        }
        if (ImGui::Button("Place Marker")) {
            SetCustomQuestMarker(world_map_click_pos);
            return false;
        }
        place_marker_rect = c->LastItemData.Rect;
        place_marker_rect.Translate(viewport_offset);
        memset(&remove_marker_rect, 0, sizeof(remove_marker_rect));
        if (GW::QuestMgr::GetQuest(custom_quest_id)) {
            if(ImGui::Button("Remove Marker")) {
                SetCustomQuestMarker({ 0, 0 });
                return false;
            }
            remove_marker_rect = c->LastItemData.Rect;
            remove_marker_rect.Translate(viewport_offset);
        }
        return true;
    }

    GW::HookEntry OnUIMessage_HookEntry;

    void OnUIMessage(GW::HookStatus* status, GW::UI::UIMessage message_id, void* wparam, void*) {
        if (status->blocked)
            return;
        
        switch (message_id) {
        case GW::UI::UIMessage::kQuestAdded:
        case GW::UI::UIMessage::kQuestDetailsChanged: {
            const auto quest_id = *(GW::Constants::QuestID*)wparam;
            if (quest_id == custom_quest_id) {
                const auto quest = GW::QuestMgr::GetQuest(quest_id);
                quest->log_state |= 1; // Avoid asking for description about this quest
                quest->log_state |= 2; // Avoid asking for marker about this quest
            }
        } break;
        case GW::UI::UIMessage::kSendSetActiveQuest: {
            const auto quest_id = static_cast<GW::Constants::QuestID>((uint32_t)wparam);
            if (quest_id == custom_quest_id) {
                status->blocked = true;
                EmulateSelectedQuest(GW::QuestMgr::GetQuest(quest_id));
            }
        } break;
        case GW::UI::UIMessage::kMapLoaded:
            if (custom_quest_marker.quest_id != (GW::Constants::QuestID)0) {
                SetCustomQuestMarker(custom_quest_marker_world_pos);
            }
        }

    }

    void TriggerWorldMapRedraw() {
        GW::GameThread::Enqueue([] {
            // Trigger a benign ui message e.g. guild context update; world map subscribes to this, and automatically updates the view.
            // GW::UI::SendUIMessage((GW::UI::UIMessage)0x100000ca); // disables guild/ally chat until reloading char/map
            GW::UI::SendUIMessage(GW::UI::UIMessage::kMapLoaded);
            });
    }

    bool WorldMapToGamePos(GW::Vec2f& world_map_pos, GW::GamePos* game_map_pos) {
        const auto area_info = GW::Map::GetMapInfo();
        if (!area_info)
            return false;
        const auto world_map_rect = GetMapWorldMapBounds(area_info);
        if (!world_map_rect.Contains({ world_map_pos.x, world_map_pos.y }))
            return false; // Current map doesn't contain these coords; we can't plot a position

        const auto current_map_context = GW::GetMapContext();
        if (!current_map_context)
            return false;

        ImRect game_map_rect = ImRect({
            current_map_context->map_boundaries[1],current_map_context->map_boundaries[2],
            current_map_context->map_boundaries[3],current_map_context->map_boundaries[4],
            });

        GW::Vec2f map_mid_world_point = {
            world_map_rect.Min.x + (abs(game_map_rect.Min.x) / 96.f),
            world_map_rect.Min.y + (abs(game_map_rect.Max.y) / 96.f),
        };

        // NB: World map is 96 gwinches per unit, this is hard coded in the GW source 

        game_map_pos->x = (world_map_pos.x - map_mid_world_point.x) * 96.f;
        game_map_pos->y = ((world_map_pos.y - map_mid_world_point.y) * 96.f) * -1.f; // Inverted Y Axis
        return true;
    }
}

void WorldMapWidget::Initialize()
{
    ToolboxWidget::Initialize();

    memset(&custom_quest_marker, 0, sizeof(custom_quest_marker));

    uintptr_t address = GW::Scanner::Find("\x8b\x45\xfc\xf7\x40\x10\x00\x00\x01\x00", "xxxxxxxxxx", 0xa);
    if (address) {
        view_all_outposts_patch.SetPatch(address, "\xeb", 1);
    }
    address = GW::Scanner::Find("\x8b\xd8\x83\xc4\x10\x8b\xcb\x8b\xf3\xd1\xe9","xxxxxxxxxxx",-0x5);
    if (address) {
        view_all_carto_areas_patch.SetRedirect(address, GetCartographyFlagsForArea);
    }

    ASSERT(view_all_outposts_patch.IsValid());
    ASSERT(view_all_carto_areas_patch.IsValid());

    const GW::UI::UIMessage ui_messages[] = {
        GW::UI::UIMessage::kQuestAdded,
        GW::UI::UIMessage::kQuestDetailsChanged,
        GW::UI::UIMessage::kSendSetActiveQuest,
        GW::UI::UIMessage::kMapLoaded
    };
    for (auto ui_message : ui_messages) {
        GW::UI::RegisterUIMessageCallback(&OnUIMessage_HookEntry, ui_message, OnUIMessage);
    }

}
void WorldMapWidget::SignalTerminate() {
    ToolboxWidget::Terminate();
    SetCustomQuestMarker({ 0,0 });
    view_all_outposts_patch.Reset();
    view_all_carto_areas_patch.Reset();
    GW::UI::RemoveUIMessageCallback(&OnUIMessage_HookEntry);
}

void WorldMapWidget::ShowAllOutposts(const bool show = showing_all_outposts)
{
    if (view_all_outposts_patch.IsValid())
        view_all_outposts_patch.TogglePatch(show);
    if (view_all_carto_areas_patch.IsValid())
        view_all_carto_areas_patch.TogglePatch(show);
    TriggerWorldMapRedraw();
}

void WorldMapWidget::LoadSettings(ToolboxIni* ini)
{
    ToolboxWidget::LoadSettings(ini);
    LOAD_BOOL(showing_all_outposts);
}

void WorldMapWidget::SaveSettings(ToolboxIni* ini)
{
    ToolboxWidget::SaveSettings(ini);
    SAVE_BOOL(showing_all_outposts);
}

void WorldMapWidget::Draw(IDirect3DDevice9*)
{

    if (!GW::UI::GetIsWorldMapShowing()) {
        //ShowAllOutposts(showing_all_outposts = false);
        drawn = false;
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::SetNextWindowPos(ImVec2(16.f, 16.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), nullptr, GetWinFlags() | ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto c = ImGui::GetCurrentContext();
        auto viewport_offset = c->CurrentViewport->Pos;
        viewport_offset.x *= -1;
        viewport_offset.y *= -1;

        ImGui::Checkbox("Show all areas", &showing_all_outposts);
        show_all_rect = c->LastItemData.Rect;
        show_all_rect.Translate(viewport_offset);
        bool is_hard_mode = GW::PartyMgr::GetIsPartyInHardMode();
        ImGui::Checkbox("Hard mode", &is_hard_mode);
        hard_mode_rect = c->LastItemData.Rect;
        hard_mode_rect.Translate(viewport_offset);
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Draw custom quest marker on world map
    if (custom_quest_marker_world_pos.x || custom_quest_marker_world_pos.y) {
        const auto world_map_context = GW::Map::GetWorldMapContext();
        if (!(world_map_context && world_map_context->zoom == 1.0f))
            return;

        const auto viewport = ImGui::GetMainViewport();
        const auto& viewport_offset = viewport->Pos;
        static const ImVec2 UV0 = ImVec2(0.0F, 0.0f);
        static const ImVec2 ICON_SIZE = ImVec2(24.0f, 24.0f);

        ImVec2 viewport_quest_pos = {
            custom_quest_marker_world_pos.x - world_map_context->top_left.x + viewport_offset.x - (ICON_SIZE.x / 2.f),
            custom_quest_marker_world_pos.y - world_map_context->top_left.y + viewport_offset.y - (ICON_SIZE.y / 2.f)
        };

        ImRect quest_marker_image_rect = {
            viewport_quest_pos, { viewport_quest_pos.x + ICON_SIZE.x, viewport_quest_pos.y + ICON_SIZE.y }
        };

        const auto texture = GwDatTextureModule::LoadTextureFromFileId(0x1b4d5);

        auto uv1 = ImGui::CalculateUvCrop(*texture, ICON_SIZE);
        ImGui::GetBackgroundDrawList(viewport)->AddImage(*texture, quest_marker_image_rect.Min, quest_marker_image_rect.Max, UV0, uv1);
        if (quest_marker_image_rect.Contains(ImGui::GetMousePos())) {
            ImGui::SetTooltip("Custom marker placed @ %.2f, %.2f", custom_quest_marker_world_pos.x, custom_quest_marker_world_pos.y);
        }
    }
    drawn = true;
}

bool WorldMapWidget::WndProc(const UINT Message, WPARAM, LPARAM lParam)
{

    switch (Message) {
    case WM_RBUTTONDOWN: {
        world_map_clicking = true;
    } break;
    case WM_MOUSEMOVE: {
        world_map_clicking = false;
    } break;
    case WM_RBUTTONUP: {
        if (!world_map_clicking)
            break;
        world_map_clicking = false;
        const auto world_map_context = GW::Map::GetWorldMapContext();
        if (!(world_map_context && world_map_context->zoom == 1.0f))
            break;

        world_map_click_pos = { GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam) };
        world_map_click_pos.x += world_map_context->top_left.x;
        world_map_click_pos.y += world_map_context->top_left.y;
        ImGui::SetContextMenu(WorldMapContextMenu);
    } break;
        case WM_LBUTTONDOWN:
            if (!drawn || !GW::UI::GetIsWorldMapShowing()) {
                return false;
            }
            auto check_rect = [lParam](const ImRect& rect) {
                ImVec2 p = { (float)GET_X_LPARAM(lParam) ,(float)GET_Y_LPARAM(lParam) };
                return rect.Contains(p);
            };
            if (check_rect(remove_marker_rect)) {
                return true;
            }
            if (check_rect(place_marker_rect)) {
                return true;
            }
            if (check_rect(hard_mode_rect)) {
                GW::PartyMgr::SetHardMode(!GW::PartyMgr::GetIsPartyInHardMode());
                return true;
            }
            if (check_rect(show_all_rect)) {
                showing_all_outposts = !showing_all_outposts;
                ShowAllOutposts(showing_all_outposts);
                return true;
            }
            if (check_rect(hard_mode_rect)) {
                GW::PartyMgr::SetHardMode(!GW::PartyMgr::GetIsPartyInHardMode());
                return true;
            }
            break;
    }
    return false;
}

bool WorldMapWidget::CanTerminate()
{
    return !GW::QuestMgr::GetQuest(custom_quest_id);
}

void WorldMapWidget::DrawSettingsInternal()
{
    ImGui::Text("Note: only visible in Hard Mode explorable areas.");
}
