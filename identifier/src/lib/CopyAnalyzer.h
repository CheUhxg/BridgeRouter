/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen
 *
 * For licensing details see LICENSE
 */

#ifndef CS_H_
#define CS_H_

#include "GlobalCtx.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include <set>

class CopyAnalyzerPass : public IterativeModulePass {

private:

    void runOnFunction(llvm::Function*);
    bool isCall2Copy(std::string calleeName);
    void backwardUseAnalysis(llvm::Value *V, std::set<llvm::Value *> &DefineSet);
    void forwardAnalysis(llvm::Value *V, std::set<llvm::StoreInst *> &StoreInstSet, std::set<llvm::Value *> &TrackSet);
    llvm::Value* getOffset(llvm::GetElementPtrInst *GEP);

    void analyzeBridge(llvm::CallInst* callInst, std::string calleeName);
    void analyzeRouter(llvm::CallInst* callInst, std::string calleeName);

    bool isPriviledged(llvm::Function *F);
    bool isAtStack(std::vector<Value *> setV);

    SmallPtrSet<Value *, 16> getAliasSet(llvm::Value *V, llvm::Function *F);
    // void findLenSources(llvm::Value* V, std::vector<llvm::Value *> &srcSet, std::set<llvm::Value* > &trackedSet);
    void findSources(llvm::Value* V, std::vector<llvm::Value *> &srcSet, std::set<llvm::Value* > &trackedSet);
    void addCopyInfo(StructInfo *stInfo, llvm::CallInst *callInst, unsigned offset, llvm::Instruction *I, llvm::StructType *st, unsigned argOffset);
    void setupBridgeInfo(std::vector<Value*> &lenSet, llvm::CallInst *callInst, llvm::Value *to, llvm::Value *from);
    void setupRouterInfo(std::vector<Value*> &srcSet, llvm::CallInst *callInst, llvm::Value *from);
    void setupSiteInfo(std::vector<llvm::Value*> &srcSet, StructInfo *stInfo, llvm::CallInst *callInst, unsigned offset, unsigned argOffset);
    FuncSet getSyscalls(Function *F);
    FuncSet reachableSyscall(llvm::Function*);

    // hard-coded copy API
    std::vector<std::string> copyAPIVec = {
    // "get_user", "copy_from_user", "_copy_from_user",
    // "nla_strscpy", "nla_memcpy",
    // "skb_copy_from_linear_data", "skb_copy_from_linear_data_offset",
    // "skb_get", "copyin"
    "copy_from_user", "_copy_from_user",
    // "put_user", "skb_put_data",
    };

    // hard-coded privileged syscalls
    std::vector<std::string> rootSyscall = {
    // CAP_SYS_BOOT
    "sys_reboot", "sys_kexec_load",
    // CAP_SYS_ADMIN
    "sys_swapon", "sys_swapoff", "sys_umount", "sys_oldumount", "sys_quotactl", 
    "sys_mount", "sys_pivot_root", "sys_lookup_dcookie", "sys_bdflush", "sys_seccomp",
    // CAP_SYS_MODULE
    "sys_finit_module", "sys_init_module", "sys_delete_module",
    // CAP_DAC_READ_SEARCH
    "sys_open_by_handle_at",
    // CAP_CHOWN
    "sys_fchown", "sys_fchown16", "sys_fchownat", "sys_lchown", "sys_lchown16",
    "sys_chown", "sys_chown16", "sys_fchmodat", "sys_chmod", "sys_fchmod",
    // CAP_SYS_PACCT
    "sys_acct",
    // CAP_SYS_TIME
    "sys_settimeofday", "sys_stime", "sys_adjtimex",
    // CAP_SYS_CHROOT
    "sys_chroot",
    // CAP_SYSLOG
    "sys_syslog"
    };

    typedef llvm::DenseMap<llvm::Function*, FuncSet> ReachableSyscallCache;
    ReachableSyscallCache reachableSyscallCache;

public:

    CopyAnalyzerPass(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "CopyAnalyzer") {}
    virtual bool doInitialization(llvm::Module* );
    virtual bool doFinalization(llvm::Module* );
    virtual bool doModulePass(llvm::Module* );

    // debug
    void dumpKeyStructs();
    void dumpSimplifiedKeyStructs();
};

#endif
