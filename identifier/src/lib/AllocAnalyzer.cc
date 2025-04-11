/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen, Zhenpeng Lin
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

#include "AllocAnalyzer.h"
#include "Annotation.h"
#include "Common.h"
#include "GlobalCtx.h"
#include "StructAnalyzer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

using namespace llvm;
using namespace std;

extern cl::opt<bool> IgnoreReachable;

// initialize moduleStructMap
bool AllocAnalyzerPass::doInitialization(Module* M) {

    StructTypeSet structTypeSet;
    TypeFinder usedStructTypes;
    usedStructTypes.run(*M, false);

    for (TypeFinder::iterator itr = usedStructTypes.begin(), 
            ite = usedStructTypes.end(); itr != ite; itr++) {

        StructType* st = *itr;
        // only deal with non-opaque type
        if (st->isOpaque()) 
            continue;

        structTypeSet.insert(st);
    }

    Ctx->moduleStructMap.insert(std::make_pair(M, structTypeSet));

    return false;
}

// determine "allocable" and "copyable" to compute allocInstMap and copyInstMap
bool AllocAnalyzerPass::doModulePass(Module* M) {

    ModuleStructMap::iterator it = Ctx->moduleStructMap.find(M);
    assert(it != Ctx->moduleStructMap.end() && 
            "M is not analyzed in doInitialization");

	for (Function &F : *M)
        runOnFunction(&F);

    return false;
}

// check if the function is called by a priviledged device
// return true if the function is priviledged.
bool AllocAnalyzerPass::isPriviledged(llvm::Function *F) {
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
void AllocAnalyzerPass::runOnFunction(Function *F) {
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
                if (isCall2Alloc(calleeName)) {
                    analyzeAlloc(callInst);
                }
            }
        }
    }

    return;
}

bool AllocAnalyzerPass::isCall2Alloc(std::string calleeName) {
    if (std::find(allocAPIVec.begin(), allocAPIVec.end(), 
            calleeName) != allocAPIVec.end())
        return true;
    return false;
}

llvm::Value* AllocAnalyzerPass::getOffset(llvm::GetElementPtrInst *GEP){
    // FIXME: consider using more sophisicated method
    // Use the last indice of GEP
    return GEP->getOperand(GEP->getNumIndices());
}


void AllocAnalyzerPass::forwardAnalysis(llvm::Value *V, 
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

        } else if (auto *BCI = dyn_cast<BitCastInst>(User)) {
            forwardAnalysis(BCI, StoreInstSet, TrackSet);
            KA_LOGS(0, "got BCI\n");
        } else {
            errs() << "\nForwardAnalysis Fatal errors , please handle " << *User << "\n";
            // exit(0);
        }
    }
}


// every time adding a new struct to allocInstMap, 
// update allocSyscallMap
void AllocAnalyzerPass::analyzeAlloc(llvm::CallInst* callInst) {

    StructType* stType = nullptr;
    Function *F;
    Module *M;
    unsigned fromOffset = -1;
    bool isOrigin = false;

    M = callInst->getModule();
    F = callInst->getCalledFunction();

    if (!F) {
        if (Function *FF = dyn_cast<Function>(callInst->getCalledOperand()->stripPointerCasts())) {
            F = FF;
        }
    }

    if (F) {
        Type *baseType = F->getReturnType();
        stType = dyn_cast<StructType>(baseType);
        if (!stType) {
            for (auto user : callInst->users()) {
                if (auto BCI = dyn_cast<BitCastInst>(user)) {
                    if (auto pointType = dyn_cast<PointerType>(BCI->getDestTy())) {
                        stType = dyn_cast<StructType>(pointType->getPointerElementType());
                        if (stType) break;
                    }
                }
            }
        }
    }

    if (stType)
        isOrigin = true;

    std::set<llvm::StoreInst *> storeInstSet;
    std::set<llvm::Value *> trackSet;
    forwardAnalysis(callInst, storeInstSet, trackSet);

    for (auto SI : storeInstSet) {
        Value *Op0 = SI->getOperand(0);
        Value *Op1 = SI->getOperand(1);
        if (auto BCI = dyn_cast<BitCastInst>(Op0)) {
            PointerType *ptrTy = dyn_cast<PointerType>(BCI->getDestTy());
            if (!ptrTy || isOrigin)
                continue;
            stType = dyn_cast<StructType>(ptrTy->getPointerElementType());
            if (stType)
                break;
        }
        else if (auto CSI = dyn_cast<ConstantInt>(Op0)) {
            continue;
        }
        else if (auto GEP = dyn_cast<GetElementPtrInst>(Op1)) {
            if (auto CSI = dyn_cast<ConstantInt>(getOffset(GEP)))
                fromOffset = CSI->getZExtValue();
            else
                continue;

            stType = dyn_cast<StructType>(GEP->getSourceElementType());
            if (stType)
                break;
            else
                fromOffset = -1;
        }
    }

    if (!stType)
        return;

    // compose allocInst map
    string structName = getScopeName(stType, M);

    
    KA_LOGS(1, "We found " << structName << "\n");
    if (structName.find("struct") == string::npos)
        return;

    if (isOrigin) {
        KA_LOGS(0, "[ALLOC] " << structName << ">");
        DEBUG_Inst(0, callInst); 
    }

    if (fromOffset != -1)
        KA_LOGS(0, "[ALLOC] " << structName + "." + to_string(fromOffset) << ">");
    else if (!isOrigin)
        KA_LOGS(0, "[ALLOC] " << structName << ">");
    DEBUG_Inst(0, callInst);

    Function *body = callInst->getFunction();
    if (isPriviledged(body)) {
        outs() << body->getName() << " is priviledged function for allocating\n";
        return;
    }

    KeyStructMap::iterator it = Ctx->keyStructMap.find(structName);
    if (it != Ctx->keyStructMap.end()) {

        it->second->allocaInst.insert(callInst);
        if (fromOffset != -1) it->second->fieldAllocGEP.insert(fromOffset);

    } else {
        StructInfo *stInfo = Ctx->structAnalyzer.getStructInfo(stType, M);
        if (!stInfo) return;
        stInfo->allocaInst.insert(callInst);
        if (fromOffset != -1) stInfo->fieldAllocGEP.insert(fromOffset);
        Ctx->keyStructMap.insert(std::make_pair(structName, stInfo));
    }
}

// join allocInstMap and copyInstMap to compute moduleStructMap
// reverse moduleStructMap to obtain structModuleMap
// reachable analysis to compute allocSyscallMap and copySyscallMap
// join allocSyscallMap and copySyscallMap to compute keyStructList
bool AllocAnalyzerPass::doFinalization(Module* M) {

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
        // AllocSyscallMap::iterator asit = Ctx->allocSyscallMap.find(structName);
        
        if (lsit == Ctx->copySyscallMap.end())
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

FuncSet AllocAnalyzerPass::getSyscalls(Function *F){
    ReachableSyscallCache::iterator it = reachableSyscallCache.find(F);
    if (it != reachableSyscallCache.end())
        return it->second;
    FuncSet null;
    return null;
}

FuncSet AllocAnalyzerPass::reachableSyscall(llvm::Function* F) {

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
