# Security Policy

This project follows the Eclipse Foundation Vulnerability Reporting Policy:

* https://www.eclipse.org/security/

## Reporting a Vulnerability

Please report vulnerabilities that are not yet publicly disclosed by email to
the Eclipse Foundation Security Team at security@eclipse-foundation.org. Do
NOT open a public GitHub issue for an undisclosed vulnerability.

## Scope

This repository contains the board support packages (BSP): board-level configuration, kernel build and boot/flash tooling for the supported boards (e.g. the hybris_generic Volla X23 board and the x86_general emulator board). Please report here only vulnerabilities in
content authored by this project:

* Vulnerabilities in upstream OpenHarmony components (the
  `eclipse-oniro-mirrors` repositories) should be reported to the upstream
  OpenAtom OpenHarmony project.
* Vulnerabilities in the Linux kernel should be reported upstream
  (see https://docs.kernel.org/process/security-bugs.html).
* Vulnerabilities in third-party vendor binaries fetched at build time (e.g.
  Halium / vendor blobs) should be reported to their respective vendors.

## Supported Versions

Only the most recent release branch (`OpenHarmony-6.1-Release`) is supported with security updates. Older release branches are not maintained.
