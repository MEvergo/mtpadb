# Third-party license notices

## Android Open Source Project ADB

The Android integration patches modify AOSP ADB source files and reuse their public implementation through the pinned AOSP build targets. The source pins are:

- `platform/system/core`, tag `android-11.0.0_r48`, commit [`348efca472d810d3152568913da41a081893a4e3`](https://android.googlesource.com/platform/system/core/+/refs/tags/android-11.0.0_r48/).
- `platform/packages/modules/adb`, commit [`1cf2f017d312f73b3dc53bda85ef2610e35a80e9`](https://android.googlesource.com/platform/packages/modules/adb/+/1cf2f017d312f73b3dc53bda85ef2610e35a80e9/).

AOSP ADB source is distributed under the Apache License 2.0. Existing AOSP source-file copyright and SPDX notices remain in the upstream files modified by the patch series; new files authored here are not represented as upstream AOSP source. Consult the pinned source trees for the applicable upstream license texts and notices.

The project also refers to Linux FunctionFS/configfs and Android `frameworks/av/media/mtp` sources for interface research in `docs/upstream-notes.md`; their implementation files are not vendored here.

## Project license

No project-wide license was specified for MTPADB. Do not infer a license grant for project-authored source from this directory or from the Apache-2.0 license of AOSP source. Select and add a project license before publishing project-authored source for reuse.
