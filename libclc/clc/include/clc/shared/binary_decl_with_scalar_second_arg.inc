//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

_CLC_OVERLOAD _CLC_DECL __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                               __CLC_GENTYPE y);

#ifndef __CLC_SCALAR
_CLC_OVERLOAD _CLC_DECL __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                               __CLC_SCALAR_GENTYPE y);
#endif
