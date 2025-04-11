/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen, Zhenpeng Lin
 *
 * For licensing details see LICENSE
 */

#include <cstddef>
#include <llvm/IR/Argument.h>
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

#include "CopyAnalyzer.h"
#include "Annotation.h"
#include "Common.h"
#include "GlobalCtx.h"
#include "StructAnalyzer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

using namespace llvm;
using namespace std;

extern cl::opt<bool> IgnoreReachable;

// initialize moduleStructMap
bool CopyAnalyzerPass::doInitialization(Module* M) {
    return false;
}

// determine "allocable" and "copyable" to compute allocInstMap and copyInstMap
bool CopyAnalyzerPass::doModulePass(Module* M) {

    ModuleStructMap::iterator it = Ctx->moduleStructMap.find(M);
    assert(it != Ctx->moduleStructMap.end() && 
            "M is not analyzed in doInitialization");

	for (Function &F : *M)
        runOnFunction(&F);

    return false;
}

// check if the function is called by a priviledged device
// return true if the function is priviledged.
bool CopyAnalyzerPass::isPriviledged(llvm::Function *F) {
    // return false;
    SmallVector<Function*, 4> workList;
    workList.clear();
    workList.push_back(F);

    FuncSet seen;
    seen.clear();

    while (!workList.empty()) {
        Function* F = workList.pop_back_val();

        // check if the function lies in the deny list
        if (Ctx->devDenyList.find(F) != Ctx->devDenyList.end()) {
            return true;
        }

        if (!seen.insert(F).second)
            continue;

        CallerMap::iterator it = Ctx->Callers.find(F);
        if (it != Ctx->Callers.end()) {
            for (auto calleeInst: it->second) {
                Function* F = calleeInst->getParent()->getParent();
                workList.push_back(F);
            }
        }
    }
    return false;
}


// start analysis from calling to allocation or copy functions
void CopyAnalyzerPass::runOnFunction(Function *F) {
    if(!IgnoreReachable){
        FuncSet Syscalls = reachableSyscall(F);
        if(Syscalls.size() == 0){
            return;
        }
        KA_LOGS(1, F->getName() << " can be reached by " << Syscalls.size() << " syscalls\n");
    }

    // skip functions in .init.text which is used only during booting
    if(F->hasSection() && F->getSection().str() == ".init.text")
        return;

    for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; i++) {
        Instruction* I = &*i;
        if (CallInst *callInst = dyn_cast<CallInst>(I)) {
            const Function* callee = callInst->getCalledFunction();
            if (!callee)
                callee = dyn_cast<Function>(callInst->getCalledOperand()->stripPointerCasts());
            if (callee) {
                std::string calleeName = callee->getName().str();
                if (isCall2Copy(calleeName)){
                    analyzeRouter(callInst, calleeName);
                    analyzeBridge(callInst, calleeName);
                }
            }
        }
    }
    return;
}

bool CopyAnalyzerPass::isCall2Copy(std::string calleeName) {
    if (std::find(copyAPIVec.begin(), copyAPIVec.end(),
            calleeName) != copyAPIVec.end())
        return true;
    else if (calleeName.find("memcpy") != string::npos)
        return true;
    else
        return false;
}

void CopyAnalyzerPass::backwardUseAnalysis(llvm::Value *V, std::set<llvm::Value *> &DefineSet){
    // TODO: handle reg2mem store load pair
    if(auto *I = dyn_cast<Instruction>(V)){
        KA_LOGS(2, "backward handling " << *I << "\n");
        if(I->isBinaryOp() || dyn_cast<ICmpInst>(I)){
            KA_LOGS(2, *I << " backward Adding " << *V << "\n");
            DefineSet.insert(V);

            for (unsigned i = 0, e = I->getNumOperands(); i != e; i++) {
                Value* Opd = I->getOperand(i);
                KA_LOGS(2, "backward Adding " << *V << "\n");
                DefineSet.insert(V);
                if (dyn_cast<ConstantInt>(Opd))
                    continue;
                backwardUseAnalysis(Opd, DefineSet);
            }

        } else if(dyn_cast<CallInst>(I) ||
                      dyn_cast<SelectInst>(I)){
            KA_LOGS(2, "backward Adding " << *V << "\n");
            DefineSet.insert(V);
        } else if(auto *PN = dyn_cast<PHINode>(I)){

            if(DefineSet.find(V) != DefineSet.end())
                return;

            KA_LOGS(2, "backward Adding " << *V << "\n");
            DefineSet.insert(V);
            // aggressive analysis
            for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; i++) {
                Value* IV = PN->getIncomingValue(i);
                if (dyn_cast<ConstantInt>(IV))
                    continue;
                backwardUseAnalysis(IV, DefineSet);
            }

        }else if(UnaryInstruction* UI = dyn_cast<UnaryInstruction>(V)){
            KA_LOGS(2, "backward Adding " << *V << "\n");
            DefineSet.insert(V);

            backwardUseAnalysis(UI->getOperand(0), DefineSet);
        }else if(auto *GEP = dyn_cast<GetElementPtrInst>(I)){
            // may come from the same struct
            KA_LOGS(2, "backward Adding " << *V << "\n");
            DefineSet.insert(V);

            backwardUseAnalysis(GEP->getOperand(0), DefineSet);
        }else{
            errs() << "Backward Fatal errors , please handle " << *I << "\n";
            // exit(0);
        }
    }else{
        // argument
        KA_LOGS(2, "Backward Adding " << *V << "\n");
        DefineSet.insert(V);
    }
}

llvm::Value* CopyAnalyzerPass::getOffset(llvm::GetElementPtrInst *GEP){
    // FIXME: consider using more sophisicated method
    // Use the last indice of GEP
    return GEP->getOperand(GEP->getNumIndices());
}


void CopyAnalyzerPass::forwardAnalysis(llvm::Value *V, 
                                        std::set<llvm::StoreInst *> &StoreInstSet,
                                        std::set<llvm::Value *> &TrackSet){


    for (auto *User : V->users()){

        if(TrackSet.find(User) != TrackSet.end())
            continue;

        TrackSet.insert(User);

        KA_LOGS(2, "Forward " << *User << "\n");

        // FIXME: should we check if V is SI's pointer?
        if(StoreInst *SI = dyn_cast<StoreInst>(User)){
            StoreInstSet.insert(SI);

            // forward memory alias
            Value *SV = SI->getValueOperand();
            Value *SP = SI->getPointerOperand();

            for(auto *StoreU : SP->users()){
                // alias pair
                if(dyn_cast<LoadInst>(StoreU)){
                    KA_LOGS(2, "Found Store and Load pair " << *StoreU << " " << *User << "\n");
                    forwardAnalysis(StoreU, StoreInstSet, TrackSet);
                }
            }

            // handle struct alias
            if(auto *GEP = dyn_cast<GetElementPtrInst>(SP)){
                Value *red_offset = getOffset(GEP);
                Value *red_obj = GEP->getOperand(0);
                
                KA_LOGS(2, "Marking " << *red_obj << " as red\n");

                for(auto *ObjU : red_obj->users()){
                    if(auto *ObjGEP = dyn_cast<GetElementPtrInst>(ObjU)){

                        if(ObjGEP != GEP && getOffset(ObjGEP) == red_offset){
                            // we found it
                            // and then check if its user is LOAD.
                            for(auto *OGEPUser : ObjGEP->users()){
                                if(dyn_cast<LoadInst>(OGEPUser)){
                                    KA_LOGS(2, "Solved Alias : " << *OGEPUser << " == " << *User << "\n");
                                    forwardAnalysis(OGEPUser, StoreInstSet, TrackSet);
                                }
                            }
                        }
                    }
                }
                // should we forward sturct ?

            }
        } else if(dyn_cast<GetElementPtrInst>(User) ||
                    dyn_cast<ICmpInst>(User) ||
                        dyn_cast<BranchInst>(User) ||
                    dyn_cast<BinaryOperator>(User)){

            forwardAnalysis(User, StoreInstSet, TrackSet);

        } else if(dyn_cast<CallInst>(User) ||
                    dyn_cast<CallBrInst>(User) ||
                    dyn_cast<SwitchInst>(User) ||
                        dyn_cast<ReturnInst>(User)){

                continue;

        // } else if(dyn_cast<UnaryInstruction>(User)){
        } else if(dyn_cast<SExtInst>(User) || dyn_cast<ZExtInst>(User)
                    || dyn_cast<TruncInst>(User)){

            forwardAnalysis(User, StoreInstSet, TrackSet);

        } else if(dyn_cast<PHINode>(User) || 
                    dyn_cast<SelectInst>(User) ||
                        dyn_cast<LoadInst>(User) ||
                    dyn_cast<UnaryInstruction>(User)){
                            
            // TODO: forward PHI node
            forwardAnalysis(User, StoreInstSet, TrackSet);

        } else {
            errs() << "\nForwardAnalysis Fatal errors , please handle " << *User << "\n";
            // exit(0);
        }
    }
}

void CopyAnalyzerPass::analyzeRouter(llvm::CallInst* callInst, std::string calleeName) {
    llvm::Function* F = callInst->getParent()->getParent();

    Value *from = nullptr;
    Value *to = callInst->getArgOperand(0);

    std::vector<llvm::Value *> toSet, fromSet;
    set<llvm::Value* > trackedSet;
    findSources(to, toSet, trackedSet);

    if (calleeName == "__memcpy" ||
               calleeName == "csum_and_memcpy" ||
               calleeName == "____memcpy" ||
               calleeName == "memcpy" ||
               calleeName == "memcpy_common" ||
               calleeName == "nla_memcpy" ||
               calleeName == "llvm.memcpy.p0i8.p0i8.i64") { 
        from = callInst->getArgOperand(1);
    } else if (calleeName.find("memcpy") != std::string::npos) {
        return;
    }

    if (isAtStack(toSet)) return;
    setupRouterInfo(toSet, callInst, from);
}

Value *USER_SPACE = (Value *)0xdeadbeaf;

void CopyAnalyzerPass::analyzeBridge(llvm::CallInst* callInst, std::string calleeName) {

    llvm::Function* F = callInst->getParent()->getParent();

    Value *len = nullptr;
    Value *to = nullptr;
    Value *from = nullptr;

    if (calleeName == "copy_from_user" ||
        calleeName == "_copy_from_user") {
        if (callInst->arg_size() != 3) { 
            KA_LOGS(1, "[-] Weird copy_from_user(): ");
            KA_LOGV(1, callInst);
            return;
        }

        len = callInst->getArgOperand(2);
        from = USER_SPACE;
        to = callInst->getArgOperand(0);
    } else if (calleeName == "__memcpy" ||
               calleeName == "csum_and_memcpy" ||
               calleeName == "____memcpy" ||
               calleeName == "memcpy" ||
               calleeName == "memcpy_common" ||
               calleeName == "nla_memcpy" ||
               calleeName == "llvm.memcpy.p0i8.p0i8.i64") { 
        len = callInst->getArgOperand(2);
        from = callInst->getArgOperand(1);
        to = callInst->getArgOperand(0);
    } else if (calleeName.find("memcpy") != std::string::npos) {
        return;
    }
    else {
        RES_REPORT(calleeName << "\n");
        assert(false && "callee is not a copy channel");
    }

    assert(len != nullptr && to != nullptr && from != nullptr &&
          "both len & to & from are not nullptr");

    // check permission
    Function *body = callInst->getFunction();
    if (isPriviledged(body)) {
        outs() << body->getName() << " is priviledged function for copying\n";
        return;
    }

    KA_LOGS(1, "----- Tracing Len --------\n");
    std::vector<Value *> lenSet;
    std::set<Value *> trackedSet;
    findSources(len, lenSet, trackedSet);
    if (isAtStack(lenSet)) return;

    KA_LOGS(1, "----- Setup SiteInfo Length -------\n");
    setupBridgeInfo(lenSet, callInst, to, from);
}

SmallPtrSet<Value *, 16> CopyAnalyzerPass::getAliasSet(Value *V, Function *F){

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

// void CopyAnalyzerPass::findLenSources(Value* V, std::vector<llvm::Value *> &srcSet, 
//     std::set<llvm::Value* > &trackedSet) {
//     if (trackedSet.count(V) != 0) {
//         return;
//     }
//     trackedSet.insert(V);

//     if (Constant* CI = dyn_cast<Constant>(V)) {
//         return;
//     }

//     if (LoadInst* LI = dyn_cast<LoadInst>(V)) {

//         srcSet.push_back(V);

//         // alias handling
//         Function *F = LI->getFunction();

//         if(!F) return;

//         findLenSources(LI->getPointerOperand(), srcSet, trackedSet);
//         return;
//     }

//     if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(V)) {
//         // TODO f**k aliases
//         KA_LOGS(1, "Here may contain an alias, please check this\n");
//         srcSet.push_back(V);
//         // Heuristic 2: first GEP is enough?
//         // Lewis: Wrong
//         findLenSources(GEP->getPointerOperand(), srcSet, trackedSet);
//         return;
//     }

//     // FIXME: Not examining called function inside can introduce FP
//     // Lewis: this guess hits, add one chicken leg tonight!
//     if (CallInst* CI = dyn_cast<CallInst>(V)) {
//         // Storing callInst helps to check from value type
//         srcSet.push_back(V);
//         // Heuristic 1: calling to strlen()/vmalloc() isn't what we want
//         const Function* callee = CI->getCalledFunction();
//         if (callee != nullptr) {
//             std::string calleeName = callee->getName().str();
//             if (calleeName == "strlen"||
//                 calleeName == "vmalloc")
//                 return;
//         }

//         if(!callee) return;
//         // interprocedural analysis
//         KA_LOGS(1, "Starting interprocedural analysis for "<<callee->getName().str()<<"\n");
//         for(const BasicBlock &BB : *callee){
//             for(const Instruction &I : BB){
//                 if(const ReturnInst *RI = dyn_cast<ReturnInst>(&I)){
//                     if(Value *rValue = RI->getReturnValue()){
//                         findLenSources(rValue, srcSet, trackedSet);
//                     }
//                 }
//             }
//         }
//         // comment this because interprocedural analysis will taint the interesting arguments
//         // for (auto AI = CI->arg_begin(), E = CI->arg_end(); AI != E; AI++) {
//         //     Value* Param = dyn_cast<Value>(&*AI);
//         //     findSources(Param, srcSet, trackedSet);
//         // }
//         return;
//     }

//     if (ZExtInst* ZI = dyn_cast<ZExtInst>(V)) {
//         findLenSources(ZI->getOperand(0), srcSet, trackedSet);
//         return;
//     }

//     if (TruncInst* TI = dyn_cast<TruncInst>(V)) {
//         findLenSources(TI->getOperand(0), srcSet, trackedSet);
//         return;
//     }

//     if (BinaryOperator* BO = dyn_cast<BinaryOperator>(V)) {
//         for (unsigned i = 0, e = BO->getNumOperands(); i != e; i++) {
//             Value* Opd = BO->getOperand(i);
//             if (dyn_cast<Constant>(Opd))
//                 continue;
//             findLenSources(Opd, srcSet, trackedSet);
//         }
//         return;
//     }

//     return;
// }

void CopyAnalyzerPass::findSources(Value* V, std::vector<llvm::Value *> &srcSet, std::set<llvm::Value* > &trackedSet) {

    if (V == nullptr ||
        trackedSet.count(V) != 0
        //  || trackedSet.size() >= 8000
        )
        return;

    trackedSet.insert(V);
    KA_LOGS(2, "FindSource: Adding ");KA_LOGV(2, V);

    if (CallInst* CI = dyn_cast<CallInst>(V)) {
        // Storing callInst helps to check from value type
        srcSet.push_back(V);
        // Heuristic 1: calling to strlen()/vmalloc() isn't what we want
        const Function* callee = CI->getCalledFunction();
        if (callee != nullptr) {
            std::string calleeName = callee->getName().str();
            if (calleeName == "strlen"||
                calleeName == "vmalloc")
                return;
        }

        if(!callee) return;
        // interprocedural analysis
        StringRef tmpName = callee->getName();
        if(tmpName.lower().find("alloc") != string::npos
            || tmpName.lower().find("ALLOC") != string::npos
            || tmpName.lower().find("free") != string::npos
            || tmpName.lower().find("FREE") != string::npos
        ){
            return;
        }
        KA_LOGS(1, "Starting interprocedural analysis for "<<callee->getName().str()<<"\n");
        for(const BasicBlock &BB : *callee){
            for(const Instruction &I : BB){
                if(const ReturnInst *RI = dyn_cast<ReturnInst>(&I)){
                    if(Value *rValue = RI->getReturnValue()){
                        findSources(rValue, srcSet, trackedSet);
                    }
                }        
            }
        }
        // comment this because interprocedural analysis will taint the interesting arguments
        // for (auto AI = CI->arg_begin(), E = CI->arg_end(); AI != E; AI++) {
        //     Value* Param = dyn_cast<Value>(&*AI);
        //     findSources(Param, srcSet, trackedSet);
        // }
        return;
    }

    if(BitCastInst *BCI = dyn_cast<BitCastInst>(V)){
        srcSet.push_back(V);
        findSources(BCI->getOperand(0), srcSet, trackedSet);
        return;
    }

    if (dyn_cast<AllocaInst>(V)){
        srcSet.push_back(V);
        return;
    }

    if (dyn_cast<ConstantPointerNull>(V)){
        srcSet.push_back(V);
        return;
    }

    if (dyn_cast<Constant>(V)) {
        srcSet.push_back(V);
        return;
    }

    // Lewis: it is impossible but leave this in case
    // zipline: we need to handle this
    if (dyn_cast<GlobalVariable>(V)) {
        Constant* Ct = dyn_cast<Constant>(V);
        // if (!Ct)
        //     return;
        // srcSet.push_back(V);
        return;
    }

    // Lewis: it is impossible but leave this in case
    if (ConstantExpr* CE = dyn_cast<ConstantExpr>(V)) {
        findSources(CE->getOperand(0), srcSet, trackedSet);
        return;
    }

    if (Argument* A = dyn_cast<Argument>(V)) {
        srcSet.push_back(V);
        return; // intra-procedural

        // inter-procedural analysis begins following
        Function* callee = A->getParent();
        if (callee == nullptr)
            return;

        for (CallInst* caller : Ctx->Callers[callee]) {
            if (caller) {
                // Lewis: this should never happen
                if (A->getArgNo() >= caller->arg_size())
                    continue;
                Value* arg = caller->getArgOperand(A->getArgNo());
                if (arg == nullptr)
                    continue;

                Function* F = caller->getParent()->getParent();
                KA_LOGS(1, "<<<<<<<<< Cross Analyzing " << F->getName().str() <<  "()\n");
                KA_LOGV(1, caller);
                findSources(arg, srcSet, trackedSet);
            }
        }
    }

    if (LoadInst* LI = dyn_cast<LoadInst>(V)) {

        srcSet.push_back(V);

        // alias handling
        Function *F = LI->getFunction();

        if(!F) return;

        SmallPtrSet<Value *, 16> aliasSet;
        bool foundStore = false;

        aliasSet = getAliasSet(LI->getPointerOperand(), F);

        // add Load's pointer operand to the set
        // it may have a store successor
        aliasSet.insert(LI->getPointerOperand());

        for(auto *alias : aliasSet){
            for(auto *aliasUser : alias->users()){
                if(auto *SI = dyn_cast<StoreInst>(aliasUser)){
                    foundStore |= true;
                    KA_LOGS(1, "FindSource: resolved an alias : " << *LI << " == " << *SI << "\n");
                    findSources(SI->getValueOperand(), srcSet, trackedSet);
                }
            }
        }

        // // return because it maybe loading from a stack value
        // // since we can found a corresponding store
        // if(foundStore)
        //     return;


        findSources(LI->getPointerOperand(), srcSet, trackedSet);
        return;
    }

    if (StoreInst* SI = dyn_cast<StoreInst>(V)) {
        // findSources(SI->getValueOperand(), srcSet, trackedSet);
    }
    
    if (SelectInst* SI = dyn_cast<SelectInst>(V)) {
        findSources(SI->getTrueValue(), srcSet, trackedSet);
        findSources(SI->getFalseValue(), srcSet, trackedSet);
        return ;
    }

    if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(V)) {
        // TODO f**k aliases
        KA_LOGS(1, "Here may contain an alias, please check this\n");
        DEBUG_Inst(2, GEP);
        srcSet.push_back(V);
        // Heuristic 2: first GEP is enough?
        // Lewis: Wrong
        findSources(GEP->getPointerOperand(), srcSet, trackedSet);
        return;
    }

    if (PHINode* PN = dyn_cast<PHINode>(V)) {
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; i++) {
            Value* IV = PN->getIncomingValue(i);
            findSources(IV, srcSet, trackedSet);
        }
        return;
    } 

    if (ICmpInst* ICmp = dyn_cast<ICmpInst>(V)) {
        for (unsigned i = 0, e = ICmp->getNumOperands(); i != e; i++) {
            Value* Opd = ICmp->getOperand(i);
            findSources(Opd, srcSet, trackedSet);
        }
        return;
    }

    if (BinaryOperator* BO = dyn_cast<BinaryOperator>(V)) {
        for (unsigned i = 0, e = BO->getNumOperands(); i != e; i++) {
            Value* Opd = BO->getOperand(i);
            if (dyn_cast<Constant>(Opd))
                continue;
            findSources(Opd, srcSet, trackedSet);
        }
        return;
    }

    if (UnaryInstruction* UI = dyn_cast<UnaryInstruction>(V)) {
        findSources(UI->getOperand(0), srcSet, trackedSet);
        return;
    }

    return;
}

void CopyAnalyzerPass::addCopyInfo(StructInfo *stInfo, llvm::CallInst *callInst, 
                        unsigned offset, llvm::Instruction *I, llvm::StructType *st,
                        unsigned argOffset){

    if(!stInfo)
        return;

    if(stInfo->copyInst.find(callInst) != stInfo->copyInst.end())
        return;
    
    stInfo->copyInst.insert(callInst);

    KeyStructMap::iterator it = Ctx->keyStructMap.find(stInfo->name);
    if(it == Ctx->keyStructMap.end()){
        KA_LOGS(0, "[DEBUG] no struct: " << stInfo->name << "(add it)\n");
        Ctx->keyStructMap.insert(std::make_pair(stInfo->name, stInfo));
    }

    KA_LOGS(1, "Add "<<stInfo->name<<" successful\n");

    // add other SiteInfo in the future
    StructInfo::SiteInfo sInfo;
    sInfo.KEY_OFFSET = argOffset;
    sInfo.setSiteInfoValue(argOffset, I);
    sInfo.setSiteInfoSt(argOffset, st);
    stInfo->addCopySourceInfo(offset, dyn_cast<Value>(callInst), sInfo);

}

void CopyAnalyzerPass::setupRouterInfo(std::vector<Value*> &toSet, CallInst *callInst, Value *from){
    for (std::vector<llvm::Value*>::iterator i = toSet.begin(), 
         e = toSet.end(); i != e; i++) { 
        
        Value *V = *i;

        if(auto *LI = dyn_cast<LoadInst>(V)) {

            KA_LOGS(1, "[Load] "); KA_LOGV(1, LI);

            // check if it's loading a pointer
            Type *type = LI->getPointerOperandType();
            if(!type->getPointerElementType()->isPointerTy()){
                continue;
            }

            Value *lValue = LI->getPointerOperand();
            while(auto *GEP = dyn_cast<GetElementPtrInst>(lValue)){
                KA_LOGS(1, "[GEP] "); KA_LOGV(1, GEP);

                // only pointer value
                if (GEP->getNumIndices() == 1)
                    break;

                PointerType* ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
                assert(ptrType != nullptr);
                Type* baseType = ptrType->getPointerElementType();
                StructType* stType = dyn_cast<StructType>(baseType);
                if (stType == nullptr)
                    break;
                
                ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
                assert(CI != nullptr && "GEP's index is not constant");
                uint64_t offset = CI->getZExtValue();

                Module* M = GEP->getModule();
                StructInfo* stInfo = Ctx->structAnalyzer.getStructInfo(stType, M);

                if(!stInfo || 
                    stInfo->fieldAllocGEP.count(offset) == 0) break;

                bool isRouter = true;
                if (from) {
                    std::vector<Value *> srcFromSet;
                    std::set<Value *> trackedFromSet;
                    findSources(from, srcFromSet, trackedFromSet);

                    isRouter = false;
                    for (auto V : toSet) {
                        if (dyn_cast<Argument>(V))
                            break;
                        if (std::find(srcFromSet.begin(), srcFromSet.end(), V)
                            != srcFromSet.end()) {  // intersect between from and to.
                            setupSiteInfo(srcFromSet, stInfo, callInst, offset, 1);
                            isRouter = true;
                            break;
                        }
                    }
                }
                
                if (isRouter)
                    addCopyInfo(stInfo, callInst, offset, GEP, stType, 0);

                // next loop
                lValue = dyn_cast<Value>(GEP->getPointerOperand());
            }
        }
    }
}

void CopyAnalyzerPass::setupBridgeInfo(std::vector<Value*> &lenSet, CallInst *callInst,
    llvm::Value *to, llvm::Value *from){
    for (std::vector<llvm::Value*>::iterator i = lenSet.begin(), 
         e = lenSet.end(); i != e; i++) { 
        
        Value *V = *i;

        if(auto *LI = dyn_cast<LoadInst>(V)) {

            KA_LOGS(1, "[Load] "); KA_LOGV(1, LI);

            // check if it's loading a pointer
            Type *type = LI->getPointerOperandType();
            if(type->getPointerElementType()->isPointerTy()){
                continue;
            }

            Value *lValue = LI->getPointerOperand();
            while(auto *GEP = dyn_cast<GetElementPtrInst>(lValue)){
                KA_LOGS(1, "[GEP] "); KA_LOGV(1, GEP);

                // only pointer value
                if (GEP->getNumIndices() == 1)
                    break;

                if (auto *AI = dyn_cast<AllocaInst>(GEP->getPointerOperand()))
                    break;

                PointerType* ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
                assert(ptrType != nullptr);
                Type* baseType = ptrType->getPointerElementType();
                StructType* stType = dyn_cast<StructType>(baseType);
                if (stType == nullptr)
                    break;
                
                ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
                assert(CI != nullptr && "GEP's index is not constant");
                uint64_t offset = CI->getZExtValue();

                Module* M = GEP->getModule();
                StructInfo* stInfo = Ctx->structAnalyzer.getStructInfo(stType, M);

                if(!stInfo) break;

                std::vector<Value *> srcFromSet;
                std::set<Value *> trackedFromSet;
                if (from != USER_SPACE) {
                    findSources(from, srcFromSet, trackedFromSet);
                    if (isAtStack(srcFromSet)) from = nullptr;
                }

                std::vector<Value *> srcToSet;
                std::set<Value *> trackedToSet;
                if (to != nullptr) {
                    findSources(to, srcToSet, trackedToSet);
                    if (isAtStack(srcToSet)) to = nullptr;
                }

                if (!to || !from) break;

                // we found length info
                addCopyInfo(stInfo, callInst, offset, GEP, stType, 2);
                setupSiteInfo(srcFromSet, stInfo, callInst, offset, 1);
                setupSiteInfo(srcToSet, stInfo, callInst, offset, 0);

                // next loop
                lValue = dyn_cast<Value>(GEP->getPointerOperand());
            }
        }
    }
}

bool CopyAnalyzerPass::isAtStack(std::vector<Value *> setV) {
    for (auto V : setV) {
        if (auto AI = dyn_cast<AllocaInst>(V)) {
            KA_LOGS(1, "[AI ] " << *AI << "\n");
            if (!AI->getAllocatedType()->isPointerTy())
                return true;
        }
    }
    return false;
}

void CopyAnalyzerPass::setupSiteInfo(std::vector<llvm::Value*> &srcSet, 
                            StructInfo *stInfo, CallInst *callInst, unsigned offset, unsigned argOffset){
    // FIXME: plz keep tracking whether the value is from stack or heap
    // after finding its type.
    StructInfo::SiteInfo *siteInfo = stInfo->getSiteInfo(offset, dyn_cast<Value>(callInst));
    
    if(siteInfo == nullptr)
        return;


    for (std::vector<llvm::Value*>::iterator i = srcSet.begin(), 
         e = srcSet.end(); i != e; i++) {

        Value *V = *i;

        KA_LOGS(2, "setupFromInfo : " << *V << "\n");

        if(auto *CPointerNull = dyn_cast<ConstantPointerNull>(V)){

            siteInfo->setSiteInfoValue(argOffset, V);

            PointerType *ptrType = CPointerNull->getType();
            Type* baseType = ptrType->getPointerElementType();
            StructType* stType = dyn_cast<StructType>(baseType);

            if(!stType){
                return;
            }
            if(stType->getName().find("union.anon") != string::npos ||
                stType->getName().find("struct.anon") != string::npos){
                return;
            }

            if(stType->getName() == stInfo->name){
                siteInfo->TYPE = HEAP_SAME_OBJ;
            }else{
                siteInfo->TYPE = HEAP_DIFF_OBJ;
            }
            siteInfo->setSiteInfoSt(argOffset, stType);
            return;
        } else if(auto *allocInst = dyn_cast<AllocaInst>(V)){

            Type *type = allocInst->getAllocatedType();

            if(!type->isPointerTy()){
                siteInfo->TYPE=STACK;
                siteInfo->setSiteInfoValue(argOffset, V);
            }else{
                siteInfo->TYPE=HEAP_DIFF_OBJ;
                siteInfo->setSiteInfoValue(argOffset, V);
            }
            StructType* stType = dyn_cast<StructType>(type);
            if(stType){
                siteInfo->setSiteInfoSt(argOffset, stType);
            }
            return;

        } else if(dyn_cast<LoadInst>(V) || dyn_cast<GetElementPtrInst>(V)) {

            auto *LI = dyn_cast<LoadInst>(V);
            Value *lValue = V;
            GetElementPtrInst *GEP = nullptr;
            Instruction *I = nullptr;

            if(LI){
                // return on load anyway.
                lValue = LI->getPointerOperand();
                PointerType* ptrType = dyn_cast<PointerType>(LI->getPointerOperandType());
                Type* baseType = ptrType->getPointerElementType();
                StructType* stType = dyn_cast<StructType>(baseType);
                if (stType == nullptr)
                    continue;

                Module* M = LI->getModule();
                StructInfo* stInfoFrom = Ctx->structAnalyzer.getStructInfo(stType, M);

                if(!stInfoFrom 
                    || stType->getName().find("union.anon") == 0
                    || stType->getName().find("struct.anon") == 0)
                    continue;

                if(stInfo->name == stInfoFrom->name){
                    // we found it
                    siteInfo->TYPE = HEAP_SAME_OBJ;
                }else{
                    siteInfo->TYPE = HEAP_DIFF_OBJ;
                }

                siteInfo->setSiteInfoSt(argOffset, stType);
                siteInfo->setSiteInfoValue(argOffset, LI);
                return;
            }

            for(GEP=dyn_cast<GetElementPtrInst>(lValue); GEP;
                    GEP=dyn_cast<GetElementPtrInst>(I->getOperand(0))){

                KA_LOGS(2, "[GEP] in setupFromInfo " << *GEP <<"\n");

                if (!GEP->getPointerOperand())
                    break;
                
                I = GEP;

                if(auto *BCI = dyn_cast<BitCastInst>(GEP->getPointerOperand())){
                    I = BCI;
                }

                // only pointer value
                if (GEP->getNumIndices() == 1)
                    continue;

                PointerType* ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
                assert(ptrType != nullptr);
                Type* baseType = ptrType->getPointerElementType();
                StructType* stType = dyn_cast<StructType>(baseType);
                if (stType == nullptr)
                    continue;

                Module* M = GEP->getModule();
                StructInfo* stInfoFrom = Ctx->structAnalyzer.getStructInfo(stType, M);

                if(!stInfoFrom 
                    || stType->getName().find("union.anon") == 0
                    || stType->getName().find("struct.anon") == 0)
                    continue;

                if(stInfo->name == stInfoFrom->name){
                    // we found it
                    siteInfo->TYPE = HEAP_SAME_OBJ;
                }

                siteInfo->setSiteInfoSt(argOffset, stType);
                siteInfo->setSiteInfoValue(argOffset, GEP);
                // return;
            }
        } else if(auto *BCI = dyn_cast<BitCastInst>(V)){
            KA_LOGS(1, "[BitCast] in setupFromInfo"); KA_LOGV(1, V);

            PointerType* ptrType = dyn_cast<PointerType>(BCI->getSrcTy());
            assert(ptrType != nullptr);
            Type* baseType = ptrType->getPointerElementType();

            StructType* stType = dyn_cast<StructType>(baseType);
            if (stType == nullptr)
                continue;
            if (!stType->hasName())
                continue;

            Module* M = BCI->getParent()->getParent()->getParent();
            StructInfo* stInfoFrom = Ctx->structAnalyzer.getStructInfo(stType, M);

            if(!stInfoFrom 
                || stType->getName().find("union.anon") == 0
                || stType->getName().find("struct.anon") == 0)
                continue;

            // FIXME: what if siteInfo has already been set?

            if(stInfoFrom->name == stInfo->name){
                siteInfo->TYPE = HEAP_SAME_OBJ;
            }

            siteInfo->setSiteInfoSt(argOffset, stType);
            siteInfo->setSiteInfoValue(argOffset, BCI);
            // return;
        } else if(auto *callInst = dyn_cast<CallInst>(V)){
            KA_LOGS(1, "[CallInst] in setupFromInfo " << *callInst <<"\n");
            Function* callee = callInst->getCalledFunction();
            if (!callee)
                callee = dyn_cast<Function>(callInst->getCalledOperand()->stripPointerCasts());
            if (callee) {

                // we assume all functions return memory coming from heap
                std::string calleeName = callee->getName().str();
                siteInfo->setSiteInfoValue(argOffset, callInst);
                siteInfo->TYPE = HEAP_DIFF_OBJ;

                if(calleeName == "m_mtod"){
                    if(stInfo->name == "mbuf")
                        siteInfo->TYPE = HEAP_SAME_OBJ;

                    // find mbuf
		    // use this for compatibility issue
		    // Value *arg = callee->getArg(0);
                    Value *arg = callee->arg_begin();
                    Type *t = arg->getType()->getPointerElementType();
                    if(auto *st = dyn_cast<StructType>(t)){
                        siteInfo->setSiteInfoSt(argOffset, st);
                    }
                    return;
                } else {
                    if(siteInfo->getSiteInfoSt(argOffset)){
                        if(siteInfo->getSiteInfoSt(argOffset)->getName() == stInfo->name){
                            siteInfo->TYPE = HEAP_SAME_OBJ;
                        }
                        return;
                    }
                    // get return type if no bitcast after calling a function
                    Type *type = callee->getReturnType();
                    if(auto *st = dyn_cast<StructType>(type)){
                        if(st->getName() == stInfo->name){
                            siteInfo->TYPE = HEAP_SAME_OBJ;
                        }
                        siteInfo->setSiteInfoSt(argOffset, st);
                    }
                    return;

                }
            }
            
        } else if(dyn_cast<Argument>(V)){
            return;
        }
    
    }
}

// join allocInstMap and copyInstMap to compute moduleStructMap
// reverse moduleStructMap to obtain structModuleMap
// reachable analysis to compute allocSyscallMap and copySyscallMap
// join allocSyscallMap and copySyscallMap to compute keyStructList
bool CopyAnalyzerPass::doFinalization(Module* M) {

    KA_LOGS(1, "[Finalize] " << M->getModuleIdentifier() << "\n");
    ModuleStructMap::iterator it = Ctx->moduleStructMap.find(M);
    assert(it != Ctx->moduleStructMap.end() &&
        "M is not analyzed in doInitialization");

    if (it->second.size() == 0) {
        KA_LOGS(1, "No flexible structure in this module\n");
        return false;
    }
    
    KA_LOGS(1, "Building moduleStructMap ...\n");
    // moduleStructMap: map module to flexible struct "st"
    StructTypeSet tmpStructTypeSet = Ctx->moduleStructMap[M];
    for (StructTypeSet::iterator itr = tmpStructTypeSet.begin(), 
            ite = tmpStructTypeSet.end(); itr != ite; itr++) {

        StructType* st = *itr;
        std::string structName = getScopeName(st, M);
        
        InstMap::iterator liit = Ctx->copyInstMap.find(structName);
        // AllocInstMap::iterator aiit = Ctx->allocInstMap.find(structName);

        // either copy or alloc or both
        if (liit == Ctx->copyInstMap.end() )
            //  || aiit == Ctx->allocInstMap.end() )
            Ctx->moduleStructMap[M].erase(st);
    }

    if (Ctx->moduleStructMap[M].size() == 0) {
        KA_LOGS(1, "Actually no flexible structure in this module\n");
        return false;
    }


    KA_LOGS(1, "Building structModuleMap ...\n");
    // structModuleMap: map flexible struct "st" to module
    for (StructTypeSet::iterator itr = Ctx->moduleStructMap[M].begin(), 
            ite = Ctx->moduleStructMap[M].end(); itr != ite; itr++) {

        StructType* st = *itr;
        std::string structName = getScopeName(st, M);

        StructModuleMap::iterator sit = Ctx->structModuleMap.find(structName);
        if (sit == Ctx->structModuleMap.end()) {
            ModuleSet moduleSet;
            moduleSet.insert(M);
            Ctx->structModuleMap.insert(std::make_pair(structName, moduleSet)) ;
        } else {
            sit->second.insert(M);
        }
    }

    KA_LOGS(1, "Building copySyscallMap & allocSyscallMap ...\n");
    // copySyscallMap: map structName to syscall reaching copy sites
    // allocSyscallMap: map structName to syscall reaching allocation sites
    for (StructTypeSet::iterator itr = Ctx->moduleStructMap[M].begin(),
            ite = Ctx->moduleStructMap[M].end(); itr != ite; itr++) {

        StructType* st = *itr;
        std::string structName = getScopeName(st, M);

        // copySyscallMap
        // XXX
        KA_LOGS(1, "Dealing with copying: " << structName << "\n");
        InstMap::iterator liit = Ctx->copyInstMap.find(structName);
        SyscallMap::iterator lsit = Ctx->copySyscallMap.find(structName);
        if (liit != Ctx->copyInstMap.end() &&
            lsit == Ctx->copySyscallMap.end() // to avoid redundant computation
            ) {
            for (auto I : liit->second) {

                Function* F = I->getParent()->getParent();
                FuncSet syscallSet = reachableSyscall(F);
                if (syscallSet.size() == 0)
                    continue;

                SyscallMap::iterator lsit = Ctx->copySyscallMap.find(structName);
                if (lsit == Ctx->copySyscallMap.end())
                    Ctx->copySyscallMap.insert(std::make_pair(structName, syscallSet));
                else
                    for (auto F : syscallSet)
                        lsit->second.insert(F);
            }
        }

        // allocSyscallMap
        // XXX
        /*
        KA_LOGS(1, "Dealing with allocating: " << structName << "\n");
        AllocInstMap::iterator aiit = Ctx->allocInstMap.find(structName);
        AllocSyscallMap::iterator asit = Ctx->allocSyscallMap.find(structName);
        if (aiit != Ctx->allocInstMap.end() &&
            asit == Ctx->allocSyscallMap.end()
            ) {
            for (auto I : aiit->second) {

                Function* F = I->getParent()->getParent();
                FuncSet syscallSet = reachableSyscall(F);
                if (syscallSet.size() == 0)
                    continue;

                AllocSyscallMap::iterator asit = Ctx->allocSyscallMap.find(structName);
                if (asit == Ctx->allocSyscallMap.end())
                    Ctx->allocSyscallMap.insert(std::make_pair(structName, syscallSet));
                else
                    for (auto F : syscallSet)
                        asit->second.insert(F);
            }
        }
        */
        
    }

    KA_LOGS(1, "Building keyStructList ...\n");
    for (StructTypeSet::iterator itr = Ctx->moduleStructMap[M].begin(), 
            ite = Ctx->moduleStructMap[M].end(); itr != ite; itr++) {

        StructType* st = *itr;
        std::string structName = getScopeName(st, M);

        SyscallMap::iterator lsit = Ctx->copySyscallMap.find(structName);
        // XXX 
        // AllocSyscallMap::iterator asit = Ctx->allocSyscallMap.find(structName);
        
        if (lsit == Ctx->copySyscallMap.end())
            //XXX    
            // || asit == Ctx->allocSyscallMap.end())
            continue;

        KeyStructList::iterator tit = Ctx->keyStructList.find(structName);
        if (tit == Ctx->keyStructList.end()) {
            InstSet instSet;
            for (auto I : Ctx->copyInstMap[structName])
                instSet.insert(I);
            Ctx->keyStructList.insert(std::make_pair(structName, instSet));

        } else {
            for (auto I : Ctx->copyInstMap[structName])
                tit->second.insert(I);
        }
    }
    return false;
}

FuncSet CopyAnalyzerPass::getSyscalls(Function *F){
    ReachableSyscallCache::iterator it = reachableSyscallCache.find(F);
    if (it != reachableSyscallCache.end())
        return it->second;
    FuncSet null;
    return null;
}

FuncSet CopyAnalyzerPass::reachableSyscall(llvm::Function* F) {

    ReachableSyscallCache::iterator it = reachableSyscallCache.find(F);
    if (it != reachableSyscallCache.end())
        return it->second;

    FuncSet reachableFuncs;
    reachableFuncs.clear();

    FuncSet reachableSyscalls;
    reachableSyscalls.clear();

    SmallVector<Function*, 4> workList;
    workList.clear();
    workList.push_back(F);

    while (!workList.empty()) {
        Function* F = workList.pop_back_val();
        if (!reachableFuncs.insert(F).second)
            continue;

        if(reachableSyscallCache.find(F) != reachableSyscallCache.end()){
            FuncSet RS = getSyscalls(F);
            if (!RS.empty()) {
                for(auto *RF : RS){
                    reachableFuncs.insert(RF);
                }
                continue;
            }
        }

        CallerMap::iterator it = Ctx->Callers.find(F);
        if (it != Ctx->Callers.end()) {
            for (auto calleeInst: it->second) {
                Function* F = calleeInst->getFunction();
                workList.push_back(F);
            }
        }
        else {
            FuncSet &FS = Ctx->Name2Func[F->getName().str()];
            for (auto RF : FS) {
                it = Ctx->Callers.find(RF);
                if (it != Ctx->Callers.end()) {
                    for (auto calleeInst: it->second) {
                        Function* F = calleeInst->getFunction();
                        workList.push_back(F);
                    }
                }
            }
        }
    }

    for (auto F : reachableFuncs) {
        StringRef funcNameRef = F->getName();
        std::string funcName = "";
        if (funcNameRef.startswith("__sys_")) {
            funcName = "sys_" + funcNameRef.str().substr(6);
        } else if (funcNameRef.startswith("__x64_sys_")) {
	    funcName = "sys_" + funcNameRef.str().substr(9);
	} else if (funcNameRef.startswith("__ia32_sys")) {
	    funcName = "sys_" + funcNameRef.str().substr(10);
	} else if (funcNameRef.startswith("__se_sys")) {
	    funcName = "sys_" + funcNameRef.str().substr(8);
	}

	if(funcName != "") {
            if (std::find(rootSyscall.begin(), rootSyscall.end(), funcName) ==
                rootSyscall.end()) {
                reachableSyscalls.insert(F);
            }
	}
    }

    reachableSyscallCache.insert(std::make_pair(F, reachableSyscalls));
    return  reachableSyscalls;
}

void CopyAnalyzerPass::dumpSimplifiedKeyStructs(){
    for(KeyStructMap::iterator it = Ctx->keyStructMap.begin(),
            e = Ctx->keyStructMap.end(); it != e; it++ ){

        StructInfo *st = it->second;
        if(st->copyInfo.size() == 0)
            continue;
        st->dumpSimplified();
    }
    return;
}

// dump final moduleStructMap and structModuleMap for debugging
void CopyAnalyzerPass::dumpKeyStructs() {

    RES_REPORT("\n=========  printing KeyStructMap ==========\n");

    for(KeyStructMap::iterator it = Ctx->keyStructMap.begin(),
            e = Ctx->keyStructMap.end(); it != e; it++ ){
            
        // RES_REPORT("[+] " << it->first << "\n");

        StructInfo *st = it->second;

        if(st->copyInfo.size() == 0)
            continue;

        if(!st->allocaInst.size() || !st->copyInst.size())
            continue;

        if(VerboseLevel > 0){
            st->dumpStructInfo(false);
        }else{
            st->dumpStructInfo(true);
        }
        
        // dump syscall info

        FuncSet SYSs;
        SYSs.clear();

        RES_REPORT("[+] syscalls:\n");
        for(auto *I : st->allocaInst){
            Function *F = I->getFunction();
            if(!F) continue;
            FuncSet syscalls = getSyscalls(F);
            for(auto *SF : syscalls){
                SYSs.insert(SF);
            }
        }
        for(auto *I : st->copyInst){
            Function *F = I->getFunction();
            if(!F) continue;
            FuncSet syscalls = getSyscalls(F);
            for(auto *SF : syscalls){
                SYSs.insert(SF);
            }
        }
        for(auto *SF : SYSs){
            RES_REPORT(SF->getName() << "\n");
        }
        RES_REPORT("\n");
    }


    RES_REPORT("======= end printting KeyStructMap =========\n");

    if(VerboseLevel >= 3){
        // dump alias
        for (auto const &alias : Ctx->FuncPAResults){
            KA_LOGS(2, "Function: " << getScopeName(alias.first) << "\n");
            for( auto const &aliasMap : alias.second){
                KA_LOGS(2, "Start dumping alias of Pointer : " << *aliasMap.first << "\n");
                for( auto *pointer : aliasMap.second){
                    KA_LOGS(2, *pointer << "\n");
                }
                KA_LOGS(2, "End dumping\n");
            }
            KA_LOGS(2, "\nEnding Function\n\n");
        }
    }

    unsigned cnt = 0;
    RES_REPORT("\n=========  printing structModuleMap ==========\n");
    for (StructModuleMap::iterator i = Ctx->structModuleMap.begin(), 
            e = Ctx->structModuleMap.end(); i != e; i++, cnt++) {

        std::string structName = i->first;
        ModuleSet& moduleSet = i->second;
        
        RES_REPORT("[" << cnt << "] " << structName << "\n");
        for (Module* M : moduleSet)
            RES_REPORT("-- " << M->getModuleIdentifier() << "\n");
    }
    RES_REPORT("====== end printing structModuleMap ==========\n");

    RES_REPORT("\n=========  printing copyInstMap ==========\n");
    cnt = 0;
    for (InstMap::iterator i = Ctx->copyInstMap.begin(), 
            e = Ctx->copyInstMap.end(); i != e; i++, cnt++) {

        std::string structName = i->first;
        InstSet& instSet = i->second;

        RES_REPORT("[" << cnt << "] " << structName << "\n");

         for (Instruction* I : instSet) {
            Function* F = I->getParent()->getParent();

            RES_REPORT("-- " << F->getName().str() << "(), " << 
                       F->getParent()->getModuleIdentifier() << "\n");
            RES_REPORT("   ");
            I->print(errs());
            RES_REPORT("\n");
         }
                    
    }
    RES_REPORT("====== end printing copyInstMap ==========\n");
    
    RES_REPORT("\n=========  printing allocInstMap ==========\n");
    cnt = 0;
    for (InstMap::iterator i = Ctx->allocInstMap.begin(), 
            e = Ctx->allocInstMap.end(); i != e; i++, cnt++) {

        std::string structName = i->first;
        InstSet& instSet = i->second;

        RES_REPORT("[" << cnt << "] " << structName << "\n");

         for (Instruction* I : instSet) {
            Function* F = I->getParent()->getParent();

            RES_REPORT("-- " << F->getName().str() << "(), " << 
                       F->getParent()->getModuleIdentifier() << "\n");
            RES_REPORT("   ");
            I->print(errs());
            RES_REPORT("\n");
         }
                    
    }
    RES_REPORT("====== end printing allocInstMap ==========\n");

    RES_REPORT("\n=========  printing keyStructList ==========\n");
    cnt = 0;
    for (KeyStructList::iterator i = Ctx->keyStructList.begin(), 
            e = Ctx->keyStructList.end(); i != e; i++, cnt++) {

        std::string structName = i->first;
        InstSet& instSet = i->second;

        RES_REPORT("[" << cnt << "] " << structName << "\n");

         for (Instruction* I : instSet) {
            Function* F = I->getParent()->getParent();

            RES_REPORT("-- " << F->getName().str() << "(), " << 
                       F->getParent()->getModuleIdentifier() << "\n");
            RES_REPORT("   ");
            I->print(errs());
            RES_REPORT("\n");
         }
                    
    }
    RES_REPORT("====== end printing keyStructList ==========\n");
    
    RES_REPORT("\n========= printing allocSyscallMap & copySyscallMap ==========\n");
    cnt = 0;
    for (KeyStructList::iterator i = Ctx->keyStructList.begin(), 
            e = Ctx->keyStructList.end(); i != e; i++, cnt++) {

        std::string structName = i->first;
        RES_REPORT("[" << cnt << "] " << structName << "\n");

        // XXX 
        // AllocSyscallMap::iterator asit = Ctx->allocSyscallMap.find(structName);
        SyscallMap::iterator lsit = Ctx->copySyscallMap.find(structName);

        assert(
        // XXX 
        //      asit != Ctx->allocSyscallMap.end() &&
               lsit != Ctx->copySyscallMap.end() &&
               "keyStructList is allocSyscallMap AND copySyscallMap");
        
        // XXX
        /*
        RES_REPORT("<<<<<<<<<<<<<< Allocation <<<<<<<<<<<\n");
        
        for (auto F : asit->second)
            RES_REPORT(F->getName() << "\n");
        */

        RES_REPORT("<<<<<<<<<<<<<< Copying <<<<<<<<<<<\n");

        for (auto F : lsit->second)
            RES_REPORT(F->getName() << "\n");

        RES_REPORT("\n");

    }
    RES_REPORT("======== end printing allocSyscallMap & copySyscallMap =======\n");
}
