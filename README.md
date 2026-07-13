# ExplorerBgToolRe(dux)

## Overview
**ExplorerBgToolRe(dux)** is a complete rewrite and enhancement of the original [ExplorerBgTool](https://github.com/Maplespe/explorerTool) by Maple, a shell extension used to customize the Windows Explorer background with custom images. 

This project started as a **fork** to revive the original shell extension, but ultimately resulted in an extensive **code refactoring and rewriting**, critical **bug fixes** and brand **new features**.

## Architecture Changes and Improvements
The original design has been restructured and multiple bugs have been fixed, especially regarding concurrency issues and memory leaks, as documented in the source code. The existing codebase has been cleaned and improved, including a complete rewrite of many core and generic functions, as well as specific components like the image management class.

### From Global Maps to Thread Local Storage (TLS)
The most significant architectural change is the removal of the original shared graphic data map and a complete flip of the original approach. Instead of fighting with shared data and increasingly complex locking mechanisms, I turned the problem on its head by replacing the shared graphics map with the native Win32 **Thread Local Storage (TLS)**. Thanks to TLS, each thread now owns its isolated graphic data block, making it completely independent and eliminating the need for access locks.

The synchronization locks in the original codebase were either inconsistent or entirely absent (such as in the `[]` operator usage), which may have led to synchronization issues and memory corruption. On the other hand, introducing global mutexes disrupted the thread timing within Explorer itself, causing sudden deadlocks and unexpected crashes (an explanation of these lock-related issues is documented in the `dllmain.cpp` source file).

In other words, instead of designing complex and over-engineered synchronization classes, the architecture was redesigned to grant each individual thread its own private memory space using Thread Local Storage.

## New Features

In addition to fixing and refactoring/rewriting the original code, the following new features have been introduced:

- **Dedicated loader (`ebtl.exe`):** developed a standalone utility, available in the [ebtl](https://github.com/lpierge/ebtl) repository, to handle the automated installation, registration, unregistration, reloading of the DLL and command-line Explorer restarts. It automatically requests Administrator privileges when required, fully replacing the original and unreliable batch file mechanism.
- **Process whitelist:** added a configurable whitelist to specify exactly which programs are authorized to load the DLL.
- **File dialog backgrounds:** created a dedicated whitelist to allow specified applications to load the DLL exclusively for changing the background image of standard "Open/Save File" dialogs.
- **Window subclassing:** subclassed the Explorer window to dynamically alter transparency levels based on whether the window is in the foreground or background.
- **Special folders separation:** separated special folder images from the main "Image" directory pool, allowing users to specify full custom pathnames for individual target folders.
- **Wildcard support:** integrated wildcard `*` matching support within special folder section names, allowing a single background image configuration to recursively apply to all matching subdirectories.
- **Configurable image formats:** added the ability to explicitly define allowed image file formats in the configuration file instead of hardcoding them into the binary.
- **Image safeguard limits:** introduced a strict limit on the maximum number of images that can be loaded to prevent memory exhaustion, performance degradation and potential Explorer crashes.

## Project dependencies
Source files that are not part of the core **ExplorerBgToolRe** project but are used by it as external dependencies can be found in the **Include** and **Library** repositories. Therefore, to compile this project, you need to download the following components:

* [ExplorerBgToolRe](https://github.com/lpierge/ExplorerBgToolRe) — this project
* [ebtl](https://github.com/lpierge/ebtl) — ebtl loader files
* [Include](https://github.com/lpierge/Include) — Shared header (.h) files
* [Library](https://github.com/lpierge/Library) — Shared source (.c/.cpp) files

While the [ebtl](https://github.com/lpierge/ebtl) loader is _not required_ to compile or link the DLL, _it is  absolutely required_ for managing the installation, registration, unregistration and reloading processes, as well as restarting Explorer when needed. See the related [repository](https://github.com/lpierge/ebtl) and the below **Windows binaries and Installer** section.

## Implementation notes

Most of the code rewrite is located within the `dllmain.*` and `WinAPI.*` source files. The `ShellLoader.*` files has been patched in specific targeted sections, while other minor corrections throughout the codebase are explicitly marked with the `//LPI` comment tag.

Debugging a DLL is quite tricky, especially when it is an Explorer extension. Therefore, instead of using standard debugging techniques, when compiled in DEBUG mode the DLL outputs all debug information to a dedicated TRACE window: a real life example of the famous principle “less is more”. The traceexpr.* files contain all the related code.

**Important note on projects structure:**

The Visual Studio project for **ExplorerBgToolRe** is hardcoded to search for dependencies using absolute paths starting from the root of a virtual L: drive. The expected directory structure is as follows:

```text
L:\
  |-- ExplorerBgToolRe\
  |-- Include\
  |-- Library\
```
Instead of changing the Visual Studio settings in the project file, I recommend mapping a local folder to a virtual L: drive with the Windows SUBST command:
- Create a directory on your local drive, for example `C:\DEV`.
- Download and extract all the repositories inside that directory.
- Open the Windows Command Prompt (press `Win + R` to open the Run dialog, type `cmd.exe` and press `Enter`) and from the Console run the following command: `SUBST L: C:\DEV`

## Windows binaries and Installer

The [Installer](https://github.com/lpierge/ExplorerBgToolRe/tree/main/Installer) directory in the [ExplorerBgToolRe](https://github.com/lpierge/ExplorerBgToolRe) repository contains the DLL loader (**ebtl.exe**), already compiled for Windows and provided in a zipped archive.

After downloading and unzipping the file, open a Command Prompt (press `Win + R`, type `cmd.exe` and press Enter), navigate to the folder where you extracted the `ebtl.exe` file, close all the running programs and run the following command:

`ebtl -i`

This will install and register the DLL in the default folder `C:\ExplorerBgToolRe`. The installation process will also create two subdirectories (`Image` and `Chibi`), containing sample images and a `config.ini` configuration file. Make sure to read the comments inside the `config.ini` carefully before modifying it.

P.S. If you are into the manga/anime genre and have run out of sources to download images from, or if you are tired of manually saving them one by one, [Calimero](https://github.com/lpierge/Calimero/tree/main/wchg/res#readme) (a Windows desktop utility available [here](https://github.com/lpierge/Calimero/tree/main/Installer)) includes a module to download and manage images directly from sites like **Picsum**, **Pexels**, **Reddit** and **Danbooru**. Reddit and Danbooru are excellent sources for this kind of artworks, just keep in mind that most content on Danbooru is NSFW.

## Screenshots
_**This PC and Documents folders**_

![Calimero](https://i.ibb.co/x8SkYLXM/screenshot01.jpg)

_**Standard (foreground) and Special (background) folders**_

![Calimero](https://i.ibb.co/9H1s2vpG/screenshot02.jpg)

_**Standard folders**_

![Calimero](https://i.ibb.co/F4GfFkv5/screenshot03.jpg)

Luca P.
