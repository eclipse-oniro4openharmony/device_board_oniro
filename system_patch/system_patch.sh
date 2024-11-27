#!/bin/bash
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

set -e

PROJECT_ROOT=$(pwd)/
PATCH_SRC_PATH=${PROJECT_ROOT}device/board/oniro/system_patch

#whitelist
cp -arfL ${PATCH_SRC_PATH}/whitelist/compile_standard_whitelist.json ${PROJECT_ROOT}build/compile_standard_whitelist.json

#graphic_2d
cp -arfL ${PATCH_SRC_PATH}/graphic_2d/rs_draw_cmd.cpp ${PROJECT_ROOT}foundation/graphic/graphic_2d/rosen/modules/render_service_base/src/pipeline/rs_draw_cmd.cpp
cp -arfL ${PATCH_SRC_PATH}/graphic_2d/surface_image.cpp ${PROJECT_ROOT}foundation/graphic/graphic_2d/frameworks/surfaceimage/src/surface_image.cpp


# patching base/startup/init
PATCH_FILE="${PROJECT_ROOT}device/board/oniro/system_patch/base_startup_init/base_startup_init.patch"

cd "${PROJECT_ROOT}base/startup/init" || exit 1

# Check if the patch has been applied
if git apply --check "$PATCH_FILE" >/dev/null 2>&1; then
  echo "The patch has not been applied yet. Applying the patch..."
  git apply "$PATCH_FILE"
  if [ $? -eq 0 ]; then
    echo "Patch applied successfully."
  else
    echo "Failed to apply the patch."
    exit 1
  fi
else
  echo "The patch is already applied or cannot be applied."
fi