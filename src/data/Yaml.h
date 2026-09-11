#pragma once
// vcpkg's older layout and distribution-provided rapidyaml are both supported.
#if __has_include(<ryml/ryml.hpp>)
#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp>
#else
#include <ryml.hpp>
#include <ryml_std.hpp>
#endif
