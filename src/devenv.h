// devenv.h - development-only switches. The release build (no RUGRATS_DEV) ignores the
// test/trace environment variables and the test command-line options entirely.
#pragma once
#include <cstdlib>

#ifdef RUGRATS_DEV
constexpr bool kDevBuild = true;
inline const char* DevEnv(const char* name) { return std::getenv(name); }
#else
constexpr bool kDevBuild = false;
inline const char* DevEnv(const char*) { return nullptr; }
#endif
