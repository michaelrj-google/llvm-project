! RUN: bbc -emit-fir %s -o - --math-runtime=fast | FileCheck --check-prefixes=ALL,FAST %s
! RUN: %flang_fc1 -emit-fir -mllvm -math-runtime=fast %s -o - | FileCheck --check-prefixes=ALL,FAST %s
! RUN: bbc -emit-fir %s -o - --math-runtime=relaxed | FileCheck --check-prefixes=ALL,RELAXED %s
! RUN: %flang_fc1 -emit-fir -mllvm -math-runtime=relaxed %s -o - | FileCheck --check-prefixes=ALL,RELAXED %s
! RUN: bbc -emit-fir %s -o - --math-runtime=precise | FileCheck --check-prefixes=ALL,PRECISE %s
! RUN: %flang_fc1 -emit-fir -mllvm -math-runtime=precise %s -o - | FileCheck --check-prefixes=ALL,PRECISE %s

function test_real4(x)
  real :: x, test_real4
  test_real4 = acos(x)
end function

! ALL-LABEL: @_QPtest_real4
! FAST: {{%[A-Za-z0-9._]+}} = math.acos {{%[A-Za-z0-9._]+}} {{.*}}: f32
! RELAXED: {{%[A-Za-z0-9._]+}} = math.acos {{%[A-Za-z0-9._]+}} {{.*}}: f32
! PRECISE: {{%[A-Za-z0-9._]+}} = fir.call @acosf({{%[A-Za-z0-9._]+}}) {{.*}}: (f32) -> f32

function test_complex4(x)
  complex :: x, test_complex4
  test_complex4 = acos(x)
end function

! ALL-LABEL: @_QPtest_complex4
! ALL: {{%[A-Za-z0-9._]+}} = fir.call @cacosf({{%[A-Za-z0-9._]+}}) {{.*}}: (complex<f32>) -> complex<f32>

function test_complex8(x)
  complex(kind=8) :: x, test_complex8
  test_complex8 = acos(x)
end function

! ALL-LABEL: @_QPtest_complex8
! ALL: {{%[A-Za-z0-9._]+}} = fir.call @cacos({{%[A-Za-z0-9._]+}}) {{.*}}: (complex<f64>) -> complex<f64>

function test_real8(x)
  real(8) :: x, test_real8
  test_real8 = acos(x)
end function

! ALL-LABEL: @_QPtest_real8
! FAST: {{%[A-Za-z0-9._]+}} = math.acos {{%[A-Za-z0-9._]+}} {{.*}}: f64
! RELAXED: {{%[A-Za-z0-9._]+}} = math.acos {{%[A-Za-z0-9._]+}} {{.*}}: f64
! PRECISE: {{%[A-Za-z0-9._]+}} = fir.call @acos({{%[A-Za-z0-9._]+}}) {{.*}}: (f64) -> f64

! PRECISE-DAG: func.func private @acosf(f32) -> f32 attributes {fir.bindc_name = "acosf", fir.runtime}
! PRECISE-DAG: func.func private @acos(f64) -> f64 attributes {fir.bindc_name = "acos", fir.runtime}
