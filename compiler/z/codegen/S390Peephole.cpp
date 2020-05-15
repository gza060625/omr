/*******************************************************************************
 * Copyright (c) 2018, 2019 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "z/codegen/S390Peephole.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "codegen/CodeGenPhase.hpp"
#include "codegen/CodeGenerator.hpp"
#include "codegen/CodeGenerator_inlines.hpp"
#include "codegen/ConstantDataSnippet.hpp"
#include "env/FrontEnd.hpp"
#include "codegen/InstOpCode.hpp"
#include "codegen/Instruction.hpp"
#include "codegen/Linkage.hpp"
#include "codegen/Linkage_inlines.hpp"
#include "codegen/MemoryReference.hpp"
#include "codegen/RealRegister.hpp"
#include "codegen/Register.hpp"
#include "codegen/RegisterDependency.hpp"
#include "codegen/RegisterPair.hpp"
#include "codegen/Snippet.hpp"
#include "il/LabelSymbol.hpp"
#include "ras/Debug.hpp"
#include "ras/DebugCounter.hpp"
#include "ras/Delimiter.hpp"
#include "runtime/Runtime.hpp"
#include "z/codegen/CallSnippet.hpp"
#include "z/codegen/OpMemToMem.hpp"
#include "z/codegen/S390Evaluator.hpp"
#include "z/codegen/S390GenerateInstructions.hpp"
#include "z/codegen/S390Instruction.hpp"
#include "z/codegen/S390OutOfLineCodeSection.hpp"
#include "z/codegen/SystemLinkage.hpp"

TR_S390Peephole::TR_S390Peephole(TR::Compilation* comp)
   : _fe(comp->fe()),
     _outFile(comp->getOutFile()),
     _cursor(comp->cg()->getFirstInstruction()),
     _cg(comp->cg())
   {
   }

///////////////////////////////////////////////////////////////////////////////

TR::Instruction* realInstruction(TR::Instruction* inst, bool forward)
   {
   while (inst && (inst->getKind() == TR::Instruction::IsPseudo      ||
                   inst->getKind() == TR::Instruction::IsNotExtended ||
                   inst->getKind() == TR::Instruction::IsLabel))
      {
      inst = forward ? inst->getNext() : inst->getPrev();
      }

   return inst;
   }

TR::Instruction* realInstructionWithLabels(TR::Instruction* inst, bool forward)
   {
   while (inst && (inst->getKind() == TR::Instruction::IsPseudo ||
                   inst->getKind() == TR::Instruction::IsNotExtended))
      {
      inst = forward ? inst->getNext() : inst->getPrev();
      }

   return inst;
   }

bool
TR_S390Peephole::isBarrierToPeepHoleLookback(TR::Instruction *current)
   {
   if (NULL == current) return true;

   TR::Instruction *s390current = current;

   if (s390current->isLabel()) return true;
   if (s390current->isCall())  return true;
   if (s390current->isBranchOp()) return true;
   if (s390current->getOpCodeValue() == TR::InstOpCode::DCB) return true;

   return false;
   }

bool
TR_S390Peephole::clearsHighBitOfAddressInReg(TR::Instruction *inst, TR::Register *targetReg)
   {
   if (inst->defsRegister(targetReg))
      {
      if (inst->getOpCodeValue() == TR::InstOpCode::LA)
         {
         if (comp()->getOption(TR_TraceCG))
            traceMsg(comp(), "LA inst 0x%x clears high bit on targetReg 0x%x (%s)\n", inst, targetReg, targetReg->getRegisterName(comp()));
         return true;
         }

      if (inst->getOpCodeValue() == TR::InstOpCode::LAY)
         {
         if (comp()->getOption(TR_TraceCG))
            traceMsg(comp(), "LAY inst 0x%x clears high bit on targetReg 0x%x (%s)\n", inst, targetReg, targetReg->getRegisterName(comp()));
         return true;
         }

      if (inst->getOpCodeValue() == TR::InstOpCode::NILH)
         {
         TR::S390RIInstruction * NILH_RI = (TR::S390RIInstruction *)inst;
         if (NILH_RI->isImm() &&
             NILH_RI->getSourceImmediate() == 0x7FFF)
            {
            if (comp()->getOption(TR_TraceCG))
               traceMsg(comp(), "NILH inst 0x%x clears high bit on targetReg 0x%x (%s)\n", inst, targetReg, targetReg->getRegisterName(comp()));
            return true;
            }
         }
      }
   return false;
   }

static TR::Instruction *
findActiveCCInst(TR::Instruction *curr, TR::InstOpCode::Mnemonic op, TR::Register *ccReg)
   {
   TR::Instruction * next = curr;
   while (next = next->getNext())
      {
      if (next->getOpCodeValue() == op)
         return next;
      if (next->usesRegister(ccReg) ||
          next->isLabel() ||
          next->getOpCode().setsCC() ||
          next->isCall() ||
          next->getOpCode().setsCompareFlag())
         break;
      }
   return NULL;
   }

/**
 * Peek ahead in instruction stream to see if we find register being used
 * in a memref
 */
bool
TR_S390Peephole::seekRegInFutureMemRef(int32_t maxWindowSize, TR::Register *targetReg)
   {
   TR::Instruction * current = _cursor->getNext();
   int32_t windowSize=0;

   while ((current != NULL) &&
         !current->matchesTargetRegister(targetReg) &&
         !isBarrierToPeepHoleLookback(current) &&
         windowSize<maxWindowSize)
      {
      // does instruction load or store? otherwise just ignore and move to next instruction
      if (current->isLoad() || current->isStore())
         {
         TR::MemoryReference *mr = current->getMemoryReference();

         if (mr && (mr->getBaseRegister()==targetReg || mr->getIndexRegister()==targetReg))
            {
            return true;
            }
         }
      current = current->getNext();
      windowSize++;
      }

   return false;
   }

/**
 * LRReduction performs several LR reduction/removal transformations:
 *
 * (design 1980)
 * convert
 *      LTR GPRx, GPRx
 * to
 *      CHI GPRx, 0
 * This is an AGI reduction as LTR defines GPRx once again, while CHI simply sets the condition code
 *
 *
 *  removes unnecessary LR/LGR/LTR/LGTR's of the form
 *      LR  GPRx, GPRy
 *      LR  GPRy, GPRx   <--- Removed.
 *      CHI GPRx, 0
 * Most of the  redundant LR's are independently generated by global and
 * local register assignment.
 *
 * There is a further extension to this peephole which can transform
 *      LR  GPRx, GPRy
 *      LTR GPRx, GPRx
 * to
 *      LTR GPRx, GPRy
 * assuming that condition code is not incorrectly clobbered between the
 * LR and LTR.  However, there are very few opportunities to exercise this
 * peephole, so is not included.
 *
 * Convert
 *       LR GPRx, GPRy
 *       CHI GPRx, 0
 * to
 *       LTR GPRx, GPRy
 */
bool
TR_S390Peephole::LRReduction()
   {
   if (comp()->getOption(TR_Randomize))
      {
      if (_cg->randomizer.randomBoolean() && performTransformation(comp(),"O^O Random Codegen  - Disable LRReduction on 0x%p.\n",_cursor))
         return false;
      }

   bool performed = false;
   int32_t windowSize=0;
   const int32_t maxWindowSize=20;
   static char *disableLRReduction = feGetEnv("TR_DisableLRReduction");

   if (disableLRReduction != NULL) return false;

   //The _defRegs in the instruction records virtual def reg till now that needs to be reset to real reg.
   _cursor->setUseDefRegisters(false);

   TR::Register *lgrSourceReg = ((TR::S390RRInstruction*)_cursor)->getRegisterOperand(2);
   TR::Register *lgrTargetReg = ((TR::S390RRInstruction*)_cursor)->getRegisterOperand(1);
   TR::InstOpCode lgrOpCode = _cursor->getOpCode();

   if (lgrTargetReg == lgrSourceReg &&
      (lgrOpCode.getOpCodeValue() == TR::InstOpCode::LR ||
       lgrOpCode.getOpCodeValue() == TR::InstOpCode::LGR ||
       lgrOpCode.getOpCodeValue() == TR::InstOpCode::LDR ||
       lgrOpCode.getOpCodeValue() == TR::InstOpCode::CPYA))
       {
       if (performTransformation(comp(), "O^O S390 PEEPHOLE: Removing redundant LR/LGR/LDR/CPYA at %p\n", _cursor))
          {
            // Removing redundant LR.
          _cg->deleteInst(_cursor);
          performed = true;
          return performed;
          }
       }

   // If both target and source are the same, and we have a load and test,
   // convert it to a CHI
   if  (lgrTargetReg == lgrSourceReg &&
      (lgrOpCode.getOpCodeValue() == TR::InstOpCode::LTR || lgrOpCode.getOpCodeValue() == TR::InstOpCode::LTGR))
      {
      bool isAGI = seekRegInFutureMemRef(4, lgrTargetReg);

      if (isAGI && performTransformation(comp(), "\nO^O S390 PEEPHOLE: Transforming load and test to compare halfword immediate at %p\n", _cursor))
         {
         // replace LTGR with CGHI, LTR with CHI
         TR::Instruction* oldCursor = _cursor;
         _cursor = generateRIInstruction(_cg, lgrOpCode.is64bit() ? TR::InstOpCode::CGHI : TR::InstOpCode::CHI, comp()->getStartTree()->getNode(), lgrTargetReg, 0, _cursor->getPrev());

         _cg->replaceInst(oldCursor, _cursor);

         performed = true;

         // instruction is now a CHI, not a LTR, so we must return
         return performed;
         }

      TR::Instruction *prev = _cursor->getPrev();
      if((prev->getOpCodeValue() == TR::InstOpCode::LR && lgrOpCode.getOpCodeValue() == TR::InstOpCode::LTR) ||
         (prev->getOpCodeValue() == TR::InstOpCode::LGR && lgrOpCode.getOpCodeValue() == TR::InstOpCode::LTGR))
        {
        TR::Register *prevTargetReg = ((TR::S390RRInstruction*)prev)->getRegisterOperand(1);
        TR::Register *prevSourceReg = ((TR::S390RRInstruction*)prev)->getRegisterOperand(2);
        if((lgrTargetReg == prevTargetReg || lgrTargetReg == prevSourceReg) &&
           performTransformation(comp(), "\nO^O S390 PEEPHOLE: Transforming load register into load and test register and removing current at %p\n", _cursor))
          {
          TR::Instruction *newInst = generateRRInstruction(_cg, lgrOpCode.is64bit() ? TR::InstOpCode::LTGR : TR::InstOpCode::LTR, prev->getNode(), prevTargetReg, prevSourceReg, prev->getPrev());
          _cg->replaceInst(prev, newInst);
          if (comp()->getOption(TR_TraceCG))
            printInstr(comp(), _cursor);
          _cg->deleteInst(prev);
          _cg->deleteInst(_cursor);
          _cursor = newInst;
          return true;
          }
        }
      TR::Instruction *next = _cursor->getNext();
      //try to remove redundant LTR, LTGR when we can reuse the condition code of an arithmetic logical operation, ie. Add/Subtract Logical
      //this is also done by isActiveLogicalCC, and the end of generateS390CompareAndBranchOpsHelper when the virtual registers match
      //but those cannot handle the case when the virtual registers are not the same but we do have the same restricted register
      //which is why we are handling it here when all the register assignments are done, and the redundant LR's from the
      //clobber evaluate of the add/sub logical are cleaned up as well.
      // removes the redundant LTR/LTRG, and corrects the mask of the BRC
      // from:
      // SLR @01, @04
      // LTR @01, @01
      // BRC (MASK8, 0x8) Label
      //
      // to:
      // SLR @01, @04
      // BRC (0x10) Label
      //checks that the prev instruction is an add/sub logical operation that sets the same target register as the LTR/LTGR insn, and that we branch immediately after
      if (prev->getOpCode().setsCC() && prev->getOpCode().setsCarryFlag() && prev->getRegisterOperand(1) == lgrTargetReg && next->getOpCodeValue() == TR::InstOpCode::BRC)
         {
         TR::InstOpCode::S390BranchCondition branchCond = ((TR::S390BranchInstruction *) next)->getBranchCondition();

         if ((branchCond == TR::InstOpCode::COND_BE || branchCond == TR::InstOpCode::COND_BNE) &&
            performTransformation(comp(), "\nO^O S390 PEEPHOLE: Removing redundant Load and Test instruction at %p, because CC can be reused from logical instruction %p\n",_cursor, prev))
            {
            _cg->deleteInst(_cursor);
            if (branchCond == TR::InstOpCode::COND_BE)
               ((TR::S390BranchInstruction *) next)->setBranchCondition(TR::InstOpCode::COND_MASK10);
            else if (branchCond == TR::InstOpCode::COND_BNE)
               ((TR::S390BranchInstruction *) next)->setBranchCondition(TR::InstOpCode::COND_MASK5);
            performed = true;
            return performed;
            }
         }
      }

   TR::Instruction * current = _cursor->getNext();

   // In order to remove LTR's, we need to ensure that there are no
   // instructions that set CC or read CC.
   bool lgrSetCC = lgrOpCode.setsCC();
   bool setCC = false, useCC = false;

   while ((current != NULL) &&
            !isBarrierToPeepHoleLookback(current) &&
           !(current->isBranchOp() && current->getKind() == TR::Instruction::IsRIL &&
              ((TR::S390RILInstruction *)  current)->getTargetSnippet() ) &&
           windowSize < maxWindowSize)
      {

      // do not look across Transactional Regions, the Register save mask is optimistic and does not allow renaming
      if (current->getOpCodeValue() == TR::InstOpCode::TBEGIN ||
          current->getOpCodeValue() == TR::InstOpCode::TBEGINC ||
          current->getOpCodeValue() == TR::InstOpCode::TEND ||
          current->getOpCodeValue() == TR::InstOpCode::TABORT)
         {
         return false;
         }

      TR::InstOpCode curOpCode = current->getOpCode();
      current->setUseDefRegisters(false);
      // if we encounter the CHI GPRx, 0, attempt the transformation the LR->LTR
      // and remove the CHI GPRx, 0
      if ((curOpCode.getOpCodeValue() == TR::InstOpCode::CHI || curOpCode.getOpCodeValue() == TR::InstOpCode::CGHI) &&
            ((curOpCode.is32bit() && lgrOpCode.is32bit()) ||
             (curOpCode.is64bit() && lgrOpCode.is64bit())))
         {
         TR::Register *curTargetReg=((TR::S390RIInstruction*)current)->getRegisterOperand(1);
         int32_t srcImm = ((TR::S390RIInstruction*)current)->getSourceImmediate();
         if(curTargetReg == lgrTargetReg && (srcImm == 0) && !(setCC || useCC))
            {
            if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
            if(performTransformation(comp(), "O^O S390 PEEPHOLE: Transforming LR/CHI to LTR at %p\n", _cursor))
               {
               if (comp()->getOption(TR_TraceCG))
                  {
                  printInfo("\nRemoving CHI instruction:");
                  printInstr(comp(), current);
                  char tmp[50];
                  sprintf(tmp,"\nReplacing load at %p with load and test", _cursor);
                  printInfo(tmp);
                  }

               // Remove the CHI
               _cg->deleteInst(current);

               // Replace the LR with LTR
               TR::Instruction* oldCursor = _cursor;
               _cursor = generateRRInstruction(_cg, lgrOpCode.is64bit() ? TR::InstOpCode::LTGR : TR::InstOpCode::LTR, comp()->getStartTree()->getNode(), lgrTargetReg, lgrSourceReg, _cursor->getPrev());

               _cg->replaceInst(oldCursor, _cursor);

               lgrOpCode = _cursor->getOpCode();

               lgrSetCC = true;

               performed = true;
               }
            }
         }

      // if we encounter the LR  GPRy, GPRx that we want to remove

      if (curOpCode.getOpCodeValue() == lgrOpCode.getOpCodeValue() &&
          current->getKind() == TR::Instruction::IsRR)
         {
         TR::Register *curSourceReg = ((TR::S390RRInstruction*)current)->getRegisterOperand(2);
         TR::Register *curTargetReg = ((TR::S390RRInstruction*)current)->getRegisterOperand(1);

         if ( ((curSourceReg == lgrTargetReg && curTargetReg == lgrSourceReg) ||
              (curSourceReg == lgrSourceReg && curTargetReg == lgrTargetReg)))
            {
            // We are either replacing LR/LGR (lgrSetCC won't be set)
            // or if we are modifying LTR/LGTR, then no instruction can
            // set or read CC between our original and current instruction.

            if ((!lgrSetCC || !(setCC || useCC)))
               {
               if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
               if (performTransformation(comp(), "O^O S390 PEEPHOLE: Duplicate LR/CPYA removal at %p\n", current))
                  {
                  if (comp()->getOption(TR_TraceCG))
                     {
                     printInfo("\nDuplicate LR/CPYA:");
                     printInstr(comp(), current);
                     char tmp[50];
                     sprintf(tmp,"is removed as duplicate of %p.", _cursor);
                     printInfo(tmp);
                     }

                  // Removing redundant LR/CPYA.
                  _cg->deleteInst(current);

                  performed = true;
                  current = current->getNext();
                  windowSize = 0;
                  setCC = setCC || current->getOpCode().setsCC();
                  useCC = useCC || current->getOpCode().readsCC();
                  continue;
                  }
               }
            }
         }

      // Flag if current instruction sets or reads CC -> used to determine
      // whether LTR/LGTR transformation is valid.
      setCC = setCC || curOpCode.setsCC();
      useCC = useCC || curOpCode.readsCC();

      // If instruction overwrites either of the original source and target registers,
      // we cannot remove any duplicates, as register contents may have changed.
      if (current->isDefRegister(lgrSourceReg) ||
          current->isDefRegister(lgrTargetReg))
         break;

      current = current->getNext();
      windowSize++;
      }

   return performed;
   }

bool swapOperands(TR::Register * trueReg, TR::Register * compReg, TR::Instruction * curr)
   {
   TR::InstOpCode::S390BranchCondition branchCond;
   uint8_t mask;
   switch (curr->getKind())
      {
      case TR::Instruction::IsRR:
         branchCond = ((TR::S390RRInstruction *) curr)->getBranchCondition();
         ((TR::S390RRInstruction *) curr)->setBranchCondition(getReverseBranchCondition(branchCond));
         ((TR::S390RRInstruction *) curr)->setRegisterOperand(2,trueReg);
         ((TR::S390RRInstruction *) curr)->setRegisterOperand(1,compReg);
         break;
      case TR::Instruction::IsRIE:
         branchCond = ((TR::S390RIEInstruction *) curr)->getBranchCondition();
         ((TR::S390RIEInstruction *) curr)->setBranchCondition(getReverseBranchCondition(branchCond));
         ((TR::S390RIEInstruction *) curr)->setRegisterOperand(2,trueReg);
         ((TR::S390RIEInstruction *) curr)->setRegisterOperand(1,compReg);
         break;
      case TR::Instruction::IsRRS:
         branchCond = ((TR::S390RRSInstruction *) curr)->getBranchCondition();
         ((TR::S390RRSInstruction *) curr)->setBranchCondition(getReverseBranchCondition(branchCond));
         ((TR::S390RRSInstruction *) curr)->setRegisterOperand(2,trueReg);
         ((TR::S390RRSInstruction *) curr)->setRegisterOperand(1,compReg);
         break;
      case TR::Instruction::IsRRD: // RRD is encoded use RRF
      case TR::Instruction::IsRRF:
         branchCond = ((TR::S390RRFInstruction *) curr)->getBranchCondition();
         ((TR::S390RRFInstruction *) curr)->setBranchCondition(getReverseBranchCondition(branchCond));
         ((TR::S390RRFInstruction *) curr)->setRegisterOperand(2,trueReg);
         ((TR::S390RRFInstruction *) curr)->setRegisterOperand(1,compReg);
         break;
      case TR::Instruction::IsRRF2:
         mask = ((TR::S390RRFInstruction *) curr)->getMask3();
         ((TR::S390RRFInstruction *) curr)->setMask3(getReverseBranchMask(mask));
         ((TR::S390RRFInstruction *) curr)->setRegisterOperand(2,trueReg);
         ((TR::S390RRFInstruction *) curr)->setRegisterOperand(1,compReg);
         break;
      default:
         // unsupport instruction type, bail
        return false;
      }
      return true;
   }

void insertLoad(TR::Compilation * comp, TR::CodeGenerator * cg, TR::Instruction * i, TR::Register * r)
   {
   switch(r->getKind())
     {
     case TR_FPR:
       new (comp->trHeapMemory()) TR::S390RRInstruction(TR::InstOpCode::LDR, i->getNode(), r, r, i, cg);
       break;
     default:
       new (comp->trHeapMemory()) TR::S390RRInstruction(TR::InstOpCode::LR, i->getNode(), r, r, i, cg);
       break;
     }
   }

bool hasDefineToRegister(TR::Instruction * curr, TR::Register * reg)
   {
   TR::Instruction * prev = curr->getPrev();
   prev = realInstruction(prev, false);
   for(int32_t i = 0; i < 3 && prev; ++i)
      {
      if (prev->defsRegister(reg))
         {
         return true;
         }

      prev = prev->getPrev();
      prev = realInstruction(prev, false);
      }
   return false;
   }

/**
 * z10 specific hw performance bug
 * On z10, applies to GPRs only.
 * There are cases where load of a GPR and its complemented value are required
 * in same grouping, causing pipeline flush + late load = perf hit.
 */
bool
TR_S390Peephole::trueCompEliminationForCompare()
   {
   // z10 specific
   if (!comp()->target().cpu.getSupportsArch(TR::CPU::z10) || comp()->target().cpu.getSupportsArch(TR::CPU::z196))
      {
      return false;
      }
   TR::Instruction * curr = _cursor;
   TR::Instruction * prev = _cursor->getPrev();
   TR::Instruction * next = _cursor->getNext();
   TR::Register * compReg;
   TR::Register * trueReg;

   prev = realInstruction(prev, false);
   next = realInstruction(next, true);

   switch(curr->getKind())
      {
      case TR::Instruction::IsRR:
         compReg = ((TR::S390RRInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RRInstruction *) curr)->getRegisterOperand(1);
         break;
      case TR::Instruction::IsRIE:
         compReg = ((TR::S390RIEInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RIEInstruction *) curr)->getRegisterOperand(1);
         break;
      case TR::Instruction::IsRRS:
         compReg = ((TR::S390RRSInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RRSInstruction *) curr)->getRegisterOperand(1);
         break;
      case TR::Instruction::IsRRD: // RRD is encoded use RRF
      case TR::Instruction::IsRRF:
         compReg = ((TR::S390RRFInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RRFInstruction *) curr)->getRegisterOperand(1);
         break;
      default:
         // unsupport instruction type, bail
         return false;
      }

   // only applies to GPR's
   if (compReg->getKind() != TR_GPR || trueReg->getKind() != TR_GPR)
      {
      return false;
      }

   if (!hasDefineToRegister(curr, compReg))
      {
      return false;
      }

   TR::Instruction * branchInst = NULL;
   // current instruction sets condition code or compare flag, check to see
   // if it has multiple branches using this condition code..if so, abort.
   TR::Instruction * nextInst = next;
   while(nextInst && !nextInst->isLabel() && !nextInst->getOpCode().setsCC() &&
         !nextInst->isCall() && !nextInst->getOpCode().setsCompareFlag())
      {
      if(nextInst->isBranchOp())
         {
         if(branchInst == NULL)
            {
            branchInst = nextInst;
            }
         else
            {
            // there are multiple branches using the same branch condition
            // just give up.
            // we can probably just insert load instructions here still, but
            // we have to sort out the wild branches first
            return false;
            }
         }
      nextInst = nextInst->getNext();
      }
   if (branchInst && prev && prev->usesRegister(compReg) && !prev->usesRegister(trueReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare case 1 at %p.\n",curr))
         {
         swapOperands(trueReg, compReg, curr);
         if(next && next->usesRegister(trueReg)) insertLoad (comp(), _cg, next->getPrev(), compReg);
         TR::InstOpCode::S390BranchCondition branchCond = ((TR::S390BranchInstruction *) branchInst)->getBranchCondition();
         ((TR::S390BranchInstruction *) branchInst)->setBranchCondition(getReverseBranchCondition(branchCond));
         return true;
         }
      else
         return false;
      }
   if (next && next->usesRegister(compReg) && !next->usesRegister(trueReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare case 2 at %p.\n",curr))
         {
         if (branchInst && prev && !prev->usesRegister(trueReg))
            {
            swapOperands(trueReg, compReg, curr);
            TR::InstOpCode::S390BranchCondition branchCond = ((TR::S390BranchInstruction *) branchInst)->getBranchCondition();
            ((TR::S390BranchInstruction *) branchInst)->setBranchCondition(getReverseBranchCondition(branchCond));
            }
         else
            {
            insertLoad (comp(), _cg, next->getPrev(), compReg);
            }
         return true;
         }
      else
         return false;
      }

   bool loadInserted = false;
   if (prev && prev->usesRegister(compReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare case 3 at %p.\n",curr))
         {
         insertLoad (comp(), _cg, prev, trueReg);
         loadInserted = true;
         }
      }
   if (next && next->usesRegister(compReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare case 4 at %p.\n",curr))
         {
         insertLoad (comp(), _cg, next->getPrev(), trueReg);
         loadInserted = true;
         }
      }
   return loadInserted ;
   }

/**
 * z10 specific hw performance bug
 * On z10, applies to GPRs only.
 * There are cases where load of a GPR and its complemented value are required
 * in same grouping, causing pipeline flush + late load = perf hit.
 */
bool
TR_S390Peephole::trueCompEliminationForCompareAndBranch()
   {
   // z10 specific
   if (!comp()->target().cpu.getSupportsArch(TR::CPU::z10) || comp()->target().cpu.getSupportsArch(TR::CPU::z196))
      {
      return false;
      }

   TR::Instruction * curr = _cursor;
   TR::Instruction * prev = _cursor->getPrev();
   prev = realInstruction(prev, false);

   TR::Instruction * next = _cursor->getNext();
   next = realInstruction(next, true);

   TR::Register * compReg;
   TR::Register * trueReg;
   TR::Instruction * btar = NULL;
   TR::LabelSymbol * ls = NULL;

   bool isWCodeCmpEqSwap = false;

   switch (curr->getKind())
      {
      case TR::Instruction::IsRIE:
         compReg = ((TR::S390RIEInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RIEInstruction *) curr)->getRegisterOperand(1);
         btar = ((TR::S390RIEInstruction *) curr)->getBranchDestinationLabel()->getInstruction();
         break;
      case TR::Instruction::IsRRS:
         compReg = ((TR::S390RRSInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RRSInstruction *) curr)->getRegisterOperand(1);
         break;
      case TR::Instruction::IsRRD: // RRD is encoded use RRF
      case TR::Instruction::IsRRF:
      case TR::Instruction::IsRRF2:
         compReg = ((TR::S390RRFInstruction *) curr)->getRegisterOperand(2);
         trueReg = ((TR::S390RRFInstruction *) curr)->getRegisterOperand(1);
         break;
      default:
         // unsupport instruction type, bail
         return false;
      }

   // only applies to GPR's
   if (compReg->getKind() != TR_GPR || trueReg->getKind() != TR_GPR)
      {
      return false;
      }

   if (!hasDefineToRegister(curr, compReg))
      {
      return false;
      }

   btar = realInstruction(btar, true);
   bool backwardBranch = false;
   if (btar)
      {
      backwardBranch = curr->getIndex() - btar->getIndex() > 0;
      }

   if (backwardBranch)
      {
      if ((prev && btar) &&
          (prev->usesRegister(compReg) || btar->usesRegister(compReg)) &&
          (!prev->usesRegister(trueReg) && !btar->usesRegister(trueReg)) )
         {
         if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
         if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 1 at %p.\n",curr))
            {
            swapOperands(trueReg, compReg, curr);
            if (next && next->usesRegister(trueReg)) insertLoad (comp(), _cg, next->getPrev(), compReg);
            return true;
            }
         else
            return false;
         }
      }
   else
      {
      if ((prev && next) &&
          (prev->usesRegister(compReg) || next->usesRegister(compReg)) &&
          (!prev->usesRegister(trueReg) && !next->usesRegister(trueReg)) )
         {
         if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
         if(!isWCodeCmpEqSwap && performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 2 at %p.\n",curr))
            {
            swapOperands(trueReg, compReg, curr);
            if (btar && btar->usesRegister(trueReg)) insertLoad (comp(), _cg, btar->getPrev(), compReg);
            return true;
            }
         else
            return false;
         }
      }
   if (prev && prev->usesRegister(compReg) && !prev->usesRegister(trueReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(!isWCodeCmpEqSwap && performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 3 at %p.\n",curr))
         {
         swapOperands(trueReg, compReg, curr);
         if (btar && btar->usesRegister(trueReg)) insertLoad (comp(), _cg, btar->getPrev(), compReg);
         if (next && next->usesRegister(trueReg)) insertLoad (comp(), _cg, next->getPrev(), compReg);
         return true;
         }
      else
         return false;
      }

   bool loadInserted = false;
   if (prev && prev->usesRegister(compReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 4 at %p.\n",curr) )
         {
         insertLoad (comp(), _cg, prev, trueReg);
         loadInserted = true;
         }
      }
   if (btar && btar->usesRegister(compReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 5 at %p.\n",curr) )
         {
         insertLoad (comp(), _cg, btar->getPrev(), trueReg);
         loadInserted = true;
         }
      }
   if (next && next->usesRegister(compReg))
      {
      if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
      if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for compare and branch case 6 at %p.\n",curr) )
         {
         insertLoad (comp(), _cg, next->getPrev(), trueReg);
         loadInserted = true;
         }
      }
   return loadInserted;
   }

bool
TR_S390Peephole::trueCompEliminationForLoadComp()
   {
   if (!comp()->target().cpu.getSupportsArch(TR::CPU::z10) || comp()->target().cpu.getSupportsArch(TR::CPU::z196))
      {
      return false;
      }
   TR::Instruction * curr = _cursor;
   TR::Instruction * next = _cursor->getNext();
   next = realInstruction(next, true);
   TR::Instruction * prev = _cursor->getPrev();
   prev = realInstruction(prev, false);

   TR::Register * srcReg = ((TR::S390RRInstruction *) curr)->getRegisterOperand(2);
   TR::RealRegister *tempReg = NULL;
   if((toRealRegister(srcReg))->getRegisterNumber() == TR::RealRegister::GPR1)
      {
      tempReg = _cg->machine()->getRealRegister(TR::RealRegister::GPR2);
      }
   else
      {
      tempReg = _cg->machine()->getRealRegister(TR::RealRegister::GPR1);
      }

   if (prev && prev->defsRegister(srcReg))
      {
      // src register is defined in the previous instruction, check to see if it's
      // used in the next instruction, if so, inject a load after the current insruction
      if (next && next->usesRegister(srcReg))
         {
         insertLoad (comp(), _cg, curr, tempReg);
         return true;
         }
      }

   TR::Instruction * prev2 = NULL;
   if (prev)
      {
      prev2 = realInstruction(prev->getPrev(), false);
      }
   if (prev2 && prev2->defsRegister(srcReg))
      {
      // src register is defined 2 instructions ago, insert a load before the current instruction
      // if the true value is used before or after
      if ((next && next->usesRegister(srcReg)) || (prev && prev->usesRegister(srcReg)))
         {
         if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
         if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for load complement at %p.\n",curr))
            {
            insertLoad (comp(), _cg, curr->getPrev(), tempReg);
            return true;
            }
         else
            return false;
         }
      }

   TR::Instruction * prev3 = NULL;
   if (prev2)
      {
      prev3 = realInstruction(prev2->getPrev(), false);
      }
   if (prev3 && prev3->defsRegister(srcReg))
      {
      // src registers is defined 3 instructions ago, insert a load before the current instruction
      // if the true value is use before
      if(prev && prev->usesRegister(srcReg))
         {
         if (comp()->getOption(TR_TraceCG)) { printInfo("\n"); }
         if(performTransformation(comp(), "O^O S390 PEEPHOLE: true complement elimination for load complement at %p.\n",curr))
            {
            insertLoad (comp(), _cg, curr->getPrev(), tempReg);
            return true;
            }
         else
            return false;
         }
      }
   return false;
   }

/**
 * Exploit zGryphon distinct Operands facility:
 * ex:
 * LR      GPR6,GPR0   ; clobber eval
 * AHI     GPR6,-1
 *
 * becomes:
 * AHIK    GPR6,GPR0, -1
 */
bool
TR_S390Peephole::attemptZ7distinctOperants()
   {
   if (comp()->getOption(TR_Randomize))
      {
      if (_cg->randomizer.randomBoolean() && performTransformation(comp(),"O^O Random Codegen  - Disable attemptZ7distinctOperants on 0x%p.\n",_cursor))
         return false;
      }

   bool performed=false;
   int32_t windowSize=0;
   const int32_t maxWindowSize=4;

   TR::Instruction * instr = _cursor;

   if (!comp()->target().cpu.getSupportsArch(TR::CPU::z196))
      {
      return false;
      }

   if (instr->getOpCodeValue() != TR::InstOpCode::LR && instr->getOpCodeValue() != TR::InstOpCode::LGR)
      {
      return false;
      }

   TR::Register *lgrTargetReg = instr->getRegisterOperand(1);
   TR::Register *lgrSourceReg = instr->getRegisterOperand(2);

   TR::Instruction * current = _cursor->getNext();

   while ( (current != NULL) &&
           !current->isLabel() &&
           !current->isCall() &&
           !(current->isBranchOp() && !(current->isExceptBranchOp())) &&
           windowSize < maxWindowSize)
      {
      // do not look across Transactional Regions, the Register save mask is optimistic and does not allow renaming
      if (current->getOpCodeValue() == TR::InstOpCode::TBEGIN ||
          current->getOpCodeValue() == TR::InstOpCode::TBEGINC ||
          current->getOpCodeValue() == TR::InstOpCode::TEND ||
          current->getOpCodeValue() == TR::InstOpCode::TABORT)
         {
         return false;
         }

      TR::InstOpCode::Mnemonic curOpCode = current->getOpCodeValue();

      // found the first next def of lgrSourceReg
      // todo : verify that reg pair is handled
      if (current->defsRegister(lgrSourceReg))
         {
         // we cannot do this if the source register's value is updated:
         // ex:
         // LR      GPR6,GPR0
         // SR      GPR0,GPR11
         // AHI     GPR6,-1
         //
         // this sequence cannot be transformed into:
         // SR      GPR0,GPR11
         // AHIK    GPR6,GPR0, -1
         return false;
         }
      // found the first next use/def of lgrTargetRegister
      if (current->usesRegister(lgrTargetReg))
         {
         if (current->defsRegister(lgrTargetReg))
            {
            TR::Instruction * newInstr = NULL;
            TR::Instruction * prevInstr = current->getPrev();
            TR::Node * node = instr->getNode();
            TR::Register * srcReg = NULL;

            if (curOpCode != TR::InstOpCode::AHI && curOpCode != TR::InstOpCode::AGHI &&
                curOpCode != TR::InstOpCode::SLL && curOpCode != TR::InstOpCode::SLA &&
                curOpCode != TR::InstOpCode::SRA && curOpCode != TR::InstOpCode::SRLK)
               {
               srcReg = current->getRegisterOperand(2);

               // ex:  LR R1, R2
               //      XR R1, R1
               // ==>
               //      XRK R1, R2, R2
               if (srcReg == lgrTargetReg)
                  {
                  srcReg = lgrSourceReg;
                  }
               }

            if (  (current->getOpCode().is32bit() &&  instr->getOpCodeValue() == TR::InstOpCode::LGR)
               || (current->getOpCode().is64bit() &&  instr->getOpCodeValue() == TR::InstOpCode::LR))
               {

               // Make sure we abort if the register copy and the subsequent operation
               //             do not have the same word length (32-bit or 64-bit)
               //
               //  e.g:    LGR R1, R2
               //          SLL R1, 1
               //      ==>
               //          SLLK R1, R2, 1
               //
               //      NOT valid as R1's high word will not be cleared

               return false;
               }

            switch (curOpCode)
               {
               case TR::InstOpCode::AR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::ARK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::AGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::AGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::ALR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::ALRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::ALGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::ALGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::AHI:
                  {
                  int16_t imm = ((TR::S390RIInstruction*)current)->getSourceImmediate();
                  newInstr = generateRIEInstruction(_cg, TR::InstOpCode::AHIK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }
               case TR::InstOpCode::AGHI:
                  {
                  int16_t imm = ((TR::S390RIInstruction*)current)->getSourceImmediate();
                  newInstr = generateRIEInstruction(_cg, TR::InstOpCode::AGHIK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }
               case TR::InstOpCode::NR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::NRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::NGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::NGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::XR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::XRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::XGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::XGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::OR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::ORK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::OGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::OGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SLA:
                  {
                  int16_t imm = ((TR::S390RSInstruction*)current)->getSourceImmediate();
                  TR::MemoryReference * mf = ((TR::S390RSInstruction*)current)->getMemoryReference();
                  if(mf != NULL)
                  {
                    mf->resetMemRefUsedBefore();
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SLAK, node, lgrTargetReg, lgrSourceReg, mf, prevInstr);
                  }
                  else
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SLAK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SLL:
                  {
                  int16_t imm = ((TR::S390RSInstruction*)current)->getSourceImmediate();
                  TR::MemoryReference * mf = ((TR::S390RSInstruction*)current)->getMemoryReference();
                  if(mf != NULL)
                  {
                   mf->resetMemRefUsedBefore();
                   newInstr = generateRSInstruction(_cg, TR::InstOpCode::SLLK, node, lgrTargetReg, lgrSourceReg, mf, prevInstr);
                  }
                  else
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SLLK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }

               case TR::InstOpCode::SRA:
                  {
                  int16_t imm = ((TR::S390RSInstruction*)current)->getSourceImmediate();
                  TR::MemoryReference * mf = ((TR::S390RSInstruction *)current)->getMemoryReference();
                  if(mf != NULL)
                  {
                    mf->resetMemRefUsedBefore();
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SRAK, node, lgrTargetReg, lgrSourceReg, mf, prevInstr);
                  }
                  else
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SRAK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SRL:
                  {
                  int16_t imm = ((TR::S390RSInstruction*)current)->getSourceImmediate();
                  TR::MemoryReference * mf = ((TR::S390RSInstruction*)current)->getMemoryReference();
                  if(mf != NULL)
                  {
                    mf->resetMemRefUsedBefore();
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SRLK, node, lgrTargetReg, lgrSourceReg, mf, prevInstr);
                  }
                  else
                    newInstr = generateRSInstruction(_cg, TR::InstOpCode::SRLK, node, lgrTargetReg, lgrSourceReg, imm, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::SRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::SGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SLR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::SLRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               case TR::InstOpCode::SLGR:
                  {
                  newInstr = generateRRRInstruction(_cg, TR::InstOpCode::SLGRK, node, lgrTargetReg, lgrSourceReg, srcReg, prevInstr);
                  break;
                  }
               default:
                  return false;
               }

            // Merge LR and the current inst into distinct Operants
            _cg->deleteInst(instr);
            _cg->replaceInst(current, newInstr);
            _cursor = instr->getNext();
            performed = true;
            TR::Instruction * s390NewInstr = newInstr;
            }
         return performed;
         }
      windowSize++;
      current = current->getNext();
      }

   return performed;
   }

void
TR_S390Peephole::markBlockThatModifiesRegister(TR::Instruction * cursor,
                                               TR::Register * targetReg)
   {
   // some stores use targetReg as part of source
   if (targetReg && !cursor->isStore() && !cursor->isCompare())
      {
      if (targetReg->getRegisterPair())
         {
         TR::RealRegister * lowReg = toRealRegister(targetReg->getLowOrder());
         TR::RealRegister * highReg = toRealRegister(targetReg->getHighOrder());
         lowReg->setModified(true);
         highReg->setModified(true);
         if (cursor->getOpCodeValue() == TR::InstOpCode::getLoadMultipleOpCode())
            {
            uint8_t numRegs = (lowReg->getRegisterNumber() - highReg->getRegisterNumber())-1;
            if (numRegs > 0)
               {
               for (uint8_t i=highReg->getRegisterNumber()+1; i++; i<= numRegs)
                  {
                  _cg->getS390Linkage()->getRealRegister(REGNUM(i))->setModified(true);
                  }
               }
            }
         }
      else
         {
         // some stores use targetReg as part of source
         TR::RealRegister * rReg = toRealRegister(targetReg);
         rReg->setModified(true);
         }
      }
   }

void
TR_S390Peephole::reloadLiteralPoolRegisterForCatchBlock()
   {
   // When dynamic lit pool reg is disabled, we lock R6 as dedicated lit pool reg.
   // This causes a failure when we come back to a catch block because the register context will not be preserved.
   // Hence, we can not assume that R6 will still contain the lit pool register and hence need to reload it.

   bool isZ10 = comp()->target().cpu.getSupportsArch(TR::CPU::z10);

   // we only need to reload literal pool for Java on older z architecture on zos when on demand literal pool is off
   if ( comp()->target().isZOS() && !isZ10 && !_cg->isLiteralPoolOnDemandOn())
      {
      // check to make sure that we actually need to use the literal pool register
      TR::Snippet * firstSnippet = _cg->getFirstSnippet();
      if ((_cg->getLinkage())->setupLiteralPoolRegister(firstSnippet) > 0)
         {
         // the imm. operand will be patched when the actual address of the literal pool is known at binary encoding phase
         TR::S390RILInstruction * inst = (TR::S390RILInstruction *) generateRILInstruction(_cg, TR::InstOpCode::LARL, _cursor->getNode(), _cg->getLitPoolRealRegister(), reinterpret_cast<void*>(0xBABE), _cursor);
         inst->setIsLiteralPoolAddress();
         }
      }
   }

/** \details
 *     This transformation may not always be possible because the LHI instruction does not modify the condition
 *     code while the XR instruction does. We must be pessimistic in our algorithm and carry out the transformation
 *     if and only if there exists an instruction B that sets the condition code between the LHI instruction A and
 *     some instruction C that reads the condition code.
 *
 *     That is, we are trying to find instruction that comes after the LHI in the execution order that will clobber
 *     the condition code before any instruction that consumes a condition code.
 */
bool TR_S390Peephole::ReduceLHIToXR()
  {
  TR::S390RIInstruction* lhiInstruction = static_cast<TR::S390RIInstruction*>(_cursor);

  if (lhiInstruction->getSourceImmediate() == 0)
     {
     TR::Instruction* nextInstruction = lhiInstruction->getNext();

     while (nextInstruction != NULL && !nextInstruction->getOpCode().readsCC())
        {
        if (nextInstruction->getOpCode().setsCC() || nextInstruction->getNode()->getOpCodeValue() == TR::BBEnd)
           {
           TR::DebugCounter::incStaticDebugCounter(_cg->comp(), "z/peephole/LHI/XR");

           TR::Instruction* xrInstruction = generateRRInstruction(_cg, TR::InstOpCode::XR, lhiInstruction->getNode(), lhiInstruction->getRegisterOperand(1), lhiInstruction->getRegisterOperand(1));

           _cg->replaceInst(lhiInstruction, xrInstruction);

           _cursor = xrInstruction;

           return true;
           }

        nextInstruction = nextInstruction->getNext();
        }
     }

  return false;
  }

void
TR_S390Peephole::perform()
   {
   TR::Delimiter d(comp(), comp()->getOption(TR_TraceCG), "Peephole");

   if (comp()->getOption(TR_TraceCG))
      printInfo("\nPeephole Optimization Instructions:\n");

   bool moveInstr;

      {
      while (_cursor != NULL)
         {
         if (_cursor->getNode() != NULL && _cursor->getNode()->getOpCodeValue() == TR::BBStart)
            {
            comp()->setCurrentBlock(_cursor->getNode()->getBlock());
            // reload literal pool for catch blocks that need it
            TR::Block * blk = _cursor->getNode()->getBlock();
            if (blk->isCatchBlock() && (blk->getFirstInstruction() == _cursor))
               reloadLiteralPoolRegisterForCatchBlock();
            }

         if (_cursor->getOpCodeValue() != TR::InstOpCode::FENCE &&
            _cursor->getOpCodeValue() != TR::InstOpCode::ASSOCREGS &&
            _cursor->getOpCodeValue() != TR::InstOpCode::DEPEND)
            {
            TR::RegisterDependencyConditions * deps = _cursor->getDependencyConditions();
            bool depCase = (_cursor->isBranchOp() || _cursor->isLabel()) && deps;
            if (depCase)
               {
               _cg->getS390Linkage()->markPreservedRegsInDep(deps);
               }

            //handle all other regs
            TR::Register *reg = _cursor->getRegisterOperand(1);
            markBlockThatModifiesRegister(_cursor, reg);
            }

         // this code is used to handle all compare instruction which sets the compare flag
         // we can eventually extend this to include other instruction which sets the
         // condition code and and uses a complemented register
         if (_cursor->getOpCode().setsCompareFlag() &&
            _cursor->getOpCodeValue() != TR::InstOpCode::CHLR &&
            _cursor->getOpCodeValue() != TR::InstOpCode::CLHLR)
            {
            trueCompEliminationForCompare();
            if (comp()->getOption(TR_TraceCG))
               printInst();
            }

         if (_cursor->isBranchOp())
            forwardBranchTarget();

         moveInstr = true;
         switch (_cursor->getOpCodeValue())
            {
            case TR::InstOpCode::CPYA:
               {
               LRReduction();
               break;
               }
            case TR::InstOpCode::LDR:
               {
               LRReduction();
               break;
               }

            case TR::InstOpCode::LHI:
               {
               // This optimization is disabled by default because there exist cases in which we cannot determine whether this transformation
               // is functionally valid or not. The issue resides in the various runtime patching sequences using the LHI instruction as a
               // runtime patch point for an offset. One concrete example can be found in the virtual dispatch sequence for unresolved calls
               // on 31-bit platforms where an LHI instruction is used and is patched at runtime.
               //
               // TODO (Issue #255): To enable this optimization we need to implement an API which marks instructions that will be patched at
               // runtime and prevent ourselves from modifying such instructions in any way.
               //
               // ReduceLHIToXR();
               }
               break;
            case TR::InstOpCode::LHR:
               {
               break;
               }
            case TR::InstOpCode::LR:
            case TR::InstOpCode::LTR:
               {
               LRReduction();
               if (comp()->getOption(TR_TraceCG))
                  {
                  printInst();
                  }

               if (attemptZ7distinctOperants())
                  {
                  moveInstr = false;
                  if (comp()->getOption(TR_TraceCG))
                     printInst();
                  }

               break;
               }
            case TR::InstOpCode::LGR:
            case TR::InstOpCode::LTGR:
               {
               LRReduction();
               if (comp()->getOption(TR_TraceCG))
                  {
                  printInst();
                  }

               if (attemptZ7distinctOperants())
                  {
                  moveInstr = false;
                  if (comp()->getOption(TR_TraceCG))
                     printInst();
                  }

               break;
               }
            case TR::InstOpCode::CRJ:
               {
               trueCompEliminationForCompareAndBranch();

               if (comp()->getOption(TR_TraceCG))
                  printInst();

               break;
               }

            case TR::InstOpCode::CGRJ:
               {
               trueCompEliminationForCompareAndBranch();

               if (comp()->getOption(TR_TraceCG))
                  printInst();

               break;
               }

            case TR::InstOpCode::CRB:
            case TR::InstOpCode::CRT:
            case TR::InstOpCode::CGFR:
            case TR::InstOpCode::CGRT:
            case TR::InstOpCode::CLR:
               {
               trueCompEliminationForCompareAndBranch();

               if (comp()->getOption(TR_TraceCG))
                  printInst();

               break;
               }
            case TR::InstOpCode::CLRB:
            case TR::InstOpCode::CLRJ:
            case TR::InstOpCode::CLRT:
            case TR::InstOpCode::CLGRB:
            case TR::InstOpCode::CLGFR:
            case TR::InstOpCode::CLGRT:
               {
               trueCompEliminationForCompareAndBranch();

               if (comp()->getOption(TR_TraceCG))
                  printInst();

               break;
               }

            case TR::InstOpCode::LCGFR:
            case TR::InstOpCode::LCGR:
            case TR::InstOpCode::LCR:
               {
               trueCompEliminationForLoadComp();

               if (comp()->getOption(TR_TraceCG))
                  printInst();

               break;
               }

            default:
               {
               if (comp()->getOption(TR_TraceCG))
                  printInst();
               break;
               }
            }

         if (moveInstr == true)
            _cursor = _cursor->getNext();
         }
      }

   if (comp()->getOption(TR_TraceCG))
      printInfo("\n\n");
   }

bool TR_S390Peephole::forwardBranchTarget()
   {
   TR::LabelSymbol *targetLabelSym = NULL;
   switch(_cursor->getOpCodeValue())
      {
      case TR::InstOpCode::BRC: targetLabelSym = ((TR::S390BranchInstruction*)_cursor)->getLabelSymbol(); break;
      case TR::InstOpCode::CRJ:
      case TR::InstOpCode::CGRJ:
      case TR::InstOpCode::CIJ:
      case TR::InstOpCode::CGIJ:
      case TR::InstOpCode::CLRJ:
      case TR::InstOpCode::CLGRJ:
      case TR::InstOpCode::CLIJ:
      case TR::InstOpCode::CLGIJ: targetLabelSym = toS390RIEInstruction(_cursor)->getBranchDestinationLabel(); break;
      default:
         return false;
      }
   if (!targetLabelSym)
      return false;

   auto targetLabelInsn = targetLabelSym->getInstruction();
   if (!targetLabelInsn)
      return false;
   auto tmp = targetLabelInsn;
   while (tmp->isLabel() || tmp->getOpCodeValue() == TR::InstOpCode::FENCE)  // skip labels and fences
      tmp = tmp->getNext();
   if (tmp->getOpCodeValue() == TR::InstOpCode::BRC)
      {
      auto firstBranch = (TR::S390BranchInstruction*)tmp;
      if (firstBranch->getBranchCondition() == TR::InstOpCode::COND_BRC &&
          performTransformation(comp(), "\nO^O S390 PEEPHOLE: forwarding branch target in %p\n", _cursor))
         {
         auto newTargetLabelSym = firstBranch->getLabelSymbol();
         switch(_cursor->getOpCodeValue())
            {
            case TR::InstOpCode::BRC: ((TR::S390BranchInstruction*)_cursor)->setLabelSymbol(newTargetLabelSym); break;
            case TR::InstOpCode::CRJ:
            case TR::InstOpCode::CGRJ:
            case TR::InstOpCode::CIJ:
            case TR::InstOpCode::CGIJ:
            case TR::InstOpCode::CLRJ:
            case TR::InstOpCode::CLGRJ:
            case TR::InstOpCode::CLIJ:
            case TR::InstOpCode::CLGIJ:
               toS390RIEInstruction(_cursor)->setBranchDestinationLabel(newTargetLabelSym); break;
            default:
               return false;
            }
         return true;
         }
      }
   return false;
   }
