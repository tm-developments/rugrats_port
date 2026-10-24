// prepare.h - builds movies\ and sounds\ from the original .MHK archives (see prepare.cpp).
#pragma once
#include <functional>
#include <string>

namespace port {

// done/total archives so far and the one being read; return false to cancel
using PrepareProgress = std::function<bool(int done, int total, const std::string& archive)>;

// Reads every .MHK under dataDir (MHK\, PROGRAM\MHK\, ...) and writes outDir\movies\ and
// outDir\sounds\ (outDir empty: dataDir). True when movies and sounds were written.
bool PrepareAssets(const std::string& dataDir, const std::string& outDir, const PrepareProgress& progress = nullptr);

// outDir already holds a movie manifest and sounds
bool AssetsPrepared(const std::string& outDir);

} // namespace port
