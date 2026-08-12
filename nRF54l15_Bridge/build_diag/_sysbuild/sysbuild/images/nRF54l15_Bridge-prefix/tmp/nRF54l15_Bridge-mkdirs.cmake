# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/romanenko/Cancordia/nRF54l15_Bridge")
  file(MAKE_DIRECTORY "/Users/romanenko/Cancordia/nRF54l15_Bridge")
endif()
file(MAKE_DIRECTORY
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/nRF54l15_Bridge"
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix"
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/tmp"
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/src/nRF54l15_Bridge-stamp"
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/src"
  "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/src/nRF54l15_Bridge-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/src/nRF54l15_Bridge-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/romanenko/Cancordia/nRF54l15_Bridge/build_diag/_sysbuild/sysbuild/images/nRF54l15_Bridge-prefix/src/nRF54l15_Bridge-stamp${cfgdir}") # cfgdir has leading slash
endif()
