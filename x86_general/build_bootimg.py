#!/usr/bin/env python3

# Copyright (c) 2024 Institute of Software, Chinese Academy of Sciences.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys
import os
import os.path
import subprocess
import multiprocessing
import shutil

root=sys.argv[1]
# root="/home/dy/opc6.0"
def remove_file(name):
    try:
        os.unlink(name)
    except FileNotFoundError:
        return

def remove_directory(name):
    try:
        shutil.rmtree(name)
    except FileNotFoundError:
        pass

def make_boot_img():

    output_dir=root+"/out/x86_general/packages/phone/images"
    input_dir=root+"/device/board/oniro/x86_general/loader"
    oldpwd = os.getcwd()
    os.chdir(output_dir)

    imagefile = 'boot.img'
    imagefile_tmp = imagefile + '.tmp'
    boot_dir = 'x86boot'
    remove_directory(boot_dir)
    remove_file(imagefile)
    os.makedirs(boot_dir, exist_ok=True)
    # shutil.copytree(input_dir, boot_dir)
    for root2, _, files in os.walk(input_dir):
        for file in files:
            src_path = os.path.join(root2, file)
            dst_path = os.path.join(boot_dir, os.path.relpath(src_path, input_dir))
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            shutil.copy2(src_path, dst_path)
    shutil.copy(
        os.path.join(root+'/out/x86_general/kernel/OBJ/linux-6.6/arch/x86_64/boot/bzImage'),
        boot_dir
    )
    shutil.copy(
    os.path.join(root+'/out/x86_general/packages/phone/images/ramdisk.img'),
    boot_dir
    )
    with open(imagefile_tmp, 'wb') as writer:
        writer.truncate(512*1024*1024)
    subprocess.run(F'mkfs.vfat -F 32 {imagefile_tmp} -n BOOT', shell=True, check=True)
    subprocess.run(F'mcopy -i {imagefile_tmp} -s {boot_dir}/* ::/', shell=True, check=True)
    os.rename(imagefile_tmp, imagefile)

    os.chdir(oldpwd)


command_table = {
    'makeboot': make_boot_img,
}

command_table["makeboot"]()