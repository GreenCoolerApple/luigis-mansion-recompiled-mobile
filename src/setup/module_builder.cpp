#include "setup/module_builder.h"

#include "port/dol.h"
#include "port/install.h"
#include "port/update.h"
#include "recomp/ctx.h"
#include "recompiler/recompiler.h"
#include "setup/embedded.h"
#ifdef LM_MOBILE
#include "port/mobile.h"
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

namespace setup {
namespace {

// Flags for the generated code. Freestanding: no headers, no C runtime (module_rt.cpp
// provides the few symbols the compiler may call), no thread-safe statics (they need the
// CRT; only one guest thread runs at a time). Unwind tables are required: the runtime ends
// guest threads (OSExitThread, OSCancelThread, shutdown) by throwing sched::ThreadExit through
// the module's frames, which Windows can only unwind with .pdata and Linux with .eh_frame
// (+ the .eh_frame_hdr lookup table the linker writes).
struct TargetSpec {
    std::vector<std::string> compile;
    std::vector<std::string> link;
    const char* moduleFile;
    const char* objExt;
    bool msvcLink;
    bool embedKey;
    const char* installNamePrefix;
};

const TargetSpec& androidSpec() {
    static const TargetSpec s{
        {"--target=aarch64-linux-android29", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden", "-ffreestanding",
         "-nostdinc", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno",
         "-fno-stack-protector", "-ffp-contract=off", "-mno-outline-atomics", "-funwind-tables",
         "-Wno-unused-label", "-Wno-unused-variable"},
        {"-flavor", "gnu", "-shared", "-nostdlib", "--eh-frame-hdr", "-Bsymbolic", "-z", "defs", "-z", "noexecstack",
         "-z", "now", "-z", "max-page-size=16384", "-O1", "--hash-style=both", "-soname", "lm_game_module.so"},
        "module.so", ".o", false, true, nullptr};
    return s;
}

const TargetSpec& iosSpec() {
    static const TargetSpec s{
        {"--target=arm64-apple-ios15.0", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden", "-ffreestanding",
         "-nostdinc", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno",
         "-fno-stack-protector", "-ffp-contract=off", "-funwind-tables", "-Wno-unused-label",
         "-Wno-unused-variable"},
        {"-flavor", "darwin", "-dylib", "-arch", "arm64", "-platform_version", "ios", "15.0", "15.0",
         "-undefined", "error", "-dead_strip", "-O1"},
        "module.dylib", ".o", false, true, "@rpath/lm_game_"};
    return s;
}

#if defined(__ANDROID__)
const TargetSpec& hostSpec() { return androidSpec(); }
#elif defined(LM_MOBILE) && defined(__APPLE__)
const TargetSpec& hostSpec() { return iosSpec(); }
#elif defined(_WIN32)
// Windows: stack probes stay on (module_rt.cpp provides __chkstk: a generated function with
// a frame above a page would otherwise skip the stack guard page and fault; SMS port fix).
const TargetSpec& hostSpec() {
    static const TargetSpec s{
        {"--target=x86_64-pc-windows-msvc", "-std=c++20", "-O2", "-ffreestanding", "-nostdinc", "-fno-exceptions",
         "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno", "-fno-stack-protector", "-funwind-tables",
         "-Wno-unused-label", "-Wno-unused-variable"},
        {"/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/opt:ref", "/incremental:no"},
        "module.dll", ".obj", true, false, nullptr};
    return s;
}
#elif defined(__APPLE__)
// macOS (docs/MACOS.md): a Mach-O dylib for the host architecture, linked by ld64.lld without
// the SDK (the module needs nothing from libSystem). Same visibility rules as Linux: only
// lm_module_init is exported. -ffp-contract=off: arm64 compilers fuse a*b+c into one fma by
// default, which rounds differently from the PowerPC's separate fmul/fadd (fmadd is emitted
// as fma_ explicitly); x86-64 builds without -mfma never fuse.
#if defined(__aarch64__)
#define LM_MODULE_ARCH "arm64"
#else
#define LM_MODULE_ARCH "x86_64"
#endif
const TargetSpec& hostSpec() {
    static const TargetSpec s{
        {"--target=" LM_MODULE_ARCH "-apple-macos13.0", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden",
         "-ffreestanding", "-nostdinc", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno",
         "-fno-stack-protector", "-ffp-contract=off", "-funwind-tables", "-Wno-unused-label", "-Wno-unused-variable"},
        {"-dylib", "-arch", LM_MODULE_ARCH, "-platform_version", "macos", "13.0", "13.0", "-install_name",
         "@rpath/lm_game_module.dylib", "-undefined", "error", "-dead_strip", "-O1"},
        "module.dylib", ".o", false, false, nullptr};
    return s;
}
#else
// Linux: a position-independent shared object. Everything is hidden and bound locally
// (-fvisibility=hidden, -Bsymbolic) except lm_module_init, so the module's g_mem, memcpy, ...
// can never be interposed by the executable's or libc's symbols. -z defs turns a missing
// helper (one the compiler calls that module_rt.cpp does not provide) into a link error
// instead of a dlopen failure.
const TargetSpec& hostSpec() {
    static const TargetSpec s{
        {"--target=x86_64-pc-linux-gnu", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden", "-ffreestanding",
         "-nostdinc", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno",
         "-fno-stack-protector", "-funwind-tables", "-Wno-unused-label", "-Wno-unused-variable"},
        {"-shared", "-nostdlib", "--eh-frame-hdr", "-Bsymbolic", "-z", "defs", "-z", "noexecstack", "-z", "now", "-O1",
         "--hash-style=both", "-soname", "lm_game_module.so"},
        "module.so", ".o", false, false, nullptr};
    return s;
}
#endif

const TargetSpec& specFor(ModuleTarget t) {
    switch (t) {
    case ModuleTarget::Android: return androidSpec();
    case ModuleTarget::Ios: return iosSpec();
    default: return hostSpec();
    }
}

#ifdef _WIN32
[[maybe_unused]] constexpr const char* kExeSuffix = ".exe";
[[maybe_unused]] constexpr const char* kLinkerName = "lld-link";
#elif defined(__APPLE__)
[[maybe_unused]] constexpr const char* kExeSuffix = "";
[[maybe_unused]] constexpr const char* kLinkerName = "ld64.lld";
#else
[[maybe_unused]] constexpr const char* kExeSuffix = "";
[[maybe_unused]] constexpr const char* kLinkerName = "ld.lld";
#endif

constexpr const char* kKeyMarker = "LMMODKEY:";

// Paths here are plain (they are also clang/lld arguments, which take no \\?\ prefix; LLVM
// itself handles plain paths past MAX_PATH). Our own file calls go through port::longPath, so a
// deep %APPDATA% works without Windows' long path policy (docs/SETUP.md "Long paths").
using port::longPath;

bool writeText(const std::filesystem::path& p, std::string_view text) {
    std::ofstream o(longPath(p), std::ios::binary);
    o.write(text.data(), std::streamsize(text.size()));
    return bool(o);
}

[[maybe_unused]] std::string utf8(const std::filesystem::path& p) { return port::displayPath(p); }

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(longPath(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

#ifndef LM_MOBILE
#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Runs a tool and waits; its output goes to `log`. Returns the exit code (-1: not started).
int run(const std::filesystem::path& exe, const std::vector<std::string>& args, const std::filesystem::path& log) {
    std::wstring cmd = L"\"" + port::plainPath(exe).wstring() + L"\"";
    for (const auto& a : args) cmd += L" \"" + widen(a) + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(longPath(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // The program by its plain path: started by an extended name, it sees that (4 characters
    // longer) as its own path, and LLVM's tools fail on it sooner (toolAtShortPath).
    const BOOL ok = CreateProcessW(port::plainPath(exe).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}
#else
// Runs a tool and waits; stdout/stderr go to `log`, stdin is /dev/null. Returns the exit
// code (-1: not started, 128 + n: killed by signal n). The tool runs at a lower priority so
// the setup screen stays responsive while every core compiles.
int run(const std::filesystem::path& exe, const std::vector<std::string>& args, const std::filesystem::path& log) {
    std::vector<std::string> argStore;
    argStore.push_back(exe.string());
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = -1;
    const int err = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) return -1;
    setpriority(PRIO_PROCESS, id_t(pid), 10);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
#endif

std::filesystem::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    return port::selfExeDir();
#endif
}

#ifdef _WIN32
// clang.exe and lld-link.exe read their own path into a MAX_PATH buffer and stop ("The data area
// passed to a system call is too small") when it is longer, whatever the long path policy says
// (LLVM 22: 257 characters run, 259 fail). From such a game folder they are linked (or copied,
// on another drive) into <dataDir>\llvm and run from there; the copy is refreshed when the
// original changes (an update). The program is also started by its plain path (run()).
constexpr size_t kMaxToolPath = 250;
bool toolAtShortPath(std::filesystem::path& tool, std::string& error) {
    namespace fs = std::filesystem;
    if (tool.native().size() <= kMaxToolPath) return true;
    const fs::path dir = port::dataDir() / "llvm", to = dir / tool.filename();
    if (to.native().size() > kMaxToolPath) {
        error = "The game's folders are too deep for the bundled compiler: " + utf8(tool) + " is " +
                std::to_string(tool.native().size()) + " characters long (at most " +
                std::to_string(kMaxToolPath) + "). Move the game to a shorter folder (e.g. C:\\Games\\Luigi's "
                "Mansion Recompiled) and start it from there.";
        return false;
    }
    std::error_code ec;
    const fs::path from = longPath(tool), dst = longPath(to);
    const auto size = fs::file_size(from, ec);
    const auto time = fs::last_write_time(from, ec);
    std::error_code e1, e2;
    if (fs::file_size(dst, e1) != size || fs::last_write_time(dst, e2) != time || e1 || e2) {
        fs::create_directories(longPath(dir), ec);
        fs::remove(dst, ec);
        const char* how = "linked";
        if (!CreateHardLinkW(dst.c_str(), from.c_str(), nullptr)) {
            how = "copied";
            fs::copy_file(from, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::last_write_time(dst, time, ec);
            if (ec) {
                error = "The game's folder is too deep for the bundled compiler (" + utf8(tool) +
                        "), and copying it to " + utf8(to) + " failed: " + ec.message() +
                        ". Move the game to a shorter folder (e.g. C:\\Games\\Luigi's Mansion Recompiled).";
                return false;
            }
        }
        std::printf("[setup] %s is %zu characters long (deeper than the compiler accepts); %s to %s\n",
                    utf8(tool).c_str(), tool.native().size(), how, utf8(to).c_str());
        std::fflush(stdout);
    }
    tool = to;
    return true;
}
#endif
#endif  // LM_MOBILE

}  // namespace

std::filesystem::path moduleDir(port::GameVersion v) { return port::dataDir() / "game" / port::gameId(v); }

const char* targetLabel(ModuleTarget t) {
    switch (t) {
    case ModuleTarget::Android: return "android-arm64";
    case ModuleTarget::Ios: return "ios-arm64";
    default: return "host";
    }
}

std::string mobileModuleName(port::GameVersion v, ModuleTarget t) {
    return std::string("lm_game_") + port::gameId(v) + (t == ModuleTarget::Ios ? ".dylib" : ".so");
}

std::filesystem::path exportDir(ModuleTarget t) { return port::dataDir() / "mobile" / targetLabel(t); }

std::string moduleKey(const Disc& disc, ModuleTarget target) {
    const TargetSpec& spec = specFor(target);
    const bool pal = disc.version() == port::GameVersion::PAL;
    std::string text = "abi " + std::to_string(recomp::kModuleAbiVersion) + "\n";
    // The program version: an update (docs/SETUP.md "Updates") always rebuilds the module, also
    // for changes the embedded sources below do not show (headers the recompiler includes, the
    // bundled clang/lld).
    text += std::string("program ") + port::update::buildVersion() + "\n";
    text += std::string("game ") + port::gameId(disc.version()) + " " + disc.dolSha1() + "\n";
    for (auto& f : spec.compile) text += f + " ";
    for (auto& f : spec.link) text += f + " ";
    for (std::string_view part : {embedded::symbols(pal), embedded::splits(pal), embedded::versionMap(pal),
                                  embedded::hooks(), embedded::ctxHeader(), embedded::moduleAbiHeader(),
                                  embedded::moduleRt(), embedded::recompilerSource()}) {
        const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(part.data()), part.size());
        text += hex(d.data(), d.size()) + "\n";
    }
    const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hex(d.data(), d.size());
}

namespace {

bool readEmbeddedMarker(const std::filesystem::path& file, std::string& key, std::string& game) {
    const std::string data = readText(file);
    const std::string marker = kKeyMarker;
    for (size_t pos = data.find(marker); pos != std::string::npos; pos = data.find(marker, pos + 1)) {
        const size_t begin = pos + marker.size();
        if (begin + 40 + 1 + 6 + 4 > data.size()) break;
        if (data[begin + 40] != ':' || data.compare(begin + 47, 4, ":END") != 0) continue;
        key = data.substr(begin, 40);
        game = data.substr(begin + 41, 6);
        return true;
    }
    return false;
}

}

std::string embeddedModuleKey(const std::filesystem::path& file) {
    std::string key, game;
    return readEmbeddedMarker(file, key, game) ? key : std::string();
}

std::string embeddedModuleGame(const std::filesystem::path& file) {
    std::string key, game;
    return readEmbeddedMarker(file, key, game) ? game : std::string();
}

#ifdef LM_MOBILE
bool findToolchain(std::filesystem::path&, std::filesystem::path&, std::string& error) {
    error = "The game module is built on a PC (docs/MOBILE.md).";
    return false;
}
#else
bool findToolchain(std::filesystem::path& clang, std::filesystem::path& lld, std::string& error) {
    const char* exe = kExeSuffix;
    std::vector<std::filesystem::path> dirs{exeDir() / "llvm" / "bin"};
    if (const char* env = std::getenv("LM_LLVM_DIR")) dirs.emplace_back(env);
#ifdef LM_DEV_LLVM_DIR
    dirs.emplace_back(LM_DEV_LLVM_DIR);
#endif
#ifdef LM_DEV_LLD_DIR
    dirs.emplace_back(LM_DEV_LLD_DIR);
#endif
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        for (size_t pos = 0; pos <= p.size();) {
            size_t end = p.find(sep, pos);
            if (end == std::string::npos) end = p.size();
            if (end > pos) dirs.emplace_back(p.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    for (const auto& d : dirs) {
        std::error_code ec;
        const auto c = d / (std::string("clang") + exe), l = d / (std::string(kLinkerName) + exe);
        if (std::filesystem::exists(longPath(c), ec) && std::filesystem::exists(longPath(l), ec)) {
            clang = port::plainPath(longPath(c));
            lld = port::plainPath(longPath(l));
            return true;
        }
    }
#ifdef __APPLE__
    // Homebrew keeps clang (llvm) and ld64.lld (lld) in separate folders: the first of each.
    clang.clear();
    lld.clear();
    for (const auto& d : dirs) {
        std::error_code ec;
        if (clang.empty() && std::filesystem::exists(d / "clang", ec)) clang = d / "clang";
        if (lld.empty() && std::filesystem::exists(d / kLinkerName, ec)) lld = d / kLinkerName;
    }
    if (!clang.empty() && !lld.empty()) return true;
#endif
#ifdef _WIN32
    error = "The bundled compiler (llvm\\bin\\clang.exe and lld-link.exe next to the game) is missing. "
            "Reinstall the game, or set LM_LLVM_DIR to an LLVM bin directory.";
#else
    error = std::string("The bundled compiler (llvm/bin/clang and llvm/bin/") + kLinkerName +
            " next to the game) is missing. "
            "Reinstall the game, or set LM_LLVM_DIR to an LLVM bin directory.";
#endif
    return false;
}

namespace {

bool buildInto(const Disc& disc, ModuleTarget target, const std::filesystem::path& dir, const std::string& outName,
               const Progress& progress, std::string& error) {
    const TargetSpec& spec = specFor(target);
    auto report = [&](const std::string& s, float f) {
        if (progress.report) progress.report(s, f);
    };
    auto cancelled = [&] { return progress.cancel && progress.cancel->load(); };
    std::filesystem::path clang, lld;
    if (!findToolchain(clang, lld, error)) return false;
#ifdef __APPLE__
    // A package downloaded with a browser carries Gatekeeper's quarantine flag, which stops the
    // bundled tools when they are started (the player already allowed this program to run).
    // Only the package's own llvm/bin: a toolchain found through LM_LLVM_DIR, Homebrew or PATH
    // is the user's, and its flags are left alone.
    {
        std::error_code ec;
        const std::filesystem::path bundled = std::filesystem::weakly_canonical(exeDir() / "llvm" / "bin", ec);
        for (const auto* t : {&clang, &lld}) {
            std::error_code ec2;
            const std::filesystem::path dir = std::filesystem::weakly_canonical(*t, ec2).parent_path();
            if (!ec && !ec2 && dir == bundled) removexattr(t->c_str(), "com.apple.quarantine", 0);
        }
    }
#endif
#ifdef _WIN32
    if (!toolAtShortPath(clang, error) || !toolAtShortPath(lld, error)) return false;
#endif
    std::printf("[setup] building the %s game module (%s) with %s and %s\n", port::gameId(disc.version()),
                targetLabel(target), utf8(clang).c_str(), utf8(lld).c_str());
    std::fflush(stdout);
    const auto buildStart = std::chrono::steady_clock::now();

    const bool pal = disc.version() == port::GameVersion::PAL;
    const auto work = dir / "build";
    std::error_code ec;
    std::filesystem::remove_all(longPath(work), ec);
    std::filesystem::create_directories(longPath(work / "src"), ec);
    std::filesystem::create_directories(longPath(work / "obj"), ec);
    std::filesystem::create_directories(longPath(work / "include" / "recomp"), ec);

    // 1. Recompile.
    report("Translating the game code", 0.0f);
    lmrc::Input in;
    in.dol = disc.dol();
    in.version = port::gameId(disc.version());
    in.symbols = std::string(embedded::symbols(pal));
    in.splits = std::string(embedded::splits(pal));
    in.versionMap = std::string(embedded::versionMap(pal));
    in.hooks = std::string(embedded::hooks());
    lmrc::Output out;
    if (!lmrc::recompile(in, out, error)) {
        error = "Recompiling main.dol failed: " + error;
        return false;
    }
    std::vector<std::filesystem::path> sources;
    for (auto& [name, text] : out.files) {
        if (!writeText(work / "src" / name, text)) {
            error = "Could not write to " + utf8(work / "src");
            return false;
        }
        if (name.size() > 4 && name.substr(name.size() - 4) == ".cpp") sources.push_back(work / "src" / name);
    }
    out.files.clear();
    writeText(work / "include" / "recomp" / "ctx.h", embedded::ctxHeader());
    writeText(work / "include" / "recomp" / "module_abi.h", embedded::moduleAbiHeader());
    writeText(work / "src" / "module_rt.cpp", embedded::moduleRt());
    sources.push_back(work / "src" / "module_rt.cpp");
    if (spec.embedKey) {
        const std::string key = moduleKey(disc, target);
        writeText(work / "src" / "module_key.cpp",
                  "extern \"C\" __attribute__((used, visibility(\"default\"))) const char lm_module_key[] = \"" +
                      std::string(kKeyMarker) + key + ":" + port::gameId(disc.version()) + ":END\";\n");
        sources.push_back(work / "src" / "module_key.cpp");
    }
    writeText(work / "recompiler.log", out.summary + "\n");
    if (cancelled()) return false;

    // 2. Compile in parallel (largest files first keeps all cores busy to the end).
    std::sort(sources.begin(), sources.end(), [](auto& a, auto& b) {
        std::error_code e;
        return std::filesystem::file_size(longPath(a), e) > std::filesystem::file_size(longPath(b), e);
    });
    const unsigned jobs = std::max(1u, std::min<unsigned>(unsigned(sources.size()), std::thread::hardware_concurrency()));
    std::mutex m;
    size_t next = 0, done = 0;
    std::string failure;
    report("Compiling the game", 0.02f);
    auto worker = [&] {
        for (;;) {
            size_t i;
            {
                std::lock_guard lk(m);
                if (next >= sources.size() || !failure.empty() || cancelled()) return;
                i = next++;
            }
            const auto& src = sources[i];
            auto obj = work / "obj" / src.stem();
            obj += spec.objExt;
            auto log = work / "obj" / src.stem();
            log += ".log";
            std::vector<std::string> args(spec.compile.begin(), spec.compile.end());
            args.push_back("-I" + utf8(work / "include"));
            args.push_back("-c");
            args.push_back(utf8(src));
            args.push_back("-o");
            args.push_back(utf8(obj));
            const int rc = run(clang, args, log);
            std::lock_guard lk(m);
            if (rc != 0 && failure.empty())
                failure = "Compiling " + src.filename().string() + " failed (exit " + std::to_string(rc) + "):\n" +
                          readText(log).substr(0, 2000);
            done++;
            report("Compiling the game (" + std::to_string(done) + "/" + std::to_string(sources.size()) + ")",
                   0.02f + 0.93f * float(done) / float(sources.size()));
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (cancelled()) {
        error = "Cancelled.";
        return false;
    }
    if (!failure.empty()) {
        error = failure;
        return false;
    }

    // 3. Link.
    report("Linking the game module", 0.96f);
    const auto tmpDll = work / spec.moduleFile;
    std::vector<std::string> args(spec.link.begin(), spec.link.end());
    if (spec.installNamePrefix) {
        args.push_back("-install_name");
        args.push_back(std::string(spec.installNamePrefix) + port::gameId(disc.version()) + ".dylib");
    }
    if (spec.msvcLink) {
        args.push_back("/out:" + utf8(tmpDll));
        args.push_back("/implib:" + utf8(work / "module.lib"));
    } else {
        args.push_back("-o");
        args.push_back(utf8(tmpDll));
    }
    for (const auto& s : sources) {
        auto obj = work / "obj" / s.stem();
        obj += spec.objExt;
        args.push_back(utf8(obj));
    }
    if (const int rc = run(lld, args, work / "link.log"); rc != 0) {
        error = "Linking the game module failed (exit " + std::to_string(rc) + "):\n" + readText(work / "link.log").substr(0, 2000);
        return false;
    }
    std::filesystem::remove(longPath(dir / outName), ec);
    std::filesystem::rename(longPath(tmpDll), longPath(dir / outName), ec);
    if (ec) {
        error = "Could not install the game module: " + ec.message();
        return false;
    }
    if (target == ModuleTarget::Host) {
        writeText(dir / "module.key", moduleKey(disc, target));
        writeText(dir / "recompiler.log", out.summary + "\n");
    }
    // Generated code is derived from the user's game; keep it only while building.
    if (!std::getenv("LM_KEEP_BUILD")) std::filesystem::remove_all(longPath(work), ec);
    std::printf("[setup] game module built in %.1f s (%u compile jobs)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count(), jobs);
    std::fflush(stdout);
    report("Done", 1.0f);
    return true;
}

}

#endif  // LM_MOBILE

#ifndef LM_MOBILE
ModuleTarget hostTarget() { return ModuleTarget::Host; }

std::filesystem::path modulePath(port::GameVersion v) { return moduleDir(v) / hostSpec().moduleFile; }

bool moduleUpToDate(const Disc& disc) {
    const auto dir = moduleDir(disc.version());
    std::error_code ec;
    return std::filesystem::exists(longPath(dir / hostSpec().moduleFile), ec) &&
           readText(dir / "module.key") == moduleKey(disc);
}

bool buildModule(const Disc& disc, const Progress& progress, std::string& error) {
    return buildInto(disc, ModuleTarget::Host, moduleDir(disc.version()), hostSpec().moduleFile, progress, error);
}

bool exportModule(const Disc& disc, ModuleTarget target, const Progress& progress, std::filesystem::path& out,
                  std::string& error) {
    const auto dir = exportDir(target);
    std::error_code ec;
    std::filesystem::create_directories(longPath(dir), ec);
    const std::string name = mobileModuleName(disc.version(), target);
    if (!buildInto(disc, target, dir, name, progress, error)) return false;
    out = dir / name;
    return true;
}
#else
namespace {

namespace fs = std::filesystem;

std::vector<fs::path> mobileCandidates(port::GameVersion v, bool includeImports) {
    std::vector<fs::path> found;
    const std::string want = mobileModuleName(v, hostTarget());
    const std::string stem = want.substr(0, want.find('.'));
    const std::string ext = want.substr(want.find('.'));
    auto consider = [&](const fs::path& p) {
        const std::string name = p.filename().string();
        if (name.rfind(stem, 0) == 0 && p.extension().string() == ext) found.push_back(p);
    };
    std::error_code ec;
#ifdef __APPLE__
    for (auto it = fs::recursive_directory_iterator(port::mobile::bundleDir(), ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) consider(it->path());
    }
#endif
    if (includeImports) {
        ec.clear();
        for (auto it = fs::directory_iterator(port::mobile::importDir(), ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) consider(it->path());
        }
    }
    return found;
}

#ifdef __APPLE__
fs::path bundledModule(port::GameVersion v, const std::string* key) {
    for (const auto& c : mobileCandidates(v, false))
        if (!key || embeddedModuleKey(c) == *key) return c;
    return {};
}
#endif

}

ModuleTarget hostTarget() {
#ifdef __APPLE__
    return ModuleTarget::Ios;
#else
    return ModuleTarget::Android;
#endif
}

std::filesystem::path modulePath(port::GameVersion v) {
#ifdef __APPLE__
    if (auto p = bundledModule(v, nullptr); !p.empty()) return p;
#endif
    return moduleDir(v) / hostSpec().moduleFile;
}

bool moduleUpToDate(const Disc& disc) {
#ifdef __APPLE__
    const std::string key = moduleKey(disc);
    return !bundledModule(disc.version(), &key).empty();
#else
    const auto dir = moduleDir(disc.version());
    std::error_code ec;
    return std::filesystem::exists(dir / hostSpec().moduleFile, ec) && readText(dir / "module.key") == moduleKey(disc);
#endif
}

bool buildModule(const Disc& disc, const Progress& progress, std::string& error) {
    if (progress.report) progress.report("Looking for the game module exported on your PC", 0.1f);
    const std::string key = moduleKey(disc);
    const std::string name = mobileModuleName(disc.version(), hostTarget());
    const auto candidates = mobileCandidates(disc.version(), true);
    bool stale = false;
    for (const auto& c : candidates) {
        if (embeddedModuleKey(c) != key) {
            stale = true;
            continue;
        }
#ifdef __APPLE__
        if (c.parent_path() == port::mobile::importDir()) {
            error = name + " is in the app's Documents folder, but iOS only loads code that is signed inside the "
                           "app. Put it into the IPA with tools/mobile/inject_ios_module.py (or your sideloading "
                           "tool's \"inject dylib\" option) and install the app again (docs/MOBILE.md).";
            return false;
        }
        if (progress.report) progress.report("Done", 1.0f);
        return true;
#else
        const auto dir = moduleDir(disc.version());
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto tmp = dir / "module.so.tmp";
        fs::copy_file(c, tmp, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(tmp, dir / hostSpec().moduleFile, ec);
        if (ec) {
            error = "Could not copy the game module into the app: " + ec.message();
            return false;
        }
        writeText(dir / "module.key", key);
        if (progress.report) progress.report("Done", 1.0f);
        return true;
#endif
    }
    const std::string where = port::displayPath(port::mobile::importDir());
    if (stale)
        error = name + " was exported by a different version of the game, or from a different disc image. Export it "
                       "again on your PC with the same version of Luigi's Mansion Recompiled as this app, from the "
                       "same disc (docs/MOBILE.md).";
    else
        error = "The game module " + name + " is missing. On your PC, open Luigi's Mansion Recompiled, choose this "
                "disc and press \"Export for " +
                std::string(hostTarget() == ModuleTarget::Ios ? "iPhone / iPad" : "Android") + "\", then " +
                (hostTarget() == ModuleTarget::Ios ? std::string("put it into the IPA (docs/MOBILE.md).")
                                                   : "copy " + name + " into " + where + " or use \"Import a file\".");
    return false;
}

bool exportModule(const Disc&, ModuleTarget, const Progress&, std::filesystem::path&, std::string& error) {
    error = "Exporting is done on a PC.";
    return false;
}
#endif

}  // namespace setup
