//===-- AVRISelDAGToDAG.cpp - A dag to dag inst selector for AVR ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the AVR target.
//
//===----------------------------------------------------------------------===//

#include "AVRConfig.h"

#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "AVR.h"
#include "AVRTargetMachine.h"

#define DEBUG_TYPE "avr-isel"

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

namespace llvm {

class AVRDAGToDAGISel : public SelectionDAGISel {
public:
  explicit AVRDAGToDAGISel(AVRTargetMachine &tm, CodeGenOpt::Level OptLevel) :
    SelectionDAGISel(tm, OptLevel), Subtarget(nullptr) {}

  const char *getPassName() const override
  {
    return "AVR DAG->DAG Instruction Selection";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;


  bool SelectInlineAsmMemoryOperand(const SDValue &Op, unsigned ConstraintCode,
                                    std::vector<SDValue> &OutOps) override;

  // Include the pieces autogenerated from the target description.
  #include "AVRGenDAGISel.inc"

private:

  SDNode * selectInlineAsm(SDNode* N);
  SDNode * createGPR8QuadNode(EVT VT, SDValue V0, SDValue V1, SDValue V2, SDValue V3);
  // Address Selection.
  bool selectAddr(SDNode *Op, SDValue N, SDValue &Base, SDValue &Disp);
  // Indexed load (postinc and predec) matching code.
  SDNode *selectIndexedLoad(SDNode *N);
  // Indexed progmem load (only postinc) matching code.
  unsigned selectIndexedProgMemLoad(const LoadSDNode *LD, MVT VT);

  SDNode *Select(SDNode *N) override;
  
  const AVRSubtarget *Subtarget;
};

bool
AVRDAGToDAGISel::runOnMachineFunction(MachineFunction &MF) {

  Subtarget = &static_cast<const AVRSubtarget &>(MF.getSubtarget());
  bool Ret = SelectionDAGISel::runOnMachineFunction(MF);

  return Ret;
}

bool
AVRDAGToDAGISel::selectAddr(SDNode *Op, SDValue N, SDValue &Base, SDValue &Disp)
{
  SDLoc DL(Op);

  // if N (the address) is a FI get the TargetFrameIndex.
  if (const FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(N))
  {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(),
                                       getTargetLowering()->getPointerTy());
    Disp = CurDAG->getTargetConstant(0, DL, MVT::i8);

    return true;
  }

  // Match simple Reg + uimm6 operands.
  if ((N.getOpcode() != ISD::ADD) && (N.getOpcode() != ISD::SUB)
      && !CurDAG->isBaseWithConstantOffset(N))
  {
    return false;
  }

  if (const ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1)))
  {
    int RHSC = (int)RHS->getZExtValue();
    // Convert negative offsets into positives ones.
    if (N.getOpcode() == ISD::SUB)
    {
      RHSC = -RHSC;
    }

    // <#FI + const>
    // Allow folding offsets bigger than 63 so the frame pointer can be used
    // directly instead of copying it around by adjusting and restoring it for
    // each access.
    if (N.getOperand(0).getOpcode() == ISD::FrameIndex)
    {
      int FI = cast<FrameIndexSDNode>(N.getOperand(0))->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI,
                                         getTargetLowering()->getPointerTy());
      Disp = CurDAG->getTargetConstant(RHSC, DL, MVT::i16);

      return true;
    }

    // The value type of the memory instruction determines what is the maximum
    // offset allowed.
    MVT VT = cast<MemSDNode>(Op)->getMemoryVT().getSimpleVT();

    // We only accept offsets that fit in 6 bits (unsigned).
    if ((VT == MVT::i8 && isUInt<6>(RHSC))
        || (VT == MVT::i16 && RHSC >= 0 && RHSC < 63))
    {
      Base = N.getOperand(0);
      Disp = CurDAG->getTargetConstant(RHSC, DL, MVT::i8);

      return true;
    }
  }

  return false;
}

SDNode *AVRDAGToDAGISel::selectIndexedLoad(SDNode *N)
{
  const LoadSDNode *LD = cast<LoadSDNode>(N);
  ISD::MemIndexedMode AM = LD->getAddressingMode();
  MVT VT = LD->getMemoryVT().getSimpleVT();

  // Only care if this load uses a POSTINC or PREDEC mode.
  if ((LD->getExtensionType() != ISD::NON_EXTLOAD)
      || (AM != ISD::POST_INC && AM != ISD::PRE_DEC))
  {
    return 0;
  }

  unsigned Opcode = 0;
  bool isPre = (AM == ISD::PRE_DEC);
  int Offs = cast<ConstantSDNode>(LD->getOffset())->getSExtValue();
  switch (VT.SimpleTy)
  {
  case MVT::i8:
    if ((!isPre && Offs != 1) || (isPre && Offs != -1))
    {
      return 0;
    }
    Opcode = (isPre) ? AVR::LDRdPtrPd : AVR::LDRdPtrPi;
    break;
  case MVT::i16:
    if ((!isPre && Offs != 2) || (isPre && Offs != -2))
    {
      return 0;
    }
    Opcode = (isPre) ? AVR::LDWRdPtrPd : AVR::LDWRdPtrPi;
    break;
  default:
    return 0;
  }

  return CurDAG->getMachineNode(Opcode, SDLoc(N), VT,
                                getTargetLowering()->getPointerTy(), MVT::Other,
                                LD->getBasePtr(), LD->getChain());
}

unsigned AVRDAGToDAGISel::selectIndexedProgMemLoad(const LoadSDNode *LD, MVT VT)
{
  ISD::MemIndexedMode AM = LD->getAddressingMode();

  // Progmem indexed loads only work in POSTINC mode.
  if ((LD->getExtensionType() != ISD::NON_EXTLOAD) || (AM != ISD::POST_INC))
  {
    return 0;
  }

  unsigned Opcode = 0;
  int Offs = cast<ConstantSDNode>(LD->getOffset())->getSExtValue();
  switch (VT.SimpleTy)
  {
  case MVT::i8:
    if (Offs != 1)
    {
      return 0;
    }
    Opcode = AVR::LPMRdZPi;
    break;
  case MVT::i16:
    if (Offs != 2)
    {
      return 0;
    }
    Opcode = AVR::LPMWRdZPi;
    break;
  default:
    return 0;
  }

  return Opcode;
}

/// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
/// inline asm expressions.
bool AVRDAGToDAGISel::SelectInlineAsmMemoryOperand(const SDValue &Op,
                                                   unsigned ConstraintCode,
                                                   std::vector<SDValue> &OutOps)
{
  // Yes hardcoded 'm' symbol. Just because it also has been hardcoded in
  // SelectionDAGISel (callee for this method).
  // assert(ConstraintCode == 'm' && "Unexpected asm memory constraint");

  //MachineFunction& MF = CurDAG->getMachineFunction();
  MachineRegisterInfo &RI = MF->getRegInfo();
  const AVRTargetMachine& TM = (const AVRTargetMachine&)MF->getTarget();
  const TargetLowering* TL = TM.getSubtargetImpl()->getTargetLowering();
  SDLoc DL(Op);

  const RegisterSDNode *RegNode = dyn_cast<RegisterSDNode>(Op);

  // If address operand is of PTRDISPREGS class, all is OK, then.

  if (RegNode && RI.getRegClass(RegNode->getReg()) ==
      &AVR::PTRDISPREGSRegClass)
  {
    OutOps.push_back(Op);
    return false;
  }

  if (Op->getOpcode() == ISD::FrameIndex)
  {
    SDValue Base, Disp;
    if (selectAddr(Op.getNode(), Op, Base, Disp))
    {
      OutOps.push_back(Base);
      OutOps.push_back(Disp);

      return false;
    }

    return true;
  }

  // If Op is add reg, imm and
  // reg is either virtual register or register of PTRDISPREGSRegClass
  if (Op->getOpcode() == ISD::ADD ||
      Op->getOpcode() == ISD::SUB)
  {
    SDValue CopyFromRegOp = Op->getOperand(0);
    SDValue ImmOp = Op->getOperand(1);
    ConstantSDNode *ImmNode = dyn_cast<ConstantSDNode>(ImmOp);

    unsigned Reg;

    bool CanHandleRegImmOpt = true;

    CanHandleRegImmOpt &= ImmNode != 0;
    CanHandleRegImmOpt &= ImmNode->getAPIntValue().getZExtValue() < 64;

    if (CopyFromRegOp->getOpcode() == ISD::CopyFromReg)
    {
      RegisterSDNode *RegNode =
          cast<RegisterSDNode>(CopyFromRegOp->getOperand(1));
      Reg = RegNode->getReg();
      CanHandleRegImmOpt &= (TargetRegisterInfo::isVirtualRegister(Reg) ||
                             AVR::PTRDISPREGSRegClass.contains(Reg));
    }
    else
    {
      CanHandleRegImmOpt = false;
    }

    if (CanHandleRegImmOpt)
    {
      // If we detect proper case - correct virtual register class
      // if needed and go to another inlineasm operand.

      SDValue Base, Disp;

      if (RI.getRegClass(Reg)
          != &AVR::PTRDISPREGSRegClass)
      {
        SDLoc DL(CopyFromRegOp);

        unsigned VReg = RI.createVirtualRegister(
            &AVR::PTRDISPREGSRegClass);

        SDValue CopyToReg = CurDAG->getCopyToReg(CopyFromRegOp, DL,
                                                 VReg, CopyFromRegOp);

        SDValue NewCopyFromRegOp =
            CurDAG->getCopyFromReg(CopyToReg, DL, VReg, TL->getPointerTy());

        Base = NewCopyFromRegOp;
      }
      else
      {
        Base = CopyFromRegOp;
      }

      if (ImmNode->getValueType(0) != MVT::i8)
      {
        Disp = CurDAG->getTargetConstant(
            ImmNode->getAPIntValue().getZExtValue(), DL, MVT::i8);
      }
      else
      {
        Disp = ImmOp;
      }

      OutOps.push_back(Base);
      OutOps.push_back(Disp);

      return false;
    }
  }

  // More generic case.
  // Create chain that puts Op into pointer register
  // and return that register.

  unsigned VReg = RI.createVirtualRegister(&AVR::PTRDISPREGSRegClass);

  SDValue CopyToReg = CurDAG->getCopyToReg(Op, DL, VReg, Op);
  SDValue CopyFromReg =
      CurDAG->getCopyFromReg(CopyToReg, DL, VReg, TL->getPointerTy());

  OutOps.push_back(CopyFromReg);

  return false;
}

SDNode *
AVRDAGToDAGISel::selectInlineAsm(SDNode* N) {
  std::vector<SDValue> AsmNodeOperands;
  unsigned Flag, Kind;
  bool Changed = false;
  unsigned NumOps = N->getNumOperands();

  // Normally, i64 data is bounded to two arbitrary GRPs for "%r" constraint.
  // However, some instrstions (e.g. ldrexd/strexd in ARM mode) require
  // (even/even+1) GPRs and use %n and %Hn to refer to the individual regs
  // respectively. Since there is no constraint to explicitly specify a
  // reg pair, we use GPRPair reg class for "%r" for 64-bit data. For Thumb,
  // the 64-bit data may be referred by H, Q, R modifiers, so we still pack
  // them into a GPRPair.

  SDLoc dl(N);
  SDValue Glue = N->getGluedNode() ? N->getOperand(NumOps-1) : SDValue(nullptr,0);

  SmallVector<bool, 8> OpChanged;
  // Glue node will be appended late.
  for(unsigned i = 0, e = N->getGluedNode() ? NumOps - 1 : NumOps; i < e; ++i) {
    SDValue op = N->getOperand(i);
    AsmNodeOperands.push_back(op);

    if (i < InlineAsm::Op_FirstOperand)
      continue;

    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(i))) {
      Flag = C->getZExtValue();
      Kind = InlineAsm::getKind(Flag);
    } else
      continue;

    // Immediate operands to inline asm in the SelectionDAG are modeled with
    // two operands. The first is a constant of value InlineAsm::Kind_Imm, and
    // the second is a constant with the value of the immediate. If we get here
    // and we have a Kind_Imm, skip the next operand, and continue.
    if (Kind == InlineAsm::Kind_Imm) {
      SDValue op = N->getOperand(++i);
      AsmNodeOperands.push_back(op);
      continue;
    }

    unsigned NumRegs = InlineAsm::getNumOperandRegisters(Flag);
    if (NumRegs)
      OpChanged.push_back(false);

    unsigned DefIdx = 0;
    bool IsTiedToChangedOp = false;
    // If it's a use that is tied with a previous def, it has no
    // reg class constraint.
    if (Changed && InlineAsm::isUseOperandTiedToDef(Flag, DefIdx))
      IsTiedToChangedOp = OpChanged[DefIdx];

    if (Kind != InlineAsm::Kind_RegUse && Kind != InlineAsm::Kind_RegDef
        && Kind != InlineAsm::Kind_RegDefEarlyClobber)
      continue;

    unsigned RC;
    bool HasRC = InlineAsm::hasRegClassConstraint(Flag, RC);
    if ((!IsTiedToChangedOp && (!HasRC || RC != AVR::GPR8RegClassID)) // Poof
        || NumRegs != 2)
      continue;

    assert((i+2 < NumOps) && "Invalid number of operands in inline asm");
    SDValue V0 = N->getOperand(i+1);
    SDValue V1 = N->getOperand(i+2);
    unsigned Reg0 = cast<RegisterSDNode>(V0)->getReg();
    unsigned Reg1 = cast<RegisterSDNode>(V1)->getReg();
    SDValue PairedReg;
    MachineRegisterInfo &MRI = MF->getRegInfo();

    if (Kind == InlineAsm::Kind_RegDef ||
        Kind == InlineAsm::Kind_RegDefEarlyClobber) {
      // Replace the two GPRs with 1 GPRPair and copy values from GPRPair to
      // the original GPRs.

      unsigned GPVR = MRI.createVirtualRegister(&AVR::GPR8QuadRegClass);
      PairedReg = CurDAG->getRegister(GPVR, MVT::Untyped);
      SDValue Chain = SDValue(N,0);

      SDNode *GU = N->getGluedUser();
      SDValue RegCopy = CurDAG->getCopyFromReg(Chain, dl, GPVR, MVT::Untyped,
                                               Chain.getValue(1));

      // Extract values from a GPRPair reg and copy to the original GPR reg.
      SDValue Sub0 = CurDAG->getTargetExtractSubreg(AVR::qsub_0, dl, MVT::i8,
                                                    RegCopy);
      SDValue Sub1 = CurDAG->getTargetExtractSubreg(AVR::qsub_1, dl, MVT::i8,
                                                    RegCopy);
      SDValue T0 = CurDAG->getCopyToReg(Sub0, dl, Reg0, Sub0,
                                        RegCopy.getValue(1));
      SDValue T1 = CurDAG->getCopyToReg(Sub1, dl, Reg1, Sub1, T0.getValue(1));

      // Update the original glue user.
      std::vector<SDValue> Ops(GU->op_begin(), GU->op_end()-1);
      Ops.push_back(T1.getValue(1));
      CurDAG->UpdateNodeOperands(GU, Ops);
    }
    else {
      // For Kind  == InlineAsm::Kind_RegUse, we first copy two GPRs into a
      // GPRPair and then pass the GPRPair to the inline asm.
      SDValue Chain = AsmNodeOperands[InlineAsm::Op_InputChain];

      // As REG_SEQ doesn't take RegisterSDNode, we copy them first.
      SDValue T0 = CurDAG->getCopyFromReg(Chain, dl, Reg0, MVT::i8,
                                          Chain.getValue(1));
      SDValue T1 = CurDAG->getCopyFromReg(Chain, dl, Reg1, MVT::i8,
                                          T0.getValue(1));
      SDValue Quad = SDValue(createGPR8QuadNode(MVT::Untyped, T0, T1, T1, T1), 0); // XXX

      // Copy REG_SEQ into a GPRPair-typed VR and replace the original two
      // i32 VRs of inline asm with it.
      unsigned GPVR = MRI.createVirtualRegister(&AVR::GPR8QuadRegClass);
      PairedReg = CurDAG->getRegister(GPVR, MVT::Untyped);
      Chain = CurDAG->getCopyToReg(T1, dl, GPVR, Quad, T1.getValue(1));

      AsmNodeOperands[InlineAsm::Op_InputChain] = Chain;
      Glue = Chain.getValue(1);
    }

    Changed = true;

    if(PairedReg.getNode()) {
      OpChanged[OpChanged.size() -1 ] = true;
      Flag = InlineAsm::getFlagWord(Kind, 1 /* RegNum*/);
      if (IsTiedToChangedOp)
        Flag = InlineAsm::getFlagWordForMatchingOp(Flag, DefIdx);
      else
        Flag = InlineAsm::getFlagWordForRegClass(Flag, AVR::GPR8QuadRegClassID);
      // Replace the current flag.
      AsmNodeOperands[AsmNodeOperands.size() -1] = CurDAG->getTargetConstant(Flag, dl, MVT::i32);
      // Add the new register node and skip the original two GPRs.
      AsmNodeOperands.push_back(PairedReg);
      // Skip the next two GPRs.
      i += 4;
    }
  }
  if (Glue.getNode())
    AsmNodeOperands.push_back(Glue);
  if (!Changed)
    return nullptr;

  SDValue New = CurDAG->getNode(ISD::INLINEASM, SDLoc(N),
                                CurDAG->getVTList(MVT::Other, MVT::Glue), AsmNodeOperands);
  New->setNodeId(-1);
  return New.getNode();

}

SDNode *
AVRDAGToDAGISel::createGPR8QuadNode(EVT VT, SDValue V0, SDValue V1, SDValue V2, SDValue V3) {
  SDLoc dl(V0.getNode());
  SDValue RegClass =
  CurDAG->getTargetConstant(AVR::GPR8QuadRegClassID, dl, MVT::i8);
  SDValue SubReg0 = CurDAG->getTargetConstant(AVR::qsub_0, dl, MVT::i8);
  SDValue SubReg1 = CurDAG->getTargetConstant(AVR::qsub_1, dl, MVT::i8);
  SDValue SubReg2 = CurDAG->getTargetConstant(AVR::qsub_2, dl, MVT::i8);
  SDValue SubReg3 = CurDAG->getTargetConstant(AVR::qsub_3, dl, MVT::i8);
  const SDValue Ops[] = { RegClass, V0, SubReg0, V1, SubReg1, V2, SubReg2, V3, SubReg3 };
  return CurDAG->getMachineNode(TargetOpcode::REG_SEQUENCE, dl, VT, Ops);
}

SDNode *AVRDAGToDAGISel::Select(SDNode *N)
{
  unsigned Opcode = N->getOpcode();
  SDLoc DL(N);

  // Dump information about the Node being selected.
  DEBUG(errs() << "Selecting: "; N->dump(CurDAG); errs() << "\n");

  // If we have a custom node, we already have selected!
  if (N->isMachineOpcode())
  {
    DEBUG(errs() << "== "; N->dump(CurDAG); errs() << "\n");
    return 0;
  }

  switch (Opcode)
  {
  case ISD::FrameIndex:
    {
      // Convert the frameindex into a temp instruction that will hold the
      // effective address of the final stack slot.
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      SDValue TFI =
        CurDAG->getTargetFrameIndex(FI, getTargetLowering()->getPointerTy());

      return CurDAG->SelectNodeTo(N, AVR::FRMIDX,
                                  getTargetLowering()->getPointerTy(), TFI,
                                  CurDAG->getTargetConstant(0, DL, MVT::i16));
    }
  case ISD::STORE:
    {
      // Use the STD{W}SPQRr pseudo instruction when passing arguments through
      // the stack on function calls for further expansion during the PEI phase.
      const StoreSDNode *ST = cast<StoreSDNode>(N);
      SDValue BasePtr = ST->getBasePtr();

      // Early exit when the base pointer is a frame index node or a constant.
      if (isa<FrameIndexSDNode>(BasePtr) || isa<ConstantSDNode>(BasePtr))
      {
        break;
      }

      const RegisterSDNode *RN =dyn_cast<RegisterSDNode>(BasePtr.getOperand(0));
      // Only stores where SP is the base pointer are valid.
      if (!RN || (RN->getReg() != AVR::SP))
      {
        break;
      }

      int CST =(int)cast<ConstantSDNode>(BasePtr.getOperand(1))->getZExtValue();
      SDValue Chain = ST->getChain();
      SDValue StoredVal = ST->getValue();
      SDValue Offset = CurDAG->getTargetConstant(CST, DL, MVT::i16);
      SDValue Ops[] = { BasePtr.getOperand(0), Offset, StoredVal, Chain };
      unsigned Opc = (StoredVal.getValueType() == MVT::i16) ? AVR::STDWSPQRr
                                                            : AVR::STDSPQRr;

      SDNode *ResNode = CurDAG->getMachineNode(Opc, DL, MVT::Other, Ops);

      // Transfer memoperands.
      MachineSDNode::mmo_iterator MemOp = MF->allocateMemRefsArray(1);
      MemOp[0] = ST->getMemOperand();
      cast<MachineSDNode>(ResNode)->setMemRefs(MemOp, MemOp + 1);

      ReplaceUses(SDValue(N, 0), SDValue(ResNode, 0));

      return ResNode;
    }
  case ISD::LOAD:
    {
      const LoadSDNode *LD = cast<LoadSDNode>(N);
      const Value *SV = LD->getMemOperand()->getValue();
      if (SV && cast<PointerType>(SV->getType())->getAddressSpace() == 1)
      {
        // This is a flash memory load, move the pointer into R31R30 and emit
        // the lpm instruction.
        MVT VT = LD->getMemoryVT().getSimpleVT();
        SDValue Chain = LD->getChain();
        SDValue Ptr = LD->getBasePtr();
        SDNode *ResNode;

        Chain = CurDAG->getCopyToReg(Chain, DL, AVR::R31R30, Ptr, SDValue());
        Ptr = CurDAG->getCopyFromReg(Chain, DL, AVR::R31R30, MVT::i16,
                                     Chain.getValue(1));

        // Check if the opcode can be converted into an indexed load.
        if (unsigned LPMOpc = selectIndexedProgMemLoad(LD, VT))
        {
          // It is legal to fold the load into an indexed load.
          ResNode = CurDAG->getMachineNode(LPMOpc, DL, VT, MVT::i16, MVT::Other,
                                           Ptr, Ptr.getValue(1));
          ReplaceUses(SDValue(N, 2), SDValue(ResNode, 2));
        }
        else
        {
          // Selecting an indexed load is not legal, fallback to a normal load.
          switch (VT.SimpleTy)
          {
          case MVT::i8:
            ResNode = CurDAG->getMachineNode(AVR::LPMRdZ, DL, MVT::i8,
                                             MVT::Other, Ptr, Ptr.getValue(1));
            break;
          case MVT::i16:
            ResNode = CurDAG->getMachineNode(AVR::LPMWRdZ, DL, MVT::i16,
                                             MVT::i16, MVT::Other, Ptr,
                                             Ptr.getValue(1));
            ReplaceUses(SDValue(N, 2), SDValue(ResNode, 2));
            break;
          default:
            llvm_unreachable("Unsupported VT!");
          }
        }

        // Transfer memoperands.
        MachineSDNode::mmo_iterator MemOp = MF->allocateMemRefsArray(1);
        MemOp[0] = LD->getMemOperand();
        cast<MachineSDNode>(ResNode)->setMemRefs(MemOp, MemOp + 1);

        ReplaceUses(SDValue(N, 0), SDValue(ResNode, 0));
        ReplaceUses(SDValue(N, 1), SDValue(ResNode, 1));

        return ResNode;
      }

      // Check if the opcode can be converted into an indexed load.
      if (SDNode *ResNode = selectIndexedLoad(N))
      {
        return ResNode;
      }
      // Other cases are autogenerated.
      break;
    }
  case AVRISD::CALL:
    {
      // Handle indirect calls because ICALL can only take R31R30 as its source
      // operand.
      SDValue InFlag;
      SDValue Chain = N->getOperand(0);
      SDValue Callee = N->getOperand(1);
      unsigned LastOpNum = N->getNumOperands() - 1;

      // Direct calls are autogenerated.
      if (Callee.getOpcode() == ISD::TargetGlobalAddress
          || Callee.getOpcode() == ISD::TargetExternalSymbol)
      {
        break;
      }

      // Skip the incoming flag if present
      if (N->getOperand(LastOpNum).getValueType() == MVT::Glue)
      {
        --LastOpNum;
      }

      Chain = CurDAG->getCopyToReg(Chain, DL, AVR::R31R30, Callee, InFlag);
      SmallVector<SDValue, 8> Ops;
      Ops.push_back(CurDAG->getRegister(AVR::R31R30, MVT::i16));

      // Map all operands into the new node.
      for (unsigned i = 2, e = LastOpNum + 1; i != e; ++i)
      {
        Ops.push_back(N->getOperand(i));
      }
      Ops.push_back(Chain);
      Ops.push_back(Chain.getValue(1));

      SDNode *ResNode = CurDAG->getMachineNode(AVR::ICALL, DL, MVT::Other,
                                               MVT::Glue, Ops);

      ReplaceUses(SDValue(N, 0), SDValue(ResNode, 0));
      ReplaceUses(SDValue(N, 1), SDValue(ResNode, 1));

      return ResNode;
    }
  case ISD::BRIND:
    {
      // Move the destination address of the indirect branch into R31R30.
      SDValue Chain = N->getOperand(0);
      SDValue JmpAddr = N->getOperand(1);

      Chain = CurDAG->getCopyToReg(Chain, DL, AVR::R31R30, JmpAddr);
      SDNode *ResNode = CurDAG->getMachineNode(AVR::IJMP, DL, MVT::Other,Chain);

      ReplaceUses(SDValue(N, 0), SDValue(ResNode, 0));

      return ResNode;
    }
  case ISD::INLINEASM:
    {
      SDNode * ResNode = selectInlineAsm(N);
      if (ResNode)
        return ResNode;
      break;
    }

  }

  SDNode *ResNode = SelectCode(N);

  DEBUG(errs() << "=> ";
        if (ResNode == 0 || ResNode == N)
        {
          N->dump(CurDAG);
        }
        else
        {
          ResNode->dump(CurDAG);
        }
        errs() << "\n";
       );

  return ResNode;
}

/// createAVRISelDag - This pass converts a legalized DAG into a
/// AVR-specific DAG, ready for instruction scheduling.
///
FunctionPass *
createAVRISelDag(AVRTargetMachine &TM, CodeGenOpt::Level OptLevel) {
  return new AVRDAGToDAGISel(TM, OptLevel);
}

} // end of namespace llvm

