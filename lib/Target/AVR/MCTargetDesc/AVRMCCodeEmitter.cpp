//===-- AVRMCCodeEmitter.cpp - Convert AVR Code to Machine Code -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the AVRMCCodeEmitter class.
//
//===----------------------------------------------------------------------===//
//

#include "AVRMCCodeEmitter.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/raw_ostream.h"

#include "MCTargetDesc/AVRMCExpr.h"
#include "MCTargetDesc/AVRMCTargetDesc.h"

#define DEBUG_TYPE "mccodeemitter"

#define GET_INSTRMAP_INFO
#include "AVRGenInstrInfo.inc"
#undef GET_INSTRMAP_INFO

namespace llvm {

/// Performs a post-encoding step on a `LD` or `ST` instruction.
///
/// The encoding of the LD/ST family of instructions is inconsistent w.r.t
/// the pointer register and the addressing mode.
///
/// The permutations of the format are as followed:
/// ld Rd, X    `1001 000d dddd 1100`
/// ld Rd, X+   `1001 000d dddd 1101`
/// ld Rd, -X   `1001 000d dddd 1110`
///
/// ld Rd, Y    `1000 000d dddd 1000`
/// ld Rd, Y+   `1001 000d dddd 1001`
/// ld Rd, -Y   `1001 000d dddd 1010`
///
/// ld Rd, Z    `1000 000d dddd 0000`
/// ld Rd, Z+   `1001 000d dddd 0001`
/// ld Rd, -Z   `1001 000d dddd 0010`
///                 ^
///                 |
/// Note this one inconsistent bit - it is 1 sometimes and 0 at other times.
/// There is no logical pattern. Looking at a truth table, the following
/// formula can be derived to fit the pattern:
/// ```
/// inconsistent_bit = is_predec OR is_postinc OR is_reg_x
/// ```
/// We manually set this bit in the post encoder method.
unsigned
AVRMCCodeEmitter::loadStorePostEncoder(const MCInst &MI, unsigned EncodedValue,
                                       const MCSubtargetInfo &STI) const {

  assert(MI.getOperand(0).isReg() && MI.getOperand(1).isReg() &&
         "the load/store operands must be registers");

  auto opcode = MI.getOpcode();

  // check whether either of the registers are the X pointer register.
  bool isRegX = (MI.getOperand(0).getReg() == AVR::R27R26) ||
                (MI.getOperand(1).getReg() == AVR::R27R26);

  bool isPredec = opcode == AVR::LDRdPtrPd || opcode == AVR::STPtrPdRr;

  bool isPostinc = opcode == AVR::LDRdPtrPi || opcode == AVR::STPtrPiRr;

  // check if we need to set the inconsistent bit
  if (isRegX || isPredec || isPostinc) {
    EncodedValue |= (1 << 12);
  }

  return EncodedValue;
}

template <AVR::Fixups Fixup>
unsigned
AVRMCCodeEmitter::encodeRelCondBrTarget(const MCInst &MI, unsigned OpNo,
                                        SmallVectorImpl<MCFixup> &Fixups,
                                        const MCSubtargetInfo &STI) const {
  MCOperand const &MO = MI.getOperand(OpNo);

  if (MO.isExpr()) {
    const MCOperand &MO = MI.getOperand(OpNo);
    const MCExpr *Expr = MO.getExpr();
    Fixups.push_back(MCFixup::create(0, Expr, MCFixupKind(Fixup), MI.getLoc()));
    return 0;
  }

  assert(MO.isImm());

  // take the size of the current instruction away.
  // with labels, this is implicitly handled.
  auto target = MO.getImm();
  AVR::fixups::adjustBranchTarget(target);
  return target;
}

unsigned AVRMCCodeEmitter::encodeLDSTPtrReg(const MCInst &MI, unsigned OpNo,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  // the operand should be a pointer register.
  assert(MI.getOperand(OpNo).isReg());

  auto MO = MI.getOperand(OpNo);

  unsigned encoding;

  switch (MO.getReg()) {
  case AVR::R27R26:
    encoding = 0x03;
    break; // X 0b11
  case AVR::R29R28:
    encoding = 0x02;
    break; // Y 0b10
  case AVR::R31R30:
    encoding = 0x00;
    break; // Z 0b00
  default:
    llvm_unreachable("invalid pointer register");
    break;
  }

  return encoding;
}

/// Encodes a `memri` operand.
/// The operand is 7-bits.
/// * The lower 6 bits is the immediate
/// * The upper bit is the pointer register bit (Z=0,Y=1)
unsigned AVRMCCodeEmitter::encodeMemri(const MCInst &MI, unsigned OpNo,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  auto RegOp = MI.getOperand(OpNo);
  auto OffsetOp = MI.getOperand(OpNo + 1);

  assert(RegOp.isReg() && "Expected register operand");

  uint8_t RegBit = 0;

  switch (RegOp.getReg()) {
  default:
    llvm_unreachable("Expected either Y or Z register");
  case AVR::R31R30:
    RegBit = 0;
    break; // Z register
  case AVR::R29R28:
    RegBit = 1;
    break; // Y register
  }

  int8_t OffsetBits;

  if (OffsetOp.isImm()) {
    OffsetBits = OffsetOp.getImm();
  } else if (OffsetOp.isExpr()) {
    OffsetBits = 0;
    Fixups.push_back(MCFixup::create(0, OffsetOp.getExpr(), MCFixupKind(AVR::fixup_6), MI.getLoc()));
  } else {
    llvm_unreachable("invalid value for offset");
  }

  return (RegBit << 6) | OffsetBits;
}

unsigned AVRMCCodeEmitter::encodeComplement(const MCInst &MI, unsigned OpNo,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  // The operand should be a pointer register.
  assert(MI.getOperand(OpNo).isImm());
  auto imm = MI.getOperand(OpNo).getImm();

  return (~0) - imm;
}

template <AVR::Fixups Fixup>
unsigned AVRMCCodeEmitter::encodeImm(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  auto MO = MI.getOperand(OpNo);


  if (MO.isExpr()) {
    if (isa<AVRMCExpr>(MO.getExpr())) {
      // If the expression is already an AVRMCExpr (i.e. a lo8(symbol),
      // we shouldn't perform any more fixups. Without this check, we would
      // instead create a fixup to the symbol named 'lo8(symbol)' which
      // is not correct.
      return getExprOpValue(MO.getExpr(), Fixups, STI);
    } else {
      MCFixupKind FixupKind = static_cast<MCFixupKind>(Fixup);
      Fixups.push_back(MCFixup::create(0, MO.getExpr(), FixupKind, MI.getLoc()));

      return 0;
    }
  }

  assert(MO.isImm());

  return MO.getImm();
}

unsigned AVRMCCodeEmitter::encodeCallTarget(const MCInst &MI, unsigned OpNo,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  auto MO = MI.getOperand(OpNo);

  if (MO.isExpr()) {
    MCFixupKind FixupKind = static_cast<MCFixupKind>(AVR::fixup_call);
    Fixups.push_back(MCFixup::create(0, MO.getExpr(), FixupKind, MI.getLoc()));
    return 0;
  }

  assert(MO.isImm());

  auto target = MO.getImm();
  AVR::fixups::adjustBranchTarget(target);
  return target;
}

unsigned AVRMCCodeEmitter::getExprOpValue(const MCExpr *Expr,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {

  MCExpr::ExprKind Kind = Expr->getKind();

  if (Kind == MCExpr::Binary) {
    Expr = static_cast<const MCBinaryExpr *>(Expr)->getLHS();
    Kind = Expr->getKind();
  }

  if (Kind == MCExpr::Target) {
    AVRMCExpr const *AVRExpr = cast<AVRMCExpr>(Expr);
    int64_t Result;
    if (AVRExpr->evaluateAsConstant(Result)) {
      return Result;
    }

    MCFixupKind FixupKind = static_cast<MCFixupKind>(AVRExpr->getFixupKind());
    Fixups.push_back(MCFixup::create(0, AVRExpr, FixupKind));
    return 0;
  }

  assert(Kind == MCExpr::SymbolRef);
  return 0;
}

unsigned AVRMCCodeEmitter::getMachineOpValue(const MCInst &MI,
                                             const MCOperand &MO,
                                             SmallVectorImpl<MCFixup> &Fixups,
                                             const MCSubtargetInfo &STI) const {
  if (MO.isReg()) {
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());
  } else if (MO.isImm()) {
    return static_cast<unsigned>(MO.getImm());
  } else if (MO.isFPImm()) {
    return static_cast<unsigned>(APFloat(MO.getFPImm())
                                     .bitcastToAPInt()
                                     .getHiBits(32)
                                     .getLimitedValue());
  }

  // MO must be an Expr.
  assert(MO.isExpr());

  return getExprOpValue(MO.getExpr(), Fixups, STI);
}

void AVRMCCodeEmitter::emitByte(unsigned char C, raw_ostream &OS) const {
  OS << (char)C;
}

void AVRMCCodeEmitter::emitWord(uint16_t word, raw_ostream &OS) const {
  emitByte((word & 0x00ff) >> 0, OS);
  emitByte((word & 0xff00) >> 8, OS);
}

void AVRMCCodeEmitter::emitWords(uint16_t const *Words, size_t Count,
                                 raw_ostream &OS) const {
  for (int64_t i = Count - 1; i >= 0; --i) {
    emitWord(Words[i], OS);
  }
}

void AVRMCCodeEmitter::emitInstruction(uint64_t Val, unsigned Size,
                                       const MCSubtargetInfo &STI,
                                       raw_ostream &OS) const {
  uint16_t const *words = reinterpret_cast<uint16_t const *>(&Val);
  size_t wordCount = Size / 2;
  emitWords(words, wordCount, OS);
}

void AVRMCCodeEmitter::encodeInstruction(const MCInst &MI, raw_ostream &OS,
                                         SmallVectorImpl<MCFixup> &Fixups,
                                         const MCSubtargetInfo &STI) const {
  const MCInstrDesc &Desc = MCII.get(MI.getOpcode());

  // Get byte count of instruction
  unsigned Size = Desc.getSize();

  assert(Size > 0 && "Instruction size cannot be zero");

  uint64_t BinaryOpCode = getBinaryCodeForInstr(MI, Fixups, STI);
  emitInstruction(BinaryOpCode, Size, STI, OS);
}

MCCodeEmitter *createAVRMCCodeEmitter(const MCInstrInfo &MCII,
                                      const MCRegisterInfo &MRI,
                                      MCContext &Ctx) {
  return new AVRMCCodeEmitter(MCII, Ctx);
}

#include "AVRGenMCCodeEmitter.inc"

} // end of namespace llvm