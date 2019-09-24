# Generated file, do not modify by hand
# Generated by 'rbe_ubuntu_gcc_gen' rbe_autoconfig rule
"""Definitions to be used in rbe_repo attr of an rbe_autoconf rule  """
toolchain_config_spec0 = struct(config_repos = [], create_cc_configs = True, create_java_configs = False, env = {"BAZEL_COMPILER": "clang", "BAZEL_LINKLIBS": "-l%:libstdc++.a", "BAZEL_LINKOPTS": "-lm:-static-libgcc:-fuse-ld=lld", "BAZEL_USE_LLVM_NATIVE_COVERAGE": "1", "GCOV": "llvm-profdata", "CC": "clang", "CXX": "clang++", "PATH": "/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/llvm-8/bin"}, java_home = None, name = "clang")
toolchain_config_spec1 = struct(config_repos = [], create_cc_configs = True, create_java_configs = False, env = {"BAZEL_COMPILER": "clang", "BAZEL_LINKLIBS": "-l%:libc++.a:-l%:libc++abi.a", "BAZEL_LINKOPTS": "-lm:-static-libgcc:-pthread:-fuse-ld=lld", "BAZEL_USE_LLVM_NATIVE_COVERAGE": "1", "GCOV": "llvm-profdata", "CC": "clang", "CXX": "clang++", "PATH": "/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/llvm-8/bin", "BAZEL_CXXOPTS": "-stdlib=libc++", "CXXFLAGS": "-stdlib=libc++"}, java_home = None, name = "clang_libcxx")
toolchain_config_spec2 = struct(config_repos = [], create_cc_configs = True, create_java_configs = False, env = {"BAZEL_COMPILER": "gcc", "BAZEL_LINKLIBS": "-l%:libstdc++.a", "BAZEL_LINKOPTS": "-lm:-static-libgcc", "CC": "gcc", "CXX": "g++", "PATH": "/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/llvm-8/bin"}, java_home = None, name = "gcc")
_TOOLCHAIN_CONFIG_SPECS = [toolchain_config_spec0,toolchain_config_spec1,toolchain_config_spec2]
_BAZEL_TO_CONFIG_SPEC_NAMES = {"0.28.1": ["clang", "clang_libcxx", "gcc"]}
LATEST = "sha256:d1f6087fdeb6a6e5d4fd52a5dc06b15f43f49e2c20fc813bcaaa12333485a70b"
CONTAINER_TO_CONFIG_SPEC_NAMES = {"sha256:d1f6087fdeb6a6e5d4fd52a5dc06b15f43f49e2c20fc813bcaaa12333485a70b": ["clang", "clang_libcxx", "gcc"]}
_DEFAULT_TOOLCHAIN_CONFIG_SPEC = toolchain_config_spec0
TOOLCHAIN_CONFIG_AUTOGEN_SPEC = struct(
        bazel_to_config_spec_names_map = _BAZEL_TO_CONFIG_SPEC_NAMES,
        container_to_config_spec_names_map = CONTAINER_TO_CONFIG_SPEC_NAMES,
        default_toolchain_config_spec = _DEFAULT_TOOLCHAIN_CONFIG_SPEC,
        latest_container = LATEST,
        toolchain_config_specs = _TOOLCHAIN_CONFIG_SPECS,
    )