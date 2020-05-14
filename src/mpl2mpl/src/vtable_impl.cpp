/*
 * Copyright (c) [2019-2020] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "vtable_impl.h"
#include "vtable_analysis.h"
#include "itab_util.h"
#include "reflection_analysis.h"

// This phase is mainly to lower interfacecall into icall
namespace {
#ifdef USE_32BIT_REF
constexpr char kInterfaceMethod[] = "MCC_getFuncPtrFromItab";
#else
constexpr char kInterfaceMethod[] = "MCC_getFuncPtrFromItabSecondHash64";
#endif
} // namespace

namespace maple {
VtableImpl::VtableImpl(MIRModule &mod, KlassHierarchy *kh, bool dump)
    : FuncOptimizeImpl(mod, kh, dump), mirModule(&mod) {
  mccItabFunc = builder->GetOrCreateFunction(kInterfaceMethod, TyIdx(PTY_ptr));
  mccItabFunc->SetAttr(FUNCATTR_nosideeffect);
}
#if defined(TARGARM) || defined(TARGAARCH64)
bool VtableImpl::Intrinsify(MIRFunction &func, CallNode &cnode) {
  MIRFunction *calleeFunc = GlobalTables::GetFunctionTable().GetFunctionFromPuidx(cnode.GetPUIdx());
  const std::string funcName = calleeFunc->GetName();
  MIRIntrinsicID intrnId = INTRN_UNDEFINED;
  if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CgetAndAddInt_7C_28Ljava_2Flang_2FObject_3BJI_29I") {
    intrnId = INTRN_GET_AND_ADDI;
  } else if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CgetAndAddLong_7C_28Ljava_2Flang_2FObject_3BJJ_29J") {
    intrnId = INTRN_GET_AND_ADDL;
  } else if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CgetAndSetInt_7C_28Ljava_2Flang_2FObject_3BJI_29I") {
    intrnId = INTRN_GET_AND_SETI;
  } else if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CgetAndSetLong_7C_28Ljava_2Flang_2FObject_3BJJ_29J") {
    intrnId = INTRN_GET_AND_SETL;
  } else if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CcompareAndSwapInt_7C_28Ljava_2Flang_2FObject_3BJII_29Z") {
    intrnId = INTRN_COMP_AND_SWAPI;
  } else if (funcName == "Lsun_2Fmisc_2FUnsafe_3B_7CcompareAndSwapLong_7C_28Ljava_2Flang_2FObject_3BJJJ_29Z") {
    intrnId = INTRN_COMP_AND_SWAPL;
  }
  if (intrnId == INTRN_UNDEFINED) {
    return false;
  }
  CallReturnVector retvs = cnode.GetReturnVec();
  if (!retvs.empty()) {
    StIdx stidx = retvs.begin()->first;
    StmtNode *intrnCallStmt = nullptr;
    if (stidx.Idx() != 0) {
      MIRSymbol *retSt = currFunc->GetLocalOrGlobalSymbol(stidx);
      intrnCallStmt = builder->CreateStmtIntrinsicCallAssigned(intrnId, cnode.GetNopnd(), retSt);
    } else {
      ASSERT (retvs.begin()->second.IsReg(), "return value must be preg");
      PregIdx pregIdx = retvs.begin()->second.GetPregIdx();
      intrnCallStmt = builder->CreateStmtIntrinsicCallAssigned(intrnId, cnode.GetNopnd(), pregIdx);
    }
    func.GetBody()->ReplaceStmt1WithStmt2(&cnode, intrnCallStmt);
    return true;
  }
  return false;
}
#endif
void VtableImpl::ProcessFunc(MIRFunction *func) {
  if (func->IsEmpty()) {
    return;
  }
  SetCurrentFunction(*func);
  StmtNode *stmt = func->GetBody()->GetFirst();
  StmtNode *next = nullptr;
  while (stmt != nullptr) {
    next = stmt->GetNext();
    Opcode opcode = stmt->GetOpCode();
#if defined(TARGARM) || defined(TARGAARCH64)
    if (kOpcodeInfo.IsCallAssigned(opcode)) {
      CallNode *cnode = static_cast<CallNode*>(stmt);
      MIRFunction *calleefunc = GlobalTables::GetFunctionTable().GetFunctionFromPuidx(cnode->GetPUIdx());
      const std::set<std::string> intrisicsList {
#define DEF_MIR_INTRINSIC(X, NAME, INTRN_CLASS, RETURN_TYPE, ...) NAME,
#include "simplifyintrinsics.def"
#undef DEF_MIR_INTRINSIC
      };
      const std::string funcName = calleefunc->GetName();
      if (!Options::buildApp && Options::O2 && intrisicsList.find(funcName) != intrisicsList.end() &&
          funcName != "Ljava_2Flang_2FString_3B_7CindexOf_7C_28Ljava_2Flang_2FString_3B_29I") {
        if (Intrinsify(*func, *cnode)) {
          stmt = next;
          continue;
        }
      }
    }
#endif
    switch (opcode) {
      case OP_regassign: {
        auto *regassign = static_cast<RegassignNode*>(stmt);
        BaseNode *rhs = regassign->Opnd(0);
        ASSERT_NOT_NULL(rhs);
        if (rhs->GetOpCode() == OP_resolveinterfacefunc) {
          ReplaceResolveInterface(*stmt, *(static_cast<ResolveFuncNode*>(rhs)));
        }
        break;
      }
      case OP_interfaceicallassigned:
      case OP_virtualicallassigned: {
        auto *callNode = static_cast<CallNode*>(stmt);
        MIRFunction *callee = GlobalTables::GetFunctionTable().GetFunctionFromPuidx(callNode->GetPUIdx());
        MemPool *currentFuncCodeMempool = builder->GetCurrentFuncCodeMp();
        IcallNode *icallNode =
            currentFuncCodeMempool->New<IcallNode>(*builder->GetCurrentFuncCodeMpAllocator(), OP_icallassigned);
        icallNode->SetReturnVec(callNode->GetReturnVec());
        icallNode->SetRetTyIdx(callee->GetReturnTyIdx());
        icallNode->SetSrcPos(callNode->GetSrcPos());
        icallNode->GetNopnd().resize(callNode->GetNopndSize());
        icallNode->SetNumOpnds(icallNode->GetNopndSize());
        for (size_t i = 0; i < callNode->GetNopndSize(); ++i) {
          icallNode->SetOpnd(callNode->GetNopndAt(i)->CloneTree(mirModule->GetCurFuncCodeMPAllocator()), i);
        }
        currFunc->GetBody()->ReplaceStmt1WithStmt2(stmt, icallNode);
        stmt = icallNode;
        // Fall-through
      }
      [[clang::fallthrough]];
      case OP_icallassigned: {
        auto *icall = static_cast<IcallNode*>(stmt);
        BaseNode *firstParm = icall->GetNopndAt(0);
        ASSERT_NOT_NULL(firstParm);
        if (firstParm->GetOpCode() == maple::OP_resolveinterfacefunc) {
          ReplaceResolveInterface(*stmt, *(static_cast<ResolveFuncNode*>(firstParm)));
        }
        break;
      }
      case OP_virtualcall:
      case OP_virtualcallassigned: {
        CHECK_FATAL(false, "VtableImpl::ProcessFunc does not expect to see virtucalcall");
        break;
      }
      case OP_interfacecall:
      case OP_interfacecallassigned: {
        CHECK_FATAL(false, "VtableImpl::ProcessFunc does not expect to see interfacecall");
        break;
      }
      case OP_superclasscallassigned: {
        CHECK_FATAL(false, "VtableImpl::ProcessFunc does not expect to see supercall");
        break;
      }
      default:
        break;
    }
    stmt = next;
  }
  if (trace) {
    func->Dump(false);
  }
}


void VtableImpl::ReplaceResolveInterface(StmtNode &stmt, const ResolveFuncNode &resolveNode) {
  MIRFunction *func = GlobalTables::GetFunctionTable().GetFunctionFromPuidx(resolveNode.GetPuIdx());
  std::string signature = VtableAnalysis::DecodeBaseNameWithType(*func);
  int64 hashCode = GetHashIndex(signature.c_str());
  PregIdx pregItabAddress = currFunc->GetPregTab()->CreatePreg(PTY_ptr);
  RegassignNode *itabAddressAssign =
      builder->CreateStmtRegassign(PTY_ptr, pregItabAddress, resolveNode.GetTabBaseAddr());
  currFunc->GetBody()->InsertBefore(&stmt, itabAddressAssign);
  // read funcvalue
  MIRType *compactPtrType = GlobalTables::GetTypeTable().GetCompactPtr();
  PrimType compactPtrPrim = compactPtrType->GetPrimType();
  BaseNode *offsetNode = builder->CreateIntConst(hashCode * kTabEntrySize, PTY_u32);
  BaseNode *addrNode = builder->CreateExprBinary(OP_add, *GlobalTables::GetTypeTable().GetPtr(),
                                                 builder->CreateExprRegread(PTY_ptr, pregItabAddress), offsetNode);
  BaseNode *readFuncPtr = builder->CreateExprIread(
      *compactPtrType, *GlobalTables::GetTypeTable().GetOrCreatePointerType(*compactPtrType), 0, addrNode);
  PregIdx pregFuncPtr = currFunc->GetPregTab()->CreatePreg(compactPtrPrim);
  RegassignNode *funcPtrAssign = builder->CreateStmtRegassign(compactPtrPrim, pregFuncPtr, readFuncPtr);
  currFunc->GetBody()->InsertBefore(&stmt, funcPtrAssign);
  // In case not found in the fast path, fall to the slow path
  uint64 secondHashCode = GetSecondHashIndex(signature.c_str());
  MapleAllocator *currentFuncMpAllocator = builder->GetCurrentFuncCodeMpAllocator();
  CHECK_FATAL(currentFuncMpAllocator != nullptr, "null ptr check");
  MapleVector<BaseNode*> opnds(currentFuncMpAllocator->Adapter());
  opnds.push_back(builder->CreateExprRegread(PTY_ptr, pregItabAddress));
  opnds.push_back(builder->CreateIntConst(secondHashCode, PTY_u64));
  UStrIdx strIdx = GlobalTables::GetUStrTable().GetOrCreateStrIdxFromName(signature);
  MemPool *currentFunMp = builder->GetCurrentFuncCodeMp();
  CHECK_FATAL(currentFunMp != nullptr, "null ptr check");
  ConststrNode *signatureNode = currentFunMp->New<ConststrNode>(strIdx);
  signatureNode->SetPrimType(PTY_ptr);
  opnds.push_back(signatureNode);
  StmtNode *mccCallStmt =
      builder->CreateStmtCallRegassigned(mccItabFunc->GetPuidx(), opnds, pregFuncPtr, OP_callassigned);
  BaseNode *checkExpr = builder->CreateExprCompare(OP_eq, *GlobalTables::GetTypeTable().GetUInt1(), *compactPtrType,
                                                   builder->CreateExprRegread(compactPtrPrim, pregFuncPtr),
                                                   builder->CreateIntConst(0, compactPtrPrim));
  auto *ifStmt = static_cast<IfStmtNode*>(builder->CreateStmtIf(checkExpr));
  ifStmt->GetThenPart()->AddStatement(mccCallStmt);
  currFunc->GetBody()->InsertBefore(&stmt, ifStmt);
  if (stmt.GetOpCode() == OP_regassign) {
    auto *regAssign = static_cast<RegassignNode*>(&stmt);
    regAssign->SetOpnd(builder->CreateExprRegread(compactPtrPrim, pregFuncPtr), 0);
  } else {
    auto *icall = static_cast<IcallNode*>(&stmt);
    const size_t nopndSize = icall->GetNopndSize();
    CHECK_FATAL(nopndSize > 0, "container check");
    icall->SetNOpndAt(0, builder->CreateExprRegread(compactPtrPrim, pregFuncPtr));
  }
}
}  // namespace maple
