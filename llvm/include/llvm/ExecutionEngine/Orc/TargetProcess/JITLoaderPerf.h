//===------- JITLoaderPerf.h --- Register profiler objects ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Register objects for access by profilers via the perf JIT interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_JITLOADERPERF_H
#define LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_JITLOADERPERF_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>

extern "C" LLVM_ABI llvm::orc::shared::CWrapperFunctionResult
llvm_orc_registerJITLoaderPerfImpl(const char *ArgData, size_t ArgSize);

extern "C" LLVM_ABI llvm::orc::shared::CWrapperFunctionResult
llvm_orc_registerJITLoaderPerfStart(const char *ArgData, size_t ArgSize);

extern "C" LLVM_ABI llvm::orc::shared::CWrapperFunctionResult
llvm_orc_registerJITLoaderPerfEnd(const char *ArgData, size_t ArgSize);

#endif // LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_JITLOADERPERF_H
