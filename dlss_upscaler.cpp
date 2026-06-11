#include <vultra/core/engine/engine_context.hpp>
#include <vultra/core/plugin/engine_plugin.hpp>
#include <vultra/core/rhi/backends/vk/conversions.hpp>
#include <vultra/function/services/render_backend_extension_service.hpp>
#include <vultra/function/services/render_upscaler_service.hpp>

#if VULTRA_DLSS_WITH_STREAMLINE
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <charconv>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace
{
    // Absolute path of this plugin's root directory, handed over by the engine through the
    // optional vultraSetPluginRoot export before install(). The Streamline runtime DLLs are
    // bundled there.
    std::string g_PluginRoot;

    [[nodiscard]] std::string getEnvString(const char* name)
    {
        const char* value = std::getenv(name);
        return value == nullptr ? std::string {} : std::string {value};
    }

    [[nodiscard]] uint32_t getEnvUint32(const char* name, const uint32_t fallback = 0)
    {
        const auto value = getEnvString(name);
        if (value.empty())
            return fallback;
        uint32_t out = fallback;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
        return result.ec == std::errc {} ? out : fallback;
    }

    [[nodiscard]] bool getEnvBool(const char* name, const bool fallback = false)
    {
        auto value = getEnvString(name);
        if (value.empty())
            return fallback;
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    // Directory of this plugin DLL itself. The Streamline runtime DLLs are bundled next to it,
    // so they are found with zero user configuration; env vars remain as overrides.
    [[nodiscard]] std::string pluginModuleDirectory()
    {
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&pluginModuleDirectory),
                                 &module))
        {
            wchar_t path[MAX_PATH] {};
            const auto length = ::GetModuleFileNameW(module, path, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
                return std::filesystem::path {path}.parent_path().string();
        }
#endif
        return {};
    }

    [[nodiscard]] std::string normalizeModeName(std::string value)
    {
        for (auto& ch : value)
        {
            if (ch == '-' || ch == ' ')
                ch = '_';
            else
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    [[nodiscard]] bool supportsDlssOutputExtent(const vultra::rhi::Extent2D extent)
    {
        return extent.width >= 320u && extent.height >= 180u;
    }

#if VULTRA_DLSS_WITH_STREAMLINE
    std::wstring widenPath(const std::string& value)
    {
        return std::filesystem::path {value}.wstring();
    }

    sl::float2 toSl(const glm::vec2& value) { return {value.x, value.y}; }
    sl::float3 toSl(const glm::vec3& value) { return {value.x, value.y, value.z}; }

    sl::float4x4 toSl(const glm::mat4& value)
    {
        sl::float4x4 out {};
        for (uint32_t row = 0; row < 4; ++row)
            out[row] = sl::float4 {value[0][row], value[1][row], value[2][row], value[3][row]};
        return out;
    }

    sl::DLSSMode toSlMode(const vultra::UpscalerMode mode)
    {
        switch (mode)
        {
            case vultra::UpscalerMode::eQuality:
                return sl::DLSSMode::eMaxQuality;
            case vultra::UpscalerMode::eBalanced:
                return sl::DLSSMode::eBalanced;
            case vultra::UpscalerMode::ePerformance:
                return sl::DLSSMode::eMaxPerformance;
            case vultra::UpscalerMode::eUltraQuality:
                return sl::DLSSMode::eUltraQuality;
            case vultra::UpscalerMode::eUltraPerformance:
                return sl::DLSSMode::eUltraPerformance;
            case vultra::UpscalerMode::eDLAA:
                return sl::DLSSMode::eDLAA;
            case vultra::UpscalerMode::eOff:
            default:
                return sl::DLSSMode::eOff;
        }
    }

    sl::BufferType toSlRole(const vultra::UpscalerResourceRole role)
    {
        switch (role)
        {
            case vultra::UpscalerResourceRole::eScalingOutputColor:
                return sl::kBufferTypeScalingOutputColor;
            case vultra::UpscalerResourceRole::eDepth:
                return sl::kBufferTypeDepth;
            case vultra::UpscalerResourceRole::eMotionVectors:
                return sl::kBufferTypeMotionVectors;
            case vultra::UpscalerResourceRole::eExposure:
                return sl::kBufferTypeExposure;
            case vultra::UpscalerResourceRole::eScalingInputColor:
            default:
                return sl::kBufferTypeScalingInputColor;
        }
    }

    sl::Resource toSlResource(const vultra::NativeTextureResource& texture)
    {
        sl::Resource resource {
            sl::ResourceType::eTex2d,
            reinterpret_cast<void*>(texture.imageHandle),
            nullptr,
            reinterpret_cast<void*>(texture.imageViewHandle),
            static_cast<uint32_t>(static_cast<VkImageLayout>(vultra::rhi::toVk(texture.layout))),
        };
        resource.width        = texture.extent.width;
        resource.height       = texture.extent.height;
        resource.nativeFormat = static_cast<uint32_t>(static_cast<VkFormat>(vultra::rhi::toVk(texture.format)));
        resource.mipLevels    = texture.mipLevels;
        resource.arrayLayers  = texture.arrayLayers;
        resource.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(texture.usage));
        return resource;
    }

    sl::Constants toSlConstants(const vultra::UpscalerConstants& constants)
    {
        sl::Constants out {};
        out.cameraViewToClip     = toSl(constants.projection);
        out.clipToCameraView     = toSl(glm::inverse(constants.projection));
        out.clipToPrevClip       = toSl(constants.clipToPreviousClip);
        out.prevClipToClip       = toSl(constants.previousClipToClip);
        out.jitterOffset         = toSl(constants.jitterOffsetPx);
        out.mvecScale            = toSl(constants.motionVectorScale);
        out.cameraPos            = toSl(constants.cameraPosition);
        out.cameraUp             = toSl(constants.cameraUp);
        out.cameraRight          = toSl(constants.cameraRight);
        out.cameraFwd            = toSl(constants.cameraForward);
        out.cameraNear           = constants.nearPlane;
        out.cameraFar            = constants.farPlane;
        out.cameraFOV            = constants.fovYRadians;
        out.cameraAspectRatio    = constants.aspectRatio;
        out.cameraPinholeOffset  = {0.0f, 0.0f};
        out.depthInverted        = constants.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        out.cameraMotionIncluded = constants.cameraMotionIncluded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        out.motionVectors3D      = sl::Boolean::eFalse;
        out.reset                = constants.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        return out;
    }

    void slLogCallback(sl::LogType type, const char* message)
    {
        const char* label = type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warn" : "info";
        std::printf("[vultra_plugin_dlss][sl][%s] %s\n", label, message != nullptr ? message : "");
    }

    [[nodiscard]] const char* resultName(const sl::Result result)
    {
        switch (result)
        {
            case sl::Result::eOk:
                return "eOk";
            case sl::Result::eErrorVulkanAPI:
                return "eErrorVulkanAPI";
            case sl::Result::eErrorNGXFailed:
                return "eErrorNGXFailed";
            case sl::Result::eErrorInvalidIntegration:
                return "eErrorInvalidIntegration";
            case sl::Result::eErrorNotInitialized:
                return "eErrorNotInitialized";
            case sl::Result::eErrorFeatureFailedToLoad:
                return "eErrorFeatureFailedToLoad";
            case sl::Result::eErrorFeatureNotSupported:
                return "eErrorFeatureNotSupported";
            case sl::Result::eErrorNoSupportedAdapterFound:
                return "eErrorNoSupportedAdapterFound";
            case sl::Result::eErrorAdapterNotSupported:
                return "eErrorAdapterNotSupported";
            case sl::Result::eErrorDriverOutOfDate:
                return "eErrorDriverOutOfDate";
            case sl::Result::eErrorOSOutOfDate:
                return "eErrorOSOutOfDate";
            case sl::Result::eErrorOSDisabledHWS:
                return "eErrorOSDisabledHWS";
            case sl::Result::eErrorFeatureMissingHooks:
                return "eErrorFeatureMissingHooks";
            case sl::Result::eErrorMissingOrInvalidAPI:
                return "eErrorMissingOrInvalidAPI";
            case sl::Result::eErrorInvalidState:
                return "eErrorInvalidState";
            default:
                return "unknown";
        }
    }

    [[nodiscard]] std::string resultDetails(const sl::Result result)
    {
        switch (result)
        {
            case sl::Result::eErrorNGXFailed:
                return "NGX could not create the DLSS context. Check NVIDIA driver, RTX support, Streamline DLLs, and Application ID/Project ID.";
            case sl::Result::eErrorInvalidIntegration:
                return "Streamline rejected the integration state. Check hook order and slSetVulkanInfo.";
            case sl::Result::eErrorMissingOrInvalidAPI:
                return "The Streamline render API or Vulkan device binding is invalid.";
            case sl::Result::eErrorFeatureFailedToLoad:
                return "The DLSS Streamline feature DLL failed to load. Check sl.dlss.dll and nvngx_dlss.dll in the configured bin folder.";
            case sl::Result::eErrorFeatureNotSupported:
                return "Streamline reports DLSS is not supported on the selected adapter/device state. Check RTX hardware, driver, Vulkan requirements, and whether NGX initialized the DLSS context.";
            case sl::Result::eErrorNoSupportedAdapterFound:
                return "Streamline did not find a supported NVIDIA adapter.";
            case sl::Result::eErrorAdapterNotSupported:
                return "The selected adapter does not support this DLSS feature.";
            case sl::Result::eErrorDriverOutOfDate:
                return "Update the NVIDIA display driver, then restart the editor.";
            case sl::Result::eErrorOSOutOfDate:
                return "Update Windows, then restart the editor.";
            case sl::Result::eErrorOSDisabledHWS:
                return "Enable Windows hardware-accelerated GPU scheduling if this feature requires it.";
            case sl::Result::eErrorFeatureMissingHooks:
                return "Streamline is missing required Vulkan hooks. Check sl.interposer.dll and early plugin load.";
            case sl::Result::eErrorVulkanAPI:
                return "A Vulkan call made through Streamline failed. Check the previous Vulkan/Streamline log lines.";
            case sl::Result::eErrorNotInitialized:
                return "Streamline was used before slInit completed or after slShutdown.";
            case sl::Result::eErrorInvalidState:
                return "Streamline is in an unexpected state. Check device creation, swapchain recreation, and shutdown order.";
            default:
                return {};
        }
    }

    [[nodiscard]] std::string appendResultDetails(std::string message, const sl::Result result)
    {
        const auto details = resultDetails(result);
        if (!details.empty())
        {
            message += ". ";
            message += details;
        }
        return message;
    }

    [[nodiscard]] std::string describeFeatureRequirements()
    {
        sl::FeatureRequirements requirements {};
        const auto result = slGetFeatureRequirements(sl::kFeatureDLSS, requirements);
        if (result != sl::Result::eOk)
            return std::string {"slGetFeatureRequirements(DLSS) failed: "} + resultName(result) + " (" +
                   std::to_string(static_cast<int>(result)) + ")";

        std::ostringstream out;
        out << "DLSS requirements: driver detected " << requirements.driverVersionDetected.major << "."
            << requirements.driverVersionDetected.minor << "." << requirements.driverVersionDetected.build
            << ", required " << requirements.driverVersionRequired.major << "."
            << requirements.driverVersionRequired.minor << "." << requirements.driverVersionRequired.build
            << "; Vulkan "
            << ((requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported) != 0 ? "supported" :
                                                                                             "not supported")
            << "; required tags=" << requirements.numRequiredTags << "; vk instance extensions="
            << requirements.vkNumInstanceExtensions << "; vk device extensions=" << requirements.vkNumDeviceExtensions;
        return out.str();
    }

    [[nodiscard]] std::uintptr_t getProcAddress(HMODULE module, const char* name)
    {
        return module != nullptr ? reinterpret_cast<std::uintptr_t>(::GetProcAddress(module, name)) : 0u;
    }

    [[nodiscard]] HMODULE getStreamlineCommonModule()
    {
        return ::GetModuleHandleA("sl.common.dll");
    }

    using SlHookVkBeginCommandBufferFn =
        void(VKAPI_PTR*)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
    using SlHookVkCmdBindPipelineFn =
        void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
    using SlHookVkCmdBindDescriptorSetsFn = void(VKAPI_PTR*)(VkCommandBuffer,
                                                             VkPipelineBindPoint,
                                                             VkPipelineLayout,
                                                             uint32_t,
                                                             uint32_t,
                                                             const VkDescriptorSet*,
                                                             uint32_t,
                                                             const uint32_t*);

    void VKAPI_CALL dlssPostBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                               const VkCommandBufferBeginInfo* beginInfo)
    {
        auto* hook = reinterpret_cast<SlHookVkBeginCommandBufferFn>(
            getProcAddress(getStreamlineCommonModule(), "slHookVkBeginCommandBuffer"));
        if (hook != nullptr)
            hook(commandBuffer, beginInfo);
    }

    void VKAPI_CALL dlssPostCmdBindPipeline(VkCommandBuffer   commandBuffer,
                                            VkPipelineBindPoint bindPoint,
                                            VkPipeline         pipeline)
    {
        auto* hook = reinterpret_cast<SlHookVkCmdBindPipelineFn>(
            getProcAddress(getStreamlineCommonModule(), "slHookVkCmdBindPipeline"));
        if (hook != nullptr)
            hook(commandBuffer, bindPoint, pipeline);
    }

    void VKAPI_CALL dlssPostCmdBindDescriptorSets(VkCommandBuffer      commandBuffer,
                                                  VkPipelineBindPoint  bindPoint,
                                                  VkPipelineLayout     layout,
                                                  uint32_t             firstSet,
                                                  uint32_t             descriptorSetCount,
                                                  const VkDescriptorSet* descriptorSets,
                                                  uint32_t             dynamicOffsetCount,
                                                  const uint32_t*      dynamicOffsets)
    {
        auto* hook = reinterpret_cast<SlHookVkCmdBindDescriptorSetsFn>(
            getProcAddress(getStreamlineCommonModule(), "slHookVkCmdBindDescriptorSets"));
        if (hook != nullptr)
        {
            hook(commandBuffer,
                 bindPoint,
                 layout,
                 firstSet,
                 descriptorSetCount,
                 descriptorSets,
                 dynamicOffsetCount,
                 dynamicOffsets);
        }
    }

    struct OptimalExtentKey
    {
        uint32_t             width {0};
        uint32_t             height {0};
        vultra::UpscalerMode mode {vultra::UpscalerMode::eOff};

        friend bool operator==(const OptimalExtentKey& lhs, const OptimalExtentKey& rhs)
        {
            return lhs.width == rhs.width && lhs.height == rhs.height && lhs.mode == rhs.mode;
        }
    };

    struct OptimalExtentKeyHash
    {
        size_t operator()(const OptimalExtentKey& key) const noexcept
        {
            size_t seed = std::hash<uint32_t> {}(key.width);
            seed ^= std::hash<uint32_t> {}(key.height) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
            seed ^= std::hash<uint8_t> {}(static_cast<uint8_t>(key.mode)) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
            return seed;
        }
    };
#endif

    class DlssProvider final : public vultra::IUpscalerProvider, public vultra::IRenderBackendExtension
    {
    public:
        std::string_view name() const override { return "dlss"; }

        vultra::VulkanHookTable vulkanHooks() const override { return m_Hooks; }

        void collectVulkanDeviceRequirements(vultra::VulkanDeviceRequirements& requirements) const override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            requirements.deviceExtensions.insert(requirements.deviceExtensions.end(),
                                                 m_RequiredDeviceExtensions.begin(),
                                                 m_RequiredDeviceExtensions.end());
#else
            static_cast<void>(requirements);
#endif
        }

        vultra::UpscalerStatus status() const override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            return {
                .available      = m_Initialized && m_VulkanReady,
                .activeProvider = "dlss",
                .message        = m_StatusMessage,
            };
#else
            return {
                .available      = false,
                .activeProvider = "dlss",
                .message        = "DLSS plugin DLL was built without Streamline SDK. Rebuild plugin-dlss-upscaler with VULTRA_DLSS_STREAMLINE_SDK_ROOT set in the build process environment.",
            };
#endif
        }

        vultra::rhi::Extent2D
        queryOptimalRenderExtent(vultra::rhi::Extent2D outputExtent, vultra::UpscalerMode mode) override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            if (!m_Initialized)
                return outputExtent;
            if (!supportsDlssOutputExtent(outputExtent))
                return outputExtent;

            const OptimalExtentKey key {
                .width  = outputExtent.width,
                .height = outputExtent.height,
                .mode   = mode,
            };
            if (const auto it = m_OptimalExtentCache.find(key); it != m_OptimalExtentCache.end())
                return it->second;

            sl::DLSSOptions options {};
            options.mode         = toSlMode(mode);
            options.outputWidth  = outputExtent.width;
            options.outputHeight = outputExtent.height;
            sl::DLSSOptimalSettings settings {};
            if (slDLSSGetOptimalSettings(options, settings) == sl::Result::eOk)
            {
                const vultra::rhi::Extent2D optimal {settings.optimalRenderWidth, settings.optimalRenderHeight};
                m_OptimalExtentCache.emplace(key, optimal);
                std::printf("[vultra_plugin_dlss] DLSS optimal extent: output=%ux%u mode=%s optimal=%ux%u "
                            "min=%ux%u max=%ux%u\n",
                            outputExtent.width,
                            outputExtent.height,
                            vultra::upscalerModeName(mode).data(),
                            settings.optimalRenderWidth,
                            settings.optimalRenderHeight,
                            settings.renderWidthMin,
                            settings.renderHeightMin,
                            settings.renderWidthMax,
                            settings.renderHeightMax);
                return optimal;
            }
#endif
            return outputExtent;
        }

        void onResize(vultra::rhi::Extent2D outputExtent) override
        {
            m_OutputExtent = outputExtent;
#if VULTRA_DLSS_WITH_STREAMLINE
            m_OptimalExtentCache.clear();
#endif
        }
        void beginFrame(const vultra::NativeCommandContext&) override {}

        bool evaluate(const vultra::UpscalerEvaluateContext& context) override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            if (!m_Initialized || !m_VulkanReady || context.command.commandBufferHandle == 0)
                return false;
            if (!supportsDlssOutputExtent(context.outputExtent))
            {
                m_StatusMessage = "DLSS skipped for too-small output extent";
                return false;
            }

            uint32_t frameIndex = static_cast<uint32_t>(context.command.frameToken.frameIndex);
            if (frameIndex == 0)
                frameIndex = static_cast<uint32_t>(context.command.frameIndex);

            sl::FrameToken* frameToken = nullptr;
            if (slGetNewFrameToken(frameToken, &frameIndex) != sl::Result::eOk || frameToken == nullptr)
            {
                m_StatusMessage = "slGetNewFrameToken failed. Streamline did not provide a frame token.";
                return false;
            }

            const sl::ViewportHandle viewport {context.command.viewportId};
            const auto               viewportKey = static_cast<uint32_t>(context.command.viewportId);
            auto&                    viewportState = m_ViewportStates[viewportKey];
            const bool               isDlssUpscalingMode =
                context.settings.mode != vultra::UpscalerMode::eOff &&
                context.settings.mode != vultra::UpscalerMode::eDLAA;
            if (isDlssUpscalingMode && context.renderExtent.width == context.outputExtent.width &&
                context.renderExtent.height == context.outputExtent.height && !viewportState.warnedNoScale)
            {
                std::printf(
                    "[vultra_plugin_dlss] warning: DLSS is active but render extent equals output extent "
                    "(%ux%u). The graph is evaluating DLSS, but the renderer is not yet rendering at the "
                    "DLSS optimal input resolution.\n",
                    context.renderExtent.width,
                    context.renderExtent.height);
                viewportState.warnedNoScale = true;
            }
            if (m_ViewportStates.size() > 4 && !m_WarnedManyViewports)
            {
                std::printf(
                    "[vultra_plugin_dlss] warning: DLSS has seen %zu viewport handles. Viewport IDs should be "
                    "stable; unstable IDs can make Streamline keep allocating history resources.\n",
                    m_ViewportStates.size());
                m_WarnedManyViewports = true;
            }

            sl::DLSSOptions options {};
            options.mode                   = toSlMode(context.settings.mode);
            options.outputWidth            = context.outputExtent.width;
            options.outputHeight           = context.outputExtent.height;
            options.colorBuffersHDR        = context.settings.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            options.useAutoExposure        = context.settings.autoExposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            options.alphaUpscalingEnabled  = sl::Boolean::eFalse;
            options.dlaaPreset             = sl::DLSSPreset::ePresetK;
            options.qualityPreset          = sl::DLSSPreset::ePresetK;
            options.balancedPreset         = sl::DLSSPreset::ePresetK;
            options.performancePreset      = sl::DLSSPreset::ePresetM;
            options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
            const bool optionsChanged = !viewportState.optionsSet ||
                                        viewportState.mode != context.settings.mode ||
                                        viewportState.renderExtent.width != context.renderExtent.width ||
                                        viewportState.renderExtent.height != context.renderExtent.height ||
                                        viewportState.outputExtent.width != context.outputExtent.width ||
                                        viewportState.outputExtent.height != context.outputExtent.height;
            if (optionsChanged)
            {
                if (viewportState.optionsSet)
                {
                    slFreeResources(sl::kFeatureDLSS, viewport);
                    std::printf(
                        "[vultra_plugin_dlss] DLSS viewport %u options changed; freed old feature resources "
                        "(render=%ux%u output=%ux%u mode=%s).\n",
                        viewportKey,
                        context.renderExtent.width,
                        context.renderExtent.height,
                        context.outputExtent.width,
                        context.outputExtent.height,
                        vultra::upscalerModeName(context.settings.mode).data());
                }

                const auto setOptionsResult = slDLSSSetOptions(viewport, options);
                if (setOptionsResult != sl::Result::eOk)
                {
                    m_StatusMessage = appendResultDetails(
                        std::string {"slDLSSSetOptions failed: "} + resultName(setOptionsResult) + " (" +
                            std::to_string(static_cast<int>(setOptionsResult)) + ")",
                        setOptionsResult);
                    return false;
                }
                viewportState.optionsSet   = true;
                viewportState.mode         = context.settings.mode;
                viewportState.renderExtent = context.renderExtent;
                viewportState.outputExtent = context.outputExtent;
                std::printf("[vultra_plugin_dlss] DLSS options set for viewport %u: render=%ux%u output=%ux%u "
                            "mode=%s hdr=%s autoExposure=%s\n",
                            viewportKey,
                            context.renderExtent.width,
                            context.renderExtent.height,
                            context.outputExtent.width,
                            context.outputExtent.height,
                            vultra::upscalerModeName(context.settings.mode).data(),
                            context.settings.hdr ? "true" : "false",
                            context.settings.autoExposure ? "true" : "false");
            }

            const auto constants = toSlConstants(context.constants);
            const auto constantsResult = slSetConstants(constants, *frameToken, viewport);
            if (constantsResult != sl::Result::eOk)
            {
                m_StatusMessage = appendResultDetails(
                    std::string {"slSetConstants failed: "} + resultName(constantsResult) + " (" +
                        std::to_string(static_cast<int>(constantsResult)) + ")",
                    constantsResult);
                return false;
            }

            std::vector<sl::Resource>    resources;
            std::vector<sl::Extent>      extents;
            std::vector<sl::ResourceTag> tags;
            resources.reserve(context.resources.size());
            extents.reserve(context.resources.size());
            tags.reserve(context.resources.size());
            for (const auto& tag : context.resources)
            {
                if (tag.resource.imageHandle == 0 || tag.resource.imageViewHandle == 0)
                    continue;
                resources.push_back(toSlResource(tag.resource));
                extents.push_back(sl::Extent {
                    .top    = 0,
                    .left   = 0,
                    .width  = tag.resource.extent.width,
                    .height = tag.resource.extent.height,
                });
                tags.emplace_back(
                    &resources.back(), toSlRole(tag.role), sl::ResourceLifecycle::eOnlyValidNow, &extents.back());
            }

            auto* commandBuffer = reinterpret_cast<sl::CommandBuffer*>(context.command.commandBufferHandle);
            if (tags.empty())
            {
                m_StatusMessage =
                    "No native resources were tagged for DLSS. Check ExternalUpscaler color/output/depth/motion inputs.";
                return false;
            }

            const auto tagResult =
                slSetTagForFrame(*frameToken, viewport, tags.data(), static_cast<uint32_t>(tags.size()), commandBuffer);
            if (tagResult != sl::Result::eOk)
            {
                m_StatusMessage = appendResultDetails(
                    std::string {"slSetTagForFrame failed: "} + resultName(tagResult) + " (" +
                        std::to_string(static_cast<int>(tagResult)) + ")",
                    tagResult);
                return false;
            }

            const sl::BaseStructure* inputs[] = {&viewport};
            const auto evaluateResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, 1, commandBuffer);
            if (evaluateResult != sl::Result::eOk)
            {
                m_StatusMessage = appendResultDetails(
                    std::string {"slEvaluateFeature(DLSS) failed: "} + resultName(evaluateResult) + " (" +
                        std::to_string(static_cast<int>(evaluateResult)) + ")",
                    evaluateResult);
                return false;
            }

            m_StatusMessage = "DLSS evaluated";
            ++m_EvaluateCount;
            if (m_EvaluateCount <= 3 || (m_EvaluateCount % 120) == 0)
            {
                std::printf("[vultra_plugin_dlss] slEvaluateFeature(DLSS) ok #%llu viewport=%u render=%ux%u "
                            "output=%ux%u tags=%zu\n",
                            static_cast<unsigned long long>(m_EvaluateCount),
                            viewportKey,
                            context.renderExtent.width,
                            context.renderExtent.height,
                            context.outputExtent.width,
                            context.outputExtent.height,
                            tags.size());
            }
            return true;
#else
            static_cast<void>(context);
            return false;
#endif
        }

        void freeResourcesForViewport(vultra::UpscalerViewportId viewportId) override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            if (m_Initialized)
            {
                slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle {viewportId});
                m_ViewportStates.erase(static_cast<uint32_t>(viewportId));
            }
#else
            static_cast<void>(viewportId);
#endif
        }

        void shutdown() override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            if (m_Initialized)
            {
                std::printf("[vultra_plugin_dlss] slShutdown begin\n");
                std::fflush(stdout);
                const auto result = slShutdown();
                std::printf("[vultra_plugin_dlss] slShutdown end: %s (%d)\n",
                            resultName(result),
                            static_cast<int>(result));
                std::fflush(stdout);
                m_ViewportStates.clear();
                m_OptimalExtentCache.clear();
                m_Initialized = false;
                m_VulkanReady = false;
            }
#endif
        }

        bool initialize()
        {
            m_StreamlineSdkRoot = getEnvString("VULTRA_DLSS_STREAMLINE_SDK_ROOT");
            m_StreamlineBin     = getEnvString("VULTRA_DLSS_STREAMLINE_BIN");
            m_ProjectId         = getEnvString("VULTRA_DLSS_PROJECT_ID");
            if (m_ProjectId.empty())
                m_ProjectId = "7f3a9c21-84bd-46e2-91af-c5d7382b0e64";
            m_ApplicationId = getEnvUint32("VULTRA_DLSS_APPLICATION_ID");
            if (m_StreamlineBin.empty() && !m_StreamlineSdkRoot.empty())
                m_StreamlineBin = (std::filesystem::path {m_StreamlineSdkRoot} / "bin" / "x64").string();
            // The Streamline runtime DLLs are bundled in the plugin root, so the default needs no
            // environment configuration: the engine-provided plugin root, then this DLL's own folder.
            if (m_StreamlineBin.empty())
                m_StreamlineBin = g_PluginRoot;
            if (m_StreamlineBin.empty())
                m_StreamlineBin = pluginModuleDirectory();

#if VULTRA_DLSS_WITH_STREAMLINE
            if (m_StreamlineBin.empty())
            {
                m_StatusMessage = "Streamline runtime folder could not be resolved (no bundled DLL folder, "
                                  "VULTRA_DLSS_STREAMLINE_BIN/SDK_ROOT unset)";
                return false;
            }

            m_PluginPath = widenPath(m_StreamlineBin);
            const wchar_t* pluginPaths[] = {m_PluginPath.c_str()};
            const sl::Feature features[] = {sl::kFeatureDLSS};

            sl::Preferences pref {};
            pref.pathsToPlugins    = pluginPaths;
            pref.numPathsToPlugins = 1;
            pref.featuresToLoad    = features;
            pref.numFeaturesToLoad = 1;
            pref.renderAPI         = sl::RenderAPI::eVulkan;
            pref.engine            = sl::EngineType::eCustom;
            pref.engineVersion     = "Vultra";
            pref.applicationId     = m_ApplicationId;
            pref.projectId         = m_ProjectId.c_str();
            pref.logMessageCallback = slLogCallback;
            pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                         sl::PreferenceFlags::eUseFrameBasedResourceTagging;

            const auto result = slInit(pref, sl::kSDKVersion);
            m_Initialized     = result == sl::Result::eOk;
            m_StatusMessage = m_Initialized ?
                                  std::string {"Streamline initialized in "} + integrationMode() +
                                      " mode; waiting for Vulkan device" :
                                  appendResultDetails(
                                      std::string {"slInit failed: "} + resultName(result) + " (" +
                                          std::to_string(static_cast<int>(result)) + ")",
                                      result);
            if (!m_Initialized)
            {
                std::printf("[vultra_plugin_dlss] slInit failed: %s (%d)\n",
                            resultName(result),
                            static_cast<int>(result));
            }
            if (m_Initialized)
            {
                std::printf("[vultra_plugin_dlss] Integration mode: %s\n", integrationMode().c_str());
                std::printf("[vultra_plugin_dlss] %s\n", describeFeatureRequirements().c_str());
                collectStreamlineRequirements();
                loadVulkanHooks();
            }
            return m_Initialized;
#else
            m_StatusMessage =
                "DLSS plugin DLL was built without Streamline SDK. Rebuild plugin-dlss-upscaler with VULTRA_DLSS_STREAMLINE_SDK_ROOT set in the build process environment.";
            return false;
#endif
        }

        void afterVulkanNativeDeviceCreate(const vultra::VulkanNativeDevice& device) override
        {
#if VULTRA_DLSS_WITH_STREAMLINE
            if (!m_Initialized || !device.valid())
                return;

            sl::VulkanInfo info {};
            info.instance            = reinterpret_cast<VkInstance>(device.instance);
            info.physicalDevice      = reinterpret_cast<VkPhysicalDevice>(device.physicalDevice);
            info.device              = reinterpret_cast<VkDevice>(device.device);
            info.graphicsQueueFamily = device.graphicsQueueFamily;
            info.graphicsQueueIndex  = device.graphicsQueueIndex;
            info.computeQueueFamily  = device.graphicsQueueFamily;
            info.computeQueueIndex   = device.graphicsQueueIndex;
            const auto result        = slSetVulkanInfo(info);
            m_VulkanReady            = result == sl::Result::eOk;
            if (m_VulkanReady)
            {
                m_StatusMessage = std::string {"Streamline Vulkan device ready. "} + integrationModeDescription();
                checkDlssSupport(device);
            }
            else if (result == sl::Result::eErrorInvalidIntegration && m_Hooks.vkCreateDevice != 0)
            {
                m_VulkanReady   = true;
                m_StatusMessage =
                    std::string {"Streamline Vulkan device ready via hooked vkCreateDevice. "} +
                    integrationModeDescription();
                checkDlssSupport(device);
            }
            else
            {
                m_StatusMessage = appendResultDetails(
                    std::string {"slSetVulkanInfo failed: "} + resultName(result) + " (" +
                        std::to_string(static_cast<int>(result)) + ")",
                    result);
                std::printf("[vultra_plugin_dlss] %s\n", m_StatusMessage.c_str());
            }
#else
            static_cast<void>(device);
#endif
        }

        const std::string& streamlineSdkRoot() const { return m_StreamlineSdkRoot; }
        const std::string& streamlineBin() const { return m_StreamlineBin; }

    private:
#if VULTRA_DLSS_WITH_STREAMLINE
        void loadVulkanHooks()
        {
#if defined(_WIN32)
            HMODULE module = ::GetModuleHandleA("sl.interposer.dll");
            if (module == nullptr && !m_StreamlineBin.empty())
            {
                const auto path = std::filesystem::path {m_StreamlineBin} / "sl.interposer.dll";
                module          = ::LoadLibraryW(path.wstring().c_str());
            }
            if (module == nullptr)
                module = ::LoadLibraryA("sl.interposer.dll");
            if (module == nullptr)
            {
                m_StatusMessage = "sl.interposer.dll is not loaded";
                return;
            }

            m_Hooks.vkGetInstanceProcAddr    = getProcAddress(module, "vkGetInstanceProcAddr");
            m_Hooks.vkGetDeviceProcAddr      = getProcAddress(module, "vkGetDeviceProcAddr");
            m_Hooks.vkCreateInstance         = getProcAddress(module, "vkCreateInstance");
            m_Hooks.vkCreateDevice           = getProcAddress(module, "vkCreateDevice");
            m_Hooks.vkCreateSwapchainKHR     = getProcAddress(module, "vkCreateSwapchainKHR");
            m_Hooks.vkDestroySwapchainKHR    = getProcAddress(module, "vkDestroySwapchainKHR");
            m_Hooks.vkGetSwapchainImagesKHR  = getProcAddress(module, "vkGetSwapchainImagesKHR");
            m_Hooks.vkAcquireNextImageKHR    = getProcAddress(module, "vkAcquireNextImageKHR");
            m_Hooks.vkQueuePresentKHR        = getProcAddress(module, "vkQueuePresentKHR");
            m_Hooks.vkDeviceWaitIdle         = getProcAddress(module, "vkDeviceWaitIdle");
            m_Hooks.vkCreateWin32SurfaceKHR  = getProcAddress(module, "vkCreateWin32SurfaceKHR");
            m_Hooks.vkDestroySurfaceKHR      = getProcAddress(module, "vkDestroySurfaceKHR");
            m_Hooks.vkBeginCommandBuffer     = getProcAddress(module, "vkBeginCommandBuffer");
            m_Hooks.vkCmdBindPipeline        = getProcAddress(module, "vkCmdBindPipeline");
            m_Hooks.vkCmdBindDescriptorSets  = getProcAddress(module, "vkCmdBindDescriptorSets");
            m_Hooks.vkCmdPipelineBarrier     = getProcAddress(module, "vkCmdPipelineBarrier");
            m_Hooks.vkCreateImage            = getProcAddress(module, "vkCreateImage");

            m_Hooks.vkPostBeginCommandBuffer =
                reinterpret_cast<std::uintptr_t>(&dlssPostBeginCommandBuffer);
            m_Hooks.vkPostCmdBindPipeline =
                reinterpret_cast<std::uintptr_t>(&dlssPostCmdBindPipeline);
            m_Hooks.vkPostCmdBindDescriptorSets =
                reinterpret_cast<std::uintptr_t>(&dlssPostCmdBindDescriptorSets);
            std::printf("[vultra_plugin_dlss] Vulkan dispatch hooks: %s\n",
                        m_Hooks.vkGetInstanceProcAddr != 0 && m_Hooks.vkGetDeviceProcAddr != 0 ? "ready" :
                                                                                                  "missing");
            std::printf("[vultra_plugin_dlss] Vulkan command-state callbacks: %s\n",
                        m_Hooks.vkPostBeginCommandBuffer != 0 && m_Hooks.vkPostCmdBindPipeline != 0 &&
                                m_Hooks.vkPostCmdBindDescriptorSets != 0 ?
                            "ready" :
                            "missing");
            m_StatusMessage = m_Hooks.empty() ? "Streamline Vulkan hooks unavailable" :
                                                std::string {"Streamline initialized in "} + integrationMode() +
                                                    " mode; waiting for Vulkan device";
#else
            m_StatusMessage = "Streamline Vulkan hooks require Windows in V1";
#endif
        }

        void collectStreamlineRequirements()
        {
            m_RequiredDeviceExtensions.clear();
            sl::FeatureRequirements requirements {};
            const auto result = slGetFeatureRequirements(sl::kFeatureDLSS, requirements);
            if (result != sl::Result::eOk)
            {
                std::printf("[vultra_plugin_dlss] Cannot collect DLSS Vulkan requirements: %s (%d)\n",
                            resultName(result),
                            static_cast<int>(result));
                return;
            }

            for (uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i)
            {
                if (requirements.vkDeviceExtensions != nullptr && requirements.vkDeviceExtensions[i] != nullptr)
                {
                    m_RequiredDeviceExtensions.emplace_back(requirements.vkDeviceExtensions[i]);
                    std::printf("[vultra_plugin_dlss] Requesting Vulkan device extension: %s\n",
                                requirements.vkDeviceExtensions[i]);
                }
            }
        }

        [[nodiscard]] std::string integrationMode() const
        {
            return m_ApplicationId != 0 ? "production Application ID" : "local Project ID";
        }

        [[nodiscard]] std::string integrationModeDescription() const
        {
            if (m_ApplicationId != 0)
                return "Using NVIDIA Application ID " + std::to_string(m_ApplicationId) + ".";
            return "Using local Project ID; a NVIDIA Application ID is still needed for production DLSS/NGX release.";
        }

        void checkDlssSupport(const vultra::VulkanNativeDevice& device)
        {
            sl::AdapterInfo adapterInfo {};
            adapterInfo.vkPhysicalDevice = reinterpret_cast<void*>(device.physicalDevice);
            const auto supportResult     = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
            if (supportResult == sl::Result::eOk)
            {
                std::printf("[vultra_plugin_dlss] DLSS feature support check: supported\n");
                return;
            }

            m_StatusMessage = appendResultDetails(
                std::string {"DLSS feature support check failed: "} + resultName(supportResult) + " (" +
                    std::to_string(static_cast<int>(supportResult)) + ")",
                supportResult);
            std::printf("[vultra_plugin_dlss] %s\n", m_StatusMessage.c_str());
            std::printf("[vultra_plugin_dlss] %s\n", describeFeatureRequirements().c_str());
        }

        struct ViewportState
        {
            bool                  optionsSet {false};
            bool                  warnedNoScale {false};
            vultra::UpscalerMode  mode {vultra::UpscalerMode::eOff};
            vultra::rhi::Extent2D renderExtent {};
            vultra::rhi::Extent2D outputExtent {};
        };
#endif

        vultra::rhi::Extent2D m_OutputExtent {};
        vultra::VulkanHookTable m_Hooks {};
        std::string           m_StreamlineSdkRoot;
        std::string           m_StreamlineBin;
        std::string           m_ProjectId;
        std::vector<std::string> m_RequiredDeviceExtensions;
        uint32_t              m_ApplicationId {0};
        std::string           m_StatusMessage {"DLSS plugin not initialized"};
#if VULTRA_DLSS_WITH_STREAMLINE
        std::wstring m_PluginPath;
        std::unordered_map<uint32_t, ViewportState> m_ViewportStates;
        std::unordered_map<OptimalExtentKey, vultra::rhi::Extent2D, OptimalExtentKeyHash> m_OptimalExtentCache;
        uint64_t     m_EvaluateCount {0};
        bool         m_WarnedManyViewports {false};
        bool         m_Initialized {false};
        bool         m_VulkanReady {false};
#endif
    };

    class DlssUpscalerPlugin final : public vultra::EnginePlugin
    {
    public:
        const char* name() const override { return "vultra_plugin_dlss"; }

        bool install(vultra::EngineContext& ctx) override
        {
            auto* upscaler = ctx.services.tryGet<vultra::IRenderUpscalerService>();
            if (upscaler == nullptr)
            {
                std::fprintf(stderr, "[vultra_plugin_dlss] upscaler service unavailable\n");
                return false;
            }

            m_Provider.initialize();
            std::printf("[vultra_plugin_dlss] Streamline SDK root: %s\n",
                        m_Provider.streamlineSdkRoot().empty() ? "<unset>" : m_Provider.streamlineSdkRoot().c_str());
            std::printf("[vultra_plugin_dlss] Streamline bin: %s\n",
                        m_Provider.streamlineBin().empty() ? "<derive from SDK root>" :
                                                             m_Provider.streamlineBin().c_str());

            if (auto* backendExtensions = ctx.services.tryGet<vultra::IRenderBackendExtensionService>())
                backendExtensions->registerExtension(m_Provider);

            const bool registered = upscaler->registerProvider(m_Provider);
            upscaler->setActiveProvider(m_Provider.name());
            const bool defaultEnabled = getEnvBool("VULTRA_DLSS_ENABLED", false);
            const auto defaultModeName = getEnvString("VULTRA_DLSS_MODE");
            const auto defaultMode =
                vultra::upscalerModeFromName(defaultModeName.empty() ? "performance" : defaultModeName);
            const auto normalizedModeName = normalizeModeName(defaultModeName);
            if (!defaultModeName.empty() && defaultMode == vultra::UpscalerMode::eOff &&
                normalizedModeName != "off")
            {
                std::printf("[vultra_plugin_dlss] Unknown VULTRA_DLSS_MODE='%s'; falling back to performance. "
                            "Accepted values: ultra_quality, quality, balanced, performance, "
                            "ultra_performance, dlaa, off.\n",
                            defaultModeName.c_str());
            }
            if (defaultEnabled)
                upscaler->setMode(defaultMode == vultra::UpscalerMode::eOff ? vultra::UpscalerMode::ePerformance :
                                                                            defaultMode);
            else
                upscaler->setEnabled(false);
            std::printf("[vultra_plugin_dlss] installed provider\n");
            return registered;
        }

        void uninstall(vultra::EngineContext& ctx) override
        {
            m_Provider.shutdown();

            if (auto* backendExtensions = ctx.services.tryGet<vultra::IRenderBackendExtensionService>())
                backendExtensions->unregisterExtension(m_Provider);
            if (auto* upscaler = ctx.services.tryGet<vultra::IRenderUpscalerService>())
                upscaler->unregisterProvider(m_Provider);
            std::printf("[vultra_plugin_dlss] uninstalled\n");
        }

    private:
        DlssProvider m_Provider;
    };
} // namespace

#if defined(_WIN32)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

PLUGIN_EXPORT vultra::EnginePlugin* vultraCreatePlugin() { return new DlssUpscalerPlugin(); }
PLUGIN_EXPORT void                  vultraDestroyPlugin(vultra::EnginePlugin* plugin) { delete plugin; }
PLUGIN_EXPORT void                  vultraSetPluginRoot(const char* absolutePath)
{
    g_PluginRoot = absolutePath == nullptr ? std::string {} : std::string {absolutePath};
}
