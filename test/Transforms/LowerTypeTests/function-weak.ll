; RUN: opt -S -lowertypetests -mtriple=i686-unknown-linux-gnu < %s | FileCheck %s
; RUN: opt -S -lowertypetests -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s
; RUN: opt -S -lowertypetests -mtriple=arm-unknown-linux-gnu < %s | FileCheck %s
; RUN: opt -S -lowertypetests -mtriple=aarch64-unknown-linux-gnu < %s | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; CHECK: module asm ".weak f"

; CHECK: @x = global void ()* null, align 8
@x = global void ()* @f, align 8

; CHECK: @x2 = global void ()* null, align 8
@x2 = global void ()* @f, align 8

; CHECK: @x3 = internal global void ()* null, align 8
@x3 = internal constant void ()* @f, align 8

; f + addend
; CHECK: @x4 = global void ()* null, align 8
@x4 = global void ()* bitcast (i8* getelementptr (i8, i8* bitcast (void ()* @f to i8*), i64 42) to void ()*), align 8

; aggregate initializer
; CHECK: @s = global { void ()*, void ()*, i32 } zeroinitializer, align 8
@s = global { void ()*, void ()*, i32 } { void ()* @f, void ()* @f, i32 42 }, align 8

; CHECK: @.cfi.jumptable = external hidden constant [1 x [{{.*}} x i8]]
; CHECK:  @llvm.global_ctors = appending global {{.*}}{ i32 0, void ()* @__cfi_global_var_init

; CHECK: declare !type !0 extern_weak void @f()
declare !type !0 extern_weak void @f()

; CHECK: define zeroext i1 @check_f()
define zeroext i1 @check_f() {
entry:
; CHECK: ret i1 icmp ne (void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), void ()* null)
  ret i1 icmp ne (void ()* @f, void ()* null)
}

; CHECK: define void @call_f() {
define void @call_f() {
entry:
; CHECK: call void select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null)()
  call void @f()
  ret void
}

declare i1 @llvm.type.test(i8* %ptr, metadata %bitset) nounwind readnone

define i1 @foo(i8* %p) {
  %x = call i1 @llvm.type.test(i8* %p, metadata !"typeid1")
  ret i1 %x
}

; CHECK: define internal void @__cfi_global_var_init() section ".text.startup" {
; CHECK-NEXT: entry:
; CHECK-NEXT: store { void ()*, void ()*, i32 } { void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), i32 42 }, { void ()*, void ()*, i32 }* @s, align 8
; CHECK-NEXT: store void ()* bitcast (i8* getelementptr (i8, i8* bitcast (void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null) to i8*), i64 42) to void ()*), void ()** @x4, align 8
; CHECK-NEXT: store void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), void ()** @x3, align 8
; CHECK-NEXT: store void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), void ()** @x2, align 8
; CHECK-NEXT: store void ()* select (i1 icmp ne (void ()* @f, void ()* null), void ()* bitcast ({{.*}}@.cfi.jumptable to void ()*), void ()* null), void ()** @x, align 8
; CHECK-NEXT: ret void
; CHECK-NEXT: }


!0 = !{i32 0, !"typeid1"}
