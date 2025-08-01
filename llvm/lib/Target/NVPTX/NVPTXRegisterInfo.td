//===-- NVPTXRegisterInfo.td - NVPTX Register defs ---------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//  Declarations that describe the PTX register file
//===----------------------------------------------------------------------===//

class NVPTXReg<string n> : Register<n> {
  let Namespace = "NVPTX";
}

class NVPTXRegClass<list<ValueType> regTypes, int alignment, dag regList>
     : RegisterClass <"NVPTX", regTypes, alignment, regList>;

//===----------------------------------------------------------------------===//
//  Registers
//===----------------------------------------------------------------------===//

// Special Registers used as stack pointer
def VRFrame32         : NVPTXReg<"%SP">;
def VRFrame64         : NVPTXReg<"%SP">;
def VRFrameLocal32    : NVPTXReg<"%SPL">;
def VRFrameLocal64    : NVPTXReg<"%SPL">;

// Special Registers used as the stack
def VRDepot  : NVPTXReg<"%Depot">;

// We use virtual registers, but define a few physical registers here to keep
// SDAG and the MachineInstr layers happy.
foreach i = 0...4 in {
  def P#i  : NVPTXReg<"%p"#i>;  // Predicate
  def RS#i : NVPTXReg<"%rs"#i>; // 16-bit
  def R#i  : NVPTXReg<"%r"#i>;  // 32-bit
  def RL#i : NVPTXReg<"%rd"#i>; // 64-bit
  def RQ#i : NVPTXReg<"%rq"#i>; // 128-bit
}

foreach i = 0...31 in {
  def ENVREG#i : NVPTXReg<"%envreg"#i>;
}

//===----------------------------------------------------------------------===//
//  Register classes.
//  NOTE: if you add new vector types for a register, you must update
//        NVPTX::packed_types() in NVPTXUtilities.h accordingly!
//===----------------------------------------------------------------------===//
def B1 : NVPTXRegClass<[i1], 8, (add (sequence "P%u", 0, 4))>;
def B16 : NVPTXRegClass<[i16, f16, bf16], 16, (add (sequence "RS%u", 0, 4))>;
def B32 : NVPTXRegClass<[i32, v2f16, v2bf16, v2i16, v4i8, f32], 32,
                              (add (sequence "R%u", 0, 4),
                              VRFrame32, VRFrameLocal32)>;
def B64 : NVPTXRegClass<[i64, v2f32, f64], 64, (add (sequence "RL%u", 0, 4),
                         VRFrame64, VRFrameLocal64)>;
// 128-bit regs are not defined as general regs in NVPTX. They are used for inlineASM only.
def B128 : NVPTXRegClass<[i128], 128, (add (sequence "RQ%u", 0, 4))>;

// Read NVPTXRegisterInfo.cpp to see how VRFrame and VRDepot are used.
def SpecialRegs : NVPTXRegClass<[i32], 32, (add VRFrame32, VRFrameLocal32, VRDepot,
                                            (sequence "ENVREG%u", 0, 31))>;
