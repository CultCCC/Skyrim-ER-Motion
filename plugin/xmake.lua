set_xmakever("3.0.0")

-- SE 1.5.97 only (the SDS build); the library reads these when it is included
set_config("skyrim_se", true)
set_config("skyrim_ae", false)
set_config("skyrim_vr", false)

-- CommonLibSSE-NG (alandtse/CommonLibVR, branch ng): $COMMONLIB_PATH, else the extern/ submodule, else a sibling
-- _tools/commonlibsse-ng (the author's own layout)
local commonlib = os.getenv("COMMONLIB_PATH")
if not commonlib then
    commonlib = path.join(os.scriptdir(), "../extern/commonlibsse-ng")
    if not os.isfile(path.join(commonlib, "xmake.lua")) then
        commonlib = path.join(os.scriptdir(), "../../_tools/commonlibsse-ng")
    end
end
includes(commonlib)

set_project("SDSCombat")
set_version("1.0.0")
set_license("GPL-3.0")

set_languages("c++23")
set_warnings("allextra")
set_defaultmode("releasedbg")
add_rules("mode.debug", "mode.releasedbg")

target("SDSCombat")
    add_rules("commonlibsse-ng.plugin", {
        name = "SDSCombat",
        author = "Skyrim Dragon Souls",
        description = "SDS combat runtime: annotation-driven NPC target lock, reach-to-target, magnetism control and root motion that follows the animation"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_cxflags("/utf-8")
