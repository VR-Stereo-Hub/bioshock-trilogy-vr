// BioShock-specific installation policy. The transaction and ownership checks
// follow the Dishonored launcher; no Dishonored settings or game writes transfer.
#include "model/launcher.h"
#include "sys/fs.h"
#include "sys/game_ini.h"
#include "sys/resources.h"
#include "payload_ids.h"
#include <json/json.h>
#include <algorithm>
#include <memory>
#include <set>

namespace bvr::launcher {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr const wchar_t* kFiles[] = {L"xinput1_3.dll", L"bioshockvr.dll", L"bvr_steamvr32.dll", L"openvr_api.dll"};
int g_failAfter = -1;
std::string json(const Json::Value& value) {
    Json::StreamWriterBuilder writer; writer["indentation"] = "  ";
    return Json::writeString(writer, value) + "\r\n";
}
Bytes bytes(const std::string& s) { return {s.begin(), s.end()}; }
bool load_json(const std::wstring& path, Json::Value* out) {
    Bytes b;
    if (fs::file_size(path) > 4 * 1024 * 1024 || !fs::read_file(path, &b, nullptr)) return false;
    Json::CharReaderBuilder builder; builder["rejectDupKeys"] = true; builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader()); std::string error;
    return reader->parse(reinterpret_cast<const char*>(b.data()), reinterpret_cast<const char*>(b.data()) + b.size(), out, &error) && out->isObject();
}
bool safe_path(const std::wstring& path, Report* r) {
    wchar_t full[32768]{};
    DWORD n = GetFullPathNameW(path.c_str(), 32768, full, nullptr);
    if (!n || n >= 32768 || path.size() < 3 || path[1] != L':') {
        r->fail("Invalid destination", fs::narrow(path)); return false;
    }
    // Refuse reparse points in every existing component. A write operation
    // cannot be redirected out of the folder the player selected.
    std::wstring check = full;
    bool leaf = true;
    while (check.size() > 3) {
        DWORD a = GetFileAttributesW(check.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || (leaf && (a & FILE_ATTRIBUTE_DIRECTORY))) {
                r->fail("Unsafe destination", "A file path is a link or directory: " + fs::narrow(check)); return false;
            }
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            r->fail("Cannot inspect destination", fs::narrow(check), GetLastError()); return false;
        }
        leaf = false; check = fs::parent(check);
    }
    return true;
}
bool read_optional(const std::wstring& path, Bytes* out, Report* r) {
    if (!safe_path(path, r)) return false;
    if (!fs::exists(path)) { out->clear(); return true; }
    DWORD err = 0;
    if (!fs::read_file(path, out, &err)) { r->fail("Cannot read file", fs::narrow(path), err); return false; }
    return true;
}
std::string hash(const Bytes& b) {
    std::string h; fs::sha256_bytes(b.data(), b.size(), &h); return h;
}
struct Change {
    std::wstring path;
    Bytes before, after;
    bool existed = false, remove = false;
};
struct Transaction {
    std::vector<Change> changes;
    Report* report;
    explicit Transaction(Report* r) : report(r) {}
    bool plan(const std::wstring& path, const Bytes& after, bool remove = false) {
        for (const auto& c : changes) if (fs::iequals(c.path, path)) {
            report->fail("Conflicting file plan", fs::narrow(path)); return false;
        }
        Change c; c.path = path; c.after = after; c.remove = remove;
        if (!read_optional(path, &c.before, report)) return false;
        c.existed = fs::exists(path);
        if ((remove && !c.existed) || (!remove && c.existed && c.before == after)) return true;
        changes.push_back(std::move(c)); return true;
    }
    bool commit(const std::wstring& gameDir) {
        if (changes.empty()) { report->add(Status::Ok, "Already up to date", "No files needed changing."); return true; }
        DWORD err = 0;
        const auto folder = fs::join(gameDir, L"bvr-launcher-backups");
        report->backupDir = fs::join(folder, fs::timestamp_local() + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!safe_path(fs::join(report->backupDir, L"journal.json"), report) || !fs::make_dirs(report->backupDir, &err)) {
            if (report->ok) report->fail("Backup folder could not be created", fs::narrow(report->backupDir), err);
            return false;
        }
        Json::Value journal; journal["schema"] = 1; journal["state"] = "prepared"; journal["utc"] = fs::utc_now_iso();
        for (size_t i = 0; i < changes.size(); ++i) {
            const auto& c = changes[i];
            Json::Value entry; entry["path"] = fs::narrow(c.path); entry["existed"] = c.existed; entry["remove"] = c.remove;
            entry["beforeSha256"] = c.existed ? hash(c.before) : "";
            entry["afterSha256"] = c.remove ? "" : hash(c.after);
            entry["backup"] = std::to_string(i) + "-before.bin";
            const auto backup = fs::join(report->backupDir, fs::widen(entry["backup"].asString()));
            if (c.existed && !fs::write_file_atomic(backup, c.before.data(), c.before.size(), &err)) {
                report->fail("Backup failed", fs::narrow(c.path), err); return false;
            }
            // Settings evidence includes the entire before and after file,
            // not merely the keys the operation intended to change.
            if (!c.remove && fs::iequals(c.path.substr(c.path.find_last_of(L'.')), L".ini")) {
                const auto after = fs::join(report->backupDir, std::to_wstring(i) + L"-after.ini");
                if (!fs::write_file_atomic(after, c.after.data(), c.after.size(), &err)) {
                    report->fail("Settings audit failed", fs::narrow(after), err); return false;
                }
            }
            journal["files"].append(entry);
        }
        const auto journalPath = fs::join(report->backupDir, L"journal.json");
        auto j = json(journal);
        if (!fs::write_file_atomic(journalPath, j.data(), j.size(), &err)) { report->fail("Backup journal failed", "Nothing was installed.", err); return false; }
        size_t done = 0;
        for (const auto& c : changes) {
            // Revalidate bytes before each mutation. Another launcher, F10 or
            // a file sync changing the source invalidates this transaction.
            Bytes current;
            if (!safe_path(c.path, report) || fs::exists(c.path) != c.existed ||
                (c.existed && (!fs::read_file(c.path, &current, &err) || current != c.before))) {
                if (report->ok) report->fail("A file changed during the operation", fs::narrow(c.path), err);
                break;
            }
            if (g_failAfter >= 0 && static_cast<int>(done) == g_failAfter) {
                report->fail("Injected host-test failure", "Verifying transaction rollback.", ERROR_WRITE_FAULT); break;
            }
            if (process::is_running(kGameExe) != process::Running::No) {
                report->fail("The game started during the operation", "Close BioShock Remastered and retry."); break;
            }
            if (!fs::make_dirs(fs::parent(c.path), &err) ||
                !(c.remove ? fs::delete_file(c.path, &err) : fs::write_file_atomic(c.path, c.after.data(), c.after.size(), &err))) {
                report->fail("File update failed", fs::narrow(c.path), err); break;
            }
            ++done;
            Bytes check;
            if (c.remove ? fs::exists(c.path) : (!fs::read_file(c.path, &check, &err) || check != c.after)) {
                report->fail("File verification failed", fs::narrow(c.path), err); break;
            }
        }
        if (done != changes.size() || !report->ok) {
            bool rollback = true;
            for (size_t i = done; i > 0; --i) {
                const auto& c = changes[i - 1];
                DWORD ignored = 0;
                bool restored = c.existed ? fs::write_file_atomic(c.path, c.before.data(), c.before.size(), &ignored) : fs::delete_file(c.path, &ignored);
                Bytes check;
                restored = restored && (c.existed ? fs::read_file(c.path, &check, &ignored) && check == c.before : !fs::exists(c.path));
                if (!restored) { rollback = false; report->add(Status::Failed, "Restore required", fs::narrow(c.path)); }
            }
            journal["state"] = rollback ? "rolled back" : "restore required";
            report->add(rollback ? Status::Warning : Status::Failed, rollback ? "Previous files restored" : "Recovery files retained", fs::narrow(report->backupDir));
        } else {
            journal["state"] = "complete";
            report->add(Status::Ok, "Files verified", std::to_string(changes.size()) + " file changes checked after writing.");
            report->add(Status::Note, "Recovery copy", fs::narrow(report->backupDir));
        }
        j = json(journal);
        if (!fs::write_file_atomic(journalPath, j.data(), j.size(), &err)) report->add(Status::Warning, "Could not finalize the journal", fs::narrow(journalPath));
        return report->ok;
    }
};
bool settings_plan(Transaction& tx, const Environment& e, const Choices& c) {
    auto& r = *tx.report;
    if (c.width) {
        GameIni ini; std::wstring why; std::string value;
        if (!safe_path(e.gameIni, &r) || !GameIni::load(e.gameIni, &ini, &why)) {
            if (r.ok) r.fail("Game settings unavailable", "Run the game once, close it, then apply a resolution. " + fs::narrow(why)); return false;
        }
        const char* keys[] = {"WindowedViewportX", "WindowedViewportY", "FullscreenViewportX", "FullscreenViewportY"};
        for (const auto* key : keys) if (!ini.get_unique("WinDrv.WindowsClient", key, &value)) {
            r.fail("Game settings do not match", "A viewport key is missing or duplicated: " + std::string(key)); return false;
        }
        for (int i = 0; i < 4; ++i) ini.set("WinDrv.WindowsClient", keys[i], std::to_string(i % 2 ? c.height : c.width), nullptr);
        if (!tx.plan(e.gameIni, ini.serialise())) return false;
        r.add(Status::Note, "Next launch resolution", std::to_string(c.width) + " x " + std::to_string(c.height) + ". Both viewport pairs are updated.");
    }
    const struct { std::wstring path; const char* section; const char* key; std::string value; } edits[] = {
        {fs::join(e.dataDir, L"xr.ini"), "runtime", "mode", c.runtime},
        {fs::join(e.dataDir, L"launcher.ini"), "Headset", "Model", c.headset}
    };
    for (const auto& edit : edits) if (!edit.value.empty()) {
        Bytes before; if (!read_optional(edit.path, &before, &r)) return false;
        GameIni ini; ini.parse(before); ini.ensure_section(edit.section);
        ini.set(edit.section, edit.key, edit.value, nullptr);
        std::string verified;
        if (!ini.get_unique(edit.section, edit.key, &verified) || verified != edit.value) {
            r.fail("Settings file is ambiguous", "A duplicated section or key prevents a reliable edit: " + fs::narrow(edit.path)); return false;
        }
        if (!tx.plan(edit.path, ini.serialise())) return false;
    }
    if (!c.preferences.empty()) {
        const auto path = fs::join(e.dataDir, L"menu-settings.ini");
        Bytes before; if (!read_optional(path, &before, &r)) return false;
        std::string after, why;
        if (!patch_preferences(std::string(before.begin(), before.end()), c.preferences, &after, &why)) {
            r.fail("F10 preferences could not be prepared", why); return false;
        }
        if (!tx.plan(path, bytes(after))) return false;
    }
    return true;
}
bool valid_record(const Json::Value& record) {
    if (!record.isObject() || !record["schema"].isInt() || record["schema"].asInt() != 1 || !record["files"].isObject() ||
        !record["product"].isString() || record["product"].asString() != "BioShockRemasteredVR") return false;
    for (const auto* f : kFiles) {
        const auto& entry = record["files"][fs::narrow(f)];
        if (!entry.isObject() || !entry["sha256"].isString() || entry["sha256"].asString().size() != 64 ||
            !entry["original"].isBool() || !entry["originalSha256"].isString()) return false;
        if (entry["original"].asBool() && entry["originalSha256"].asString().size() != 64) return false;
    }
    return true;
}
bool owned(const Environment& env, const Json::Value& record, bool disabled, Report* r) {
    for (const auto* f : kFiles) {
        const auto name = disabled && std::wstring(f) == L"bioshockvr.dll" ? kDisabledMod : f;
        const auto path = fs::join(env.gameDir, name);
        if (!safe_path(path, r)) return false;
        std::string actual;
        if (!fs::sha256_file(path, &actual, nullptr) || actual != record["files"][fs::narrow(f)]["sha256"].asString()) {
            r->fail("Installed files have changed", "The launcher cannot claim this file: " + fs::narrow(name) + ". Use Reinstall to back it up and repair the installation."); return false;
        }
    }
    return true;
}
}
void Report::add(Status status, const std::string& title, const std::string& detail) {
    steps.push_back({status, title, detail}); if (status == Status::Failed) ok = false;
}
void Report::fail(const std::string& title, const std::string& detail, DWORD code) {
    error = code; add(Status::Failed, title, detail + (code ? " " + fs::narrow(fs::win_error_text(code)) : ""));
}
#ifdef BVR_LAUNCHER_TESTS
void fail_transaction_after(int count) { g_failAfter = count; }
#endif
Report apply(const Environment& requested, const Payload& payload, const std::string& op, const Choices& choices) {
    Report r; std::string why;
    const std::set<std::string> ops = {"install", "update", "settings", "disable", "enable", "uninstall", "defaults"};
    if (!ops.count(op) || !valid_choices(choices, &why)) { r.fail("Invalid operation", why.empty() ? op : why); return r; }
    auto env = resolve_environment(requested);
    if (!pe32_file(fs::join(env.gameDir, kGameExe))) { r.fail("BioShock Remastered was not found", "Select the folder containing the 32 bit BioshockHD.exe."); return r; }
    if (process::is_running(kGameExe) != process::Running::No) { r.fail("Close BioShock Remastered first", "No files were changed. Try again after the game exits."); return r; }
    // Serialize every operation, including across two launcher versions.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\BioShockVRLauncherMutation");
    const auto acquired = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
    if (!mutex || (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)) {
        if (mutex) CloseHandle(mutex); r.fail("Another launcher is working", "Wait for its operation to finish."); return r;
    }
    struct Lock { HANDLE h; ~Lock() { ReleaseMutex(h); CloseHandle(h); } } lock{mutex};
    const auto recordPath = fs::join(env.gameDir, kRecord);
    Json::Value record; bool managed = false;
    if (fs::exists(recordPath)) {
        if (!safe_path(recordPath, &r) || !load_json(recordPath, &record) || !valid_record(record)) {
            if (r.ok) r.fail("Installation record is invalid", "Keep bioshockvr-install.json and the backup folder for recovery."); return r;
        }
        managed = true;
    }
    const bool modExists = fs::exists(fs::join(env.gameDir, L"bioshockvr.dll"));
    const bool disabledExists = fs::exists(fs::join(env.gameDir, kDisabledMod));
    if (modExists && disabledExists) { r.fail("Two mod copies were found", "Both enabled and disabled copies exist. Keep them for recovery and resolve the conflict first."); return r; }
    const bool disabled = !modExists && disabledExists;
    Transaction tx(&r);
    if (op == "install" || op == "update") {
        if (!payload.valid || payload.files.size() != 4) { r.fail("This launcher cannot install", payload.error); return r; }
        const auto det = detect(env, payload);
        if (det.oldModConflict) { r.fail("Old loader conflict", det.error); return r; }
        if (disabled && !managed) { r.fail("Unmanaged disabled installation", "Keep the disabled DLL and use the launcher that created it to restore it first."); return r; }
        if (!managed) { record["schema"] = 1; record["product"] = "BioShockRemasteredVR"; }
        // Immutable originals are distinct from per-operation recovery snapshots.
        for (const auto& f : payload.files) {
            if (std::find_if(std::begin(kFiles), std::end(kFiles), [&](auto n) { return f.name == n; }) == std::end(kFiles) || hash(f.bytes) != f.hash) {
                r.fail("Invalid embedded payload", "A payload name or checksum did not match."); return r;
            }
            const auto path = fs::join(env.gameDir, disabled && f.name == L"bioshockvr.dll" ? kDisabledMod : f.name);
            if (!managed) {
                Bytes old; if (!read_optional(path, &old, &r)) return r;
                const bool exists = fs::exists(path); auto& entry = record["files"][fs::narrow(f.name)];
                entry["original"] = exists; entry["originalSha256"] = exists ? hash(old) : "";
                const auto original = fs::join(env.gameDir, f.name + L".bvr-original");
                if (fs::exists(original)) { r.fail("An original backup already exists", "Keep " + fs::narrow(original) + " for recovery. It will not be overwritten."); return r; }
                if (exists && !tx.plan(original, old)) return r;
            }
            record["files"][fs::narrow(f.name)]["sha256"] = f.hash;
            if (!tx.plan(path, f.bytes)) return r;
        }
        record["version"] = payload.version; record["build"] = payload.build;
        record["configuration"] = payload.configuration; record["utc"] = fs::utc_now_iso(); record["disabled"] = disabled;
        if (!settings_plan(tx, env, choices) || !tx.plan(recordPath, bytes(json(record)))) return r;
        r.add(Status::Note, "Personal settings retained", "Existing calibration, weapon profiles and F10 preferences are kept. Only explicit edits are applied.");
    } else if (op == "settings") {
        if (!settings_plan(tx, env, choices)) return r;
    } else if (op == "defaults") {
        // An explicit, confirmed recovery action. Runtime/headset and unknown
        // files remain intact; all replaced files appear in the backup journal.
        const struct { const wchar_t* name; int id; } defaults[] = {
            {L"vrpreset.ini", IDR_VRPRESET}, {L"hands.ini", IDR_HANDS}, {L"weapons.ini", IDR_WEAPONS}};
        for (const auto& f : defaults) {
            auto b = resource_bytes(f.id);
            if (b.empty()) { r.fail("Default preset missing", fs::narrow(f.name)); return r; }
            if (!tx.plan(fs::join(env.dataDir, f.name), b)) return r;
        }
        if (!tx.plan(fs::join(env.dataDir, L"menu-settings.ini"), {}, true)) return r;
        r.add(Status::Note, "Bundled calibration", "The three bundled presets replace their previous copies. F10 overrides are cleared and backed up.");
    } else {
        if (!managed) { r.fail("This install is not managed yet", "Install this build from the launcher first. Existing files will be backed up."); return r; }
        if (!owned(env, record, disabled, &r)) return r;
        if (op == "enable" || op == "disable") {
            const bool wantDisabled = op == "disable";
            if (wantDisabled == disabled) { r.add(Status::Ok, disabled ? "VR is already disabled" : "VR is already enabled"); return r; }
            const auto from = fs::join(env.gameDir, disabled ? kDisabledMod : L"bioshockvr.dll");
            const auto to = fs::join(env.gameDir, wantDisabled ? kDisabledMod : L"bioshockvr.dll");
            Bytes b; if (!read_optional(from, &b, &r)) return r;
            if (!tx.plan(to, b) || !tx.plan(from, {}, true)) return r;
            record["disabled"] = wantDisabled;
            if (!tx.plan(recordPath, bytes(json(record)))) return r;
        } else if (op == "uninstall") {
            for (const auto* f : kFiles) {
                const auto& entry = record["files"][fs::narrow(f)];
                const auto target = fs::join(env.gameDir, f);
                if (entry["original"].asBool()) {
                    Bytes original; const auto source = fs::join(env.gameDir, std::wstring(f) + L".bvr-original");
                    if (!read_optional(source, &original, &r)) return r;
                    if (hash(original) != entry["originalSha256"].asString()) { r.fail("Original backup changed", fs::narrow(source)); return r; }
                    if (!tx.plan(target, original) || !tx.plan(source, {}, true)) return r;
                } else if (!tx.plan(target, {}, true)) return r;
            }
            if (disabled && !tx.plan(fs::join(env.gameDir, kDisabledMod), {}, true)) return r;
            if (!tx.plan(recordPath, {}, true)) return r;
            r.add(Status::Note, "Original files restored", "Your settings, saves, logs and recovery files are kept. Any mod installed before this launcher is restored too.");
        }
    }
    tx.commit(env.gameDir);
    if (r.ok) r.add(Status::Ok, "Operation complete", "Changes apply when you next launch the game.");
    return r;
}
std::string report_json(const Report& r) {
    Json::Value root; root["ok"] = r.ok; root["error"] = static_cast<Json::UInt>(r.error);
    root["backupDir"] = fs::narrow(r.backupDir); root["steps"] = Json::arrayValue;
    for (const auto& s : r.steps) { Json::Value row; row["status"] = static_cast<int>(s.status); row["title"] = s.title; row["detail"] = s.detail; root["steps"].append(row); }
    return json(root);
}
bool parse_report(const std::string& text, Report* r) {
    Json::CharReaderBuilder b; std::unique_ptr<Json::CharReader> reader(b.newCharReader()); Json::Value root; std::string why;
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &why) || !root.isObject() || !root["ok"].isBool() || !root["steps"].isArray()) return false;
    Report parsed; parsed.ok = root["ok"].asBool();
    if (root["error"].isUInt()) parsed.error = root["error"].asUInt();
    if (root["backupDir"].isString()) parsed.backupDir = fs::widen(root["backupDir"].asString());
    for (const auto& s : root["steps"]) {
        if (!s["status"].isInt() || s["status"].asInt() < 0 || s["status"].asInt() > 3 || !s["title"].isString() || !s["detail"].isString()) return false;
        parsed.steps.push_back({static_cast<Status>(s["status"].asInt()), s["title"].asString(), s["detail"].asString()});
    }
    *r = std::move(parsed); return true;
}
std::string detection_json(const Detection& d, const Payload& p) {
    Json::Value root; root["found"] = d.found; root["installed"] = d.installed; root["managed"] = d.managed;
    root["disabled"] = d.disabled; root["matchesPayload"] = d.matches; root["payloadOk"] = p.valid;
    root["version"] = p.version; root["build"] = p.build; root["configuration"] = p.configuration;
    root["gameDir"] = fs::narrow(d.env.gameDir); root["dataDir"] = fs::narrow(d.env.dataDir); root["gameIni"] = fs::narrow(d.env.gameIni);
    root["runtime"] = d.runtime; root["activeRuntime"] = d.activeRuntime; root["headset"] = d.headset;
    root["installedSha256"] = d.installedHash; root["gameRunning"] = static_cast<int>(d.running);
    root["width"] = d.width; root["height"] = d.height; root["error"] = d.error;
    for (const auto& f : p.files) root["payload"][fs::narrow(f.name)] = f.hash;
    return json(root);
}
}
