# Oniro Board Support Packages

This repository contains Board Support Packages (BSPs) for devices supported by the Oniro Project, including the Volla X23 phone and the QEMU virtual device. These BSPs enable developers to build and deploy Oniro on supported hardware.

## Documentation

### Supported Devices

- **Volla X23**: Oniro can be run on the Volla X23 using a layered approach, with Ubuntu Touch as the base OS and Oniro running in an LXC container. Detailed instructions are available in the [Volla X23 documentation](./docs/volla_x23.md).
- **QEMU Virtual Device**: A virtualized target for testing and development in a QEMU environment.

### Graphics Stack

Oniro leverages the OpenHarmony graphics stack, which includes the ArkUI framework, Render Service, and Hardware Abstraction through HDI. More details can be found in the [OpenHarmony Graphics Stack Overview](./docs/graphical_stack.md).

## Getting Started

To start building and deploying Oniro for supported devices, refer to the device-specific documentation linked above. Ensure that you have set up your build environment according to the [Oniro Quick Build Guide](https://docs.oniroproject.org/eclipse-oniro-project/building-oniro.html).

## Contributing

Contributions to improve the board support packages are welcome. Please submit a pull request with your proposed changes.

## License

This repository is distributed under the Apache 2.0 License.

