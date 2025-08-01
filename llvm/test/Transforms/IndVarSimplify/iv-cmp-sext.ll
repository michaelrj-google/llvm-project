; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 5
; RUN: opt < %s -passes=indvars -S | FileCheck %s

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64"

define void @foo(ptr %x, i32 %n) {
; CHECK-LABEL: define void @foo(
; CHECK-SAME: ptr [[X:%.*]], i32 [[N:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[CMP10:%.*]] = icmp sgt i32 [[N]], 0
; CHECK-NEXT:    br i1 [[CMP10]], label %[[FOR_BODY_PREHEADER:.*]], label %[[FOR_COND_CLEANUP:.*]]
; CHECK:       [[FOR_BODY_PREHEADER]]:
; CHECK-NEXT:    [[WIDE_TRIP_COUNT:%.*]] = zext i32 [[N]] to i64
; CHECK-NEXT:    br label %[[FOR_BODY:.*]]
; CHECK:       [[FOR_COND_CLEANUP_LOOPEXIT:.*]]:
; CHECK-NEXT:    br label %[[FOR_COND_CLEANUP]]
; CHECK:       [[FOR_COND_CLEANUP]]:
; CHECK-NEXT:    ret void
; CHECK:       [[FOR_BODY]]:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[INDVARS_IV_NEXT:%.*]], %[[FOR_INC:.*]] ], [ 0, %[[FOR_BODY_PREHEADER]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds nuw i16, ptr [[X]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP0:%.*]] = load i16, ptr [[ARRAYIDX]], align 2
; CHECK-NEXT:    [[CONV:%.*]] = sext i16 [[TMP0]] to i32
; CHECK-NEXT:    [[TMP1:%.*]] = sext i32 [[CONV]] to i64
; CHECK-NEXT:    [[CMP1:%.*]] = icmp eq i64 [[INDVARS_IV]], [[TMP1]]
; CHECK-NEXT:    br i1 [[CMP1]], label %[[IF_THEN:.*]], label %[[FOR_INC]]
; CHECK:       [[IF_THEN]]:
; CHECK-NEXT:    store i16 0, ptr [[ARRAYIDX]], align 2
; CHECK-NEXT:    br label %[[FOR_INC]]
; CHECK:       [[FOR_INC]]:
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add nuw nsw i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp ne i64 [[INDVARS_IV_NEXT]], [[WIDE_TRIP_COUNT]]
; CHECK-NEXT:    br i1 [[EXITCOND]], label %[[FOR_BODY]], label %[[FOR_COND_CLEANUP_LOOPEXIT]]
;
entry:
  %cmp10 = icmp sgt i32 %n, 0
  br i1 %cmp10, label %for.body.preheader, label %for.cond.cleanup

for.body.preheader:                               ; preds = %entry
  br label %for.body

for.cond.cleanup.loopexit:                        ; preds = %for.inc
  br label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond.cleanup.loopexit, %entry
  ret void

for.body:                                         ; preds = %for.body.preheader, %for.inc
  %i.011 = phi i32 [ %inc, %for.inc ], [ 0, %for.body.preheader ]
  %idxprom = zext nneg i32 %i.011 to i64
  %arrayidx = getelementptr inbounds nuw i16, ptr %x, i64 %idxprom
  %0 = load i16, ptr %arrayidx, align 2
  %conv = sext i16 %0 to i32
  %cmp1 = icmp eq i32 %i.011, %conv
  br i1 %cmp1, label %if.then, label %for.inc

if.then:                                          ; preds = %for.body
  store i16 0, ptr %arrayidx, align 2
  br label %for.inc

for.inc:                                          ; preds = %for.body, %if.then
  %inc = add nuw nsw i32 %i.011, 1
  %cmp = icmp slt i32 %inc, %n
  br i1 %cmp, label %for.body, label %for.cond.cleanup.loopexit
}
