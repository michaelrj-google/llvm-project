// RUN: %clang_cc1 -verify -fopenmp -ast-print %s | FileCheck %s
// RUN: %clang_cc1 -fopenmp -x c++ -std=c++11 -emit-pch -o %t %s
// RUN: %clang_cc1 -fopenmp -std=c++11 -include-pch %t -verify %s -ast-print | FileCheck %s

// RUN: %clang_cc1 -verify -fopenmp-simd -ast-print %s | FileCheck %s
// RUN: %clang_cc1 -fopenmp-simd -x c++ -std=c++11 -emit-pch -o %t %s
// RUN: %clang_cc1 -fopenmp-simd -std=c++11 -include-pch %t -verify %s -ast-print | FileCheck %s
// expected-no-diagnostics

#ifndef HEADER
#define HEADER

void foo() {}

int main (int argc, char **argv) {
  int b = argc, c, d, e, f, g;
  static int a;
// CHECK: static int a;
#pragma omp parallel
{
#pragma omp masked
{
  a=2;
}

#pragma omp masked filter(1)
{
  a=3;
}

#pragma omp masked filter(a)
{
  a=4;
}
}
// CHECK-NEXT: #pragma omp parallel
// CHECK-NEXT: {
// CHECK-NEXT: #pragma omp masked{{$}}
// CHECK-NEXT: {
// CHECK-NEXT: a = 2;
// CHECK-NEXT: }
// CHECK-NEXT: #pragma omp masked filter(1){{$}}
// CHECK-NEXT: {
// CHECK-NEXT: a = 3;
// CHECK-NEXT: }
// CHECK-NEXT: #pragma omp masked filter(a){{$}}
// CHECK-NEXT: {
// CHECK-NEXT: a = 4;
// CHECK-NEXT: }
// CHECK-NEXT: }
  return (0);
}

#endif
