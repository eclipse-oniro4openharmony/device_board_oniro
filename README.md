# Oniro Board Support Packages

This repository contains Board Support Packages (BSPs) for devices supported by the Oniro Project, including the Volla X23 phone and the QEMU virtual device. These BSPs enable developers to build and deploy Oniro on supported hardware.

## Oniro Emulator (std_emulator target)

This guide provides step-by-step instructions to **build and run the Oniro Emulator** using the `OpenHarmony-6.0-Release` source. 

### 📦 Prerequisites

Before proceeding, make sure you have followed the [Quick Build Setup](https://docs.oniroproject.org/device-development/building-oniro.html) guide to prepare your build environment.

### ⬇️ Download Oniro Source Code

Use the following commands to fetch the Oniro source:

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git -b OpenHarmony-6.0-Release -m oniro.xml --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

### 🩹 Apply source patches

Run the patching script:

```bash
bash vendor/oniro/std_emulator/hook/do_patch.sh
```

### 🛠️ Build the images

Start the build with ccache enabled:

```bash
./build.sh --product-name std_emulator --ccache --gn-args allow_sanitize_debug=true
```

### 🔄 (Optional) Revert patches

If needed, you can undo the applied patches:

```bash
bash vendor/oniro/std_emulator/hook/undo_patch.sh
```

### ▶️ Run the emulator

After a successful build, emulator image files can be found at:

```
out/std_emulator/packages/phone/images
```

#### On Windows

```bash
.\run.bat
```

#### On Linux

```bash
./run.sh
```

## Additional targets

- **x23**: target for Volla X23 device. Oniro can be run on the Volla X23 using a layered approach, with Ubuntu Touch as the base OS and Oniro running in an LXC container. Detailed instructions are available in the [Volla X23 documentation](./docs/volla_x23.md).

## Graphics Stack

Oniro leverages the OpenHarmony graphics stack, which includes the ArkUI framework, Render Service, and Hardware Abstraction through HDI. More details can be found in the [OpenHarmony Graphics Stack Overview](./docs/graphical_stack.md).

## Contributing

Contributions to improve the board support packages are welcome. Please submit a pull request with your proposed changes.

## License

This repository is distributed under the Apache 2.0 License.

