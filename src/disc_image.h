#pragma once
// The discs the port can boot. The code is compiled ahead of time from the USA
// executable, so a disc only works when its executable differs from that one in
// the game's text, as the PT-BR translation does.
#include <filesystem>
#include <string>
#include <vector>

bool orochiIsCompatibleDisc(const std::filesystem::path &disc);

// The compatible discs in `folder`, sorted by name.
std::vector<std::filesystem::path> orochiCompatibleDiscs(const std::filesystem::path &folder);

// Writes the disc's executable to `extractTo`, to be loaded from there. False,
// with `error` set, when the disc does not match the compiled code.
bool orochiExtractDiscElf(const std::filesystem::path &disc, const std::filesystem::path &extractTo,
                          std::string &error);
