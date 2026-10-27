[![Build and Test](https://github.com/mikekazakov/nimble-commander/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/mikekazakov/nimble-commander/actions/workflows/build.yml)
[![Nightly Build](https://github.com/mikekazakov/nimble-commander/actions/workflows/nightly.yml/badge.svg?branch=main)](https://github.com/mikekazakov/nimble-commander/actions/workflows/nightly.yml)

# About this fork

This is a fork of [Nimble Commander](https://github.com/mikekazakov/nimble-commander) by Michael Kazakov.

**Why it exists.** I needed a fast, keyboard-driven, dual-pane file manager on macOS with an integrated terminal, SFTP
and Amazon S3 access. My Commander One license ran out of re-activations after a macOS upgrade and paying again
wasn't justified; muCommander was too slow. Nimble Commander already had everything - native speed, the Norton
Commander style workflow, an overlapped terminal, SFTP with key files - except S3. So this fork adds it.

**What it adds.** A native S3 virtual file system: AWS S3 and S3-compatible storages (MinIO, Cloudflare R2, Wasabi,
Backblaze B2, ...) browsed directly in the file panels, with reading, writing (including multipart uploads of
arbitrarily large files), directories, renames, server-side copies, keys in the Keychain or taken from the AWS CLI
profiles. No mounting and no external tools. Details, limitations and the design are in [Docs/S3.md](Docs/S3.md).

Everything else follows upstream. The code stays under the GNU General Public License version 3.

---

# Nimble Commander
![](Docs/nc.png)
Nimble Commander is a free dual-pane file manager for macOS, designed with a focus on speed, keyboard-based navigation, and flexibility.  
The project's aim is to blend the user experience of classic file managers from the '80s-'90s with the modern look and feel of Mac computers.  
Visit the project's website at: https://magnumbytes.com.  

# Getting Nimble Commander

## Nightly Builds
You can download the latest nightly build of the app from [GitHub Actions](https://github.com/mikekazakov/nimble-commander/actions/workflows/nightly.yml).  
Go to the most recent integration and select the `nimble-commander-nightly` in the `Artifacts` section (a GitHub account is required).

## Current Release
Direct download link: https://magnumbytes.com/downloads/releases/nimble-commander.dmg.  
Available on Mac App Store: https://itunes.apple.com/app/files-lite/id905202937?ls=1&mt=12.  
Install via Homebrew: `brew install nimble-commander`.  

## Past Releases
Access all previous releases at https://github.com/mikekazakov/nimble-commander-releases. 

# Documentation
The user guide can be found here: [Help.md](Docs/Help.md).

# Changelog
Details about each released version can be found in the [WHATS_NEW.md](WHATS_NEW.md) file.

# Building from Source
For build instructions and an overview of the source code, please refer to the [Building.md](Docs/Building.md) document.

# Contributing to the Project
Your contributions are welcome and greatly appreciated! For guidelines on how to contribute, including reporting bugs, suggesting features, and submitting code changes, please see the [CONTRIBUTING.md](CONTRIBUTING.md) file.

# License
Copyright (C) 2013-2026 Michael Kazakov (mike.kazakov@gmail.com)  
The source code is distributed under GNU General Public License version 3.
