#include "stdafx.h"

#include <d3dx9_dynamic.h>

#include <Defines.h>
#include <GWToolbox.h>
#include <Logger.h>

#include <GuiUtils.h>

#include <Modules/Resources.h>
#include <GWCA/GameEntities/Item.h>
#include <Timer.h>

namespace {
    const wchar_t* d3dErrorMessage(HRESULT code) {
        
        switch (code) {
        case D3DERR_NOTAVAILABLE:
            return  L"D3DERR_NOTAVAILABLE";
        case D3DERR_OUTOFVIDEOMEMORY:
            return  L"D3DERR_OUTOFVIDEOMEMORY";
        case D3DERR_INVALIDCALL:
            return  L"D3DERR_INVALIDCALL";
        case E_OUTOFMEMORY:
            return  L"E_OUTOFMEMORY";
        case D3D_OK:
            return L"D3D_OK";
        }
        static wchar_t out[32];
        swprintf(out, 32, L"Unknown D3D error %#08x", code);
        return out;
    }

    struct ProfessionIcon {
        ProfessionIcon(const wchar_t* path) {
            wiki_path_to_file = path;
        }
        IDirect3DTexture9* texture = 0;
        const wchar_t* wiki_path_to_file;
        bool loading = false;
    };
    ProfessionIcon profession_icons[] = {
        L"",
        L"8/87/Warrior-tango-icon-48",
        L"e/e8/Ranger-tango-icon-48",
        L"5/53/Monk-tango-icon-48",
        L"b/b1/Necromancer-tango-icon-48",
        L"b/b1/Mesmer-tango-icon-48",
        L"4/47/Elementalist-tango-icon-48",
        L"2/2b/Assassin-tango-icon-48",
        L"5/5b/Ritualist-tango-icon-48",
        L"5/5e/Paragon-tango-icon-48",
        L"3/38/Dervish-tango-icon-48"
    };
}

void Resources::Initialize() {
    ToolboxModule::Initialize();
    for (size_t i = 0; i < Resources::MAX_WORKERS; i++) {
        workers.push_back(new std::thread([this]() {
            while (!should_stop) {
                queue_mutex.lock();
                if (thread_jobs.empty()) {
                    queue_mutex.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                else {
                    std::function<void()> func = thread_jobs.front();
                    thread_jobs.pop();
                    queue_mutex.unlock();
                    func();
                }
            }
            }));
    }
}
void Resources::Cleanup() {
    should_stop = true;
    for (std::thread* worker : workers) {
        if (!worker)
            continue;
        assert(worker->joinable());
        worker->join();
        delete worker;
    }
    workers.clear();
}
void Resources::Terminate() {
    ToolboxModule::Terminate();
    Cleanup();
}
void Resources::EndLoading() {
    EnqueueWorkerTask([this]() { should_stop = true; });
}

std::filesystem::path Resources::GetSettingsFolderPath()
{
    WCHAR path[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path);
    return std::filesystem::path(path) / "GWToolboxpp";
}
std::filesystem::path Resources::GetPath(const std::filesystem::path& file)
{
    return GetSettingsFolderPath() / file;
}
std::filesystem::path Resources::GetPath(const std::filesystem::path& folder, const std::filesystem::path& file)
{
    return GetSettingsFolderPath() / folder / file;
}

bool Resources::EnsureFolderExists(const std::filesystem::path& path)
{
    return std::filesystem::exists(path) || std::filesystem::create_directory(path);
}

utf8::string Resources::GetPathUtf8(std::wstring file) {
    std::wstring path = GetPath(file);
    return Unicode16ToUtf8(path.c_str());
}

bool Resources::Download(const std::filesystem::path& path_to_file, const std::wstring& url)
{
    DeleteUrlCacheEntryW(url.c_str());
    Log::Log("Downloading %ls\n", url.c_str());
    HRESULT download_result = URLDownloadToFileW(NULL, url.c_str(), path_to_file.c_str(), 0, NULL);
    if (download_result != S_OK) {
        E_OUTOFMEMORY;
        INET_E_DOWNLOAD_FAILURE;
        Log::Log("Failed to download from %ls to %ls, error 0x%08x\n", url.c_str(), path_to_file.c_str(), download_result);
        return false;
    }
    return true;
}
void Resources::Download(
    const std::filesystem::path& path_to_file, const std::wstring& url, std::function<void(bool)> callback)
{
    EnqueueWorkerTask([this, url, path_to_file, callback]() {
        bool success = Download(path_to_file, url);
        // and call the callback in the main thread
        todo.push([callback, success]() { callback(success); });
        });
}

std::string Resources::Download(const std::wstring& url) const
{
    DeleteUrlCacheEntryW(url.c_str());
    IStream* stream;
    std::string ret = "";
    if (SUCCEEDED(URLOpenBlockingStreamW(NULL, url.c_str(), &stream, 0, NULL))) {
        STATSTG stats;
        stream->Stat(&stats, STATFLAG_NONAME);
        DWORD size = stats.cbSize.LowPart;
        CHAR* chars = new CHAR[size + 1];
        stream->Read(chars, size, NULL);
        chars[size] = '\0';
        ret = std::string(chars);
        delete[] chars;
        stream->Release();
    }
    return ret;
}
void Resources::Download(const std::wstring& url, std::function<void(std::string)> callback)
{
    EnqueueWorkerTask([this, url, callback]() {
        const std::string& s = Download(url);
        todo.push([callback, s]() { callback(s); });
    });
}

void Resources::EnsureFileExists(
    const std::filesystem::path& path_to_file, const std::wstring& url, std::function<void(bool)> callback)
{
    if (std::filesystem::exists(path_to_file)) {
        // if file exists, run the callback immediately in the same thread
        callback(true);
    } else {
        // otherwise try to download it in the worker
        Download(path_to_file, url, callback);
    }
}

IDirect3DTexture9* Resources::GetProfessionIcon(GW::Constants::Profession p) {
    auto& prof_icon = profession_icons[(uint32_t)p];
    if (!prof_icon.loading ) {
        prof_icon.loading = true;
        if (prof_icon.wiki_path_to_file[0]) {
            auto path = Resources::GetPath(L"img\\professions");
            Resources::EnsureFolderExists(path);
            wchar_t local_image[MAX_PATH];
            swprintf(local_image, _countof(local_image), L"%s\\%d.png", path.c_str(), p);
            wchar_t remote_image[255];
            swprintf(remote_image, _countof(remote_image), L"https://wiki.guildwars.com/images/%s.png", prof_icon.wiki_path_to_file);
            Instance().LoadTextureAsync(&prof_icon.texture, local_image, remote_image);
        }
    }
    return prof_icon.texture;
}

void Resources::LoadTextureAsync(IDirect3DTexture9** texture, const std::filesystem::path& path_to_file, std::function<void(IDirect3DTexture9**)> callback)
{
    if (std::filesystem::exists(path_to_file)) {
        // make sure we copy the path, not use the ref
        toload.push([path_to_file, texture](IDirect3DDevice9* device) {
            TryCreateTexture(device, path_to_file.c_str(), texture);
        });
    }
}

HRESULT Resources::TryCreateTexture(IDirect3DDevice9* device, const std::filesystem::path& path_to_file, IDirect3DTexture9** texture, bool display_error) {
    // NB: Some Graphics cards seem to spit out D3DERR_NOTAVAILABLE when loading textures, haven't figured out why but retry if this error is reported
    HRESULT res = D3DERR_NOTAVAILABLE;
    size_t tries = 0;
    do {
        tries++;
        res = D3DXCreateTextureFromFileExW(device, path_to_file.c_str(), D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, texture);
    } while (res == D3DERR_NOTAVAILABLE && tries < 3);
    if (display_error && res != D3D_OK) {
        Log::Error("Error loading resource from file %ls - Error is %ls", path_to_file.filename().c_str(), d3dErrorMessage(res));
    }
    if (display_error && !texture) {
        Log::Error("Error loading resource from file %ls - texture loaded is null", path_to_file.filename().c_str());
    }
    return res;
}
HRESULT Resources::TryCreateTexture(IDirect3DDevice9* device, HMODULE hSrcModule, LPCSTR id, IDirect3DTexture9** texture, bool display_error) {
    // NB: Some Graphics cards seem to spit out D3DERR_NOTAVAILABLE when loading textures, haven't figured out why but retry if this error is reported
    HRESULT res = D3DERR_NOTAVAILABLE;
    size_t tries = 0;
    do {
        tries++;
        res = D3DXCreateTextureFromResourceExA(device, hSrcModule, id, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, texture);
    } while (res == D3DERR_NOTAVAILABLE && tries < 3);
    if (display_error && res != D3D_OK) {
        Log::Error("Error loading resource for id %d, module %p - Error is %ls", id, hSrcModule, d3dErrorMessage(res));
    }
    if (display_error && !texture) {
        Log::Error("Error loading resource for id %ds - texture loaded is null", id);
    }
    return res;
}

// @Cleanup: What should we do with error?


void Resources::LoadTextureAsync(
    IDirect3DTexture9** texture, const std::filesystem::path& path_to_file, const std::wstring& url, std::function<void(IDirect3DTexture9**)> callback)
{
    EnsureFileExists(path_to_file, url, [this, path_to_file, texture, callback](bool success) {
        if (!success) {
            Log::ErrorW(L"Error downloading resource from url %s", path_to_file.c_str());
            if (callback) {
                callback(texture);
            }
            return;
        }
        if (std::filesystem::exists(path_to_file)) {
            LoadTextureAsync(texture, path_to_file, callback);
        }
        else {
            callback(texture);
        }
    });
}

void Resources::LoadTextureAsync(IDirect3DTexture9** texture, 
    const std::filesystem::path& path_to_file, WORD id, std::function<void(IDirect3DTexture9**)> callback)
{

    // First, try to create the file in the gwtoolbox dir if it doesn't exist
    if (!std::filesystem::exists(path_to_file) && id > 0) {
        // otherwise try to install it from resource
        HRSRC hResInfo = FindResourceA(GWToolbox::GetDLLModule(), MAKEINTRESOURCE(id), RT_RCDATA);
        if (!hResInfo) {
            DWORD wfErr = GetLastError();
            Log::Error("Error calling FindResourceA on resource id %u - Error is %lu", id, wfErr);
            if (callback) {
                callback(texture);
            }
            return;
        }
        HGLOBAL hRes = LoadResource(GWToolbox::GetDLLModule(), hResInfo);
        if (!hRes) {
            DWORD wfErr = GetLastError();
            Log::Error("Error calling LoadResource on resource id %u - Error is %lu", id, wfErr);
            if (callback) {
                callback(texture);
            }
            return;
        }
        DWORD size = SizeofResource(GWToolbox::GetDLLModule(), hResInfo);
        if (!size) {
            DWORD wfErr = GetLastError();
            Log::Error("Error calling SizeofResource on resource id %u - Error is %lu", id, wfErr);
            if (callback) {
                callback(texture);
            }
            return;
        }
        // write to file so the user can customize his icons
        HANDLE hFile = CreateFileW(path_to_file.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD bytesWritten;
        BOOL wfRes = WriteFile(hFile, hRes, size, &bytesWritten, NULL);
        if (wfRes != TRUE) {
            DWORD wfErr = GetLastError();
            Log::Error("Error writing file %ls - Error is %lu", path_to_file.filename().c_str(), wfErr);
        }
        else if (bytesWritten != size) {
            Log::Error("Wrote %lu of %lu bytes for %ls", bytesWritten, size, path_to_file.filename().c_str());
        }

        CloseHandle(hFile);
        // Note: this WILL fail for some users. Don't care, it's only needed for customization.
    }

    if (std::filesystem::exists(path_to_file)) {
        // if file exists load it
        toload.push([callback,path_to_file, texture, id](IDirect3DDevice9* device) {
            HRESULT res = TryCreateTexture(device, path_to_file, texture, id == 0);
            if (!(res == D3D_OK && texture) && id != 0) {
                Log::Log("Failed to load %ls from file; error code %ls", path_to_file.filename().c_str(), d3dErrorMessage(res));
                TryCreateTexture(device, GWToolbox::GetDLLModule(), MAKEINTRESOURCE(id), texture);
                if (callback) {
                    callback(texture);
                }
                return;
            }
            if (callback) {
                callback(texture);
            }
        });
    } else if(id != 0) {
        // finally load the texture from the resource
        toload.push([callback, id, texture](IDirect3DDevice9* device) {
            TryCreateTexture(device, GWToolbox::GetDLLModule(), MAKEINTRESOURCE(id), texture);
            if (callback) {
                callback(texture);
            }
        });
    }
    else {
        Log::Error("Failed to load resource from file %ls; path doesn't exist and no resource available", path_to_file.filename().c_str());
        if (callback) {
            callback(texture);
        }
    }
}

void Resources::DxUpdate(IDirect3DDevice9* device) {
    while (!toload.empty()) {
        D3DCAPS9 caps;
        if (device->GetDeviceCaps(&caps) != D3D_OK)
            break; // Not ready yet
        toload.front()(device);
        toload.pop();
    }
}

void Resources::Update(float) {
    while (!todo.empty()) {
        todo.front()();
        todo.pop();
    }
}

// Fetches skill page from GWW, parses out the image for the skill then downloads that to disk
// Not elegent, but without a proper API to provide images, and to avoid including libxml, this is the next best thing.
void Resources::LoadSkillImage(uint32_t skill_id, IDirect3DTexture9** texture, std::function<void(IDirect3DTexture9**)> callback) {
    auto path = Resources::GetPath(L"img\\skills");
    assert(Resources::EnsureFolderExists(path));

    wchar_t path_to_file[MAX_PATH];
    swprintf(path_to_file, _countof(path_to_file), L"%s\\%d.jpg", Resources::GetPath(L"img\\skills").c_str(), skill_id);
    if (std::filesystem::exists(path_to_file)) {
        LoadTextureAsync(texture, path_to_file, callback);
    }
    else {
        wchar_t url[128];
        swprintf(url, _countof(url), L"https://wiki.guildwars.com/wiki/Game_link:Skill_%d", skill_id);
        Download(url, [callback, skill_id, texture](std::string response) {
            char regex_str[128];
            snprintf(regex_str, sizeof(regex_str), "<img[^>]+alt=['\"].*%d.*['\"].+src=['\"]([^\"']+)", skill_id);
            const std::regex image_finder(regex_str);
            std::smatch m;
            if (std::regex_search(response, m, image_finder)) {
                wchar_t path_to_file[MAX_PATH];
                swprintf(path_to_file, _countof(path_to_file), L"%s\\%d.jpg", Resources::GetPath(L"img\\skills").c_str(), skill_id);
                wchar_t url[128];
                swprintf(url, _countof(url), L"https://wiki.guildwars.com%S", m[1].str().c_str());
                Instance().LoadTextureAsync(texture, path_to_file, url, callback);
            }
            });
    }
}

void Resources::LoadItemImage(const std::wstring& item_name, IDirect3DTexture9** texture, std::function<void(IDirect3DTexture9**)> callback) {
    auto path = GetPath(ITEM_IMAGES_PATH);
    assert(EnsureFolderExists(path));

    wchar_t path_to_file[MAX_PATH];
    swprintf(path_to_file, _countof(path_to_file), L"%s\\%s.png", GetPath(ITEM_IMAGES_PATH).c_str(), item_name.c_str());
    if (std::filesystem::exists(path_to_file)) {
        LoadTextureAsync(texture, path_to_file, callback);
    }
    else {
        wchar_t url[128];
        swprintf(url, _countof(url), L"https://wiki.guildwars.com/index.php?search=%s", item_name.c_str());
        Download(url, [&,callback, item_name, texture](std::string response) {
            char regex_str[128];
            snprintf(regex_str, sizeof(regex_str), "<img[^>]+alt=['\"].*%s.*['\"].+src=['\"]([^\"']+)", GuiUtils::WStringToString(item_name).c_str());
            const std::regex image_finder(regex_str);
            std::smatch m;
            if (std::regex_search(response, m, image_finder)) {
                wchar_t path_to_file[MAX_PATH];
                swprintf(path_to_file, _countof(path_to_file), L"%s\\%s.png", GetPath(ITEM_IMAGES_PATH).c_str(), item_name.c_str());
                wchar_t url[128];
                swprintf(url, _countof(url), L"https://wiki.guildwars.com%S", m[1].str().c_str());
                Instance().LoadTextureAsync(texture, path_to_file, url, callback);
            }
            });
    }
}