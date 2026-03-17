#!/usr/bin/env python3
"""
Create dedicated binderfs devices for the OpenHarmony LXC container.

Must be run as root before ohos.service starts. The host Android servicemanager
registers as context manager on /dev/binderfs/binder first; OHOS samgr must use
separate devices (ohos-binder, ohos-hwbinder, ohos-vndbinder) to avoid collision.

Kernel struct (include/uapi/linux/android/binderfs.h):
  struct binderfs_device {
      char name[256];   // BINDERFS_MAX_NAME + 1
      __u32 major;      // NOTE: __u32, not __u8 — using __u8 gives EINVAL
      __u32 minor;
  };
  #define BINDER_CTL_ADD _IOWR('b', 1, struct binderfs_device)
  // = 0xC1086201 (struct size 264 bytes)
"""
import ctypes
import fcntl
import os
import sys

BINDERFS_MAX_NAME = 255
BINDER_CONTROL = '/dev/binderfs/binder-control'


class BinderfsDevice(ctypes.Structure):
    _fields_ = [
        ('name', ctypes.c_char * (BINDERFS_MAX_NAME + 1)),
        ('major', ctypes.c_uint32),
        ('minor', ctypes.c_uint32),
    ]


def _IOC(direction, type_, nr, size):
    return (direction << 30) | (size << 16) | (ord(type_) << 8) | nr


BINDER_CTL_ADD = _IOC(3, 'b', 1, ctypes.sizeof(BinderfsDevice))

DEVICES = [b'ohos-binder', b'ohos-hwbinder', b'ohos-vndbinder']

if os.geteuid() != 0:
    print('ERROR: must run as root', file=sys.stderr)
    sys.exit(1)

fd = os.open(BINDER_CONTROL, os.O_RDONLY)
errors = 0
for name in DEVICES:
    path = f'/dev/binderfs/{name.decode()}'
    if os.path.exists(path):
        print(f'Already exists: {path}')
        os.chmod(path, 0o666)
        continue
    dev = BinderfsDevice()
    dev.name = name
    dev.major = 0
    dev.minor = 0
    try:
        fcntl.ioctl(fd, BINDER_CTL_ADD, dev)
        os.chmod(path, 0o666)
        print(f'Created {path} (major={dev.major}, minor={dev.minor})')
    except OSError as e:
        print(f'ERROR creating {name.decode()}: {e}', file=sys.stderr)
        errors += 1
os.close(fd)
sys.exit(errors)
