// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv5 %s | FileCheck %s -check-prefix CHECK-V5
// CHECK-V5: #define __HEXAGON_ARCH__ 5
// CHECK-V5: #define __HEXAGON_V5__ 1
// CHECK-V5-NOT: #define __HVX_LENGTH__
// CHECK-V5-NOT: #define __HVX__ 1
// CHECK-V5: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv55 %s | FileCheck %s -check-prefix CHECK-V55
// CHECK-V55: #define __HEXAGON_ARCH__ 55
// CHECK-V55: #define __HEXAGON_V55__ 1
// CHECK-V55-NOT: #define __HVX_LENGTH__
// CHECK-V55-NOT: #define __HVX__ 1
// CHECK-V55: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv60 %s | FileCheck %s -check-prefix CHECK-V60
// CHECK-V60: #define __HEXAGON_ARCH__ 60
// CHECK-V60: #define __HEXAGON_V60__ 1
// CHECK-V60-NOT: #define __HVX_LENGTH__
// CHECK-V60-NOT: #define __HVX__ 1
// CHECK-V60: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv62 %s | FileCheck %s -check-prefix CHECK-V62
// CHECK-V62: #define __HEXAGON_ARCH__ 62
// CHECK-V62: #define __HEXAGON_V62__ 1
// CHECK-V62-NOT: #define __HVX_LENGTH__
// CHECK-V62-NOT: #define __HVX__ 1
// CHECK-V62: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv65 %s | FileCheck %s -check-prefix CHECK-V65
// CHECK-V65: #define __HEXAGON_ARCH__ 65
// CHECK-V65: #define __HEXAGON_V65__ 1
// CHECK-V65-NOT: #define __HVX_LENGTH__
// CHECK-V65-NOT: #define __HVX__ 1
// CHECK-V65: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv66 %s | FileCheck %s -check-prefix CHECK-V66
// CHECK-V66: #define __HEXAGON_ARCH__ 66
// CHECK-V66: #define __HEXAGON_V66__ 1
// CHECK-V66-NOT: #define __HVX_LENGTH__
// CHECK-V66-NOT: #define __HVX__ 1
// CHECK-V66: #define __hexagon__ 1

// The HVX flags are explicitly defined by the driver.
// For v60,v62,v65 - 64B mode is default
// For v66 and future archs - 128B is default
// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv60 \
// RUN: -target-feature +hvxv60 -target-feature +hvx-length64b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V60HVX-64B
// CHECK-V60HVX-64B: #define __HEXAGON_ARCH__ 60
// CHECK-V60HVX-64B: #define __HEXAGON_V60__ 1
// CHECK-V60HVX-64B-NOT: #define __HVXDBL__ 1
// CHECK-V60HVX-64B: #define __HVX_ARCH__ 60
// CHECK-V60HVX-64B: #define __HVX_LENGTH__ 64
// CHECK-V60HVX-64B: #define __HVX__ 1
// CHECK-V60HVX-64B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv60 \
// RUN: -target-feature +hvxv60 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V60HVX-128B
// CHECK-V60HVX-128B: #define __HEXAGON_ARCH__ 60
// CHECK-V60HVX-128B: #define __HEXAGON_V60__ 1
// CHECK-V60HVX-128B: #define __HVXDBL__ 1
// CHECK-V60HVX-128B: #define __HVX_ARCH__ 60
// CHECK-V60HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V60HVX-128B: #define __HVX__ 1
// CHECK-V60HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv66 \
// RUN: -target-feature +hvxv66 -target-feature +hvx-length64b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V66HVX-64B
// CHECK-V66HVX-64B: #define __HEXAGON_ARCH__ 66
// CHECK-V66HVX-64B: #define __HEXAGON_V66__ 1
// CHECK-V66HVX-64B-NOT: #define __HVXDBL__ 1
// CHECK-V66HVX-64B: #define __HVX_ARCH__ 66
// CHECK-V66HVX-64B: #define __HVX_LENGTH__ 64
// CHECK-V66HVX-64B: #define __HVX__ 1
// CHECK-V66HVX-64B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv66 \
// RUN: -target-feature +hvxv66 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V66HVX-128B
// CHECK-V66HVX-128B: #define __HEXAGON_ARCH__ 66
// CHECK-V66HVX-128B: #define __HEXAGON_V66__ 1
// CHECK-V66HVX-128B: #define __HVXDBL__ 1
// CHECK-V66HVX-128B: #define __HVX_ARCH__ 66
// CHECK-V66HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V66HVX-128B: #define __HVX__ 1
// CHECK-V66HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv67 \
// RUN: -target-feature +hvxv67 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V67HVX-128B
// CHECK-V67HVX-128B: #define __HEXAGON_ARCH__ 67
// CHECK-V67HVX-128B: #define __HEXAGON_V67__ 1
// CHECK-V67HVX-128B: #define __HVX_ARCH__ 67
// CHECK-V67HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V67HVX-128B: #define __HVX__ 1
// CHECK-V67HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv68 \
// RUN: -target-feature +hvxv68 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V68HVX-128B
// CHECK-V68HVX-128B: #define __HEXAGON_ARCH__ 68
// CHECK-V68HVX-128B: #define __HEXAGON_V68__ 1
// CHECK-V68HVX-128B: #define __HVX_ARCH__ 68
// CHECK-V68HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V68HVX-128B: #define __HVX__ 1
// CHECK-V68HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv69 \
// RUN: -target-feature +hvxv69 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V69HVX-128B
// CHECK-V69HVX-128B: #define __HEXAGON_ARCH__ 69
// CHECK-V69HVX-128B: #define __HEXAGON_V69__ 1
// CHECK-V69HVX-128B: #define __HVX_ARCH__ 69
// CHECK-V69HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V69HVX-128B: #define __HVX__ 1
// CHECK-V69HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv71 \
// RUN: -target-feature +hvxv71 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V71HVX-128B
// CHECK-V71HVX-128B: #define __HEXAGON_ARCH__ 71
// CHECK-V71HVX-128B: #define __HEXAGON_V71__ 1
// CHECK-V71HVX-128B: #define __HVX_ARCH__ 71
// CHECK-V71HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V71HVX-128B: #define __HVX__ 1
// CHECK-V71HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv73 \
// RUN: -target-feature +hvxv73 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V73HVX-128B
// CHECK-V73HVX-128B: #define __HEXAGON_ARCH__ 73
// CHECK-V73HVX-128B: #define __HEXAGON_V73__ 1
// CHECK-V73HVX-128B: #define __HVX_ARCH__ 73
// CHECK-V73HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V73HVX-128B: #define __HVX__ 1
// CHECK-V73HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv75 %s\
// RUN: | FileCheck %s -check-prefix CHECK-V75
// CHECK-V75: #define __HEXAGON_ARCH__ 75
// CHECK-V75: #define __HEXAGON_PHYSICAL_SLOTS__ 4
// CHECK-V75: #define __HEXAGON_V75__ 1
// CHECK-V75: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv75 \
// RUN: -target-feature +hvxv75 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V75HVX-128B
// CHECK-V75HVX-128B: #define __HEXAGON_ARCH__ 75
// CHECK-V75HVX-128B: #define __HEXAGON_V75__ 1
// CHECK-V75HVX-128B: #define __HVX_ARCH__ 75
// CHECK-V75HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V75HVX-128B: #define __HVX__ 1
// CHECK-V75HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv79 %s\
// RUN: | FileCheck %s -check-prefix CHECK-V79
// CHECK-V79: #define __HEXAGON_ARCH__ 79
// CHECK-V79: #define __HEXAGON_PHYSICAL_SLOTS__ 4
// CHECK-V79: #define __HEXAGON_V79__ 1
// CHECK-V79: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv79 \
// RUN: -target-feature +hvxv79 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-V79HVX-128B
// CHECK-V79HVX-128B: #define __HEXAGON_ARCH__ 79
// CHECK-V79HVX-128B: #define __HEXAGON_V79__ 1
// CHECK-V79HVX-128B: #define __HVX_ARCH__ 79
// CHECK-V79HVX-128B: #define __HVX_LENGTH__ 128
// CHECK-V79HVX-128B: #define __HVX__ 1
// CHECK-V79HVX-128B: #define __hexagon__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-elf -target-cpu hexagonv67 \
// RUN: -target-feature +hvxv67 -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-ELF
// CHECK-ELF: #define __ELF__ 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-linux-musl \
// RUN: -target-cpu hexagonv67 -target-feature +hvxv67 \
// RUN: -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-LINUX
// CHECK-LINUX: #define __gnu_linux__ 1
// CHECK-LINUX: #define __linux 1
// CHECK-LINUX: #define __linux__ 1
// CHECK-LINUX: #define __unix 1
// CHECK-LINUX: #define __unix__ 1
// CHECK-LINUX: #define linux 1
// CHECK-LINUX: #define unix 1

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-linux-musl \
// RUN: -target-cpu hexagonv67 -target-feature +hvxv67 \
// RUN: -target-feature +hvx-length128b %s | FileCheck \
// RUN: %s -check-prefix CHECK-ATOMIC
// CHECK-ATOMIC: #define __CLANG_ATOMIC_BOOL_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_CHAR16_T_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_CHAR32_T_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_CHAR_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_INT_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_LLONG_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_LONG_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_POINTER_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_SHORT_LOCK_FREE 2
// CHECK-ATOMIC: #define __CLANG_ATOMIC_WCHAR_T_LOCK_FREE 2

// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-linux-musl \
// RUN: -target-cpu hexagonv67 | FileCheck \
// RUN: %s -check-prefix CHECK-INTERFERENCE
// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-none-elf \
// RUN: -target-cpu hexagonv67 | FileCheck \
// RUN: %s -check-prefix CHECK-INTERFERENCE
// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-none-elf \
// RUN: -target-cpu hexagonv71t | FileCheck \
// RUN: %s -check-prefix CHECK-INTERFERENCE
// CHECK-INTERFERENCE: #define __GCC_CONSTRUCTIVE_SIZE 32
// CHECK-INTERFERENCE: #define __GCC_DESTRUCTIVE_SIZE 32
// RUN: %clang_cc1 -E -dM -triple hexagon-unknown-none-elf \
// RUN: -target-cpu hexagonv73 | FileCheck \
// RUN: %s -check-prefix CHECK-INTERFERENCE-73
// CHECK-INTERFERENCE-73: #define __GCC_CONSTRUCTIVE_SIZE 64
// CHECK-INTERFERENCE-73: #define __GCC_DESTRUCTIVE_SIZE 64
