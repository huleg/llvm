//===-- AVRSubtarget.cpp - AVR Subtarget Information ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the AVR specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "AVR.h"
#include "AVRSubtarget.h"
#include "AVRTargetMachine.h"
#include "llvm/Support/TargetRegistry.h"

#define DEBUG_TYPE "avr-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "AVRGenSubtargetInfo.inc"

using namespace llvm;


AVRSubtarget::AVRSubtarget(const std::string &TT, const std::string &CPU,
                           const std::string &FS, AVRTargetMachine &TM) :
  AVRGenSubtargetInfo(TT, CPU, FS),
  InstrInfo(),
  FrameLowering(),
  TLInfo(TM),
  TSInfo(*TM.getDataLayout()),
  
  // Subtarget features
  m_hasSRAM(false), m_hasJMPCALL(false), m_hasIJMPCALL(false), m_hasEIJMPCALL(false),
  m_hasADDSUBIW(false), m_hasSmallStack(false), m_hasMOVW(false), m_hasLPM(false),
  m_hasLPMX(false), m_hasELPM(false), m_hasELPMX(false), m_hasSPM(false), m_hasSPMX(false),
  m_hasDES(false), m_supportsRMW(false), m_supportsMultiplication(false),
  m_hasBREAK(false), m_hasTinyEncoding(false), m_DummyFeature(false)
{
  // Parse features string.
  ParseSubtargetFeatures(CPU, FS);
}

