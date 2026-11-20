// pk2tool - inspect and extract PK2 archives.
//
//   pk2tool info     <archive.pk2>
//   pk2tool list     <archive.pk2> [substring]
//   pk2tool extract  <archive.pk2> <path-in-archive> <output-file>
//   pk2tool manifest <archive.pk2> <output.tsv>   crc32, size and path of every file, sorted by path
//   pk2tool mount    <game-folder>                mount all five archives and report timings
//   pk2tool efp      <Particles.pk2> <path>       an effect's node tree: controllers, programs, flags, resource
//   pk2tool efp-census <game-folder> [out.tsv]    every effect the skills' presentations name (skilleffect.txt), decoded:
//                                                each feature (life, view, render, controller, program command, node flag)
//                                                with the effects and the skills that use it, and the files not decoded
//   pk2tool ban-pose <Data.pk2> <a.ban> [b.ban]  a clip's keys: how far each is from its first key (and b's first), degrees

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/Animation.h"
#include "sr/assets/Effect.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/Pk2Archive.h"
#include "sr/assets/SkillAnimation.h"
#include "sr/core/Crc32.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int Usage() {
    std::fputs("usage:\n"
               "  pk2tool info     <archive.pk2>\n"
               "  pk2tool list     <archive.pk2> [substring]\n"
               "  pk2tool extract  <archive.pk2> <path-in-archive> <output-file>\n"
               "  pk2tool manifest <archive.pk2> <output.tsv>\n"
               "  pk2tool mount    <game-folder>\n"
               "  pk2tool efp      <Particles.pk2> <path>\n"
               "  pk2tool efp-census <game-folder> [out.tsv]\n"
               "  pk2tool ban-pose <Data.pk2> <a.ban> [b.ban]\n",
               stderr);
    return 2;
}

bool OpenArchive(sr::assets::Pk2Archive& archive, const char* path, double* ms = nullptr) {
    std::string error;
    const auto start = Clock::now();
    if (!archive.Open(path, error)) {
        std::fprintf(stderr, "error: %s: %s\n", path, error.c_str());
        return false;
    }
    if (ms != nullptr) {
        *ms = MsSince(start);
    }
    return true;
}

int Info(const char* path) {
    sr::assets::Pk2Archive archive;
    double ms = 0;
    if (!OpenArchive(archive, path, &ms)) {
        return 1;
    }
    std::uint64_t total = 0;
    for (const auto& f : archive.Files()) {
        total += f.size;
    }
    std::printf("archive        %s\n", path);
    std::printf("files          %zu\n", archive.Files().size());
    std::printf("folders        %zu\n", archive.FolderCount());
    std::printf("payload bytes  %llu\n", static_cast<unsigned long long>(total));
    std::printf("key verified   %s\n", archive.KeyVerifiedByHeader() ? "yes (header)" : "no (header mismatch, blocks decoded)");
    std::printf("open+index     %.1f ms\n", ms);
    return 0;
}

int List(const char* path, std::string_view filter) {
    sr::assets::Pk2Archive archive;
    if (!OpenArchive(archive, path)) {
        return 1;
    }
    const std::string needle = sr::core::NormalizeAssetPath(filter);
    for (const auto& f : archive.Files()) {
        if (needle.empty() || sr::core::NormalizeAssetPath(f.path).find(needle) != std::string::npos) {
            std::printf("%12u  %s\n", f.size, f.path.c_str());
        }
    }
    return 0;
}

int Extract(const char* path, const char* inner, const char* out) {
    sr::assets::Pk2Archive archive;
    if (!OpenArchive(archive, path)) {
        return 1;
    }
    const auto* file = archive.Find(inner);
    if (file == nullptr) {
        std::fprintf(stderr, "error: not found: %s\n", inner);
        return 1;
    }
    const auto bytes = archive.Read(*file);
    std::ofstream os(out, std::ios::binary);
    os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!os) {
        std::fprintf(stderr, "error: could not write %s\n", out);
        return 1;
    }
    std::printf("wrote %zu bytes -> %s\n", bytes.size(), out);
    return 0;
}

int Manifest(const char* path, const char* out) {
    sr::assets::Pk2Archive archive;
    if (!OpenArchive(archive, path)) {
        return 1;
    }
    std::vector<const sr::assets::Pk2Archive::File*> files;
    files.reserve(archive.Files().size());
    for (const auto& f : archive.Files()) {
        files.push_back(&f);
    }
    std::sort(files.begin(), files.end(), [](auto* a, auto* b) { return a->path < b->path; });

    const auto start = Clock::now();
    std::uint64_t total = 0;
    std::ofstream os(out, std::ios::binary);
    char line[64];
    for (const auto* f : files) {
        const auto bytes = archive.Read(*f);
        total += bytes.size();
        std::snprintf(line, sizeof(line), "%08x\t%u\t", sr::core::Crc32(bytes), f->size);
        os << line << f->path << '\n';
    }
    if (!os) {
        std::fprintf(stderr, "error: could not write %s\n", out);
        return 1;
    }
    std::printf("%zu files, %llu bytes hashed in %.0f ms -> %s\n", files.size(),
                static_cast<unsigned long long>(total), MsSince(start), out);
    return 0;
}

int Mount(const char* gameDir) {
    sr::assets::FileSystem fs;
    std::string error;
    const auto start = Clock::now();
    if (!sr::assets::MountGameArchives(fs, gameDir, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const double ms = MsSince(start);
    std::size_t files = 0;
    for (const auto& source : fs.Sources()) {
        const auto& archive = static_cast<const sr::assets::Pk2Source&>(*source).Archive();
        std::printf("%-14s %6zu files %5zu folders\n", std::string(source->Name()).c_str(), archive.Files().size(),
                    archive.FolderCount());
        files += archive.Files().size();
    }
    std::printf("total          %6zu files, mounted in %.1f ms\n", files, ms);
    return 0;
}

void PrintCommand(const std::string& indent, const sr::assets::EffectCommand& c) {
    std::printf("%s%s flags %u schedule %u start %g spacing %g end %g", indent.c_str(), c.name.c_str(), c.flags, c.schedule, c.start,
                c.spacing, c.end);
    if (c.rows > 0) std::printf(" rows %d", c.rows);
    const std::size_t shown = std::getenv("SR_EFP_FULL") != nullptr ? c.values.size() : std::min<std::size_t>(c.values.size(), 24);
    for (std::size_t i = 0; i < shown; ++i) std::printf(i == 0 ? " [%g" : " %g", c.values[i]);
    if (shown > 0) std::printf(c.values.size() > shown ? " ...]" : "]");
    if (!c.colors.empty()) {
        const auto& a = c.colors.front();
        const auto& b = c.colors.back();
        std::printf(" colours %zu (%u,%u,%u,%u .. %u,%u,%u,%u)", c.colors.size(), a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    }
    std::printf("\n");
}

void PrintNode(const sr::assets::EffectNode& n, int depth) {
    const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
    std::printf("%snode '%s' frames %d life %s view %s render %s\n", pad.c_str(), n.name.c_str(), n.frames, n.life.name.c_str(),
                n.view.name.c_str(), n.render.name.c_str());
    std::printf("%s  keepMatrix %d keepOrigin %d positionDepth %d matrixDepth %d velocityDepth %d localMotion %d followDepth %d "
                "shapeMotion %d\n",
                pad.c_str(), n.keepMatrix, n.keepOrigin, n.positionDepth, n.matrixDepth, n.velocityDepth, n.localMotion, n.followDepth,
                n.shapeMotion);
    const auto& r = n.resource;
    std::printf("%s  resource backFace %u blend %u/%u stages %u,%u,%u %u,%u,%u", pad.c_str(), r.backFace, r.srcBlend, r.dstBlend,
                r.stages[0], r.stages[1], r.stages[2], r.stages[3], r.stages[4], r.stages[5]);
    for (const auto& m : r.meshes) {
        std::printf(" mesh '%s'", m.path.c_str());
        for (const auto& t : m.textures) std::printf(" texture '%s'", t.c_str());
    }
    std::printf("\n");
    const auto program = [&](const char* name, const std::vector<sr::assets::EffectCommand>& list) {
        if (list.empty()) return;
        std::printf("%s  %s:\n", pad.c_str(), name);
        for (const auto& c : list) PrintCommand(pad + "    ", c);
    };
    program("controllers", n.controllers);
    program("pre", n.preProgram);
    program("emitter", n.emitterProgram);
    program("post-emitter", n.postEmitterProgram);
    program("update", n.updateProgram);
    program("trailing", n.trailingProgram);
    program("render", n.renderProgram);
    for (const auto& child : n.children) PrintNode(child, depth + 1);
}

int Efp(const char* path, const char* inner) {
    sr::assets::Pk2Archive archive;
    if (!OpenArchive(archive, path)) {
        return 1;
    }
    const auto* file = archive.Find(inner);
    if (file == nullptr) {
        std::fprintf(stderr, "error: not found: %s\n", inner);
        return 1;
    }
    sr::assets::EffectData data;
    std::string error;
    if (!sr::assets::DecodeEfp(archive.Read(*file), data, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s: version %d scale %g\n", inner, data.version, data.scale);
    PrintNode(data.root, 0);
    return 0;
}

// The features of a node and its children: "life:X", "view:X", "render:X", "controller:X", "<program>:X", "flag:X".
void Features(const sr::assets::EffectNode& n, std::set<std::string>& out) {
    if (!n.life.name.empty()) out.insert("life:" + n.life.name);
    if (!n.view.name.empty()) out.insert("view:" + n.view.name);
    if (!n.render.name.empty()) out.insert("render:" + n.render.name);
    for (const auto& c : n.controllers) out.insert("controller:" + c.name);
    const auto program = [&](const char* name, const std::vector<sr::assets::EffectCommand>& list) {
        for (const auto& c : list) {
            if (!c.name.empty()) out.insert(std::string(name) + ":" + c.name);
        }
    };
    program("pre", n.preProgram);
    program("emitter", n.emitterProgram);
    program("post-emitter", n.postEmitterProgram);
    program("update", n.updateProgram);
    program("trailing", n.trailingProgram);
    program("render", n.renderProgram);
    if (n.keepMatrix) out.insert("flag:keepMatrix");
    if (n.keepOrigin) out.insert("flag:keepOrigin");
    if (n.localMotion) out.insert("flag:localMotion");
    if (n.shapeMotion) out.insert("flag:shapeMotion");
    if (n.positionDepth > 0) out.insert("flag:positionDepth");
    if (n.matrixDepth > 0) out.insert("flag:matrixDepth");
    if (n.velocityDepth > 0) out.insert("flag:velocityDepth");
    if (n.followDepth > 0) out.insert("flag:followDepth");
    for (const auto& m : n.resource.meshes) {
        if (m.path.ends_with(".bms") || m.path.ends_with(".BMS")) out.insert("resource:mesh");
    }
    if (!n.scaleGraphs.empty()) out.insert("parameter:BlendScaleGraph");
    for (const auto& child : n.children) Features(child, out);
}

int EfpCensus(const char* gameDir, const char* outPath) {
    sr::assets::FileSystem fs;
    std::string error;
    if (!sr::assets::MountGameArchives(fs, gameDir, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    // Every effect a presentation names, with the presentations (skilleffect rows and stage lists) naming it.
    std::map<std::string, std::set<std::string>> users;
    const auto add = [&](const std::string& path, const std::string& who) {
        if (path.empty()) return;
        const std::string p = sr::core::NormalizeAssetPath(path);
        if (p.ends_with(".efp")) users[p].insert(who);
    };
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\skilleffect.txt");
    std::unordered_map<std::string, sr::assets::SkillAnimation> rows;
    std::unordered_map<std::string, std::vector<sr::assets::SkillEffectStage>> lists;
    if (!file) {
        std::fprintf(stderr, "error: skilleffect.txt not found\n");
        return 1;
    }
    if (!sr::assets::DecodeSkillAnimations(sr::core::DecodeTextFile(file->Bytes()), rows, error, nullptr, &lists)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    for (const auto& [name, row] : rows) {
        for (const std::string& e : {row.defenseEffect, row.damageEffect, row.criticalEffect, row.arrowTrail, row.arrowForce, row.objectEffect}) add(e, name);
        for (const auto& s : row.stages) {
            add(s.object, name);
            add(s.arrival, name);
        }
    }
    for (const auto& [name, stages] : lists) {
        for (const auto& s : stages) {
            add(s.object, name);
            add(s.arrival, name);
        }
    }
    std::map<std::string, std::set<std::string>> featureEffects, featureUsers;
    std::vector<std::string> undecoded, missing;
    for (const auto& [path, who] : users) {
        const auto f = fs.Read(path);
        if (!f) {
            missing.push_back(path);
            continue;
        }
        sr::assets::EffectData data;
        std::string why;
        if (!sr::assets::DecodeEfp(f->Bytes(), data, why)) {
            undecoded.push_back(path + " (" + why + ")");
            for (const auto& u : who) featureUsers["undecoded"].insert(u);
            featureEffects["undecoded"].insert(path);
            continue;
        }
        std::set<std::string> features;
        Features(data.root, features);
        for (const auto& feature : features) {
            featureEffects[feature].insert(path);
            for (const auto& u : who) featureUsers[feature].insert(u);
        }
    }
    std::printf("%zu effects named by %zu presentations; %zu not in the files, %zu not decoded\n", users.size(), rows.size() + lists.size(),
                missing.size(), undecoded.size());
    std::printf("%-40s %8s %8s\n", "feature", "effects", "users");
    for (const auto& [feature, effects] : featureEffects) {
        std::printf("%-40s %8zu %8zu\n", feature.c_str(), effects.size(), featureUsers[feature].size());
    }
    for (const auto& u : undecoded) std::printf("not decoded: %s\n", u.c_str());
    for (const auto& m : missing) std::printf("missing: %s\n", m.c_str());
    if (outPath != nullptr) {
        std::ofstream out(outPath);
        out << "feature\teffects\tusers\teffect list\tuser list\n";
        for (const auto& [feature, effects] : featureEffects) {
            out << feature << '\t' << effects.size() << '\t' << featureUsers[feature].size() << '\t';
            bool first = true;
            for (const auto& e : effects) out << (std::exchange(first, false) ? "" : ",") << e;
            out << '\t';
            first = true;
            for (const auto& u : featureUsers[feature]) out << (std::exchange(first, false) ? "" : ",") << u;
            out << '\n';
        }
    }
    return 0;
}

// A clip's pose over its keys: each key's mean turn (degrees, over the tracks) away from the clip's first key and, given a
// second clip, from that one's first key (is the clip's end its start, or a stand?).
int BanPose(const char* archivePath, const char* a, const char* b) {
    sr::assets::Pk2Archive archive;
    if (!OpenArchive(archive, archivePath)) return 1;
    const auto load = [&](const char* inner, sr::assets::AnimationData& out) {
        const auto* file = archive.Find(inner);
        std::string error;
        if (file == nullptr || !sr::assets::DecodeBan(archive.Read(*file), out, error)) {
            std::fprintf(stderr, "error: %s: %s\n", inner, file == nullptr ? "not found" : error.c_str());
            return false;
        }
        return true;
    };
    sr::assets::AnimationData clip, other;
    if (!load(a, clip) || (b != nullptr && !load(b, other))) return 1;
    const auto angle = [](const std::array<float, 4>& p, const std::array<float, 4>& q) {
        const float d = std::fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3]);
        return 2.0f * std::acos(std::min(1.0f, d)) * 57.29578f;
    };
    std::printf("%s: %zu keys, %u ms, %zu tracks\n", a, clip.keyTimesMs.size(), clip.durationMs, clip.tracks.size());
    for (std::size_t k = 0; k < clip.keyTimesMs.size(); ++k) {
        float fromFirst = 0, fromOther = 0;
        int n = 0, m = 0;
        for (const auto& track : clip.tracks) {
            if (k >= track.keys.size() || track.keys.empty()) continue;
            fromFirst += angle(track.keys[k].rotation, track.keys.front().rotation);
            ++n;
            if (b != nullptr) {
                for (const auto& o : other.tracks) {
                    if (o.boneName == track.boneName && !o.keys.empty()) {
                        fromOther += angle(track.keys[k].rotation, o.keys.front().rotation);
                        ++m;
                    }
                }
            }
        }
        std::printf("  %5u ms  from its first key %6.1f deg", clip.keyTimesMs[k], n ? fromFirst / n : 0.0f);
        if (b != nullptr) std::printf("  from %s %6.1f deg", "the other's first", m ? fromOther / m : 0.0f);
        // The hips' height (the pelvis track's place, a crouch lowers it).
        for (const auto& track : clip.tracks) {
            if (track.boneName.find("Pelvis") == std::string::npos || k >= track.keys.size()) continue;
            std::printf("  pelvis y %6.2f", track.keys[k].translation[1]);
            break;
        }
        std::printf("\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return Usage();
    }
    const std::string_view command = argv[1];
    if (command == "info") {
        return Info(argv[2]);
    }
    if (command == "list") {
        return List(argv[2], argc > 3 ? argv[3] : "");
    }
    if (command == "extract" && argc == 5) {
        return Extract(argv[2], argv[3], argv[4]);
    }
    if (command == "manifest" && argc == 4) {
        return Manifest(argv[2], argv[3]);
    }
    if (command == "efp" && argc == 4) {
        return Efp(argv[2], argv[3]);
    }
    if (command == "mount") {
        return Mount(argv[2]);
    }
    if (command == "ban-pose" && argc >= 4) {
        return BanPose(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
    }
    if (command == "efp-census") {
        return EfpCensus(argv[2], argc > 3 ? argv[3] : nullptr);
    }
    return Usage();
}
