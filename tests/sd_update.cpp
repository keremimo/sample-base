// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/sd_update.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

namespace fs = std::filesystem;
using sd_update::compare_versions;

static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static void in_order(std::initializer_list<const char*> versions) {
    const char* before = nullptr;
    for (const char* v : versions) {
        assert(compare_versions(v, v) == 0);
        if (before) {
            assert(compare_versions(before, v) < 0);
            assert(compare_versions(v, before) > 0);
        }
        before = v;
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    fs::path root = argv[1];

    // Releases, pre-releases and git describe versions in order.
    in_order({"dev", "v0.0.9", "v0.1.0-alpha", "v0.1.0-alpha.2", "v0.1.0-alpha.10", "v0.1.0-alpha.10-4-gabc1234",
              "v0.1.0-beta", "v0.1.0-rc.1", "v0.1.0", "v0.1.0-dirty", "v0.1.0-3-g1a2b3c4", "v0.1.0-12-g4d3de9a-dirty",
              "v0.1.1", "v0.2.0", "v1.0.0", "v10.0.0"});
    assert(compare_versions("0.2.0", "v0.2.0") == 0);
    assert(compare_versions("dev", "garbage") == 0);
    for (const char* bad : {"", "v", "v1", "v1.2", "v1.2.x", "v1.2.3.4", "v1.2.3-", "v1.2.3-a..b", "4d3de9a"})
        assert(compare_versions(bad, "v0.0.1") < 0);

    // Top-level zips: any case; no hidden files, folders, empty or oversized files.
    fs::path card = root / "card";
    write(card / "Melodee-MPC-Sample-v0.2.0.zip", "zip a");
    write(card / "melodee v0.3.0 (1).ZIP", "zip b");
    write(card / "._Melodee-MPC-Sample-v0.2.0.zip", "appledouble");
    write(card / "empty.zip", "");
    write(card / "huge.zip", std::string(4096, 'x'));
    write(card / "notes.txt", "x");
    write(card / "Samples/inside.zip", "x");
    fs::create_directories(card / "folder.zip");
    auto zips = sd_update::zips_at(card.string(), 1024);
    assert(zips.size() == 2);
    assert(zips[0] == (card / "Melodee-MPC-Sample-v0.2.0.zip").string());
    assert(zips[1] == (card / "melodee v0.3.0 (1).ZIP").string());
    assert(sd_update::zips_at((root / "none").string()).empty());

    // find(): uses the script's check; returns the newest version newer than the current one.
    fs::path script = root / "check.sh";
    write(script, "[ \"$1 $3 $4\" = \"check melodee " + (root / "key.pem").string() + "\" ] || exit 1\n"
                  "case \"$2\" in *v0.2.0.zip) echo v0.2.0 ;; *'(1).ZIP') echo 'v0.3.0' ;; *) exit 1 ;; esac\n");
    sd_update::Setup setup {script.string(), "melodee", "melodee", (root / "key.pem").string()};
    auto offer = sd_update::find(setup, card.string(), "v0.1.0");
    assert(offer.version == "v0.3.0" && offer.zip == zips[1] && offer.size == 5);
    offer = sd_update::find(setup, card.string(), "v0.1.0", {zips[1]});
    assert(offer.version == "v0.2.0" && offer.zip == zips[0]);
    assert(sd_update::find(setup, card.string(), "v0.3.0").empty());           // same version
    assert(sd_update::find(setup, card.string(), "v0.3.0-1-gabcdef0").empty()); // built after v0.3.0
    assert(sd_update::find(setup, card.string(), "dev").version == "v0.3.0");    // unversioned build
    setup.script = (root / "missing.sh").string();
    assert(sd_update::find(setup, card.string(), "v0.1.0").empty());
    setup.script = script.string();
    setup.package = "other";
    assert(sd_update::find(setup, card.string(), "v0.1.0").empty());

    // Status file.
    auto none = sd_update::read_status((root / "no-status").string());
    assert(none.state.empty() && !none.running());
    write(root / "status", "state=writing\nversion=v0.3.0\nerror=\n");
    auto status = sd_update::read_status((root / "status").string());
    assert(status.state == "writing" && status.version == "v0.3.0" && status.error.empty() && status.running());
    write(root / "status", "state=failed\nversion=v0.3.0\nerror=card-full\n");
    status = sd_update::read_status((root / "status").string());
    assert(status.state == "failed" && status.error == "card-full" && !status.running());
    return 0;
}
