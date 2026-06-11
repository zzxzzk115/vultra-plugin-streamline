target("plugin-dlss-upscaler")
    set_kind("shared")
    set_basename("vultra_plugin_dlss")
    set_prefixname("")
    add_deps("vultra")
    add_files("dlss_upscaler.cpp")
    set_targetdir(os.scriptdir())

    on_load(function (target)
        local sdk_root = os.getenv("VULTRA_DLSS_STREAMLINE_SDK_ROOT")
        local sdk_bin = os.getenv("VULTRA_DLSS_STREAMLINE_BIN")
        if sdk_root and sdk_root ~= "" then
            if not sdk_bin or sdk_bin == "" then
                sdk_bin = path.join(sdk_root, "bin", "x64")
            end
            target:add("defines", "VULTRA_DLSS_WITH_STREAMLINE=1")
            target:add("includedirs", path.join(sdk_root, "include"))
            target:add("linkdirs", path.join(sdk_root, "lib", "x64"))
            target:add("links", "sl.interposer")
            target:set("values", "dlss.sdk_root", sdk_root)
            target:set("values", "dlss.sdk_bin", sdk_bin)
        else
            target:add("defines", "VULTRA_DLSS_WITH_STREAMLINE=0")
        end
    end)

    before_build(function (target)
        local sdk_root = target:values("dlss.sdk_root")
        local sdk_bin = target:values("dlss.sdk_bin")
        if not sdk_root or sdk_root == "" then
            raise("plugin-dlss-upscaler requires VULTRA_DLSS_STREAMLINE_SDK_ROOT in the build process environment")
        end
        if not os.isdir(path.join(sdk_root, "include")) then
            raise("plugin-dlss-upscaler Streamline include directory not found: %s", path.join(sdk_root, "include"))
        end
        if not os.isdir(path.join(sdk_root, "lib", "x64")) then
            raise("plugin-dlss-upscaler Streamline lib directory not found: %s", path.join(sdk_root, "lib", "x64"))
        end
        if not sdk_bin or sdk_bin == "" or not os.isdir(sdk_bin) then
            raise("plugin-dlss-upscaler Streamline bin directory not found: %s", sdk_bin or "<unset>")
        end
    end)

    after_build(function (target)
        local sdk_bin = target:values("dlss.sdk_bin")
        if not sdk_bin or sdk_bin == "" then
            return
        end
        local dlls = {
            "sl.interposer.dll",
            "sl.common.dll",
            "sl.dlss.dll",
            "nvngx_dlss.dll"
        }
        for _, dll in ipairs(dlls) do
            local src = path.join(sdk_bin, dll)
            if os.isfile(src) then
                os.cp(src, target:targetdir())
            end
        end
    end)
target_end()
