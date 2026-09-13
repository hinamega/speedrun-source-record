[English](README.md) | [日本語](README.ja.md)

# Speedrun Source Record

An OBS Studio filter plugin for speedrunners, based on [obs-source-record](https://github.com/exeldro/obs-source-record) by [exeldro](https://github.com/exeldro).

This fork integrates with LiveSplit to automate clean-feed source recording during RTA runs.

## Features

- LiveSplit integration: Automatically starts recording on run start, and stops when the run ends.
- Pre-buffer & Lossless Auto-Concat: Captures N seconds (default 5s) before the timer starts. Upon completing a run, the pre-buffer and main run video are automatically stitched together into a single file via OBS's bundled FFmpeg engine with zero quality loss (lossless stream copy). No external FFmpeg CLI installation is required.
- Post-buffer: Continues recording for a configurable duration after the run finishes.
- Dynamic naming: Supports `%game%`, `%category%`, and `%attempt%` placeholders in the filename formatting.
- Reset run isolation: Automatically moves incomplete recordings to a `reset_runs` subfolder when a run is reset, keeping completed runs tidy without risk of accidental data loss. Pre-buffer temporary files are automatically deleted on reset.
- Video-only option: Allows bypassing audio encoding to minimize system load.

## Audio

The filter records the audio of the source it is attached to, so attach it to your **game capture source**:

- Game audio is recorded automatically (the filtered source's audio).
- The microphone is **not** recorded by default, because it is a separate OBS source.
- **Video Only (Skip Audio)** records no audio at all. It is off by default, so leave it off to keep game audio.

> [!IMPORTANT]
> The filtered source must actually output audio. For OBS Game Capture, enable "Capture Audio"; alternatively use an **Application Audio Capture** source and select it under the filter's "Different Audio" option. Do not attach the filter to a Scene, or the microphone may be mixed in.

To record the microphone or a specific OBS audio track, enable **Different Audio** and choose the source/track.

## Requirements & Setup

### 1. LiveSplit Setup
This plugin communicates via LiveSplit's TCP server protocol (default port: 16834). Follow the instructions below depending on your LiveSplit version:

#### Standard Release (LiveSplit 1.8.x)
1. Download the latest `LiveSplit.Server.zip` from the [LiveSplit.Server Releases page](https://github.com/LiveSplit/LiveSplit.Server/releases).
2. Extract the contents (`LiveSplit.Server.dll`, etc.) into your `LiveSplit/Components/` directory.
3. Open LiveSplit, right-click -> `Edit Layout` -> click `+` -> `Control` -> add `LiveSplit Server`, then click `OK`.
4. Right-click on LiveSplit -> `Control` -> `Start Server` (default port: 16834).

#### Development Build (LiveSplit DevBuild)
1. The development build has the TCP server built-in.
2. Right-click on LiveSplit -> `Control` -> `Start TCP Server` (default port: 16834).

> [!NOTE]
> Regardless of version, the server must be started manually each time you launch LiveSplit.

### 2. OBS Studio Setup
1. In OBS Studio, add the "Speedrun Source Record" filter to your desired capture source.
2. Set the Record Mode to "LiveSplit (Speedrun)".
3. Configure the filename formatting as desired (e.g. `%game%_%category%_Run%attempt%_%CCYY-%MM-%DD_%hh-%mm-%ss`).
4. Configure the Pre-buffer duration, Post-buffer duration, and the "Move Reset Runs to 'reset_runs' Folder" option as needed.

## Credits & License

This project is a fork of [obs-source-record](https://github.com/exeldro/obs-source-record) by exeldro.  
Licensed under the GNU General Public License v2.0 (GPL-2.0).

Designed, implemented, reviewed, and optimized with the assistance of Google Gemini and DeepSeek V4.1 Flash.
