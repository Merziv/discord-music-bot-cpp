# Discord Music Bot

A high-performance Discord music bot written in modern C++. Streams audio from YouTube and other sources directly to Discord voice channels using yt-dlp for extraction and FFmpeg for transcoding.

## Features

- **YouTube playback** - Play individual videos or entire playlists
- **Search support** - Search YouTube directly with queries
- **Queue management** - Add, remove, move, and shuffle tracks
- **Playback controls** - Play, pause, resume, skip, stop
- **Now playing** - See what's currently playing with elapsed time
- **Auto-disconnect** - Leaves voice channel after inactivity
- **Low latency** - Lock-free ring buffer for smooth audio streaming

## Commands

| Command | Aliases | Description |
|---------|---------|-------------|
| `!play <query>` | `!p` | Play a YouTube URL or search query |
| `!playtop <query>` | `!ptop` | Add track to front of queue |
| `!skip` | | Skip current track |
| `!pause` | | Pause playback |
| `!resume` | `!unpause` | Resume playback |
| `!stop` | | Stop playback and clear queue |
| `!queue` | `!q` | Show current queue |
| `!shuffle` | | Shuffle the queue |
| `!remove <pos>` | `!rm` | Remove track at position |
| `!move <from> <to>` | `!mv` | Move track in queue |
| `!clear` | | Clear the queue |
| `!nowplaying` | `!np` | Show current track |
| `!uptime` | | Show bot uptime |

## Requirements

### Build-time

- C++23 compatible compiler
- CMake 3.15+
- Conan 2.x

### Runtime

- FFmpeg (system package)
- yt-dlp (pip/pipx)

## Configuration

```bash
cp config.example.json config.json
```

| Field | Description |
|-------|-------------|
| `token` | Discord bot token |
| `startup_message` | Message logged on startup |
| `status_playing_game` | Discord "Playing" status |
| `command_prefix` | Command prefix (default: `!`) |
| `channel_id` | Channel ID where bot responds |
| `reaction_image` | Emoji for "Now playing" messages |
| `idle_timeout_minutes` | Disconnect after inactivity |

## Running

```bash
./music_bot [config.json]
```

## Architecture

- **Lock-free ring buffer** - Audio frame buffering between producer/consumer threads
- **Producer thread** - Runs FFmpeg, reads PCM data, fills ring buffer
- **Consumer thread** - Encodes to Opus, sends to Discord with precise timing
- **yt-dlp integration** - Extracts stream URLs with multiple client fallbacks

## License

MIT

---

## Author's Setup (Optional Reference)

This project was developed with:

- Clang 21 on WSL2 (Ubuntu)
- Conan 2.x with custom profile for clang
- CMake with Unix Makefiles generator

Example Conan profile (`~/.conan2/profiles/clang`):

```ini
[settings]
arch=x86_64
build_type=Release
compiler=clang
compiler.cppstd=23
compiler.version=20
os=Linux

[buildenv]
CC=clang-21
CXX=clang++-21
```

Build with profile:

```bash
conan install . --output-folder=build --build=missing --profile=clang
```
