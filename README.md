<div align="center">

# Token-Talks

**A native, high-performance Discord client built entirely in C++ with zero browser overhead.**

![Platform](https://img.shields.io/badge/platform-Windows-blue?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B17-informational?style=flat-square)
![Renderer](https://img.shields.io/badge/renderer-DirectX%2011%20%2B%20ImGui-blueviolet?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

</div>

---

## What is Token-Talks?

Token-Talks is a fully featured Discord desktop client written from scratch in native C++.  
No Electron. No web engine. No heavy runtimes. Just raw DirectX 11 rendering, WinHTTP networking, and ImGui — giving you a lightweight, ultra-fast messaging experience.

---

## Features

### Messaging
- Real-time message receiving via **Discord Gateway WebSockets**
- Send messages to any text channel
- **Edit** and **Delete** your own messages via right-click context menu
- Full message history loading (last 50 messages)

### Media
- Inline image and GIF rendering powered by **stb_image + DirectX 11 textures**
- Multiple attachments per message displayed correctly
- File upload support (PNG, JPG, GIF) via native Windows file picker
- Async media downloading — UI never freezes while images load

### Navigation
- Full **Server → Channel → Chat** three-panel layout
- Private/locked channel detection with role permission parsing (including category inheritance)
- Toggle to show or hide private channels you don't have access to

### Accounts
- Multi-account support — save and switch between accounts instantly
- Token validation before saving (verified against Discord API)
- Tokens stored **XOR-encrypted + hex-encoded** on disk — never plain text

### Themes & Settings
- 5 built-in professional themes:
  - **Modern Blurple** — Discord-inspired dark mode
  - **Midnight Stealth** — pure OLED black
  - **Ruby Crimson** — deep red aesthetic
  - **Classic Light** — clean white mode
  - **Amethyst Dark** — deep purple dark mode
- Settings persist across restarts via `settings.json`

---

## Tech Stack

| Component | Technology |
|---|---|
| UI Framework | [Dear ImGui](https://github.com/ocornut/imgui) (Docking branch) |
| Renderer | DirectX 11 |
| Networking | Windows WinHTTP (native, no libcurl) |
| Image Decoding | [stb_image](https://github.com/nothings/stb) |
| JSON Parsing | [nlohmann/json](https://github.com/nlohmann/json) |
| Build System | Visual Studio 2022 (MSVC) |

---

## Building

### Requirements
- Windows 10 / 11
- Visual Studio 2022 with **Desktop C++ workload**
- No external package manager needed — all dependencies are vendored

### Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/wallski/Token-Talks.git
   ```

2. Open `dailyrem.sln` in Visual Studio 2022

3. Set configuration to **Release | x64**

4. Hit **Build → Build Solution**

5. Run the output executable

> The app hides the console window on launch. Everything happens in the ImGui window.

---

## Usage

1. Launch the app
2. On the dashboard, enter a **display name** and your **Discord token**
3. Click **Add Account** — the token is validated against the API before saving
4. Click your account name to log in
5. Browse your servers → channels → chat

> **Right-click** any message you sent to edit or delete it.  
> Click the **`+`** button next to the chat input to upload an image.  
> Access **Settings** from the top-right button to change themes or toggle private channels.

---

## Security Notice

Token-Talks stores your Discord token locally in an encrypted format (`accounts.dat`).  
Tokens are XOR-obfuscated and hex-encoded before being written to disk.

> ⚠️ **Never share your `accounts.dat` file or your raw Discord token with anyone.**  
> Using a user token against Discord's Terms of Service is done at your own risk.

---

## Roadmap

- [ ] Animated GIF playback
- [ ] Video support  
- [ ] Voice channel support (libopus + libsodium)
- [ ] Message reactions
- [ ] Direct Messages panel
- [ ] Custom font support in Settings

---

<div align="center">

Built with 💜 using pure C++ and zero Electron.

</div>
