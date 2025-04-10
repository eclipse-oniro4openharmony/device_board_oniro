# Copyright (c) 2022 Huawei Device Co., Ltd.
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
mkdir /backdata
mount /dev/block/by-name/userdata /backdata

if [ -d "/backdata/updater/log/faultlog" ];
then
    rm -rf /backdata/updater/log/faultlog/*
    cp /data/log/faultlog/temp/cpp* /backdata/updater/log/faultlog
else
    mkdir /backdata/updater/log/faultlog
    cp /data/log/faultlog/temp/cpp* /backdata/updater/log/faultlog
fi

if [ -d "/backdata/updater/log/hilog" ];
then
    rm -rf /backdata/updater/log/hilog/*
    cp /data/log/hilog/updater* /backdata/updater/log/hilog
else
    mkdir /backdata/updater/log/hilog
    cp /data/log/hilog/updater* /backdata/updater/log/hilog
fi
