;/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen
 *
 * For licensing details see LICENSE
 */

#include <llvm/IR/TypeFinder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/Pass.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/CallGraph.h>

#include "StructChecker.h"
#include "Annotation.h"

using namespace llvm;
using namespace std;

const string test_funcs[] = {
    "verify_replay",
    "user_preparse",
    "unix_bind",
	"necp_client_copy_internal"
};

bool StructCheckerPass::doInitialization(Module* M) {
    return false;
}

bool StructCheckerPass::doFinalization(Module* M) {
    return false;
}

bool StructCheckerPass::doModulePass(Module* M) {

    TypeFinder usedStructTypes;
    usedStructTypes.run(*M, false);
    for (auto &st : usedStructTypes) {
        if (st->isOpaque())
            continue;
        // only analyze modules using structures we interested in
        std::string structName = getScopeName(st, M);
        KeyStructMap::iterator it = Ctx->keyStructMap.find(structName);
        if (it == Ctx->keyStructMap.end())
            continue;
        StructInfo* structInfo = it->second;
        for (auto &copy : structInfo->copyInfo) {
            for (auto &srcInfo : copy.second) {
                Instruction* copySite = dyn_cast<Instruction>(srcInfo.first);                
                StructInfo::SiteInfo& siteInfo = srcInfo.second;
                // copy site is an instruction
                if (copySite == nullptr)
                    continue;

                Value* lenValue = siteInfo.lenValue;
                Instruction* retrieveLenInst = dyn_cast<Instruction>(lenValue);
                // retrieve site is an instruction
                if (retrieveLenInst == nullptr)
                    continue;

                Module* RetrieveLenM = retrieveLenInst->getModule();
                // only analyze this Module
                if (RetrieveLenM != M)
                    continue;

                GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(retrieveLenInst);
                // retrieve site is a GEP
                if (GEP == nullptr) {
                    KA_LOGS(0, "[WARNING] Retrieve Length Inst is not GEP: \n");
                    DEBUG_Inst(0, retrieveLenInst); KA_LOGS(0, "\n");
                    continue;
                }
                Value* base = GEP->getOperand(0);
                // FIXME: deal with alias
                for (Value::use_iterator ui = base->use_begin(), ue = base->use_end();
                    ui != ue; ui++) {
                    Instruction* I = dyn_cast<Instruction>(ui->getUser());
                    SrcSlice tracedV;
                    collectChecks(siteInfo, I, copySite, base, "", 0, tracedV);
                }

                SmallPtrSet<Value*, 16> aliasPtrSet = 
                    getAliasSet(base, GEP->getParent()->getParent());
                for (auto V : aliasPtrSet) {
                    for(Value::use_iterator ui = V->use_begin(), ue = base->use_end(); 
                        ui != ue; ui++) {
                        Instruction* I = dyn_cast<Instruction>(ui->getUser());
                        SrcSlice tracedV;
                        collectChecks(siteInfo, I, copySite, V, "", 0, tracedV);
                    }
                }
            }
        }
    }
    return false;
}

void StructCheckerPass::collectChecks(
    StructInfo::SiteInfo& siteInfo, 
    Instruction* I, 
    Instruction* copySite, 
    Value* V,
    string offset, 
    unsigned loadDep,
    SrcSlice& tracedV) {

    // recursion temination condition
    if (I == nullptr ||
        tracedV.size() > 256 ||
        std::find(tracedV.begin(), tracedV.end(), I) != tracedV.end())
        return;
    tracedV.push_back(I);

    if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(I)) {
        if (GEP->getNumIndices() == 1)
            return;

        PointerType* ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
        assert(ptrType != nullptr);
        Type* baseType = ptrType->getPointerElementType();
        StructType* stType = dyn_cast<StructType>(baseType);
        if (stType == nullptr)
            return;        

        // obtain offset
        ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
        uint64_t curOffset = CI->getZExtValue();
        offset = (offset == "") ? to_string(curOffset) : offset + "+" + to_string(curOffset);
        
        // collect forward
        for (Value::use_iterator ui = GEP->use_begin(), ue = GEP->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, GEP, offset, loadDep, tracedV);
        }
        return;
    }

    if (LoadInst* LI = dyn_cast<LoadInst>(I)) { 
        if (offset == "") { // first GEP then Load
            KA_LOGS(0, "[WARNING] weird Load: ");
            DEBUG_Inst(0, LI);
        }
        if (loadDep == 1) // one load to access structure field
            return;
        unsigned reachableRet = isReachable(I->getParent(), copySite->getParent());
        if (reachableRet == 0) { // skip not reachable Inst
            return;
        }

        // add this LI usage to siteInfo
        StructInfo::CheckSrc checkSrc; // Note LI has no check
        StructInfo::CheckMap::iterator it = siteInfo.copyCheckMap.find(offset);
        if (it == siteInfo.copyCheckMap.end()) {
            StructInfo::CheckInfo checkInfo;
            checkInfo.insert(std::make_pair(I, checkSrc));
            siteInfo.copyCheckMap.insert(std::make_pair(offset, checkInfo));
        } else {
            it->second.insert(std::make_pair(I, checkSrc));
        }

        // collect forward
        for (Value::use_iterator ui = LI->use_begin(), ue = LI->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, LI, offset, 1, tracedV);
        }
        return;
    }

    if (ICmpInst *ICmp = dyn_cast<ICmpInst>(I)) {
        BasicBlock* BB = ICmp->getParent();
        BranchInst* BI = dyn_cast<BranchInst>(&*(BB->rbegin()));
        if (BI == nullptr)
            return;
        unsigned reachableTrueRet, reachableFalseRet;
        if (BI->isUnconditional()) {
            reachableTrueRet = isReachable(BI->getSuccessor(0), copySite->getParent());
            reachableFalseRet = reachableTrueRet;
        } else if (BI->isConditional()) {
            assert(BI->getNumSuccessors() == 2);
            reachableTrueRet = isReachable(BI->getSuccessor(0), copySite->getParent());
            reachableFalseRet = isReachable(BI->getSuccessor(1), copySite->getParent());
        }

        if (reachableTrueRet == 0 && reachableFalseRet == 0) { // not reachable
            return;
        }

        // collect source of another op
        Value* op1 = ICmp->getOperand(0);
        Value* op2 = ICmp->getOperand(1);
        Value* unknownSrcOp = (op1 == V)? op2 : op1;
        StructInfo::CheckSrc checkSrc;
        SrcSlice tracedV;
        if (unknownSrcOp == op1)
            collectCmpSrc(unknownSrcOp, checkSrc.src1, tracedV, 0);
        else
            collectCmpSrc(unknownSrcOp, checkSrc.src2, tracedV, 0);
        
        if (reachableTrueRet == 1 && reachableFalseRet == 0)
            checkSrc.branchTaken = 0;
        else if (reachableTrueRet == 0 && reachableFalseRet == 1)
            checkSrc.branchTaken = 1;
        else if (reachableTrueRet == 1 && reachableFalseRet == 1)
            checkSrc.branchTaken = 2;

        // add this ICmp comparison to siteInfo
        StructInfo::CheckMap::iterator it = siteInfo.copyCheckMap.find(offset);
        if (it == siteInfo.copyCheckMap.end()) {
            StructInfo::CheckInfo checkInfo;
            checkInfo.insert(std::make_pair(I, checkSrc));
            siteInfo.copyCheckMap.insert(std::make_pair(offset, checkInfo));
        } else {
            it->second.insert(std::make_pair(I, checkSrc));
        }
        return;
    }

    if (CallInst* CI = dyn_cast<CallInst>(I)) {
        if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(I))
            return;
        unsigned i = 0;
        for (i = 0; i < CI->arg_size(); i++) {
            if (CI->getArgOperand(i) == V)
                break;
        }

        if (i == CI->arg_size()) {
            KA_LOGS(0, "[WARNING] weird call ");
            DEBUG_Inst(0, CI);
            KA_LOGV(0, V);
            return;
        }

        for (Function* F : Ctx->Callees[CI]) {
            if (F->isDeclaration())
                continue;
            if (F->arg_size() != CI->arg_size())
                continue;
            Argument* A = F->getArg(i);
            for (Value::use_iterator ui = A->use_begin(), ue = A->use_end();
                ui != ue; ui++) {
                Instruction* I = dyn_cast<Instruction>(ui->getUser());
                collectChecks(siteInfo, I, copySite, A, offset, loadDep, tracedV);
            }
        }
        return;
    }

    if (BinaryOperator* BO = dyn_cast<BinaryOperator>(I)) {
        // collect forward
        for (Value::use_iterator ui = BO->use_begin(), ue = BO->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, BO, offset, loadDep, tracedV);
        }
        return;
    }

    if (UnaryInstruction* UI = dyn_cast<UnaryInstruction>(I)) {
        // collect forward
        for (Value::use_iterator ui = UI->use_begin(), ue = UI->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, UI, offset, loadDep, tracedV);
        }
        return;
    }

    if (SelectInst* SI = dyn_cast<SelectInst>(I)) {
        for (Value::use_iterator ui = SI->use_begin(), ue = SI->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, SI, offset, loadDep, tracedV);
        }
        return;
    }

    if (PHINode* PN = dyn_cast<PHINode>(I)) {
        for (Value::use_iterator ui = PN->use_begin(), ue = PN->use_end();
            ui != ue; ui++) {
            Instruction* I = dyn_cast<Instruction>(ui->getUser());
            collectChecks(siteInfo, I, copySite, PN, offset, loadDep, tracedV);
        }
        return;
    }
}

unsigned StructCheckerPass::isReachable(BasicBlock* from, BasicBlock* to) {

    std::vector<BasicBlock*> workList;
    std::vector<BasicBlock*> tracedBB;
    workList.clear();
    workList.push_back(from);

    while (!workList.empty() & (tracedBB.size() < 512)) {
        BasicBlock* BB = workList.back();
        workList.pop_back();
        if (BB == to) // reached
            return 1;

        // BB has been traced
        if (std::find(tracedBB.begin(), tracedBB.end(), BB) != tracedBB.end())
            continue;
        tracedBB.push_back(BB);

        // add Terminator-associated successors to worklist
        Instruction* TI = BB->getTerminator();
        for(unsigned i = 0; i < TI->getNumSuccessors(); i++) {
            BasicBlock* SuccBB = TI->getSuccessor(i);
            workList.push_back(SuccBB);
        }

        // inside function
        if (from->getParent() == to->getParent())
            continue;
        // add CallInst-associated successors to worklist
        for(auto &I : *BB) {
            if (CallInst* CI = dyn_cast<CallInst>(&I)) {
                if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(&I))
                    continue;
                for (Function* F : Ctx->Callees[CI]) {
                    if (F->isDeclaration())
                        continue;
                    BasicBlock* EntryBB = &(F->getEntryBlock());
                    workList.push_back(EntryBB);
                }
            }
        }
    }
    return 0;
}

void StructCheckerPass::collectCmpSrc(
    Value* V, 
    StructInfo::CmpSrc& cmpSrc, 
    SrcSlice& tracedV, 
    unsigned loadDep) {

    if (V == nullptr ||
        tracedV.size() > 128 || 
        std::find(tracedV.begin(), tracedV.end(), V) != tracedV.end())
        return;

    tracedV.push_back(V);

    if (CallInst* CI = dyn_cast<CallInst>(V)) {
        if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(V)) {
            cmpSrc.push_back(V);
            return;
        }
        for (Function* F : Ctx->Callees[CI]) {
            if (F->isDeclaration()) {
                cmpSrc.push_back(V);
                return;
            }
            // collect backward        
            for (inst_iterator i = inst_begin(F), e = inst_end(F);
                i != e; i++) {
                if (ReturnInst* RI = dyn_cast<ReturnInst>(&*i)) {
                    Value *RV = RI->getReturnValue();
                    collectCmpSrc(RV, cmpSrc, tracedV, loadDep);
                }
            }
        }
        return;
    }

    if (UnaryInstruction* UI = dyn_cast<UnaryInstruction>(V)) {
        Value* V = UI->getOperand(0);
        // collect backward        
        collectCmpSrc(V, cmpSrc, tracedV, loadDep);
        return;
    }

    if (BinaryOperator* BO = dyn_cast<BinaryOperator>(V)) {
        // collect backward        
        for (unsigned i = 0, e = BO->getNumOperands(); i != e; i++) {
            Value* Opd = BO->getOperand(i);
            collectCmpSrc(Opd, cmpSrc, tracedV, loadDep);
        }
        return;
    }

    if (LoadInst* LI = dyn_cast<LoadInst>(V)) {
        if(loadDep == 1) // only one load to find source
            return;
        Value* V = LI->getPointerOperand();
        collectCmpSrc(V, cmpSrc, tracedV, loadDep);
        return;
    }

    if (PHINode* PN = dyn_cast<PHINode>(V)) {
        // collect backward        
        for (unsigned i = 0, e = PN->getNumIncomingValues(); 
                i != e; i++) {
            Value* IV = PN->getIncomingValue(i);
            collectCmpSrc(IV, cmpSrc, tracedV, loadDep);
        }
        return;
    }

    if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(V)) {
        if (GEP->getNumIndices() == 1)
            return;
        PointerType* ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
        assert(ptrType != nullptr);
        Type* baseType = ptrType->getPointerElementType();

        if (StructType* stType = dyn_cast<StructType>(baseType)) {
            ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
            assert(CI != nullptr && "GEP's index is not constant");
            cmpSrc.push_back(V);
        }
        return;
    }

    if (Constant *C = dyn_cast<Constant>(V)) {
        cmpSrc.push_back(V);
        return;
    }

    if (SelectInst* SI = dyn_cast<SelectInst>(V)) {
        collectCmpSrc(SI->getTrueValue(), cmpSrc, tracedV, loadDep);
        collectCmpSrc(SI->getFalseValue(), cmpSrc, tracedV, loadDep);
        return;
    }

    if (Argument* A = dyn_cast<Argument>(V)) {
       cmpSrc.push_back(V);
        return;
    }

    KA_LOGS(0, "[Unknown Value] "); KA_LOGV(0, V);

}


SmallPtrSet<Value*, 16> StructCheckerPass::getAliasSet(Value* V, Function* F) {
    
	SmallPtrSet<Value *, 16> null;
    null.clear();

    auto aliasMap = Ctx->FuncPAResults.find(F);
    if(aliasMap == Ctx->FuncPAResults.end())
        return null;

    auto alias = aliasMap->second.find(V);
    if(alias == aliasMap->second.end()){
        return null;
    }

    return alias->second; 
}

void StructCheckerPass::dumpChecks() {
    RES_REPORT("\n=========  printing copyer constraints===========\n");
    for (auto copyer : Ctx->keyStructMap) {
        StructInfo *st = copyer.second;
        if(st->copyInfo.size() == 0)
            continue;
        st->dumpCopyChecks();
    }
    RES_REPORT("\n======= end printing copyer constraints ==========\n");
}
