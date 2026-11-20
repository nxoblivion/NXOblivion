// reOblivion web (2026-11-17): the game's files in the browser (web_game_files.cpp).
#pragma once

#include <filesystem>
#include <string>

namespace oblivion::web {

// Mounts the files the server lists at <page's folder>/<url_dir>.manifest as a read-only tree at mount_point, read
// over HTTP on demand. Returns mount_point, or an empty path when there is no manifest. Call from a worker (main()).
std::filesystem::path MountGameFiles(const char *mount_point = "/game", const char *url_dir = "game");

// The player's storage at mount_point: the browser's origin private file system (OPFS), which outlives the page, or
// memory (gone with the page) where the browser has none. Returns whether it is OPFS. Call from a worker (main()),
// before anything is written under mount_point.
bool MountSaveStorage(const char *mount_point = "/save");

// Requests, bytes and cache hits so far, for the log.
std::string GameFilesStats();

}  // namespace oblivion::web
