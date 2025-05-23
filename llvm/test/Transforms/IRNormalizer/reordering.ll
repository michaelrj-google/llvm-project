; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 4
; RUN: opt -S -passes='normalize<no-rename-all>' -verify-each < %s | FileCheck %s

define void @foo() {
; CHECK-LABEL: define void @foo() {
; CHECK-NEXT:  bb17254:
; CHECK-NEXT:    ret void
;
  ret void
}

define void @empty_basic_block() {
; CHECK-LABEL: define void @empty_basic_block() {
; CHECK-NEXT:  exit:
; CHECK-NEXT:    ret void
;
exit:
  ret void
}

declare void @effecting()

; Place dead instruction(s) before the terminator
define void @call_effecting() {
; CHECK-LABEL: define void @call_effecting() {
; CHECK-NEXT:  bb15160:
; CHECK-NEXT:    call void @effecting()
; CHECK-NEXT:    [[TMP0:%.*]] = add i32 0, 1
; CHECK-NEXT:    ret void
;
  %1 = add i32 0, 1
  call void @effecting()
  ret void
}

define void @dont_move_above_phi() {
; CHECK-LABEL: define void @dont_move_above_phi() {
; CHECK-NEXT:  bb76951:
; CHECK-NEXT:    br label [[EXIT:%.*]]
; CHECK:       exit:
; CHECK-NEXT:    [[TMP0:%.*]] = phi i32 [ 0, [[BB76951:%.*]] ]
; CHECK-NEXT:    call void @effecting()
; CHECK-NEXT:    ret void
;
  br label %exit
exit:
  %1 = phi i32 [0, %0]
  call void @effecting()
  ret void
}

define void @dont_move_above_alloca() {
; CHECK-LABEL: define void @dont_move_above_alloca() {
; CHECK-NEXT:  bb15160:
; CHECK-NEXT:    [[TMP0:%.*]] = alloca i32, align 4
; CHECK-NEXT:    call void @effecting()
; CHECK-NEXT:    ret void
;
  %1 = alloca i32
  call void @effecting()
  ret void
}

declare void @effecting1()

define void @dont_reorder_effecting() {
; CHECK-LABEL: define void @dont_reorder_effecting() {
; CHECK-NEXT:  bb10075:
; CHECK-NEXT:    call void @effecting()
; CHECK-NEXT:    call void @effecting1()
; CHECK-NEXT:    ret void
;
  call void @effecting()
  call void @effecting1()
  ret void
}

declare void @effecting2(i32)

define void @dont_reorder_effecting1() {
; CHECK-LABEL: define void @dont_reorder_effecting1() {
; CHECK-NEXT:  bb10075:
; CHECK-NEXT:    [[ONE:%.*]] = add i32 1, 1
; CHECK-NEXT:    call void @effecting2(i32 [[ONE]])
; CHECK-NEXT:    [[TWO:%.*]] = add i32 2, 2
; CHECK-NEXT:    call void @effecting2(i32 [[TWO]])
; CHECK-NEXT:    ret void
;
  %one = add i32 1, 1
  %two = add i32 2, 2
  call void @effecting2(i32 %one)
  call void @effecting2(i32 %two)
  ret void
}

define void @dont_reorder_across_blocks() {
; CHECK-LABEL: define void @dont_reorder_across_blocks() {
; CHECK-NEXT:  bb76951:
; CHECK-NEXT:    [[ONE:%.*]] = add i32 1, 1
; CHECK-NEXT:    br label [[EXIT:%.*]]
; CHECK:       exit:
; CHECK-NEXT:    call void @effecting2(i32 [[ONE]])
; CHECK-NEXT:    ret void
;
  %one = add i32 1, 1
  br label %exit
exit:
  call void @effecting2(i32 %one)
  ret void
}

define void @independentldst(ptr %a, ptr %b) {
; CHECK-LABEL: define void @independentldst(
; CHECK-SAME: ptr [[A:%.*]], ptr [[B:%.*]]) {
; CHECK-NEXT:  bb10495:
; CHECK-NEXT:    %"vl12961([[B]])" = load i32, ptr [[B]], align 4
; CHECK-NEXT:    store i32 %"vl12961([[B]])", ptr [[A]], align 4
; CHECK-NEXT:    %"vl89528([[A]])" = load i32, ptr [[A]], align 4
; CHECK-NEXT:    store i32 %"vl89528([[A]])", ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %2 = load i32, ptr %a
  %3 = load i32, ptr %b
  store i32 %3, ptr %a
  store i32 %2, ptr %b
  ret void
}

define void @multiple_use_ld(ptr %a, ptr %b) {
; CHECK-LABEL: define void @multiple_use_ld(
; CHECK-SAME: ptr [[A:%.*]], ptr [[B:%.*]]) {
; CHECK-NEXT:  bb14927:
; CHECK-NEXT:    %"vl16793([[A]])" = load i32, ptr [[A]], align 4
; CHECK-NEXT:    store i32 %"vl16793([[A]])", ptr [[A]], align 4
; CHECK-NEXT:    %"vl89528([[B]])" = load i32, ptr [[B]], align 4
; CHECK-NEXT:    store i32 %"vl89528([[B]])", ptr [[B]], align 4
; CHECK-NEXT:    store i32 %"vl16793([[A]])", ptr [[A]], align 4
; CHECK-NEXT:    ret void
;
  %2 = load i32, ptr %a
  store i32 %2, ptr %a
  %3 = load i32, ptr %b
  store i32 %3, ptr %b
  store i32 %2, ptr %a
  ret void
}

; This is an incorrect transformation. Moving the store above the load could
; change the loaded value. To do this, the pointers would need to be `noalias`.
define void @undef_st(ptr %a, ptr %b) {
; CHECK-LABEL: define void @undef_st(
; CHECK-SAME: ptr [[A:%.*]], ptr [[B:%.*]]) {
; CHECK-NEXT:  bb10495:
; CHECK-NEXT:    store i32 undef, ptr [[B]], align 4
; CHECK-NEXT:    %"vl16028([[A]])" = load i32, ptr [[A]], align 4
; CHECK-NEXT:    store i32 %"vl16028([[A]])", ptr [[A]], align 4
; CHECK-NEXT:    ret void
;
  %2 = load i32, ptr %a
  store i32 undef, ptr %b
  store i32 %2, ptr %a
  ret void
}
