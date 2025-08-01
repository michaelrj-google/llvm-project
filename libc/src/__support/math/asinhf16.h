//===-- Implementation header for asinhf16 ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_ASINHF16_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_ASINHF16_H

#include "include/llvm-libc-macros/float16-macros.h"

#ifdef LIBC_TYPES_HAS_FLOAT16

#include "acoshf_utils.h"
#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/PolyEval.h"
#include "src/__support/FPUtil/cast.h"
#include "src/__support/FPUtil/except_value_utils.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/FPUtil/rounding_mode.h"
#include "src/__support/FPUtil/sqrt.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace math {

LIBC_INLINE static constexpr float16 asinhf16(float16 x) {

#ifndef LIBC_MATH_HAS_SKIP_ACCURATE_PASS
  constexpr size_t N_EXCEPTS = 8;

  constexpr fputil::ExceptValues<float16, N_EXCEPTS> ASINHF16_EXCEPTS{{
      // (input, RZ output, RU offset, RD offset, RN offset)

      // x = 0x1.da4p-2, asinhf16(x) = 0x1.ca8p-2 (RZ)
      {0x3769, 0x372a, 1, 0, 1},
      // x = 0x1.d6cp-1, asinhf16(x) = 0x1.a58p-1 (RZ)
      {0x3b5b, 0x3a96, 1, 0, 0},
      // x = 0x1.c7cp+3, asinhf16(x) = 0x1.accp+1 (RZ)
      {0x4b1f, 0x42b3, 1, 0, 0},
      // x = 0x1.26cp+4, asinhf16(x) = 0x1.cd8p+1 (RZ)
      {0x4c9b, 0x4336, 1, 0, 1},
      // x = -0x1.da4p-2, asinhf16(x) = -0x1.ca8p-2 (RZ)
      {0xb769, 0xb72a, 0, 1, 1},
      // x = -0x1.d6cp-1, asinhf16(x) = -0x1.a58p-1 (RZ)
      {0xbb5b, 0xba96, 0, 1, 0},
      // x = -0x1.c7cp+3, asinhf16(x) = -0x1.accp+1 (RZ)
      {0xcb1f, 0xc2b3, 0, 1, 0},
      // x = -0x1.26cp+4, asinhf16(x) = -0x1.cd8p+1 (RZ)
      {0xcc9b, 0xc336, 0, 1, 1},
  }};
#endif // !LIBC_MATH_HAS_SKIP_ACCURATE_PASS

  using namespace acoshf_internal;
  using FPBits = fputil::FPBits<float16>;
  FPBits xbits(x);

  uint16_t x_u = xbits.uintval();
  uint16_t x_abs = x_u & 0x7fff;

  if (LIBC_UNLIKELY(xbits.is_inf_or_nan())) {
    if (xbits.is_signaling_nan()) {
      fputil::raise_except_if_required(FE_INVALID);
      return FPBits::quiet_nan().get_val();
    }

    return x;
  }

#ifndef LIBC_MATH_HAS_SKIP_ACCURATE_PASS
  // Handle exceptional values
  if (auto r = ASINHF16_EXCEPTS.lookup(x_u); LIBC_UNLIKELY(r.has_value()))
    return r.value();
#endif // !LIBC_MATH_HAS_SKIP_ACCURATE_PASS

  float xf = x;
  const float SIGN[2] = {1.0f, -1.0f};
  float x_sign = SIGN[x_u >> 15];

  // |x| <= 0.25
  if (LIBC_UNLIKELY(x_abs <= 0x3400)) {
    // when |x| < 0x1.718p-5, asinhf16(x) = x. Adjust by 1 ULP for certain
    // rounding types.
    if (LIBC_UNLIKELY(x_abs < 0x29c6)) {
      int rounding = fputil::quick_get_round();
      if ((rounding == FE_UPWARD || rounding == FE_TOWARDZERO) && xf < 0)
        return fputil::cast<float16>(xf + 0x1p-24f);
      if ((rounding == FE_DOWNWARD || rounding == FE_TOWARDZERO) && xf > 0)
        return fputil::cast<float16>(xf - 0x1p-24f);
      return fputil::cast<float16>(xf);
    }

    float x_sq = xf * xf;
    // Generated by Sollya with:
    // > P = fpminimax(asinh(x)/x, [|0, 2, 4, 6, 8|], [|SG...|], [0, 2^-2]);
    // The last coefficient 0x1.bd114ep-6f has been changed to 0x1.bd114ep-5f
    // for better accuracy.
    float p = fputil::polyeval(x_sq, 1.0f, -0x1.555552p-3f, 0x1.332f6ap-4f,
                               -0x1.6c53dep-5f, 0x1.bd114ep-5f);

    return fputil::cast<float16>(xf * p);
  }

  // General case: asinh(x) = ln(x + sqrt(x^2 + 1))
  float sqrt_term = fputil::sqrt<float>(fputil::multiply_add(xf, xf, 1.0f));
  return fputil::cast<float16>(
      x_sign * log_eval(fputil::multiply_add(xf, x_sign, sqrt_term)));
}

} // namespace math

} // namespace LIBC_NAMESPACE_DECL

#endif // LIBC_TYPES_HAS_FLOAT16

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_ASINHF16_H
