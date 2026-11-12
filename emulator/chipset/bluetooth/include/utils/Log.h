/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef BT_VENDOR_LOG_H
#define BT_VENDOR_LOG_H

#include "hilog/log.h"

#define HILOGD(...) HiLogPrint(LOG_CORE, LOG_DEBUG, LOG_DOMAIN, "BTVENDOR", __VA_ARGS__)
#define HILOGI(...) HiLogPrint(LOG_CORE, LOG_INFO, LOG_DOMAIN, "BTVENDOR", __VA_ARGS__)
#define HILOGW(...) HiLogPrint(LOG_CORE, LOG_WARN, LOG_DOMAIN, "BTVENDOR", __VA_ARGS__)
#define HILOGE(...) HiLogPrint(LOG_CORE, LOG_ERROR, LOG_DOMAIN, "BTVENDOR", __VA_ARGS__)

#endif