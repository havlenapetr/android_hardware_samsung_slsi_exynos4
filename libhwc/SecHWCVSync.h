/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SecVSync_hpp
#define SecVSync_hpp

#include "SecHWCUtils.h"

/*
 * enable hw or sw vsync polling thread
 */
int exynos4_vsync_set_enabled(struct exynos4_hwc_composer_device_1_t *dev, int enabled);

/*
 * start hw or sw vsync polling thread
 */
int exynos4_vsync_init(struct exynos4_hwc_composer_device_1_t *dev);

/*
 * stop vsync thread
 */
int exynos4_vsync_deinit(struct exynos4_hwc_composer_device_1_t *dev);

#endif /* SecVSync_hpp */
