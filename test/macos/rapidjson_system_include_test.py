#!/usr/bin/env python3
"""Keep vendor warnings isolated when Homebrew's prefix is an implicit include."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'darwin', 'AppleClang implicit-header regression')
class RapidJsonSystemIncludeTest(unittest.TestCase):
    def test_implicit_symlinked_headers_are_system_but_project_warnings_still_fail(self):
        with tempfile.TemporaryDirectory(prefix='vectorwarp-rapidjson-system-') as directory:
            work = Path(directory).resolve()
            actual = work / 'Cellar/rapidjson/1.1.0/include/rapidjson'
            actual.mkdir(parents=True)
            (actual / 'allocators.h').write_text('// find_path fixture\n')
            (actual / 'document.h').write_text('''#pragma once
namespace rapidjson {
[[deprecated("vendor header diagnostic")]] inline int legacy() { return 0; }
inline int value() { return legacy(); }
}
''')
            prefix = work / 'prefix/include'
            prefix.mkdir(parents=True)
            (prefix / 'rapidjson').symlink_to(actual, target_is_directory=True)
            (work / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(rapidjson_system_include LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
include("{(ROOT / 'cmake/RapidJson.cmake').as_posix()}")
add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE blah2RapidJson)
target_compile_options(probe PRIVATE -Wall -Werror -Wdeprecated-declarations)
''')
            main = work / 'main.cpp'
            main.write_text('#include <rapidjson/document.h>\nint main() { return rapidjson::value(); }\n')
            # Make CMake discover an implicit, non-system compiler search path,
            # stressing the same omitted SYSTEM flag as Intel /usr/local/include.
            # The injected -I has stricter search priority than that builtin.
            # CPATH alone does not reproduce the omission on all CMake versions.
            compiler = shutil.which('c++')
            self.assertIsNotNone(compiler, 'A C++ compiler is required')
            wrapper = work / 'cxx'
            wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(compiler) +
                               ' -I' + shlex.quote(str(prefix)) + ' "$@"\n')
            wrapper.chmod(0o755)
            environment = dict(os.environ)
            configured = subprocess.run(['cmake', '-S', str(work), '-B', str(work / 'build'),
                                         '-G', 'Ninja', f'-DRAPIDJSON_INCLUDE_DIRS={prefix}',
                                         f'-DCMAKE_CXX_COMPILER={wrapper}'],
                                        env=environment, text=True, capture_output=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            compiler_files = list((work / 'build/CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
            self.assertTrue(any(str(prefix) in p.read_text() for p in compiler_files),
                            'Fixture did not reproduce an implicit compiler include directory')
            built = subprocess.run(['cmake', '--build', str(work / 'build'), '--verbose'],
                                   env=environment, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            main.write_text('''#include <rapidjson/document.h>
[[deprecated("project diagnostic must fail")]] int old_project_api() { return 0; }
int main() { return old_project_api() + rapidjson::value(); }
''')
            rejected = subprocess.run(['cmake', '--build', str(work / 'build')],
                                      env=environment, text=True, capture_output=True)
            self.assertNotEqual(rejected.returncode, 0, 'Project warnings lost -Werror protection')
            self.assertIn('project diagnostic must fail', rejected.stdout + rejected.stderr)


if __name__ == '__main__':
    unittest.main()
