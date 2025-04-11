# Oniro Board Support Packages

This repository contains Board Support Packages (BSPs) for devices supported by the Oniro Project, including the Volla X23 phone and the QEMU virtual device. These BSPs enable developers to build and deploy Oniro on supported hardware.

## Oniro Emulator (std_emulator target)

This guide provides step-by-step instructions to **build and run the Oniro Emulator** using the `OpenHarmony-5.0.2-Release` source. 

### 📦 Prerequisites

Before proceeding, make sure you have followed the [Quick Build Setup](https://docs.oniroproject.org/quick-build.html) guide to prepare your build environment.

### ⬇️ Download Oniro Source Code

Use the following commands to fetch the Oniro source:

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git -b OpenHarmony-5.0.2-Release -m oniro.xml --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

### 🧰 Switch to Required Kernel Version

Replace the default kernel with the required version:

```bash
rm -rf kernel/linux/linux-6.6
git clone -b weekly_20241216 https://gitee.com/openharmony/kernel_linux_6.6.git kernel/linux/linux-6.6 --depth=1
```

### 🩹 Apply source patches

Run the patching script:

```bash
bash vendor/oniro/std_emulator/hook/hook_start.sh
```

### 🛠️ Build the images

Start the build with ccache enabled:

```bash
./build.sh --product-name std_emulator --ccache
```

### 🔄 (Optional) Revert patches

If needed, you can undo the applied patches:

```bash
bash vendor/oobemulator/std_emulator/hook/hook_end.sh
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
- **qemu**: this target facilitates the generation of rootfs that can be used by [meta-openharmony](https://github.com/eclipse-oniro4openharmony/meta-openharmony) for a Yocto based build. 

## Graphics Stack

Oniro leverages the OpenHarmony graphics stack, which includes the ArkUI framework, Render Service, and Hardware Abstraction through HDI. More details can be found in the [OpenHarmony Graphics Stack Overview](./docs/graphical_stack.md).

## Contributing

Contributions to improve the board support packages are welcome. Please submit a pull request with your proposed changes.

## License

This repository is distributed under the Apache 2.0 License.

