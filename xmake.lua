add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("fmt 11.2.0")

target("FlyingPet")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/main.cpp")
    add_packages("preloader", "fmt")

    if is_plat("android") then
        add_cxflags("-fPIC", "-Oz", "-ffunction-sections", "-fdata-sections", "-flto", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fno-stack-protector", "-fexceptions", "-fvisibility=hidden")
        add_cxxflags("-fvisibility-inlines-hidden")
        add_shflags("-Wl,--gc-sections", "-Wl,--icf=all", "-flto", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384")
        add_links("android", "log", "dl")
    end

    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        assert(python, "Python 3 is required to package FlyingPet.levipack")
        os.vrunv(python.program, {
            path.join(os.projectdir(), "scripts", "package_levipack.py"),
            "--library", target:targetfile(),
            "--output", path.join(target:targetdir(), "FlyingPet.levipack")
        })
    end)
