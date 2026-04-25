<div align="center">

# Token-Talks

**A native, high-performance Discord client built in C++ with zero browser overhead.**

![Platform](https://img.shields.io/badge/platform-Windows-blue?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B17-informational?style=flat-square)
![Renderer](https://img.shields.io/badge/renderer-DirectX%2011%20%2B%20ImGui-blueviolet?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

</div>

---

## Overview

Token-Talks is a native Discord desktop client developed in C++. By bypassing Electron and traditional web-based runtimes, it achieves extremely low latency and a minimal resource footprint. The application utilizes DirectX 11 for rendering and Dear ImGui for its highly optimized user interface.

---

## Core Features

### Messaging
- **Modern Chat Interface**: High-fidelity layout featuring circular avatars, display names, and optimized message spacing.
- **Infinite Scrolling**: Automatic lazy-loading of message history as the user scrolls.
- **Direct Messaging**: Dedicated support for private conversations and group DMs.
- **Reactions**: View and interact with emoji reactions on any message.
- **Message Lifecycle**: Full integration for sending, editing, and deleting messages.

### Media & Rendering
- **GIF Playback**: High-performance GIF decoding and real-time playback.
- **Native Video**: Integrated video support utilizing the Windows Media Foundation (WMF) pipeline.
- **Lightbox**: Full-screen overlay for detailed inspection of images and video content.
- **Asynchronous Pipeline**: Media assets are downloaded and decoded on background threads to ensure consistent 60+ FPS performance.

### Audio & Voice (Experimental)
- **Voice Channels**: Support for joining guild voice channels and initiating direct calls.
- **Audio Routing**: Comprehensive configuration for input/output device selection and volume control.
- **Signaling**: Integrated ringing and call state management.
- **Codec Support**: Built with **libopus** for encoding/decoding and **libsodium** for encryption.

### Customization & Privacy
- **Themes**: Multiple built-in professional themes (Blurple, Midnight, Ruby, Light, Amethyst).
- **Custom Typography**: Support for external `.ttf` and `.otf` font assets.
- **Visibility Control**: Toggle to show or hide locked/private channels.

---

## Technical Specifications

| Component | Technology |
|---|---|
| **UI Framework** | Dear ImGui (Docking) |
| **Renderer** | DirectX 11 (D3D11) |
| **Networking** | Native Windows WinHTTP |
| **Audio Codec** | libopus |
| **Encryption** | libsodium |
| **Video Pipeline** | Windows Media Foundation |
| **Image Pipeline** | stb_image |
| **JSON Parser** | nlohmann/json |

---

## Building from Source

### Prerequisites
- Windows 10 / 11
- Visual Studio 2022 (Desktop C++ Workload)
- All dependencies are included in the `/vendor` directory.

### Build Instructions

1. Clone the repository:
   ```bash
   git clone https://github.com/wallski/Token-Talks.git
   ```
2. Open `dailyrem.sln` in Visual Studio 2022.
3. Select the **Release | x64** configuration.
4. Execute **Build → Build Solution**.
5. The executable is located in the `x64/Release` directory.

---

## Roadmap

- [ ] **Voice E2EE (MLS)**: Implementation of Discord's mandatory end-to-end encryption for voice connections.
- [ ] **Global Search**: Integration for message and server-wide searching.
- [ ] **Discord RPC**: Native Game Activity and Rich Presence support.
- [ ] **Extensibility**: Support for custom themes and client-side plugins.

---

<div align="center">

Built with native C++ and zero Electron overhead.

</div>
