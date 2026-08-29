from __future__ import annotations

import shutil

from scripts.bunyan import BuildContext, cmake_build, cmake_configure, main, run_cmd


def build_haikal(ctx: BuildContext) -> None:
    haikal_build_dir = ctx.root_dir / "extern" / "haikal" / "build"
    haikal_build_dir.mkdir(parents=True, exist_ok=True)

    cmake_configure(
        source_dir=ctx.root_dir / "extern" / "haikal",
        build_dir=haikal_build_dir,
        generator=ctx.generator,
        c_compiler=ctx.c_compiler,
        build_type="Release",
    )
    cmake_build(haikal_build_dir)
    # haikal infers the project root from the compile database, so it needs the
    # path rather than just the entry file. command_config runs before this hook,
    # so compile_commands.json is already on disk.
    run_cmd(
        [
            haikal_build_dir / "haikal.exe",
            "--compile-db",
            ctx.build_dir / "compile_commands.json",
            "--meta",
            ctx.root_dir / "extern" / "haikal" / "src" / "meta_arena",
            "--entry",
            ctx.root_dir / "src" / "main.c",
        ],
        cwd=ctx.root_dir,
    )


def sync_std(ctx: BuildContext) -> None:
    """Copy src/std next to the freshly built compiler.

    CMake does this in a POST_BUILD step, which only fires when the target
    relinks -- so editing a .rin in src/std and rebuilding left build/std stale
    and the test suite silently running against the old library. Doing it here
    makes it unconditional.
    """
    build_std = ctx.build_dir / "std"
    if build_std.exists():
        shutil.rmtree(build_std)
    shutil.copytree(ctx.root_dir / "src" / "std", build_std)


def package_rin(ctx: BuildContext) -> None:
    package_dir = ctx.root_dir.parent / "rin-windows-x64"
    package_std_dir = package_dir / "std"
    rinbind_path = ctx.build_dir / "rinbind.exe"

    sync_std(ctx)

    package_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ctx.exe_path, package_dir / "rin.exe")
    if rinbind_path.exists():
        shutil.copy2(rinbind_path, package_dir / "rinbind.exe")

    if package_std_dir.exists():
        shutil.rmtree(package_std_dir)
    shutil.copytree(ctx.root_dir / "src" / "std", package_std_dir)

    print(f"package {ctx.exe_path} -> {package_dir / 'rin.exe'}")
    if rinbind_path.exists():
        print(f"package {rinbind_path} -> {package_dir / 'rinbind.exe'}")
    print(f"package {ctx.root_dir / 'src' / 'std'} -> {package_std_dir}")


if __name__ == "__main__":
    main(
        project_name="rin",
        hooks={
            "pre_build": build_haikal,
            "post_build": package_rin,
        },
        extra_clean_paths=(
            "extern/haikal/build",
        ),
    )
