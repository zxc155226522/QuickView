#include "pch.h"
#include "ThemeSystem.h"
#include "yyjson.h"
#include <commdlg.h>
#include <shlwapi.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

extern void SaveConfig(); // Defined in main.cpp
namespace QuickView::UI::ThemeSystem {

    static std::wstring ShowFileDialog(HWND hwnd, bool isSave) {
        wchar_t szFile[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = {}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"QuickView Theme (*.qvtheme)\0*.qvtheme\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrDefExt = L"qvtheme";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (isSave) {
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&ofn)) return szFile;
        } else {
            if (GetOpenFileNameW(&ofn)) return szFile;
        }
        return L"";
    }

    bool ExportTheme(HWND hwnd, const AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, true);
        if (path.empty()) return false;

        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_real(doc, root, "version", 1.0);
        yyjson_mut_obj_add_int(doc, root, "theme_mode", config.ThemeMode);
        yyjson_mut_obj_add_int(doc, root, "menu_backdrop_style", config.MenuBackdropStyle);
        yyjson_mut_obj_add_bool(doc, root, "glass_enabled", config.EnableGeekGlass);
        yyjson_mut_obj_add_bool(doc, root, "animations", config.GlassUIAnimations);
        yyjson_mut_obj_add_real(doc, root, "blur", config.GlassBlurSigma);
        yyjson_mut_obj_add_real(doc, root, "tint_alpha", config.GlassTintAlpha);
        yyjson_mut_obj_add_real(doc, root, "specular", config.GlassSpecularOpacity);
        yyjson_mut_obj_add_real(doc, root, "shadow", config.GlassShadowOpacity);
        
        yyjson_mut_obj_add_real(doc, root, "density_osd", config.GlassOsdOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_panels", config.GlassPanelsOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_modals", config.GlassModalsOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_menus", config.GlassMenusOpacity);
        
        yyjson_mut_obj_add_int(doc, root, "stroke_weight", config.GlassVectorStrokeWeightIndex);
        yyjson_mut_obj_add_int(doc, root, "tint_profile", config.GlassTintProfile);
        
        auto add_color = [&](const char* key, float r, float g, float b) {
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_real(doc, arr, r);
            yyjson_mut_arr_add_real(doc, arr, g);
            yyjson_mut_arr_add_real(doc, arr, b);
            yyjson_mut_obj_add_val(doc, root, key, arr);
        };

        add_color("tint_color", config.GlassCustomTintR, config.GlassCustomTintG, config.GlassCustomTintB);
        add_color("accent_color", config.ThemeCustomAccentR, config.ThemeCustomAccentG, config.ThemeCustomAccentB);
        add_color("text_color", config.ThemeCustomTextR, config.ThemeCustomTextG, config.ThemeCustomTextB);

        yyjson_mut_obj_add_int(doc, root, "canvas_color", config.CanvasColor);
        yyjson_mut_obj_add_int(doc, root, "canvas_effect_style", config.CanvasEffectStyle);
        {
            yyjson_mut_val *ca = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_int(doc, ca, config.CanvasCustomR);
            yyjson_mut_arr_add_int(doc, ca, config.CanvasCustomG);
            yyjson_mut_arr_add_int(doc, ca, config.CanvasCustomB);
            yyjson_mut_obj_add_val(doc, root, "canvas_custom", ca);
        }

        // [Swatch] Export swatch colors and index
        yyjson_mut_obj_add_int(doc, root, "swatch_color_index", config.SwatchColorIndex);
        for (int i = 3; i < 9; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "swatch_color_%d", i);
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_int(doc, arr, config.SwatchColors[i][0]);
            yyjson_mut_arr_add_int(doc, arr, config.SwatchColors[i][1]);
            yyjson_mut_arr_add_int(doc, arr, config.SwatchColors[i][2]);
            yyjson_mut_arr_add_int(doc, arr, config.SwatchColors[i][3]);
            yyjson_mut_obj_add_val(doc, root, key, arr);
        }

        size_t len;
        char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
        if (json) {
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                BOOL res = WriteFile(hFile, json, static_cast<DWORD>(len), &written, nullptr);
                CloseHandle(hFile);
                free(json);
                yyjson_mut_doc_free(doc);
                return res && (written == len);
            }
            free(json);
        }
        yyjson_mut_doc_free(doc);
        return false;
    }

    bool ImportTheme(HWND hwnd, AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, false);
        if (path.empty()) return false;

        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
            CloseHandle(hFile);
            return false;
        }

        std::string json_str(static_cast<size_t>(fileSize.QuadPart), '\0');
        DWORD bytesRead = 0;
        BOOL success = ReadFile(hFile, json_str.data(), static_cast<DWORD>(fileSize.QuadPart), &bytesRead, nullptr);
        CloseHandle(hFile);

        if (!success || bytesRead != fileSize.QuadPart) return false;

        yyjson_doc *doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
        if (!doc) return false;

        yyjson_val *obj = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(obj)) {
            yyjson_doc_free(doc);
            return false;
        }

        // Safe extraction helpers
        auto get_double = [&](const char* key, float& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_num(v)) out = (float)yyjson_get_num(v);
        };
        auto get_int = [&](const char* key, int& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_num(v)) out = (int)yyjson_get_num(v);
        };
        auto get_bool = [&](const char* key, bool& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_bool(v)) out = yyjson_get_bool(v);
        };
        auto get_color = [&](const char* key, float& r, float& g, float& b) {
            yyjson_val *arr = yyjson_obj_get(obj, key);
            if (arr && yyjson_is_arr(arr) && yyjson_arr_size(arr) >= 3) {
                r = (float)yyjson_get_num(yyjson_arr_get(arr, 0));
                g = (float)yyjson_get_num(yyjson_arr_get(arr, 1));
                b = (float)yyjson_get_num(yyjson_arr_get(arr, 2));
            }
        };

        get_int("theme_mode", config.ThemeMode);
        get_int("menu_backdrop_style", config.MenuBackdropStyle);
        get_bool("glass_enabled", config.EnableGeekGlass);
        get_bool("animations", config.GlassUIAnimations);
        get_double("blur", config.GlassBlurSigma);
        get_double("tint_alpha", config.GlassTintAlpha);
        get_double("specular", config.GlassSpecularOpacity);
        get_double("shadow", config.GlassShadowOpacity);
        
        get_double("density_osd", config.GlassOsdOpacity);
        get_double("density_panels", config.GlassPanelsOpacity);
        get_double("density_modals", config.GlassModalsOpacity);
        get_double("density_menus", config.GlassMenusOpacity);
        
        get_int("stroke_weight", config.GlassVectorStrokeWeightIndex);
        get_int("tint_profile", config.GlassTintProfile);
        
        get_color("tint_color", config.GlassCustomTintR, config.GlassCustomTintG, config.GlassCustomTintB);
        get_color("accent_color", config.ThemeCustomAccentR, config.ThemeCustomAccentG, config.ThemeCustomAccentB);
        get_color("text_color", config.ThemeCustomTextR, config.ThemeCustomTextG, config.ThemeCustomTextB);

        get_int("canvas_color", config.CanvasColor);
        config.CanvasColor = 5; // Force swatch mode
        get_int("canvas_effect_style", config.CanvasEffectStyle);
        {
            yyjson_val *ca = yyjson_obj_get(obj, "canvas_custom");
            if (ca && yyjson_is_arr(ca) && yyjson_arr_size(ca) >= 3) {
                config.CanvasCustomR = (int)yyjson_get_num(yyjson_arr_get(ca, 0));
                config.CanvasCustomG = (int)yyjson_get_num(yyjson_arr_get(ca, 1));
                config.CanvasCustomB = (int)yyjson_get_num(yyjson_arr_get(ca, 2));
            }
        }

        // [Swatch] Import swatch colors and index
        {
            yyjson_val *v;
            v = yyjson_obj_get(obj, "swatch_color_index");
            if (v && yyjson_is_int(v)) config.SwatchColorIndex = (int)yyjson_get_int(v);
        }
        for (int i = 3; i < 9; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "swatch_color_%d", i);
            yyjson_val *arr = yyjson_obj_get(obj, key);
            if (arr && yyjson_is_arr(arr) && yyjson_arr_size(arr) >= 4) {
                config.SwatchColors[i][0] = (int)yyjson_get_num(yyjson_arr_get(arr, 0));
                config.SwatchColors[i][1] = (int)yyjson_get_num(yyjson_arr_get(arr, 1));
                config.SwatchColors[i][2] = (int)yyjson_get_num(yyjson_arr_get(arr, 2));
                config.SwatchColors[i][3] = (int)yyjson_get_num(yyjson_arr_get(arr, 3));
            }
        }

        config.EnforceGlassSafetyLimits();
        ::SaveConfig(); // Persist to QuickView.ini as requested
        yyjson_doc_free(doc);
        return true;
    }
}
