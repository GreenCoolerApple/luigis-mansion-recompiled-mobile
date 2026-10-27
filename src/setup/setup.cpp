// First-run setup screen and pipeline (docs/SETUP.md).
#include "setup/setup.h"

#include "port/crash_handler.h"
#include "port/dol.h"
#include "recomp/precache.h"
#include "recomp/ui_scale.h"
#include "setup/disc.h"
#include "setup/hm_overlay.h"
#include "setup/module_builder.h"
#ifdef LM_MOBILE
#include "port/mobile.h"
#endif

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace setup {
namespace {

using port::GameVersion;

constexpr const char* kOverlayFormat = "hidden-mansion-overlay 3";  // 2: + usa/ (reverse), 3: lab menu choice (event08)

struct Slot {
    std::string path;     // UTF-8
    bool valid = false;
    GameVersion version = GameVersion::USA;
    std::string message;  // validation result
};

// State shared with the worker thread.
struct Job {
    std::mutex m;
    std::string stage;
    float fraction = 0.0f;
    bool done = false, ok = false;
    std::string error;
    std::atomic<bool> cancel{false};
    std::thread thread;
    ~Job() {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// Results of the file dialogs (SDL may call back on another thread).
std::mutex sPickM;
std::optional<std::string> sPicked[2];
std::string sDialogError;  // the file dialog could not open (Linux: no xdg-desktop-portal/zenity)

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(port::longPath(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string overlayKey(const Disc& us, const Disc& pal) {
    const std::string text = std::string(kOverlayFormat) + "\n" + us.dolSha1() + "\n" + pal.dolSha1();
    const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hex(d.data(), d.size());
}

bool overlayUpToDate(const Disc& us, const Disc& pal) {
    return readText(port::hiddenMansionOverlayDir() / "overlay.key") == overlayKey(us, pal);
}

void validate(Slot& s, GameVersion expected) {
    s.valid = false;
    s.message.clear();
    if (s.path.empty()) return;
    GameVersion v{};
    std::string err;
    if (!probeDisc(s.path, v, err)) {
        s.message = err;
        return;
    }
    if (v != expected) {
        s.message = std::string("This is the ") + (v == GameVersion::PAL ? "European" : "USA") +
                    " disc; put it in the other slot.";
        return;
    }
    s.valid = true;
    s.message = v == GameVersion::PAL ? "Luigi's Mansion (Europe, GLMP01)" : "Luigi's Mansion (USA, GLME01)";
}

struct Plan {
    std::vector<GameVersion> modules;  // modules to (re)build
    bool overlay = false;               // PAL Hidden Mansion overlay to (re)build
    bool empty() const { return modules.empty() && !overlay; }
};

Plan makePlan(const Slot slots[2]) {
    Plan p;
    std::unique_ptr<Disc> discs[2];
    for (int i = 0; i < 2; i++) {
        if (!slots[i].valid) continue;
        discs[i] = std::make_unique<Disc>();
        std::string err;
        if (!discs[i]->open(slots[i].path, err)) {
            discs[i].reset();
            continue;
        }
        if (!moduleUpToDate(*discs[i])) p.modules.push_back(discs[i]->version());
    }
    if (discs[0] && discs[1] && !overlayUpToDate(*discs[0], *discs[1])) p.overlay = true;
    return p;
}

void startJob(Job& job, const Slot slots[2], const Plan& plan) {
    const std::string paths[2] = {slots[0].valid ? slots[0].path : "", slots[1].valid ? slots[1].path : ""};
    job.thread = std::thread([&job, paths, plan] {
        auto set = [&](const std::string& stage, float f) {
            std::lock_guard lk(job.m);
            job.stage = stage;
            job.fraction = f;
        };
        auto fail = [&](const std::string& e) {
            std::lock_guard lk(job.m);
            job.error = e;
            job.done = true;
        };
        const float steps = float(plan.modules.size() + (plan.overlay ? 1 : 0));
        float step = 0.0f;
        for (GameVersion v : plan.modules) {
            const int i = v == GameVersion::PAL ? 1 : 0;
            const char* name = v == GameVersion::PAL ? "Europe" : "USA";
            set(std::string("Reading the ") + name + " disc", step / steps);
            Disc disc;
            std::string err;
            if (!disc.open(paths[i], err)) return fail(err);
            Progress pr;
            pr.cancel = &job.cancel;
            pr.report = [&](const std::string& s, float f) { set(std::string(name) + ": " + s, (step + f) / steps); };
            if (!buildModule(disc, pr, err)) return fail(err);
            step += 1.0f;
        }
        if (plan.overlay) {
            set("Building the European Hidden Mansion data", step / steps);
            Disc us, pal;
            std::string err;
            if (!us.open(paths[0], err) || !pal.open(paths[1], err)) return fail(err);
            const auto dir = port::hiddenMansionOverlayDir();
            if (!buildHiddenMansionOverlay(us, pal, dir, err)) return fail(err);
            std::ofstream(port::longPath(dir / "overlay.key"), std::ios::binary) << overlayKey(us, pal);
        }
        std::lock_guard lk(job.m);
        job.stage = "Done";
        job.fraction = 1.0f;
        job.ok = true;
        job.done = true;
    });
}

#ifndef LM_MOBILE
std::string sExportResult;
std::string sExportFolder;

void startExport(Job& job, const Slot slots[2], ModuleTarget target) {
    const std::string paths[2] = {slots[0].valid ? slots[0].path : "", slots[1].valid ? slots[1].path : ""};
    job.thread = std::thread([&job, paths, target] {
        auto set = [&](const std::string& stage, float f) {
            std::lock_guard lk(job.m);
            job.stage = stage;
            job.fraction = f;
        };
        auto fail = [&](const std::string& e) {
            std::lock_guard lk(job.m);
            job.error = e;
            job.done = true;
        };
        std::vector<int> todo;
        for (int i = 0; i < 2; i++)
            if (!paths[i].empty()) todo.push_back(i);
        std::string made;
        float step = 0.0f;
        for (int i : todo) {
            const char* name = i ? "Europe" : "USA";
            Disc disc;
            std::string err;
            if (!disc.open(paths[i], err)) return fail(err);
            Progress pr;
            pr.cancel = &job.cancel;
            pr.report = [&](const std::string& st, float f) {
                set(std::string(name) + " for " + (target == ModuleTarget::Ios ? "iPhone / iPad" : "Android") + ": " + st,
                    (step + f) / float(todo.size()));
            };
            std::filesystem::path out;
            if (!exportModule(disc, target, pr, out, err)) return fail(err);
            made += (made.empty() ? "" : ", ") + out.filename().string();
            step += 1.0f;
        }
        const auto dir = exportDir(target).u8string();
        std::lock_guard lk(job.m);
        sExportFolder.assign(dir.begin(), dir.end());
        sExportResult = "Made " + made + " in " + sExportFolder + ". " +
                        (target == ModuleTarget::Ios
                             ? "Put it into the app's IPA with tools/mobile/inject_ios_module.py (or your sideloading "
                               "tool's \"inject dylib\" option), then install the IPA."
                             : "Copy it to your phone and open it with \"Import a file\" in the app, or copy it into "
                               "Android/data/io.github.lmrecompiled.mobile/files.");
        job.stage = "Done";
        job.fraction = 1.0f;
        job.ok = true;
        job.done = true;
    });
}
#else
std::optional<std::string> sImportPick;
std::string sImportResult;

bool isModuleFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    unsigned char m[4] = {};
    in.read(reinterpret_cast<char*>(m), 4);
    const bool elf = m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
    const bool macho = m[0] == 0xCF && m[1] == 0xFA && m[2] == 0xED && m[3] == 0xFE;
    return elf || macho;
}

const char* discExtension(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    unsigned char m[4] = {};
    in.read(reinterpret_cast<char*>(m), 4);
    if (std::memcmp(m, "RVZ\x01", 4) == 0) return ".rvz";
    if (std::memcmp(m, "WIA\x01", 4) == 0) return ".wia";
    if (std::memcmp(m, "CISO", 4) == 0) return ".ciso";
    if (m[0] == 0x01 && m[1] == 0xC0 && m[2] == 0x0B && m[3] == 0xB1) return ".gcz";
    return ".iso";
}

void scanImports(Slot slots[2]) {
    for (const auto& f : port::mobile::imports()) {
        if (isModuleFile(f)) continue;
        const auto u = f.u8string();
        const std::string path(u.begin(), u.end());
        if (path == slots[0].path || path == slots[1].path) continue;
        GameVersion v{};
        std::string err;
        if (!probeDisc(path, v, err)) continue;
        Slot& s = slots[v == GameVersion::PAL ? 1 : 0];
        if (s.valid) continue;
        s.path = path;
        validate(s, v);
    }
}

void startImport(Job& job, const std::string& source) {
    job.thread = std::thread([&job, source] {
        auto set = [&](const std::string& stage, float f) {
            std::lock_guard lk(job.m);
            job.stage = stage;
            job.fraction = f;
        };
        auto fail = [&](const std::string& e) {
            std::lock_guard lk(job.m);
            job.error = e;
            job.done = true;
        };
        const std::string tmpName =
            "import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";
        std::filesystem::path out;
        std::string err;
        set("Copying the file into the app", 0.0f);
        if (!port::mobile::importFile(
                source, tmpName,
                [&](float f) {
                    set("Copying the file into the app", f);
                    return !job.cancel.load();
                },
                out, err))
            return fail(err);
        std::string finalName, result;
        if (isModuleFile(out)) {
            const std::string game = embeddedModuleGame(out);
            if (game.empty()) {
                std::error_code ec;
                std::filesystem::remove(out, ec);
                return fail("This is not a Luigi's Mansion Recompiled game module.");
            }
            const auto v = game == "GLMP01" ? GameVersion::PAL : GameVersion::USA;
            finalName = mobileModuleName(v, hostTarget());
            result = "Imported the game module for " + game + ".";
        } else {
            GameVersion v{};
            if (!probeDisc(port::displayPath(out), v, err)) {
                std::error_code ec;
                std::filesystem::remove(out, ec);
                return fail("Not a Luigi's Mansion disc image or game module: " + err);
            }
            finalName = std::string(port::gameId(v)) + discExtension(out);
            result = std::string("Imported the ") + (v == GameVersion::PAL ? "European" : "USA") + " disc.";
        }
        std::error_code ec;
        const auto dst = port::mobile::importDir() / finalName;
        std::filesystem::remove(dst, ec);
        std::filesystem::rename(out, dst, ec);
        if (ec) return fail("Could not rename the imported file: " + ec.message());
        std::lock_guard lk(job.m);
        sImportResult = result;
        job.stage = "Done";
        job.fraction = 1.0f;
        job.ok = true;
        job.done = true;
    });
}

void SDLCALL onImportPicked(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) {
        const char* e = SDL_GetError();
        sDialogError = std::string("The file picker could not be opened") + (e && *e ? std::string(": ") + e : "") +
                       ". Copy the files into the folder shown above instead.";
        return;
    }
    if (files[0]) sImportPick = files[0];
}
#endif

void SDLCALL onPicked(void* userdata, const char* const* files, int) {
    const int slot = int(reinterpret_cast<intptr_t>(userdata));
    std::lock_guard lk(sPickM);
    if (!files) {  // error (cancel is an empty list)
        const char* e = SDL_GetError();
        sDialogError = std::string("The file dialog could not be opened") + (e && *e ? std::string(": ") + e : "") +
                       ". Type or paste the path of the disc image instead.";
        return;
    }
    if (!files[0]) return;  // cancelled
    sPicked[slot] = files[0];
}

void browse(int slot, const std::string& current) {
    static const SDL_DialogFileFilter filters[] = {
        {"GameCube disc images", "iso;gcm;rvz;gcz;ciso;wia;wbfs;nfs;tgc"},
        {"All files", "*"},
    };
    const size_t slash = current.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : current.substr(0, slash + 1);
    SDL_ShowOpenFileDialog(&onPicked, reinterpret_cast<void*>(intptr_t(slot)), aurora_get_window(), filters, 2,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

enum class Screen { Pick, Working, Shaders, Failed };
enum class JobKind { Install, Export, Import };

}  // namespace

bool run(const Options& opts, LaunchInfo& launch) {
    recomp::ui::install(recomp::ui::Context::Setup);
    port::loadInstall();
    auto& inst = port::install();
    Slot slots[2];
    slots[0].path = !opts.usDisc.empty() ? opts.usDisc : inst.usDisc;
    slots[1].path = !opts.palDisc.empty() ? opts.palDisc : inst.palDisc;
    validate(slots[0], GameVersion::USA);
    validate(slots[1], GameVersion::PAL);
    GameVersion base = inst.base;

    auto chooseBase = [&] {
        if (!slots[1].valid) base = GameVersion::USA;
        else if (!slots[0].valid) base = GameVersion::PAL;
    };
    chooseBase();

    // Nothing to ask: the configured discs are all readable and the base is available.
    bool showUi = opts.forceUi || opts.autoStart || !(slots[0].valid || slots[1].valid) ||
                  (!slots[0].path.empty() && !slots[0].valid) || (!slots[1].path.empty() && !slots[1].valid);
#ifdef LM_MOBILE
    scanImports(slots);
    chooseBase();
    if (port::mobile::takeSetupRequest()) showUi = true;
    showUi = showUi || !(slots[0].valid || slots[1].valid);
#endif
    Screen screen = Screen::Pick;
    JobKind jobKind = JobKind::Install;
    std::unique_ptr<Job> job;
    Plan plan;
    bool started = false;

    auto begin = [&] {
        inst.usDisc = slots[0].valid ? slots[0].path : "";
        inst.palDisc = slots[1].valid ? slots[1].path : "";
        inst.base = base;
        port::saveInstall();
        plan = makePlan(slots);
        jobKind = JobKind::Install;
        job = std::make_unique<Job>();
        if (!plan.empty()) startJob(*job, slots, plan);
        else job->done = job->ok = true;
        screen = Screen::Working;
        started = true;
    };
    if (!showUi) begin();
    else if (opts.autoStart && (slots[0].valid || slots[1].valid)) begin();

    port::setPhase("setup");
    bool finished = false;
    int framesDrawn = 0;  // the background precache is queued by the first frame (Aurora patch 0013)
    while (!finished) {
        port::heartbeat();
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) return false;
            if (ev->type == AURORA_PAUSED) port::setPhase(nullptr);  // waits for the window: not a hang
            if (ev->type == AURORA_UNPAUSED) port::setPhase("setup");
        }
#ifdef LM_MOBILE
        {
            std::optional<std::string> pick;
            {
                std::lock_guard lk(sPickM);
                pick.swap(sImportPick);
            }
            if (pick && screen == Screen::Pick) {
                sImportResult.clear();
                jobKind = JobKind::Import;
                job = std::make_unique<Job>();
                startImport(*job, *pick);
                screen = Screen::Working;
            }
        }
#endif
        {
            std::lock_guard lk(sPickM);
            for (int i = 0; i < 2; i++) {
                if (sPicked[i]) {
                    slots[i].path = *sPicked[i];
                    sPicked[i].reset();
                    validate(slots[i], i ? GameVersion::PAL : GameVersion::USA);
                    if (slots[i].valid && !slots[1 - i].valid) base = slots[i].version;
                    chooseBase();
                }
            }
        }
        if (job && screen == Screen::Working && framesDrawn > 0 && jobKind != JobKind::Install) {
            bool done = false, ok = false;
            {
                std::lock_guard lk(job->m);
                done = job->done;
                ok = job->ok;
            }
            if (done) {
                screen = ok ? Screen::Pick : Screen::Failed;
                if (ok) job.reset();
#ifdef LM_MOBILE
                if (ok) {
                    scanImports(slots);
                    chooseBase();
                }
#endif
            }
        } else if (job && screen == Screen::Working && framesDrawn > 0) {
            std::lock_guard lk(job->m);
            if (job->done) {
                if (!job->ok) screen = Screen::Failed;
                // Setup was visible (first run, disc change, rebuild after an update): also finish
                // compiling the shaders here, so the first game session runs as smoothly as later
                // ones. Later launches compile whatever is missing in the background (precache.h).
                else if ((showUi || !plan.empty()) && !recomp::precache::progress().finished()) screen = Screen::Shaders;
                else finished = true;
            }
        }
        if (screen == Screen::Shaders && recomp::precache::progress().finished()) finished = true;
        if (finished) break;
        if (!aurora_begin_frame()) continue;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        // Menu size (ui_scale.cpp; setup's Auto: 600 lines = 100%), font rasterized at that size.
        const float scale = recomp::ui::scale();
        ImGui::Dummy(ImVec2(0, 12 * scale));
        ImGui::Text("Luigi's Mansion Recompiled");
        ImGui::Separator();
        if (screen == Screen::Pick) {
#ifdef LM_MOBILE
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "Unofficial, vibe coded %s port: expect bugs.",
                               port::mobile::platformName());
            ImGui::TextWrapped("This app runs your own copy of Luigi's Mansion. It needs your disc image (ISO or RVZ) "
                               "and the game module exported for this device by Luigi's Mansion Recompiled on a PC "
                               "(docs/MOBILE.md). Import them with the button below, or copy them into this folder:");
            {
                const auto d = port::mobile::importDir().u8string();
                ImGui::TextDisabled("%s", std::string(d.begin(), d.end()).c_str());
            }
            if (ImGui::Button("Import a file...")) {
                std::lock_guard lk(sPickM);
                sDialogError.clear();
                SDL_ShowOpenFileDialog(&onImportPicked, nullptr, aurora_get_window(), nullptr, 0, nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Look in the folder again")) {
                scanImports(slots);
                chooseBase();
            }
            if (!sImportResult.empty()) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", sImportResult.c_str());
#else
            ImGui::TextWrapped("This port runs your own copy of Luigi's Mansion. Choose your disc image (ISO or RVZ). "
                               "The game is translated and compiled for this PC once; that takes a minute or two.");
#endif
            // Portable mode (docs/SETUP.md): where everything goes, or why it was not possible.
            if (const auto& loc = port::dataLocation(); loc.portable) {
                const auto s = loc.root.u8string();
                ImGui::TextDisabled("Portable mode: everything is stored in %s", std::string(s.begin(), s.end()).c_str());
            } else if (!loc.problem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1));
                ImGui::TextWrapped("%s", loc.problem.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            const char* labels[2] = {"USA disc (GLME01)", "Europe disc (GLMP01)"};
            const char* notes[2] = {"", "Playable on its own; with a USA disc it also adds the European Hidden Mansion."};
            for (int i = 0; i < 2; i++) {
                ImGui::PushID(i);
                ImGui::Text("%s", labels[i]);
                ImGui::SameLine();
                if (*notes[i]) ImGui::TextDisabled("%s", notes[i]);
                ImGui::TextWrapped("%s", slots[i].path.empty() ? "(none)" : slots[i].path.c_str());
#ifdef LM_MOBILE
                if (slots[i].valid) {
                    Disc d;
                    std::string err;
                    static std::string sCheckedPath[2];
                    static bool sModuleOk[2];
                    if (sCheckedPath[i] != slots[i].path) {
                        sCheckedPath[i] = slots[i].path;
                        sModuleOk[i] = d.open(slots[i].path, err) && moduleUpToDate(d);
                    }
                    if (sModuleOk[i]) ImGui::TextDisabled("Game module: ready");
                    else
                        ImGui::TextDisabled("Game module: %s (looked for when you press Install and play)",
                                            mobileModuleName(slots[i].version, hostTarget()).c_str());
                }
#else
                if (ImGui::Button("Browse...")) browse(i, slots[i].path);
#endif
#if !defined(_WIN32) && !defined(LM_MOBILE)
                // Linux: the file dialog needs xdg-desktop-portal (or zenity/kdialog); a typed or
                // pasted path always works (docs/LINUX.md).
                {
                    static char typed[2][1024];
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                    const bool enter = ImGui::InputTextWithHint("##path", "or type / paste the path here",
                                                                typed[i], sizeof(typed[i]),
                                                                ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Use") || enter) && typed[i][0]) {
                        std::string p = typed[i];
                        while (!p.empty() && (p.back() == ' ' || p.back() == '\n')) p.pop_back();
                        if (p.size() >= 2 && (p.front() == '\'' || p.front() == '"') && p.back() == p.front())
                            p = p.substr(1, p.size() - 2);  // pasted from a file manager / shell
                        if (p.rfind("file://", 0) == 0) p = p.substr(7);
                        std::lock_guard lk(sPickM);
                        sPicked[i] = p;
                        typed[i][0] = 0;
                    }
                }
#endif
                if (!slots[i].path.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Remove")) {
                        slots[i] = Slot{};
                        chooseBase();
                    }
                    ImGui::SameLine();
                    if (slots[i].valid) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", slots[i].message.c_str());
                    else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", slots[i].message.c_str());
                }
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::PopID();
            }
            {
                std::lock_guard lk(sPickM);
                if (!sDialogError.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1));
                    ImGui::TextWrapped("%s", sDialogError.c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (slots[0].valid && slots[1].valid) {
                ImGui::Text("Version to play:");
                ImGui::SameLine();
                if (ImGui::RadioButton("USA", base == GameVersion::USA)) base = GameVersion::USA;
                ImGui::SameLine();
                if (ImGui::RadioButton("Europe (50/60 Hz, languages)", base == GameVersion::PAL)) base = GameVersion::PAL;
                ImGui::TextDisabled("Both versions are prepared; you can switch later in the F1 menu (Game tab).");
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::BeginDisabled(!(slots[0].valid || slots[1].valid));
            if (ImGui::Button(started ? "Continue" : "Install and play")) begin();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit")) {
                ImGui::End();
                aurora_end_frame();
                return false;
            }
#ifndef LM_MOBILE
            ImGui::Dummy(ImVec2(0, 12 * scale));
            ImGui::SeparatorText("Play on a phone (unofficial, vibe coded)");
            ImGui::TextWrapped("Builds the game module the Android or iPhone / iPad app needs, from the discs above. "
                               "Phones cannot compile it themselves (docs/MOBILE.md).");
            ImGui::BeginDisabled(!(slots[0].valid || slots[1].valid));
            auto exportFor = [&](ModuleTarget t) {
                sExportResult.clear();
                jobKind = JobKind::Export;
                job = std::make_unique<Job>();
                startExport(*job, slots, t);
                screen = Screen::Working;
            };
            if (ImGui::Button("Export for Android")) exportFor(ModuleTarget::Android);
            ImGui::SameLine();
            if (ImGui::Button("Export for iPhone / iPad")) exportFor(ModuleTarget::Ios);
            ImGui::EndDisabled();
            if (!sExportResult.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Done.");
                ImGui::TextWrapped("%s", sExportResult.c_str());
                if (ImGui::Button("Open folder")) SDL_OpenURL(("file://" + sExportFolder).c_str());
            }
#endif
        } else {
            std::string stage, error;
            float f = 0.0f;
            {
                std::lock_guard lk(job->m);
                stage = job->stage;
                f = job->fraction;
                error = job->error;
            }
            if (screen == Screen::Shaders) {
                const auto pr = recomp::precache::progress();
                ImGui::TextWrapped("Compiling shaders for your graphics card. This happens once (and again after a "
                                   "driver or game update).");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::ProgressBar(pr.fraction(), ImVec2(-1, 0));
                ImGui::Text("Compiling shaders: %u / %u", pr.done, pr.total);
                // (No "play now": the game waits for its shaders anyway, precache.h holdGame.)
            } else if (screen == Screen::Working) {
                ImGui::TextWrapped("%s", jobKind == JobKind::Export ? "Building the game module for the phone app."
                                         : jobKind == JobKind::Import
                                             ? "Importing the file."
                                             : "Preparing the game. This happens once per disc (and again after updates).");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::ProgressBar(f, ImVec2(-1, 0));
                ImGui::TextWrapped("%s", stage.c_str());
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Cancel")) {
                    job.reset();  // joins the worker after it notices
                    screen = Screen::Pick;
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "Setup failed");
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Back")) {
                    job.reset();
                    screen = Screen::Pick;
                }
            }
        }
        ImGui::End();
        aurora_end_frame();
        framesDrawn++;
        // While the game is prepared and the shaders compile (from the first frame on): frames below
        // the refresh rate, as in the game's shader hold (issue #33; precache.h holdFramePause).
        if (screen != Screen::Pick) recomp::precache::holdFramePause(0.0);
    }
    job.reset();

    // Ready: report what is installed and what to launch.
    inst.usReady = slots[0].valid;
    inst.palReady = slots[1].valid;
    inst.hmOverlayReady = slots[0].valid && slots[1].valid &&
                          std::filesystem::exists(port::longPath(port::hiddenMansionOverlayDir() / "manifest.txt")) &&
                          std::filesystem::exists(port::longPath(port::hiddenMansionOverlayDir() / "usa" / "manifest.txt"));
    inst.running = base;
    launch.version = base;
    launch.disc = slots[base == GameVersion::PAL ? 1 : 0].path;
    launch.module = modulePath(base);
    return true;
}

}  // namespace setup
