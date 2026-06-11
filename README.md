# Vultra Streamline Plugin

NVIDIA Streamline / DLSS upscaler plugin for Vultra.

## Using the plugin

Install it from the Vultra plugin catalog (or import this repository by Git URL) and enable it in
*Project Settings → Plugins*. No environment variables are required: the Streamline runtime DLLs
(`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`) are bundled in the plugin
root, and the plugin resolves them from its own folder at runtime.

Runtime options (DLSS mode, startup enable, NVIDIA application/project id) are edited in the
editor's plugin configuration UI and stored per project.

## Building the native plugin

Environment variables are only needed when (re)building `vultra_plugin_dlss.dll`. They point the
build at a local Streamline SDK (headers/libs) and tell it where to copy the runtime DLLs from:

```text
VULTRA_DLSS_STREAMLINE_SDK_ROOT=C:\path\to\streamline-sdk-vX   (required to build)
VULTRA_DLSS_STREAMLINE_BIN=...\bin\x64                          (optional; derived from SDK root)
```

```text
xmake build -y plugin-dlss-upscaler
```

The build copies the Streamline DLLs next to the plugin DLL (the plugin root), which is exactly
where the plugin looks for them at runtime. Keep NVIDIA SDK sources outside version control.
