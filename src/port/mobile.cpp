#include "port/mobile.h"

#ifdef LM_MOBILE
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace port::mobile {
namespace fs = std::filesystem;
namespace {

fs::path homeDir() {
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path(".");
}

#ifdef __ANDROID__
void unpackAsset(const char* name) {
    SDL_IOStream* in = SDL_IOFromFile(name, "rb");
    if (!in) return;
    const Sint64 size = SDL_GetIOSize(in);
    const fs::path dst = bundleDir() / name;
    std::error_code ec;
    if (size >= 0 && fs::exists(dst, ec) && Sint64(fs::file_size(dst, ec)) == size) {
        SDL_CloseIO(in);
        return;
    }
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = dst.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        std::vector<char> buf(1 << 20);
        for (;;) {
            const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
            if (n == 0) break;
            out.write(buf.data(), std::streamsize(n));
        }
    }
    SDL_CloseIO(in);
    fs::rename(tmp, dst, ec);
    std::printf("[mobile] unpacked %s (%lld bytes)\n", name, static_cast<long long>(size));
}
#endif

fs::path setupFlag() { return storageRoot() / ".lm-open-setup"; }

}

void init() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    std::error_code ec;
    fs::create_directories(storageRoot(), ec);
    fs::create_directories(importDir(), ec);
    const std::string logPath = (importDir() / "last_run.log").string();
    if (std::freopen(logPath.c_str(), "w", stdout)) {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::fflush(stderr);
        dup2(fileno(stdout), fileno(stderr));
    }
#ifdef __ANDROID__
    unpackAsset("pipelines.bin");
#endif
    std::printf("[mobile] %s: data %s, imports %s\n", platformName(), storageRoot().string().c_str(),
                importDir().string().c_str());
    std::fflush(stdout);
}

fs::path storageRoot() {
#ifdef __ANDROID__
    static const fs::path p = [] {
        const char* d = SDL_GetAndroidInternalStoragePath();
        return d ? fs::path(d) : fs::path(".");
    }();
    return p;
#else
    return homeDir() / "Library" / "Application Support";
#endif
}

fs::path importDir() {
#ifdef __ANDROID__
    static const fs::path p = [] {
        const char* d = SDL_GetAndroidExternalStoragePath();
        return d ? fs::path(d) : storageRoot() / "import";
    }();
#else
    static const fs::path p = homeDir() / "Documents";
#endif
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path bundleDir() {
#ifdef __ANDROID__
    return storageRoot() / "bundle";
#else
    static const fs::path p = [] {
        const char* b = SDL_GetBasePath();
        return b ? fs::path(b) : fs::path(".");
    }();
    return p;
#endif
}

const char* platformName() {
#ifdef __ANDROID__
    return "Android";
#else
    return "iPhone / iPad";
#endif
}

std::vector<fs::path> imports() {
    std::vector<fs::path> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(importDir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = it->path().filename().string();
        const std::string ext = it->path().extension().string();
        if (name.empty() || name[0] == '.' || ext == ".tmp" || ext == ".log") continue;
        v.push_back(it->path());
    }
    std::sort(v.begin(), v.end());
    return v;
}

bool importFile(const std::string& source, const std::string& name, const std::function<bool(float)>& progress,
                fs::path& out, std::string& error) {
    SDL_IOStream* in = SDL_IOFromFile(source.c_str(), "rb");
    if (!in) {
        error = std::string("Could not open the chosen file: ") + SDL_GetError();
        return false;
    }
    const Sint64 size = SDL_GetIOSize(in);
    out = importDir() / name;
    const fs::path tmp = out.string() + ".tmp";
    bool ok = true;
    {
        std::ofstream o(tmp, std::ios::binary);
        if (!o) {
            error = "Could not write " + tmp.string();
            SDL_CloseIO(in);
            return false;
        }
        std::vector<char> buf(4 << 20);
        Sint64 done = 0;
        for (;;) {
            const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
            if (n == 0) break;
            o.write(buf.data(), std::streamsize(n));
            if (!o) {
                error = "Not enough space to copy the file into the app.";
                ok = false;
                break;
            }
            done += Sint64(n);
            if (progress && !progress(size > 0 ? float(double(done) / double(size)) : 0.0f)) {
                error = "Cancelled.";
                ok = false;
                break;
            }
        }
    }
    SDL_CloseIO(in);
    std::error_code ec;
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) {
        error = "Could not finish the copy: " + ec.message();
        return false;
    }
    return true;
}

void requestSetup() { std::ofstream(setupFlag()) << "1"; }

bool takeSetupRequest() {
    std::error_code ec;
    if (!fs::exists(setupFlag(), ec)) return false;
    fs::remove(setupFlag(), ec);
    return true;
}

}
#endif
