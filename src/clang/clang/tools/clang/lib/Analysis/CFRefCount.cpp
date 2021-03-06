// CFRefCount.cpp - Transfer functions for tracking simple values -*- C++ -*--//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the methods for CFRefCount, which implements
//  a reference count checker for Core Foundation (Mac OS X).
//
//===----------------------------------------------------------------------===//

#include "GRSimpleVals.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Analysis/PathSensitive/GRExprEngineBuilders.h"
#include "clang/Analysis/PathSensitive/GRStateTrait.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/Analysis/LocalCheckers.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/Analysis/PathSensitive/BugReporter.h"
#include "clang/Analysis/PathSensitive/SymbolManager.h"
#include "clang/AST/DeclObjC.h"   
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/ImmutableList.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/ADT/STLExtras.h"
#include <ostream>
#include <stdarg.h>

using namespace clang;

//===----------------------------------------------------------------------===//
// Utility functions.
//===----------------------------------------------------------------------===//

// The "fundamental rule" for naming conventions of methods:
//  (url broken into two lines)
//  http://developer.apple.com/documentation/Cocoa/Conceptual/
//     MemoryMgmt/Tasks/MemoryManagementRules.html
//
// "You take ownership of an object if you create it using a method whose name
//  begins with “alloc” or “new” or contains “copy” (for example, alloc, 
//  newObject, or mutableCopy), or if you send it a retain message. You are
//  responsible for relinquishing ownership of objects you own using release
//  or autorelease. Any other time you receive an object, you must
//  not release it."
//

using llvm::CStrInCStrNoCase;
using llvm::StringsEqualNoCase;

enum NamingConvention { NoConvention, CreateRule, InitRule };

static inline bool isWordEnd(char ch, char prev, char next) {
  return ch == '\0'
      || (islower(prev) && isupper(ch)) // xxxC
      || (isupper(prev) && isupper(ch) && islower(next)) // XXCreate
      || !isalpha(ch);
}
  
static inline const char* parseWord(const char* s) {  
  char ch = *s, prev = '\0';
  assert(ch != '\0');
  char next = *(s+1);
  while (!isWordEnd(ch, prev, next)) {
    prev = ch;
    ch = next;
    next = *((++s)+1);
  }
  return s;
}

static NamingConvention deriveNamingConvention(Selector S) {
  IdentifierInfo *II = S.getIdentifierInfoForSlot(0);
  
  if (!II)
    return NoConvention;
  
  const char *s = II->getName();
  
  // A method/function name may contain a prefix.  We don't know it is there,
  // however, until we encounter the first '_'.
  bool InPossiblePrefix = true;
  bool AtBeginning = true;
  NamingConvention C = NoConvention;
  
  while (*s != '\0') {
    // Skip '_'.
    if (*s == '_') {
      if (InPossiblePrefix) {
        InPossiblePrefix = false;
        AtBeginning = true;
        // Discard whatever 'convention' we
        // had already derived since it occurs
        // in the prefix.
        C = NoConvention;
      }
      ++s;
      continue;
    }
    
    // Skip numbers, ':', etc.
    if (!isalpha(*s)) {
      ++s;
      continue;
    }
    
    const char *wordEnd = parseWord(s);
    assert(wordEnd > s);
    unsigned len = wordEnd - s;
    
    switch (len) {
    default:
      break;
    case 3:
      // Methods starting with 'new' follow the create rule.
      if (AtBeginning && StringsEqualNoCase("new", s, len))
        C = CreateRule;      
      break;
    case 4:
      // Methods starting with 'alloc' or contain 'copy' follow the
      // create rule
      if (C == NoConvention && StringsEqualNoCase("copy", s, len))
        C = CreateRule;
      else // Methods starting with 'init' follow the init rule.
        if (AtBeginning && StringsEqualNoCase("init", s, len))
          C = InitRule;
      break;
    case 5:
      if (AtBeginning && StringsEqualNoCase("alloc", s, len))
        C = CreateRule;
      break;
    }
    
    // If we aren't in the prefix and have a derived convention then just
    // return it now.
    if (!InPossiblePrefix && C != NoConvention)
      return C;

    AtBeginning = false;
    s = wordEnd;
  }

  // We will get here if there wasn't more than one word
  // after the prefix.
  return C;
}

static bool followsFundamentalRule(Selector S) {
  return deriveNamingConvention(S) == CreateRule;
}

static const ObjCMethodDecl*
ResolveToInterfaceMethodDecl(const ObjCMethodDecl *MD, ASTContext &Context) {  
  ObjCInterfaceDecl *ID =
    const_cast<ObjCInterfaceDecl*>(MD->getClassInterface());
  
  return MD->isInstanceMethod()
         ? ID->lookupInstanceMethod(Context, MD->getSelector())
         : ID->lookupClassMethod(Context, MD->getSelector());
}

namespace {
class VISIBILITY_HIDDEN GenericNodeBuilder {
  GRStmtNodeBuilder<GRState> *SNB;
  Stmt *S;
  const void *tag;
  GREndPathNodeBuilder<GRState> *ENB;
public:
  GenericNodeBuilder(GRStmtNodeBuilder<GRState> &snb, Stmt *s,
                     const void *t)
  : SNB(&snb), S(s), tag(t), ENB(0) {}
  GenericNodeBuilder(GREndPathNodeBuilder<GRState> &enb)
  : SNB(0), S(0), tag(0), ENB(&enb) {}
  
  ExplodedNode<GRState> *MakeNode(const GRState *state,
                                  ExplodedNode<GRState> *Pred) {
    if (SNB)
      return SNB->generateNode(PostStmt(S, tag), state, Pred);
    
    assert(ENB);
    return ENB->generateNode(state, Pred);
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Selector creation functions.
//===----------------------------------------------------------------------===//

static inline Selector GetNullarySelector(const char* name, ASTContext& Ctx) {
  IdentifierInfo* II = &Ctx.Idents.get(name);
  return Ctx.Selectors.getSelector(0, &II);
}

static inline Selector GetUnarySelector(const char* name, ASTContext& Ctx) {
  IdentifierInfo* II = &Ctx.Idents.get(name);
  return Ctx.Selectors.getSelector(1, &II);
}

//===----------------------------------------------------------------------===//
// Type querying functions.
//===----------------------------------------------------------------------===//

static bool hasPrefix(const char* s, const char* prefix) {
  if (!prefix)
    return true;
  
  char c = *s;
  char cP = *prefix;
  
  while (c != '\0' && cP != '\0') {
    if (c != cP) break;
    c = *(++s);
    cP = *(++prefix);
  }
  
  return cP == '\0';
}

static bool hasSuffix(const char* s, const char* suffix) {
  const char* loc = strstr(s, suffix);
  return loc && strcmp(suffix, loc) == 0;
}

static bool isRefType(QualType RetTy, const char* prefix,
                      ASTContext* Ctx = 0, const char* name = 0) {
  
  // Recursively walk the typedef stack, allowing typedefs of reference types.
  while (1) {
    if (TypedefType* TD = dyn_cast<TypedefType>(RetTy.getTypePtr())) {
      const char* TDName = TD->getDecl()->getIdentifier()->getName();
      if (hasPrefix(TDName, prefix) && hasSuffix(TDName, "Ref"))
        return true;
      
      RetTy = TD->getDecl()->getUnderlyingType();
      continue;
    }
    break;
  }

  if (!Ctx || !name)
    return false;

  // Is the type void*?
  const PointerType* PT = RetTy->getAsPointerType();
  if (!(PT->getPointeeType().getUnqualifiedType() == Ctx->VoidTy))
    return false;

  // Does the name start with the prefix?
  return hasPrefix(name, prefix);
}

//===----------------------------------------------------------------------===//
// Primitives used for constructing summaries for function/method calls.
//===----------------------------------------------------------------------===//

/// ArgEffect is used to summarize a function/method call's effect on a
/// particular argument.
enum ArgEffect { Autorelease, Dealloc, DecRef, DecRefMsg, DoNothing,
                 DoNothingByRef, IncRefMsg, IncRef, MakeCollectable, MayEscape,
                 NewAutoreleasePool, SelfOwn, StopTracking };

namespace llvm {
template <> struct FoldingSetTrait<ArgEffect> {
static inline void Profile(const ArgEffect X, FoldingSetNodeID& ID) {
  ID.AddInteger((unsigned) X);
}
};
} // end llvm namespace

/// ArgEffects summarizes the effects of a function/method call on all of
/// its arguments.
typedef llvm::ImmutableMap<unsigned,ArgEffect> ArgEffects;

namespace {

///  RetEffect is used to summarize a function/method call's behavior with
///  respect to its return value.  
class VISIBILITY_HIDDEN RetEffect {
public:
  enum Kind { NoRet, Alias, OwnedSymbol, OwnedAllocatedSymbol,
              NotOwnedSymbol, GCNotOwnedSymbol, ReceiverAlias,
              OwnedWhenTrackedReceiver };
    
  enum ObjKind { CF, ObjC, AnyObj };  

private:
  Kind K;
  ObjKind O;
  unsigned index;

  RetEffect(Kind k, unsigned idx = 0) : K(k), O(AnyObj), index(idx) {}
  RetEffect(Kind k, ObjKind o) : K(k), O(o), index(0) {}
  
public:
  Kind getKind() const { return K; }

  ObjKind getObjKind() const { return O; }
  
  unsigned getIndex() const { 
    assert(getKind() == Alias);
    return index;
  }
  
  bool isOwned() const {
    return K == OwnedSymbol || K == OwnedAllocatedSymbol ||
           K == OwnedWhenTrackedReceiver;
  }
    
  static RetEffect MakeOwnedWhenTrackedReceiver() {
    return RetEffect(OwnedWhenTrackedReceiver, ObjC);
  }
  
  static RetEffect MakeAlias(unsigned Idx) {
    return RetEffect(Alias, Idx);
  }
  static RetEffect MakeReceiverAlias() {
    return RetEffect(ReceiverAlias);
  }  
  static RetEffect MakeOwned(ObjKind o, bool isAllocated = false) {
    return RetEffect(isAllocated ? OwnedAllocatedSymbol : OwnedSymbol, o);
  }  
  static RetEffect MakeNotOwned(ObjKind o) {
    return RetEffect(NotOwnedSymbol, o);
  }
  static RetEffect MakeGCNotOwned() {
    return RetEffect(GCNotOwnedSymbol, ObjC);
  }
    
  static RetEffect MakeNoRet() {
    return RetEffect(NoRet);
  }
  
  void Profile(llvm::FoldingSetNodeID& ID) const {
    ID.AddInteger((unsigned)K);
    ID.AddInteger((unsigned)O);
    ID.AddInteger(index);
  }
};
  
  
class VISIBILITY_HIDDEN RetainSummary {
  /// Args - an ordered vector of (index, ArgEffect) pairs, where index
  ///  specifies the argument (starting from 0).  This can be sparsely
  ///  populated; arguments with no entry in Args use 'DefaultArgEffect'.
  ArgEffects Args;
  
  /// DefaultArgEffect - The default ArgEffect to apply to arguments that
  ///  do not have an entry in Args.
  ArgEffect   DefaultArgEffect;
  
  /// Receiver - If this summary applies to an Objective-C message expression,
  ///  this is the effect applied to the state of the receiver.
  ArgEffect   Receiver;
  
  /// Ret - The effect on the return value.  Used to indicate if the
  ///  function/method call returns a new tracked symbol, returns an
  ///  alias of one of the arguments in the call, and so on.
  RetEffect   Ret;
  
  /// EndPath - Indicates that execution of this method/function should
  ///  terminate the simulation of a path.
  bool EndPath;
  
public:
  RetainSummary(ArgEffects A, RetEffect R, ArgEffect defaultEff,
                ArgEffect ReceiverEff, bool endpath = false)
    : Args(A), DefaultArgEffect(defaultEff), Receiver(ReceiverEff), Ret(R),
      EndPath(endpath) {}  
  
  /// getArg - Return the argument effect on the argument specified by
  ///  idx (starting from 0).
  ArgEffect getArg(unsigned idx) const {
    if (const ArgEffect *AE = Args.lookup(idx))
      return *AE;
    
    return DefaultArgEffect;
  }
  
  /// setDefaultArgEffect - Set the default argument effect.
  void setDefaultArgEffect(ArgEffect E) {
    DefaultArgEffect = E;
  }
  
  /// setArg - Set the argument effect on the argument specified by idx.
  void setArgEffect(ArgEffects::Factory& AF, unsigned idx, ArgEffect E) {
    Args = AF.Add(Args, idx, E);
  }
  
  /// getRetEffect - Returns the effect on the return value of the call.
  RetEffect getRetEffect() const { return Ret; }
  
  /// setRetEffect - Set the effect of the return value of the call.
  void setRetEffect(RetEffect E) { Ret = E; }
  
  /// isEndPath - Returns true if executing the given method/function should
  ///  terminate the path.
  bool isEndPath() const { return EndPath; }
  
  /// getReceiverEffect - Returns the effect on the receiver of the call.
  ///  This is only meaningful if the summary applies to an ObjCMessageExpr*.
  ArgEffect getReceiverEffect() const { return Receiver; }
  
  /// setReceiverEffect - Set the effect on the receiver of the call.
  void setReceiverEffect(ArgEffect E) { Receiver = E; }
  
  typedef ArgEffects::iterator ExprIterator;
  
  ExprIterator begin_args() const { return Args.begin(); }
  ExprIterator end_args()   const { return Args.end(); }
  
  static void Profile(llvm::FoldingSetNodeID& ID, ArgEffects A,
                      RetEffect RetEff, ArgEffect DefaultEff,
                      ArgEffect ReceiverEff, bool EndPath) {
    ID.Add(A);
    ID.Add(RetEff);
    ID.AddInteger((unsigned) DefaultEff);
    ID.AddInteger((unsigned) ReceiverEff);
    ID.AddInteger((unsigned) EndPath);
  }
      
  void Profile(llvm::FoldingSetNodeID& ID) const {
    Profile(ID, Args, Ret, DefaultArgEffect, Receiver, EndPath);
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Data structures for constructing summaries.
//===----------------------------------------------------------------------===//

namespace {
class VISIBILITY_HIDDEN ObjCSummaryKey {
  IdentifierInfo* II;
  Selector S;
public:    
  ObjCSummaryKey(IdentifierInfo* ii, Selector s)
    : II(ii), S(s) {}

  ObjCSummaryKey(const ObjCInterfaceDecl* d, Selector s)
    : II(d ? d->getIdentifier() : 0), S(s) {}

  ObjCSummaryKey(const ObjCInterfaceDecl* d, IdentifierInfo *ii, Selector s)
    : II(d ? d->getIdentifier() : ii), S(s) {}
  
  ObjCSummaryKey(Selector s)
    : II(0), S(s) {}
  
  IdentifierInfo* getIdentifier() const { return II; }
  Selector getSelector() const { return S; }
};
}

namespace llvm {
template <> struct DenseMapInfo<ObjCSummaryKey> {
  static inline ObjCSummaryKey getEmptyKey() {
    return ObjCSummaryKey(DenseMapInfo<IdentifierInfo*>::getEmptyKey(),
                          DenseMapInfo<Selector>::getEmptyKey());
  }
    
  static inline ObjCSummaryKey getTombstoneKey() {
    return ObjCSummaryKey(DenseMapInfo<IdentifierInfo*>::getTombstoneKey(),
                          DenseMapInfo<Selector>::getTombstoneKey());      
  }
  
  static unsigned getHashValue(const ObjCSummaryKey &V) {
    return (DenseMapInfo<IdentifierInfo*>::getHashValue(V.getIdentifier())
            & 0x88888888) 
        | (DenseMapInfo<Selector>::getHashValue(V.getSelector())
            & 0x55555555);
  }
  
  static bool isEqual(const ObjCSummaryKey& LHS, const ObjCSummaryKey& RHS) {
    return DenseMapInfo<IdentifierInfo*>::isEqual(LHS.getIdentifier(),
                                                  RHS.getIdentifier()) &&
           DenseMapInfo<Selector>::isEqual(LHS.getSelector(),
                                           RHS.getSelector());
  }
  
  static bool isPod() {
    return DenseMapInfo<ObjCInterfaceDecl*>::isPod() &&
           DenseMapInfo<Selector>::isPod();
  }
};
} // end llvm namespace
  
namespace {
class VISIBILITY_HIDDEN ObjCSummaryCache {
  typedef llvm::DenseMap<ObjCSummaryKey, RetainSummary*> MapTy;
  MapTy M;
public:
  ObjCSummaryCache() {}
  
  typedef MapTy::iterator iterator;
  
  iterator find(const ObjCInterfaceDecl* D, IdentifierInfo *ClsName,
                Selector S) {
    // Lookup the method using the decl for the class @interface.  If we
    // have no decl, lookup using the class name.
    return D ? find(D, S) : find(ClsName, S);
  }
  
  iterator find(const ObjCInterfaceDecl* D, Selector S) {    
    // Do a lookup with the (D,S) pair.  If we find a match return
    // the iterator.
    ObjCSummaryKey K(D, S);
    MapTy::iterator I = M.find(K);
    
    if (I != M.end() || !D)
      return I;
    
    // Walk the super chain.  If we find a hit with a parent, we'll end
    // up returning that summary.  We actually allow that key (null,S), as
    // we cache summaries for the null ObjCInterfaceDecl* to allow us to
    // generate initial summaries without having to worry about NSObject
    // being declared.
    // FIXME: We may change this at some point.
    for (ObjCInterfaceDecl* C=D->getSuperClass() ;; C=C->getSuperClass()) {
      if ((I = M.find(ObjCSummaryKey(C, S))) != M.end())
        break;
      
      if (!C)
        return I;
    }
    
    // Cache the summary with original key to make the next lookup faster 
    // and return the iterator.
    M[K] = I->second;
    return I;
  }
  

  iterator find(Expr* Receiver, Selector S) {
    return find(getReceiverDecl(Receiver), S);
  }
  
  iterator find(IdentifierInfo* II, Selector S) {
    // FIXME: Class method lookup.  Right now we dont' have a good way
    // of going between IdentifierInfo* and the class hierarchy.
    iterator I = M.find(ObjCSummaryKey(II, S));
    return I == M.end() ? M.find(ObjCSummaryKey(S)) : I;
  }
  
  ObjCInterfaceDecl* getReceiverDecl(Expr* E) {
    
    const PointerType* PT = E->getType()->getAsPointerType();
    if (!PT) return 0;
    
    ObjCInterfaceType* OI = dyn_cast<ObjCInterfaceType>(PT->getPointeeType());
    if (!OI) return 0;
    
    return OI ? OI->getDecl() : 0;
  }
  
  iterator end() { return M.end(); }
  
  RetainSummary*& operator[](ObjCMessageExpr* ME) {
    
    Selector S = ME->getSelector();
    
    if (Expr* Receiver = ME->getReceiver()) {
      ObjCInterfaceDecl* OD = getReceiverDecl(Receiver);
      return OD ? M[ObjCSummaryKey(OD->getIdentifier(), S)] : M[S];
    }
    
    return M[ObjCSummaryKey(ME->getClassName(), S)];
  }
  
  RetainSummary*& operator[](ObjCSummaryKey K) {
    return M[K];
  }
  
  RetainSummary*& operator[](Selector S) {
    return M[ ObjCSummaryKey(S) ];
  }
};   
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Data structures for managing collections of summaries.
//===----------------------------------------------------------------------===//

namespace {
class VISIBILITY_HIDDEN RetainSummaryManager {

  //==-----------------------------------------------------------------==//
  //  Typedefs.
  //==-----------------------------------------------------------------==//
  
  typedef llvm::DenseMap<FunctionDecl*, RetainSummary*>
          FuncSummariesTy;
  
  typedef ObjCSummaryCache ObjCMethodSummariesTy;
    
  //==-----------------------------------------------------------------==//
  //  Data.
  //==-----------------------------------------------------------------==//
  
  /// Ctx - The ASTContext object for the analyzed ASTs.
  ASTContext& Ctx;

  /// CFDictionaryCreateII - An IdentifierInfo* representing the indentifier
  ///  "CFDictionaryCreate".
  IdentifierInfo* CFDictionaryCreateII;
  
  /// GCEnabled - Records whether or not the analyzed code runs in GC mode.
  const bool GCEnabled;
    
  /// FuncSummaries - A map from FunctionDecls to summaries.
  FuncSummariesTy FuncSummaries; 
  
  /// ObjCClassMethodSummaries - A map from selectors (for instance methods)
  ///  to summaries.
  ObjCMethodSummariesTy ObjCClassMethodSummaries;

  /// ObjCMethodSummaries - A map from selectors to summaries.
  ObjCMethodSummariesTy ObjCMethodSummaries;

  /// BPAlloc - A BumpPtrAllocator used for allocating summaries, ArgEffects,
  ///  and all other data used by the checker.
  llvm::BumpPtrAllocator BPAlloc;
  
  /// AF - A factory for ArgEffects objects.
  ArgEffects::Factory AF;  
  
  /// ScratchArgs - A holding buffer for construct ArgEffects.
  ArgEffects ScratchArgs;
  
  /// ObjCAllocRetE - Default return effect for methods returning Objective-C
  ///  objects.
  RetEffect ObjCAllocRetE;

  /// ObjCInitRetE - Default return effect for init methods returning Objective-C
  ///  objects.
  RetEffect ObjCInitRetE;
  
  RetainSummary DefaultSummary;
  RetainSummary* StopSummary;
  
  //==-----------------------------------------------------------------==//
  //  Methods.
  //==-----------------------------------------------------------------==//
  
  /// getArgEffects - Returns a persistent ArgEffects object based on the
  ///  data in ScratchArgs.
  ArgEffects getArgEffects();

  enum UnaryFuncKind { cfretain, cfrelease, cfmakecollectable };  
  
public:
  RetEffect getObjAllocRetEffect() const { return ObjCAllocRetE; }

  RetainSummary *getDefaultSummary() {
    RetainSummary *Summ = (RetainSummary*) BPAlloc.Allocate<RetainSummary>();
    return new (Summ) RetainSummary(DefaultSummary);
  }
  
  RetainSummary* getUnarySummary(const FunctionType* FT, UnaryFuncKind func);
  
  RetainSummary* getCFSummaryCreateRule(FunctionDecl* FD);
  RetainSummary* getCFSummaryGetRule(FunctionDecl* FD);  
  RetainSummary* getCFCreateGetRuleSummary(FunctionDecl* FD, const char* FName);
  
  RetainSummary* getPersistentSummary(ArgEffects AE, RetEffect RetEff,
                                      ArgEffect ReceiverEff = DoNothing,
                                      ArgEffect DefaultEff = MayEscape,
                                      bool isEndPath = false);

  RetainSummary* getPersistentSummary(RetEffect RE,
                                      ArgEffect ReceiverEff = DoNothing,
                                      ArgEffect DefaultEff = MayEscape) {
    return getPersistentSummary(getArgEffects(), RE, ReceiverEff, DefaultEff);
  }
  
  RetainSummary *getPersistentStopSummary() {
    if (StopSummary)
      return StopSummary;
    
    StopSummary = getPersistentSummary(RetEffect::MakeNoRet(),
                                       StopTracking, StopTracking);

    return StopSummary;
  }  

  RetainSummary *getInitMethodSummary(QualType RetTy);

  void InitializeClassMethodSummaries();
  void InitializeMethodSummaries();
  
  bool isTrackedObjCObjectType(QualType T);
  bool isTrackedCFObjectType(QualType T);
  
private:
  
  void addClsMethSummary(IdentifierInfo* ClsII, Selector S,
                         RetainSummary* Summ) {
    ObjCClassMethodSummaries[ObjCSummaryKey(ClsII, S)] = Summ;
  }
  
  void addNSObjectClsMethSummary(Selector S, RetainSummary *Summ) {
    ObjCClassMethodSummaries[S] = Summ;
  }
    
  void addNSObjectMethSummary(Selector S, RetainSummary *Summ) {
    ObjCMethodSummaries[S] = Summ;
  }

  void addClassMethSummary(const char* Cls, const char* nullaryName,
                           RetainSummary *Summ) {
    IdentifierInfo* ClsII = &Ctx.Idents.get(Cls);
    Selector S = GetNullarySelector(nullaryName, Ctx);
    ObjCClassMethodSummaries[ObjCSummaryKey(ClsII, S)]  = Summ;
  }
  
  void addInstMethSummary(const char* Cls, const char* nullaryName,
                          RetainSummary *Summ) {
    IdentifierInfo* ClsII = &Ctx.Idents.get(Cls);
    Selector S = GetNullarySelector(nullaryName, Ctx);
    ObjCMethodSummaries[ObjCSummaryKey(ClsII, S)]  = Summ;
  }
  
  Selector generateSelector(va_list argp) {
    llvm::SmallVector<IdentifierInfo*, 10> II;

    while (const char* s = va_arg(argp, const char*))
      II.push_back(&Ctx.Idents.get(s));

    return Ctx.Selectors.getSelector(II.size(), &II[0]);    
  }
  
  void addMethodSummary(IdentifierInfo *ClsII, ObjCMethodSummariesTy& Summaries,
                        RetainSummary* Summ, va_list argp) {
    Selector S = generateSelector(argp);
    Summaries[ObjCSummaryKey(ClsII, S)] = Summ;
  }
  
  void addInstMethSummary(const char* Cls, RetainSummary* Summ, ...) {
    va_list argp;
    va_start(argp, Summ);
    addMethodSummary(&Ctx.Idents.get(Cls), ObjCMethodSummaries, Summ, argp);
    va_end(argp);    
  }
  
  void addClsMethSummary(const char* Cls, RetainSummary* Summ, ...) {
    va_list argp;
    va_start(argp, Summ);
    addMethodSummary(&Ctx.Idents.get(Cls),ObjCClassMethodSummaries, Summ, argp);
    va_end(argp);
  }
  
  void addClsMethSummary(IdentifierInfo *II, RetainSummary* Summ, ...) {
    va_list argp;
    va_start(argp, Summ);
    addMethodSummary(II, ObjCClassMethodSummaries, Summ, argp);
    va_end(argp);
  }

  void addPanicSummary(const char* Cls, ...) {
    RetainSummary* Summ = getPersistentSummary(AF.GetEmptyMap(),
                                               RetEffect::MakeNoRet(),
                                               DoNothing,  DoNothing, true);
    va_list argp;
    va_start (argp, Cls);
    addMethodSummary(&Ctx.Idents.get(Cls), ObjCMethodSummaries, Summ, argp);
    va_end(argp);
  }
  
public:
  
  RetainSummaryManager(ASTContext& ctx, bool gcenabled)
   : Ctx(ctx),
     CFDictionaryCreateII(&ctx.Idents.get("CFDictionaryCreate")),
     GCEnabled(gcenabled), AF(BPAlloc), ScratchArgs(AF.GetEmptyMap()),
     ObjCAllocRetE(gcenabled ? RetEffect::MakeGCNotOwned()
                             : RetEffect::MakeOwned(RetEffect::ObjC, true)),
     ObjCInitRetE(gcenabled ? RetEffect::MakeGCNotOwned()
                            : RetEffect::MakeOwnedWhenTrackedReceiver()),
     DefaultSummary(AF.GetEmptyMap() /* per-argument effects (none) */,
                    RetEffect::MakeNoRet() /* return effect */,
                    MayEscape, /* default argument effect */
                    DoNothing /* receiver effect */),
     StopSummary(0) {

    InitializeClassMethodSummaries();
    InitializeMethodSummaries();
  }
  
  ~RetainSummaryManager();
  
  RetainSummary* getSummary(FunctionDecl* FD);  
  
  RetainSummary* getInstanceMethodSummary(ObjCMessageExpr* ME,
                                          const ObjCInterfaceDecl* ID) {
    return getInstanceMethodSummary(ME->getSelector(), ME->getClassName(),
                            ID, ME->getMethodDecl(), ME->getType());    
  }
  
  RetainSummary* getInstanceMethodSummary(Selector S, IdentifierInfo *ClsName,
                                          const ObjCInterfaceDecl* ID,
                                          const ObjCMethodDecl *MD,
                                          QualType RetTy);

  RetainSummary *getClassMethodSummary(Selector S, IdentifierInfo *ClsName,
                                       const ObjCInterfaceDecl *ID,
                                       const ObjCMethodDecl *MD,
                                       QualType RetTy);
  
  RetainSummary *getClassMethodSummary(ObjCMessageExpr *ME) {
    return getClassMethodSummary(ME->getSelector(), ME->getClassName(),
                                 ME->getClassInfo().first,
                                 ME->getMethodDecl(), ME->getType());
  }

  /// getMethodSummary - This version of getMethodSummary is used to query
  ///  the summary for the current method being analyzed.
  RetainSummary *getMethodSummary(const ObjCMethodDecl *MD) {
    // FIXME: Eventually this should be unneeded.
    const ObjCInterfaceDecl *ID = MD->getClassInterface();
    Selector S = MD->getSelector();
    IdentifierInfo *ClsName = ID->getIdentifier();
    QualType ResultTy = MD->getResultType();
    
    // Resolve the method decl last.    
    if (const ObjCMethodDecl *InterfaceMD =
        ResolveToInterfaceMethodDecl(MD, Ctx))
      MD = InterfaceMD;
    
    if (MD->isInstanceMethod())
      return getInstanceMethodSummary(S, ClsName, ID, MD, ResultTy);
    else
      return getClassMethodSummary(S, ClsName, ID, MD, ResultTy);
  }
  
  RetainSummary* getCommonMethodSummary(const ObjCMethodDecl* MD,
                                        Selector S, QualType RetTy);

  void updateSummaryFromAnnotations(RetainSummary &Summ,
                                    const ObjCMethodDecl *MD);

  void updateSummaryFromAnnotations(RetainSummary &Summ,
                                    const FunctionDecl *FD);

  bool isGCEnabled() const { return GCEnabled; }
  
  RetainSummary *copySummary(RetainSummary *OldSumm) {
    RetainSummary *Summ = (RetainSummary*) BPAlloc.Allocate<RetainSummary>();
    new (Summ) RetainSummary(*OldSumm);
    return Summ;
  }  
};
  
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Implementation of checker data structures.
//===----------------------------------------------------------------------===//

RetainSummaryManager::~RetainSummaryManager() {}

ArgEffects RetainSummaryManager::getArgEffects() {
  ArgEffects AE = ScratchArgs;
  ScratchArgs = AF.GetEmptyMap();
  return AE;
}

RetainSummary*
RetainSummaryManager::getPersistentSummary(ArgEffects AE, RetEffect RetEff,
                                           ArgEffect ReceiverEff,
                                           ArgEffect DefaultEff,
                                           bool isEndPath) {  
  // Create the summary and return it.
  RetainSummary *Summ = (RetainSummary*) BPAlloc.Allocate<RetainSummary>();
  new (Summ) RetainSummary(AE, RetEff, DefaultEff, ReceiverEff, isEndPath);
  return Summ;
}

//===----------------------------------------------------------------------===//
// Predicates.
//===----------------------------------------------------------------------===//

bool RetainSummaryManager::isTrackedObjCObjectType(QualType Ty) {
  if (!Ctx.isObjCObjectPointerType(Ty))
    return false;

  // We assume that id<..>, id, and "Class" all represent tracked objects.
  const PointerType *PT = Ty->getAsPointerType();
  if (PT == 0)
    return true;
    
  const ObjCInterfaceType *OT = PT->getPointeeType()->getAsObjCInterfaceType();

  // We assume that id<..>, id, and "Class" all represent tracked objects.
  if (!OT)
    return true;
    
  // Does the interface subclass NSObject?    
  // FIXME: We can memoize here if this gets too expensive.    
  ObjCInterfaceDecl* ID = OT->getDecl();  

  // Assume that anything declared with a forward declaration and no
  // @interface subclasses NSObject.
  if (ID->isForwardDecl())
    return true;
  
  IdentifierInfo* NSObjectII = &Ctx.Idents.get("NSObject");


  for ( ; ID ; ID = ID->getSuperClass())
    if (ID->getIdentifier() == NSObjectII)
      return true;
  
  return false;
}

bool RetainSummaryManager::isTrackedCFObjectType(QualType T) {
  return isRefType(T, "CF") || // Core Foundation.
         isRefType(T, "CG") || // Core Graphics.
         isRefType(T, "DADisk") || // Disk Arbitration API.
         isRefType(T, "DADissenter") ||
         isRefType(T, "DASessionRef");
}

//===----------------------------------------------------------------------===//
// Summary creation for functions (largely uses of Core Foundation).
//===----------------------------------------------------------------------===//

static bool isRetain(FunctionDecl* FD, const char* FName) {
  const char* loc = strstr(FName, "Retain");
  return loc && loc[sizeof("Retain")-1] == '\0';
}

static bool isRelease(FunctionDecl* FD, const char* FName) {
  const char* loc = strstr(FName, "Release");
  return loc && loc[sizeof("Release")-1] == '\0';
}

RetainSummary* RetainSummaryManager::getSummary(FunctionDecl* FD) {
  // Look up a summary in our cache of FunctionDecls -> Summaries.
  FuncSummariesTy::iterator I = FuncSummaries.find(FD);
  if (I != FuncSummaries.end())
    return I->second;

  // No summary?  Generate one.
  RetainSummary *S = 0;
  
  do {
    // We generate "stop" summaries for implicitly defined functions.
    if (FD->isImplicit()) {
      S = getPersistentStopSummary();
      break;
    }
    
    // [PR 3337] Use 'getAsFunctionType' to strip away any typedefs on the
    // function's type.
    const FunctionType* FT = FD->getType()->getAsFunctionType();
    const char* FName = FD->getIdentifier()->getName();
    
    // Strip away preceding '_'.  Doing this here will effect all the checks
    // down below.
    while (*FName == '_') ++FName;
    
    // Inspect the result type.
    QualType RetTy = FT->getResultType();
    
    // FIXME: This should all be refactored into a chain of "summary lookup"
    //  filters.
    assert (ScratchArgs.isEmpty());
    
    switch (strlen(FName)) {
      default: break;
        

      case 17:
        // Handle: id NSMakeCollectable(CFTypeRef)
        if (!memcmp(FName, "NSMakeCollectable", 17)) {
          S = (RetTy == Ctx.getObjCIdType())
              ? getUnarySummary(FT, cfmakecollectable)
              : getPersistentStopSummary();
        }
        else if (!memcmp(FName, "IOBSDNameMatching", 17) ||
                 !memcmp(FName, "IOServiceMatching", 17)) {
          // Part of <rdar://problem/6961230>. (IOKit)
          // This should be addressed using a API table.
          S = getPersistentSummary(RetEffect::MakeOwned(RetEffect::CF, true),
                                   DoNothing, DoNothing);
        }
        break;

      case 21:
        if (!memcmp(FName, "IOServiceNameMatching", 21)) {
          // Part of <rdar://problem/6961230>. (IOKit)
          // This should be addressed using a API table.
          S = getPersistentSummary(RetEffect::MakeOwned(RetEffect::CF, true),
                                   DoNothing, DoNothing);
        }
        break;

      case 24:
        if (!memcmp(FName, "IOServiceAddNotification", 24)) {
          // Part of <rdar://problem/6961230>. (IOKit)
          // This should be addressed using a API table.
          ScratchArgs = AF.Add(ScratchArgs, 2, DecRef);
          S = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, DoNothing);         
        }
        break;
        
      case 25:
        if (!memcmp(FName, "IORegistryEntryIDMatching", 25)) {
          // Part of <rdar://problem/6961230>. (IOKit)
          // This should be addressed using a API table.
          S = getPersistentSummary(RetEffect::MakeOwned(RetEffect::CF, true),
                                   DoNothing, DoNothing);
        }
        break;
        
      case 26:
        if (!memcmp(FName, "IOOpenFirmwarePathMatching", 26)) {
          // Part of <rdar://problem/6961230>. (IOKit)
          // This should be addressed using a API table.
          S = getPersistentSummary(RetEffect::MakeOwned(RetEffect::CF, true),
                                   DoNothing, DoNothing);          
        }
        break;

      case 27:
        if (!memcmp(FName, "IOServiceGetMatchingService", 27)) {
          // Part of <rdar://problem/6961230>.
          // This should be addressed using a API table.
          ScratchArgs = AF.Add(ScratchArgs, 1, DecRef);
          S = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, DoNothing);         
        }
        break;

      case 28:
        if (!memcmp(FName, "IOServiceGetMatchingServices", 28)) {
          // FIXES: <rdar://problem/6326900>
          // This should be addressed using a API table.  This strcmp is also
          // a little gross, but there is no need to super optimize here.
          ScratchArgs = AF.Add(ScratchArgs, 1, DecRef);
          S = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, DoNothing);
        }
        break;
        
      case 32:
        if (!memcmp(FName, "IOServiceAddMatchingNotification", 32)) {
          // Part of <rdar://problem/6961230>.
          // This should be addressed using a API table.
          ScratchArgs = AF.Add(ScratchArgs, 2, DecRef);
          S = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, DoNothing);         
        }
        break;
    }
    
    // Did we get a summary?
    if (S)
      break;

    // Enable this code once the semantics of NSDeallocateObject are resolved
    // for GC.  <rdar://problem/6619988>
#if 0
    // Handle: NSDeallocateObject(id anObject);
    // This method does allow 'nil' (although we don't check it now).
    if (strcmp(FName, "NSDeallocateObject") == 0) {      
      return RetTy == Ctx.VoidTy
        ? getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, Dealloc)
        : getPersistentStopSummary();
    }
#endif

    if (RetTy->isPointerType()) {
      // For CoreFoundation ('CF') types.
      if (isRefType(RetTy, "CF", &Ctx, FName)) {
        if (isRetain(FD, FName))
          S = getUnarySummary(FT, cfretain);
        else if (strstr(FName, "MakeCollectable"))
          S = getUnarySummary(FT, cfmakecollectable);
        else 
          S = getCFCreateGetRuleSummary(FD, FName);

        break;
      }

      // For CoreGraphics ('CG') types.
      if (isRefType(RetTy, "CG", &Ctx, FName)) {
        if (isRetain(FD, FName))
          S = getUnarySummary(FT, cfretain);
        else
          S = getCFCreateGetRuleSummary(FD, FName);

        break;
      }

      // For the Disk Arbitration API (DiskArbitration/DADisk.h)
      if (isRefType(RetTy, "DADisk") ||
          isRefType(RetTy, "DADissenter") ||
          isRefType(RetTy, "DASessionRef")) {
        S = getCFCreateGetRuleSummary(FD, FName);
        break;
      }
      
      break;
    }

    // Check for release functions, the only kind of functions that we care
    // about that don't return a pointer type.
    if (FName[0] == 'C' && (FName[1] == 'F' || FName[1] == 'G')) {
      // Test for 'CGCF'.
      if (FName[1] == 'G' && FName[2] == 'C' && FName[3] == 'F')
        FName += 4;
      else
        FName += 2;
      
      if (isRelease(FD, FName))
        S = getUnarySummary(FT, cfrelease);
      else {
        assert (ScratchArgs.isEmpty());
        // Remaining CoreFoundation and CoreGraphics functions.
        // We use to assume that they all strictly followed the ownership idiom
        // and that ownership cannot be transferred.  While this is technically
        // correct, many methods allow a tracked object to escape.  For example:
        //
        //   CFMutableDictionaryRef x = CFDictionaryCreateMutable(...);        
        //   CFDictionaryAddValue(y, key, x);
        //   CFRelease(x); 
        //   ... it is okay to use 'x' since 'y' has a reference to it
        //
        // We handle this and similar cases with the follow heuristic.  If the
        // function name contains "InsertValue", "SetValue" or "AddValue" then
        // we assume that arguments may "escape."
        //
        ArgEffect E = (CStrInCStrNoCase(FName, "InsertValue") ||
                       CStrInCStrNoCase(FName, "AddValue") ||
                       CStrInCStrNoCase(FName, "SetValue") ||
                       CStrInCStrNoCase(FName, "AppendValue"))
                      ? MayEscape : DoNothing;
        
        S = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, E);
      }
    }
  }
  while (0);
  
  if (!S)
    S = getDefaultSummary();

  // Annotations override defaults.
  assert(S);
  updateSummaryFromAnnotations(*S, FD);
  
  FuncSummaries[FD] = S;
  return S;  
}

RetainSummary*
RetainSummaryManager::getCFCreateGetRuleSummary(FunctionDecl* FD,
                                                const char* FName) {
  
  if (strstr(FName, "Create") || strstr(FName, "Copy"))
    return getCFSummaryCreateRule(FD);
  
  if (strstr(FName, "Get"))
    return getCFSummaryGetRule(FD);
  
  return getDefaultSummary();
}

RetainSummary*
RetainSummaryManager::getUnarySummary(const FunctionType* FT,
                                      UnaryFuncKind func) {

  // Sanity check that this is *really* a unary function.  This can
  // happen if people do weird things.
  const FunctionProtoType* FTP = dyn_cast<FunctionProtoType>(FT);
  if (!FTP || FTP->getNumArgs() != 1)
    return getPersistentStopSummary();
  
  assert (ScratchArgs.isEmpty());
  
  switch (func) {
    case cfretain: {
      ScratchArgs = AF.Add(ScratchArgs, 0, IncRef);
      return getPersistentSummary(RetEffect::MakeAlias(0),
                                  DoNothing, DoNothing);
    }
      
    case cfrelease: {
      ScratchArgs = AF.Add(ScratchArgs, 0, DecRef);
      return getPersistentSummary(RetEffect::MakeNoRet(),
                                  DoNothing, DoNothing);
    }
      
    case cfmakecollectable: {
      ScratchArgs = AF.Add(ScratchArgs, 0, MakeCollectable);
      return getPersistentSummary(RetEffect::MakeAlias(0),DoNothing, DoNothing);    
    }
      
    default:
      assert (false && "Not a supported unary function.");
      return getDefaultSummary();
  }
}

RetainSummary* RetainSummaryManager::getCFSummaryCreateRule(FunctionDecl* FD) {
  assert (ScratchArgs.isEmpty());
  
  if (FD->getIdentifier() == CFDictionaryCreateII) {
    ScratchArgs = AF.Add(ScratchArgs, 1, DoNothingByRef);
    ScratchArgs = AF.Add(ScratchArgs, 2, DoNothingByRef);
  }
  
  return getPersistentSummary(RetEffect::MakeOwned(RetEffect::CF, true));
}

RetainSummary* RetainSummaryManager::getCFSummaryGetRule(FunctionDecl* FD) {
  assert (ScratchArgs.isEmpty());  
  return getPersistentSummary(RetEffect::MakeNotOwned(RetEffect::CF),
                              DoNothing, DoNothing);
}

//===----------------------------------------------------------------------===//
// Summary creation for Selectors.
//===----------------------------------------------------------------------===//

RetainSummary*
RetainSummaryManager::getInitMethodSummary(QualType RetTy) {
  assert(ScratchArgs.isEmpty());    
  // 'init' methods conceptually return a newly allocated object and claim
  // the receiver.  
  if (isTrackedObjCObjectType(RetTy) || isTrackedCFObjectType(RetTy))
    return getPersistentSummary(ObjCInitRetE, DecRefMsg);
  
  return getDefaultSummary();
}

void
RetainSummaryManager::updateSummaryFromAnnotations(RetainSummary &Summ,
                                                   const FunctionDecl *FD) {
  if (!FD)
    return;

  QualType RetTy = FD->getResultType();
  
  // Determine if there is a special return effect for this method.
  if (isTrackedObjCObjectType(RetTy)) {
    if (FD->getAttr<NSReturnsRetainedAttr>()) {
      Summ.setRetEffect(ObjCAllocRetE);
    }
    else if (FD->getAttr<CFReturnsRetainedAttr>()) {
      Summ.setRetEffect(RetEffect::MakeOwned(RetEffect::CF, true));
    }
  }
  else if (RetTy->getAsPointerType()) {
    if (FD->getAttr<CFReturnsRetainedAttr>()) {
      Summ.setRetEffect(RetEffect::MakeOwned(RetEffect::CF, true));
    }
  }
}

void
RetainSummaryManager::updateSummaryFromAnnotations(RetainSummary &Summ,
                                                  const ObjCMethodDecl *MD) {
  if (!MD)
    return;

  // Determine if there is a special return effect for this method.
  if (isTrackedObjCObjectType(MD->getResultType())) {
    if (MD->getAttr<NSReturnsRetainedAttr>()) {
      Summ.setRetEffect(ObjCAllocRetE);
    }
    else if (MD->getAttr<CFReturnsRetainedAttr>()) {
      Summ.setRetEffect(RetEffect::MakeOwned(RetEffect::CF, true));
    }
  }
}

RetainSummary*
RetainSummaryManager::getCommonMethodSummary(const ObjCMethodDecl* MD,
                                             Selector S, QualType RetTy) {

  if (MD) {
    // Scan the method decl for 'void*' arguments.  These should be treated
    // as 'StopTracking' because they are often used with delegates.
    // Delegates are a frequent form of false positives with the retain
    // count checker.
    unsigned i = 0;
    for (ObjCMethodDecl::param_iterator I = MD->param_begin(),
         E = MD->param_end(); I != E; ++I, ++i)
      if (ParmVarDecl *PD = *I) {
        QualType Ty = Ctx.getCanonicalType(PD->getType());
        if (Ty.getUnqualifiedType() == Ctx.VoidPtrTy)
          ScratchArgs = AF.Add(ScratchArgs, i, StopTracking);
      }
  }
  
  // Any special effect for the receiver?
  ArgEffect ReceiverEff = DoNothing;
  
  // If one of the arguments in the selector has the keyword 'delegate' we
  // should stop tracking the reference count for the receiver.  This is
  // because the reference count is quite possibly handled by a delegate
  // method.
  if (S.isKeywordSelector()) {
    const std::string &str = S.getAsString();
    assert(!str.empty());
    if (CStrInCStrNoCase(&str[0], "delegate:")) ReceiverEff = StopTracking;
  }
  
  // Look for methods that return an owned object.
  if (isTrackedObjCObjectType(RetTy)) {    
    // EXPERIMENTAL: Assume the Cocoa conventions for all objects returned
    //  by instance methods.
    RetEffect E = followsFundamentalRule(S)
                  ? ObjCAllocRetE : RetEffect::MakeNotOwned(RetEffect::ObjC);
    
    return getPersistentSummary(E, ReceiverEff, MayEscape);    
  }
  
  // Look for methods that return an owned core foundation object.
  if (isTrackedCFObjectType(RetTy)) {
    RetEffect E = followsFundamentalRule(S)
      ? RetEffect::MakeOwned(RetEffect::CF, true)
      : RetEffect::MakeNotOwned(RetEffect::CF);
    
    return getPersistentSummary(E, ReceiverEff, MayEscape);
  }
  
  if (ScratchArgs.isEmpty() && ReceiverEff == DoNothing)
    return getDefaultSummary();
  
  return getPersistentSummary(RetEffect::MakeNoRet(), ReceiverEff, MayEscape);
}

RetainSummary*
RetainSummaryManager::getInstanceMethodSummary(Selector S,
                                               IdentifierInfo *ClsName,
                                               const ObjCInterfaceDecl* ID,
                                               const ObjCMethodDecl *MD,
                                               QualType RetTy) {

  // Look up a summary in our summary cache.
  ObjCMethodSummariesTy::iterator I = ObjCMethodSummaries.find(ID, ClsName, S);
  
  if (I != ObjCMethodSummaries.end())
    return I->second;

  assert(ScratchArgs.isEmpty());
  RetainSummary *Summ = 0;
  
  // "initXXX": pass-through for receiver.
  if (deriveNamingConvention(S) == InitRule)
    Summ = getInitMethodSummary(RetTy);
  else
    Summ = getCommonMethodSummary(MD, S, RetTy);
  
  // Annotations override defaults.
  updateSummaryFromAnnotations(*Summ, MD);
  
  // Memoize the summary.
  ObjCMethodSummaries[ObjCSummaryKey(ID, ClsName, S)] = Summ;
  return Summ;
}

RetainSummary*
RetainSummaryManager::getClassMethodSummary(Selector S, IdentifierInfo *ClsName,
                                            const ObjCInterfaceDecl *ID,
                                            const ObjCMethodDecl *MD,
                                            QualType RetTy) {

  assert(ClsName && "Class name must be specified.");
  ObjCMethodSummariesTy::iterator I =
    ObjCClassMethodSummaries.find(ID, ClsName, S);  
  
  if (I != ObjCClassMethodSummaries.end())
    return I->second;
    
  RetainSummary *Summ = getCommonMethodSummary(MD, S, RetTy);
  
  // Annotations override defaults.
  updateSummaryFromAnnotations(*Summ, MD);

  // Memoize the summary.
  ObjCClassMethodSummaries[ObjCSummaryKey(ID, ClsName, S)] = Summ;
  return Summ;
}

void RetainSummaryManager::InitializeClassMethodSummaries() {  
  assert(ScratchArgs.isEmpty());
  RetainSummary* Summ = getPersistentSummary(ObjCAllocRetE);
  
  // Create the summaries for "alloc", "new", and "allocWithZone:" for
  // NSObject and its derivatives.
  addNSObjectClsMethSummary(GetNullarySelector("alloc", Ctx), Summ);
  addNSObjectClsMethSummary(GetNullarySelector("new", Ctx), Summ);
  addNSObjectClsMethSummary(GetUnarySelector("allocWithZone", Ctx), Summ);
  
  // Create the [NSAssertionHandler currentHander] summary.  
  addClsMethSummary(&Ctx.Idents.get("NSAssertionHandler"),
                GetNullarySelector("currentHandler", Ctx),
                getPersistentSummary(RetEffect::MakeNotOwned(RetEffect::ObjC)));
  
  // Create the [NSAutoreleasePool addObject:] summary.
  ScratchArgs = AF.Add(ScratchArgs, 0, Autorelease);
  addClsMethSummary(&Ctx.Idents.get("NSAutoreleasePool"),
                    GetUnarySelector("addObject", Ctx),
                    getPersistentSummary(RetEffect::MakeNoRet(),
                                         DoNothing, Autorelease));
  
  // Create the summaries for [NSObject performSelector...].  We treat
  // these as 'stop tracking' for the arguments because they are often
  // used for delegates that can release the object.  When we have better
  // inter-procedural analysis we can potentially do something better.  This
  // workaround is to remove false positives.
  Summ = getPersistentSummary(RetEffect::MakeNoRet(), DoNothing, StopTracking);
  IdentifierInfo *NSObjectII = &Ctx.Idents.get("NSObject");
  addClsMethSummary(NSObjectII, Summ, "performSelector", "withObject",
                    "afterDelay", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelector", "withObject",
                    "afterDelay", "inModes", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelectorOnMainThread",
                    "withObject", "waitUntilDone", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelectorOnMainThread",
                    "withObject", "waitUntilDone", "modes", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelector", "onThread",
                    "withObject", "waitUntilDone", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelector", "onThread",
                    "withObject", "waitUntilDone", "modes", NULL);
  addClsMethSummary(NSObjectII, Summ, "performSelectorInBackground",
                    "withObject", NULL);
  
  // Specially handle NSData.
  RetainSummary *dataWithBytesNoCopySumm =
    getPersistentSummary(RetEffect::MakeNotOwned(RetEffect::ObjC), DoNothing,
                         DoNothing);
  addClsMethSummary("NSData", dataWithBytesNoCopySumm,
                    "dataWithBytesNoCopy", "length", NULL);
  addClsMethSummary("NSData", dataWithBytesNoCopySumm,
                    "dataWithBytesNoCopy", "length", "freeWhenDone", NULL);
}

void RetainSummaryManager::InitializeMethodSummaries() {
  
  assert (ScratchArgs.isEmpty());  
  
  // Create the "init" selector.  It just acts as a pass-through for the
  // receiver.
  addNSObjectMethSummary(GetNullarySelector("init", Ctx),
                         getPersistentSummary(ObjCInitRetE, DecRefMsg));
  
  // The next methods are allocators.
  RetainSummary *AllocSumm = getPersistentSummary(ObjCAllocRetE);  
  
  // Create the "copy" selector.  
  addNSObjectMethSummary(GetNullarySelector("copy", Ctx), AllocSumm);  

  // Create the "mutableCopy" selector.
  addNSObjectMethSummary(GetNullarySelector("mutableCopy", Ctx), AllocSumm);
  
  // Create the "retain" selector.
  RetEffect E = RetEffect::MakeReceiverAlias();
  RetainSummary *Summ = getPersistentSummary(E, IncRefMsg);
  addNSObjectMethSummary(GetNullarySelector("retain", Ctx), Summ);
  
  // Create the "release" selector.
  Summ = getPersistentSummary(E, DecRefMsg);
  addNSObjectMethSummary(GetNullarySelector("release", Ctx), Summ);
  
  // Create the "drain" selector.
  Summ = getPersistentSummary(E, isGCEnabled() ? DoNothing : DecRef);
  addNSObjectMethSummary(GetNullarySelector("drain", Ctx), Summ);
  
  // Create the -dealloc summary.
  Summ = getPersistentSummary(RetEffect::MakeNoRet(), Dealloc);
  addNSObjectMethSummary(GetNullarySelector("dealloc", Ctx), Summ);

  // Create the "autorelease" selector.
  Summ = getPersistentSummary(E, Autorelease);
  addNSObjectMethSummary(GetNullarySelector("autorelease", Ctx), Summ);
  
  // Specially handle NSAutoreleasePool.
  addInstMethSummary("NSAutoreleasePool", "init",
                     getPersistentSummary(RetEffect::MakeReceiverAlias(),
                                          NewAutoreleasePool));
  
  // For NSWindow, allocated objects are (initially) self-owned.  
  // FIXME: For now we opt for false negatives with NSWindow, as these objects
  //  self-own themselves.  However, they only do this once they are displayed.
  //  Thus, we need to track an NSWindow's display status.
  //  This is tracked in <rdar://problem/6062711>.
  //  See also http://llvm.org/bugs/show_bug.cgi?id=3714.
  RetainSummary *NoTrackYet = getPersistentSummary(RetEffect::MakeNoRet(),
                                                   StopTracking,
                                                   StopTracking);
  
  addClassMethSummary("NSWindow", "alloc", NoTrackYet);

#if 0
  addInstMethSummary("NSWindow", NoTrackYet, "initWithContentRect",
                     "styleMask", "backing", "defer", NULL);
  
  addInstMethSummary("NSWindow", NoTrackYet, "initWithContentRect",
                     "styleMask", "backing", "defer", "screen", NULL);
#endif
  
  // For NSPanel (which subclasses NSWindow), allocated objects are not
  //  self-owned.
  // FIXME: For now we don't track NSPanels. object for the same reason
  //   as for NSWindow objects.
  addClassMethSummary("NSPanel", "alloc", NoTrackYet);
  
#if 0
  addInstMethSummary("NSPanel", NoTrackYet, "initWithContentRect",
                     "styleMask", "backing", "defer", NULL);
  
  addInstMethSummary("NSPanel", NoTrackYet, "initWithContentRect",
                     "styleMask", "backing", "defer", "screen", NULL);
#endif
  
  // Don't track allocated autorelease pools yet, as it is okay to prematurely
  // exit a method.
  addClassMethSummary("NSAutoreleasePool", "alloc", NoTrackYet);

  // Create NSAssertionHandler summaries.
  addPanicSummary("NSAssertionHandler", "handleFailureInFunction", "file",
                  "lineNumber", "description", NULL); 
  
  addPanicSummary("NSAssertionHandler", "handleFailureInMethod", "object",
                  "file", "lineNumber", "description", NULL);
  
  // Create summaries QCRenderer/QCView -createSnapShotImageOfType:
  addInstMethSummary("QCRenderer", AllocSumm,
                     "createSnapshotImageOfType", NULL);
  addInstMethSummary("QCView", AllocSumm,
                     "createSnapshotImageOfType", NULL);

  // Create summaries for CIContext, 'createCGImage' and
  // 'createCGLayerWithSize'.
  addInstMethSummary("CIContext", AllocSumm,
                     "createCGImage", "fromRect", NULL);
  addInstMethSummary("CIContext", AllocSumm,
                     "createCGImage", "fromRect", "format", "colorSpace", NULL);  
  addInstMethSummary("CIContext", AllocSumm, "createCGLayerWithSize",
           "info", NULL);
}

//===----------------------------------------------------------------------===//
// Reference-counting logic (typestate + counts).
//===----------------------------------------------------------------------===//

namespace {
  
class VISIBILITY_HIDDEN RefVal {
public:  
  enum Kind {
    Owned = 0, // Owning reference.    
    NotOwned,  // Reference is not owned by still valid (not freed).    
    Released,  // Object has been released.
    ReturnedOwned, // Returned object passes ownership to caller.
    ReturnedNotOwned, // Return object does not pass ownership to caller.
    ERROR_START,
    ErrorDeallocNotOwned, // -dealloc called on non-owned object.
    ErrorDeallocGC, // Calling -dealloc with GC enabled.
    ErrorUseAfterRelease, // Object used after released.    
    ErrorReleaseNotOwned, // Release of an object that was not owned.
    ERROR_LEAK_START,
    ErrorLeak,  // A memory leak due to excessive reference counts.
    ErrorLeakReturned, // A memory leak due to the returning method not having
                      // the correct naming conventions.
    ErrorGCLeakReturned,
    ErrorOverAutorelease,
    ErrorReturnedNotOwned
  };

private:  
  Kind kind;
  RetEffect::ObjKind okind;
  unsigned Cnt;
  unsigned ACnt;
  QualType T;

  RefVal(Kind k, RetEffect::ObjKind o, unsigned cnt, unsigned acnt, QualType t)
    : kind(k), okind(o), Cnt(cnt), ACnt(acnt), T(t) {}

  RefVal(Kind k, unsigned cnt = 0)
    : kind(k), okind(RetEffect::AnyObj), Cnt(cnt), ACnt(0) {}

public:    
  Kind getKind() const { return kind; }
  
  RetEffect::ObjKind getObjKind() const { return okind; }

  unsigned getCount() const { return Cnt; }
  unsigned getAutoreleaseCount() const { return ACnt; }
  unsigned getCombinedCounts() const { return Cnt + ACnt; }
  void clearCounts() { Cnt = 0; ACnt = 0; }
  void setCount(unsigned i) { Cnt = i; }
  void setAutoreleaseCount(unsigned i) { ACnt = i; }
  
  QualType getType() const { return T; }
  
  // Useful predicates.
  
  static bool isError(Kind k) { return k >= ERROR_START; }
  
  static bool isLeak(Kind k) { return k >= ERROR_LEAK_START; }
  
  bool isOwned() const {
    return getKind() == Owned;
  }
  
  bool isNotOwned() const {
    return getKind() == NotOwned;
  }
  
  bool isReturnedOwned() const {
    return getKind() == ReturnedOwned;
  }
  
  bool isReturnedNotOwned() const {
    return getKind() == ReturnedNotOwned;
  }
  
  bool isNonLeakError() const {
    Kind k = getKind();
    return isError(k) && !isLeak(k);
  }
  
  static RefVal makeOwned(RetEffect::ObjKind o, QualType t,
                          unsigned Count = 1) {
    return RefVal(Owned, o, Count, 0, t);
  }
  
  static RefVal makeNotOwned(RetEffect::ObjKind o, QualType t,
                             unsigned Count = 0) {
    return RefVal(NotOwned, o, Count, 0, t);
  }
  
  // Comparison, profiling, and pretty-printing.
  
  bool operator==(const RefVal& X) const {
    return kind == X.kind && Cnt == X.Cnt && T == X.T && ACnt == X.ACnt;
  }
  
  RefVal operator-(size_t i) const {
    return RefVal(getKind(), getObjKind(), getCount() - i,
                  getAutoreleaseCount(), getType());
  }
  
  RefVal operator+(size_t i) const {
    return RefVal(getKind(), getObjKind(), getCount() + i,
                  getAutoreleaseCount(), getType());
  }
  
  RefVal operator^(Kind k) const {
    return RefVal(k, getObjKind(), getCount(), getAutoreleaseCount(),
                  getType());
  }
  
  RefVal autorelease() const {
    return RefVal(getKind(), getObjKind(), getCount(), getAutoreleaseCount()+1,
                  getType());
  }
    
  void Profile(llvm::FoldingSetNodeID& ID) const {
    ID.AddInteger((unsigned) kind);
    ID.AddInteger(Cnt);
    ID.AddInteger(ACnt);
    ID.Add(T);
  }

  void print(std::ostream& Out) const;
};
  
void RefVal::print(std::ostream& Out) const {
  if (!T.isNull())
    Out << "Tracked Type:" << T.getAsString() << '\n';
    
  switch (getKind()) {
    default: assert(false);
    case Owned: { 
      Out << "Owned";
      unsigned cnt = getCount();
      if (cnt) Out << " (+ " << cnt << ")";
      break;
    }
      
    case NotOwned: {
      Out << "NotOwned";
      unsigned cnt = getCount();
      if (cnt) Out << " (+ " << cnt << ")";
      break;
    }
      
    case ReturnedOwned: { 
      Out << "ReturnedOwned";
      unsigned cnt = getCount();
      if (cnt) Out << " (+ " << cnt << ")";
      break;
    }
      
    case ReturnedNotOwned: {
      Out << "ReturnedNotOwned";
      unsigned cnt = getCount();
      if (cnt) Out << " (+ " << cnt << ")";
      break;
    }
            
    case Released:
      Out << "Released";
      break;

    case ErrorDeallocGC:
      Out << "-dealloc (GC)";
      break;
    
    case ErrorDeallocNotOwned:
      Out << "-dealloc (not-owned)";
      break;
      
    case ErrorLeak:
      Out << "Leaked";
      break;            
      
    case ErrorLeakReturned:
      Out << "Leaked (Bad naming)";
      break;
      
    case ErrorGCLeakReturned:
      Out << "Leaked (GC-ed at return)";
      break;

    case ErrorUseAfterRelease:
      Out << "Use-After-Release [ERROR]";
      break;
      
    case ErrorReleaseNotOwned:
      Out << "Release of Not-Owned [ERROR]";
      break;
      
    case RefVal::ErrorOverAutorelease:
      Out << "Over autoreleased";
      break;
      
    case RefVal::ErrorReturnedNotOwned:
      Out << "Non-owned object returned instead of owned";
      break;
  }
  
  if (ACnt) {
    Out << " [ARC +" << ACnt << ']';
  }
}
  
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// RefBindings - State used to track object reference counts.
//===----------------------------------------------------------------------===//
  
typedef llvm::ImmutableMap<SymbolRef, RefVal> RefBindings;
static int RefBIndex = 0;

namespace clang {
  template<>
  struct GRStateTrait<RefBindings> : public GRStatePartialTrait<RefBindings> {
    static inline void* GDMIndex() { return &RefBIndex; }  
  };
}

//===----------------------------------------------------------------------===//
// AutoreleaseBindings - State used to track objects in autorelease pools.
//===----------------------------------------------------------------------===//

typedef llvm::ImmutableMap<SymbolRef, unsigned> ARCounts;
typedef llvm::ImmutableMap<SymbolRef, ARCounts> ARPoolContents;
typedef llvm::ImmutableList<SymbolRef> ARStack;

static int AutoRCIndex = 0;
static int AutoRBIndex = 0;

namespace { class VISIBILITY_HIDDEN AutoreleasePoolContents {}; }
namespace { class VISIBILITY_HIDDEN AutoreleaseStack {}; }

namespace clang {
template<> struct GRStateTrait<AutoreleaseStack>
  : public GRStatePartialTrait<ARStack> {
  static inline void* GDMIndex() { return &AutoRBIndex; }  
};

template<> struct GRStateTrait<AutoreleasePoolContents>
  : public GRStatePartialTrait<ARPoolContents> {
  static inline void* GDMIndex() { return &AutoRCIndex; }  
};
} // end clang namespace

static SymbolRef GetCurrentAutoreleasePool(const GRState* state) {
  ARStack stack = state->get<AutoreleaseStack>();
  return stack.isEmpty() ? SymbolRef() : stack.getHead();
}

static GRStateRef SendAutorelease(GRStateRef state, ARCounts::Factory &F,
                                  SymbolRef sym) {

  SymbolRef pool = GetCurrentAutoreleasePool(state);
  const ARCounts *cnts = state.get<AutoreleasePoolContents>(pool);
  ARCounts newCnts(0);
  
  if (cnts) {
    const unsigned *cnt = (*cnts).lookup(sym);
    newCnts = F.Add(*cnts, sym, cnt ? *cnt  + 1 : 1);
  }
  else
    newCnts = F.Add(F.GetEmptyMap(), sym, 1);
  
  return state.set<AutoreleasePoolContents>(pool, newCnts);
}

//===----------------------------------------------------------------------===//
// Transfer functions.
//===----------------------------------------------------------------------===//

namespace {
  
class VISIBILITY_HIDDEN CFRefCount : public GRSimpleVals {
public:
  class BindingsPrinter : public GRState::Printer {
  public:
    virtual void Print(std::ostream& Out, const GRState* state,
                       const char* nl, const char* sep);
  };

private:
  typedef llvm::DenseMap<const GRExprEngine::NodeTy*, const RetainSummary*>
          SummaryLogTy;  

  RetainSummaryManager Summaries;  
  SummaryLogTy SummaryLog;
  const LangOptions&   LOpts;
  ARCounts::Factory    ARCountFactory;

  BugType *useAfterRelease, *releaseNotOwned;
  BugType *deallocGC, *deallocNotOwned;
  BugType *leakWithinFunction, *leakAtReturn;
  BugType *overAutorelease;
  BugType *returnNotOwnedForOwned;
  BugReporter *BR;
  
  GRStateRef Update(GRStateRef state, SymbolRef sym, RefVal V, ArgEffect E,
                    RefVal::Kind& hasErr);

  void ProcessNonLeakError(ExplodedNodeSet<GRState>& Dst,
                           GRStmtNodeBuilder<GRState>& Builder,
                           Expr* NodeExpr, Expr* ErrorExpr,
                           ExplodedNode<GRState>* Pred,
                           const GRState* St,
                           RefVal::Kind hasErr, SymbolRef Sym);
  
  GRStateRef HandleSymbolDeath(GRStateRef state, SymbolRef sid, RefVal V,
                               llvm::SmallVectorImpl<SymbolRef> &Leaked);
    
  ExplodedNode<GRState>* ProcessLeaks(GRStateRef state,
                                      llvm::SmallVectorImpl<SymbolRef> &Leaked,
                                      GenericNodeBuilder &Builder,
                                      GRExprEngine &Eng,
                                      ExplodedNode<GRState> *Pred = 0);
  
public:  
  CFRefCount(ASTContext& Ctx, bool gcenabled, const LangOptions& lopts)
    : Summaries(Ctx, gcenabled),
      LOpts(lopts), useAfterRelease(0), releaseNotOwned(0),
      deallocGC(0), deallocNotOwned(0),
      leakWithinFunction(0), leakAtReturn(0), overAutorelease(0),
      returnNotOwnedForOwned(0), BR(0) {}
  
  virtual ~CFRefCount() {}
  
  void RegisterChecks(BugReporter &BR);
 
  virtual void RegisterPrinters(std::vector<GRState::Printer*>& Printers) {
    Printers.push_back(new BindingsPrinter());
  }
  
  bool isGCEnabled() const { return Summaries.isGCEnabled(); }
  const LangOptions& getLangOptions() const { return LOpts; }
  
  const RetainSummary *getSummaryOfNode(const ExplodedNode<GRState> *N) const {
    SummaryLogTy::const_iterator I = SummaryLog.find(N);
    return I == SummaryLog.end() ? 0 : I->second;
  }
  
  // Calls.

  void EvalSummary(ExplodedNodeSet<GRState>& Dst,
                   GRExprEngine& Eng,
                   GRStmtNodeBuilder<GRState>& Builder,
                   Expr* Ex,
                   Expr* Receiver,
                   const RetainSummary& Summ,
                   ExprIterator arg_beg, ExprIterator arg_end,
                   ExplodedNode<GRState>* Pred);
    
  virtual void EvalCall(ExplodedNodeSet<GRState>& Dst,
                        GRExprEngine& Eng,
                        GRStmtNodeBuilder<GRState>& Builder,
                        CallExpr* CE, SVal L,
                        ExplodedNode<GRState>* Pred);  
  
  
  virtual void EvalObjCMessageExpr(ExplodedNodeSet<GRState>& Dst,
                                   GRExprEngine& Engine,
                                   GRStmtNodeBuilder<GRState>& Builder,
                                   ObjCMessageExpr* ME,
                                   ExplodedNode<GRState>* Pred);
  
  bool EvalObjCMessageExprAux(ExplodedNodeSet<GRState>& Dst,
                              GRExprEngine& Engine,
                              GRStmtNodeBuilder<GRState>& Builder,
                              ObjCMessageExpr* ME,
                              ExplodedNode<GRState>* Pred);

  // Stores.  
  virtual void EvalBind(GRStmtNodeBuilderRef& B, SVal location, SVal val);

  // End-of-path.
  
  virtual void EvalEndPath(GRExprEngine& Engine,
                           GREndPathNodeBuilder<GRState>& Builder);
  
  virtual void EvalDeadSymbols(ExplodedNodeSet<GRState>& Dst,
                               GRExprEngine& Engine,
                               GRStmtNodeBuilder<GRState>& Builder,
                               ExplodedNode<GRState>* Pred,
                               Stmt* S, const GRState* state,
                               SymbolReaper& SymReaper);
  
  std::pair<ExplodedNode<GRState>*, GRStateRef>
  HandleAutoreleaseCounts(GRStateRef state, GenericNodeBuilder Bd,
                          ExplodedNode<GRState>* Pred, GRExprEngine &Eng,
                          SymbolRef Sym, RefVal V, bool &stop);
  // Return statements.
  
  virtual void EvalReturn(ExplodedNodeSet<GRState>& Dst,
                          GRExprEngine& Engine,
                          GRStmtNodeBuilder<GRState>& Builder,
                          ReturnStmt* S,
                          ExplodedNode<GRState>* Pred);

  // Assumptions.

  virtual const GRState* EvalAssume(GRStateManager& VMgr,
                                       const GRState* St, SVal Cond,
                                       bool Assumption, bool& isFeasible);
};

} // end anonymous namespace

static void PrintPool(std::ostream &Out, SymbolRef Sym, const GRState *state) {
  Out << ' ';
  if (Sym)
    Out << Sym->getSymbolID();
  else
    Out << "<pool>";
  Out << ":{";
    
  // Get the contents of the pool.
  if (const ARCounts *cnts = state->get<AutoreleasePoolContents>(Sym))
    for (ARCounts::iterator J=cnts->begin(), EJ=cnts->end(); J != EJ; ++J)
      Out << '(' << J.getKey() << ',' << J.getData() << ')';

  Out << '}';  
}

void CFRefCount::BindingsPrinter::Print(std::ostream& Out, const GRState* state,
                                        const char* nl, const char* sep) {
  
  
    
  RefBindings B = state->get<RefBindings>();
  
  if (!B.isEmpty())
    Out << sep << nl;
  
  for (RefBindings::iterator I=B.begin(), E=B.end(); I!=E; ++I) {
    Out << (*I).first << " : ";
    (*I).second.print(Out);
    Out << nl;
  }
  
  // Print the autorelease stack.
  Out << sep << nl << "AR pool stack:";
  ARStack stack = state->get<AutoreleaseStack>();
  
  PrintPool(Out, SymbolRef(), state);  // Print the caller's pool.
  for (ARStack::iterator I=stack.begin(), E=stack.end(); I!=E; ++I)
    PrintPool(Out, *I, state);

  Out << nl;
}

//===----------------------------------------------------------------------===//
// Error reporting.
//===----------------------------------------------------------------------===//

namespace {
  
  //===-------------===//
  // Bug Descriptions. //
  //===-------------===//  
  
  class VISIBILITY_HIDDEN CFRefBug : public BugType {
  protected:
    CFRefCount& TF;
    
    CFRefBug(CFRefCount* tf, const char* name) 
    : BugType(name, "Memory (Core Foundation/Objective-C)"), TF(*tf) {}    
  public:
    
    CFRefCount& getTF() { return TF; }
    const CFRefCount& getTF() const { return TF; }
    
    // FIXME: Eventually remove.
    virtual const char* getDescription() const = 0;
    
    virtual bool isLeak() const { return false; }
  };
  
  class VISIBILITY_HIDDEN UseAfterRelease : public CFRefBug {
  public:
    UseAfterRelease(CFRefCount* tf)
    : CFRefBug(tf, "Use-after-release") {}
    
    const char* getDescription() const {
      return "Reference-counted object is used after it is released";
    }    
  };
  
  class VISIBILITY_HIDDEN BadRelease : public CFRefBug {
  public:
    BadRelease(CFRefCount* tf) : CFRefBug(tf, "Bad release") {}
    
    const char* getDescription() const {
      return "Incorrect decrement of the reference count of an "
      "object is not owned at this point by the caller";
    }
  };
  
  class VISIBILITY_HIDDEN DeallocGC : public CFRefBug {
  public:
    DeallocGC(CFRefCount *tf)
      : CFRefBug(tf, "-dealloc called while using garbage collection") {}
    
    const char *getDescription() const {
      return "-dealloc called while using garbage collection";
    }
  };
  
  class VISIBILITY_HIDDEN DeallocNotOwned : public CFRefBug {
  public:
    DeallocNotOwned(CFRefCount *tf)
      : CFRefBug(tf, "-dealloc sent to non-exclusively owned object") {}
    
    const char *getDescription() const {
      return "-dealloc sent to object that may be referenced elsewhere";
    }
  };  
  
  class VISIBILITY_HIDDEN OverAutorelease : public CFRefBug {
  public:
    OverAutorelease(CFRefCount *tf) : 
      CFRefBug(tf, "Object sent -autorelease too many times") {}
    
    const char *getDescription() const {
      return "Object sent -autorelease too many times";
    }
  };
  
  class VISIBILITY_HIDDEN ReturnedNotOwnedForOwned : public CFRefBug {
  public:
    ReturnedNotOwnedForOwned(CFRefCount *tf) :
      CFRefBug(tf, "Method should return an owned object") {}
    
    const char *getDescription() const {
      return "Object with +0 retain counts returned to caller where a +1 "
             "(owning) retain count is expected";
    }
  };
  
  class VISIBILITY_HIDDEN Leak : public CFRefBug {
    const bool isReturn;
  protected:
    Leak(CFRefCount* tf, const char* name, bool isRet)
    : CFRefBug(tf, name), isReturn(isRet) {}
  public:
    
    const char* getDescription() const { return ""; }
    
    bool isLeak() const { return true; }
  };
  
  class VISIBILITY_HIDDEN LeakAtReturn : public Leak {
  public:
    LeakAtReturn(CFRefCount* tf, const char* name)
    : Leak(tf, name, true) {}
  };
  
  class VISIBILITY_HIDDEN LeakWithinFunction : public Leak {
  public:
    LeakWithinFunction(CFRefCount* tf, const char* name)
    : Leak(tf, name, false) {}
  };  
  
  //===---------===//
  // Bug Reports.  //
  //===---------===//
  
  class VISIBILITY_HIDDEN CFRefReport : public RangedBugReport {
  protected:
    SymbolRef Sym;
    const CFRefCount &TF;
  public:
    CFRefReport(CFRefBug& D, const CFRefCount &tf,
                ExplodedNode<GRState> *n, SymbolRef sym)
      : RangedBugReport(D, D.getDescription(), n), Sym(sym), TF(tf) {}

    CFRefReport(CFRefBug& D, const CFRefCount &tf,
                ExplodedNode<GRState> *n, SymbolRef sym, const char* endText)
      : RangedBugReport(D, D.getDescription(), endText, n), Sym(sym), TF(tf) {}
    
    virtual ~CFRefReport() {}
    
    CFRefBug& getBugType() {
      return (CFRefBug&) RangedBugReport::getBugType();
    }
    const CFRefBug& getBugType() const {
      return (const CFRefBug&) RangedBugReport::getBugType();
    }
    
    virtual void getRanges(BugReporter& BR, const SourceRange*& beg,           
                           const SourceRange*& end) {
      
      if (!getBugType().isLeak())
        RangedBugReport::getRanges(BR, beg, end);
      else
        beg = end = 0;
    }
    
    SymbolRef getSymbol() const { return Sym; }
    
    PathDiagnosticPiece* getEndPath(BugReporterContext& BRC,
                                    const ExplodedNode<GRState>* N);
    
    std::pair<const char**,const char**> getExtraDescriptiveText();
    
    PathDiagnosticPiece* VisitNode(const ExplodedNode<GRState>* N,
                                   const ExplodedNode<GRState>* PrevN,
                                   BugReporterContext& BRC);
  };

  class VISIBILITY_HIDDEN CFRefLeakReport : public CFRefReport {
    SourceLocation AllocSite;
    const MemRegion* AllocBinding;
  public:
    CFRefLeakReport(CFRefBug& D, const CFRefCount &tf,
                    ExplodedNode<GRState> *n, SymbolRef sym,
                    GRExprEngine& Eng);
    
    PathDiagnosticPiece* getEndPath(BugReporterContext& BRC,
                                    const ExplodedNode<GRState>* N);
    
    SourceLocation getLocation() const { return AllocSite; }
  };  
} // end anonymous namespace

void CFRefCount::RegisterChecks(BugReporter& BR) {
  useAfterRelease = new UseAfterRelease(this);
  BR.Register(useAfterRelease);
  
  releaseNotOwned = new BadRelease(this);
  BR.Register(releaseNotOwned);
  
  deallocGC = new DeallocGC(this);
  BR.Register(deallocGC);
  
  deallocNotOwned = new DeallocNotOwned(this);
  BR.Register(deallocNotOwned);
  
  overAutorelease = new OverAutorelease(this);
  BR.Register(overAutorelease);
  
  returnNotOwnedForOwned = new ReturnedNotOwnedForOwned(this);
  BR.Register(returnNotOwnedForOwned);
  
  // First register "return" leaks.
  const char* name = 0;
  
  if (isGCEnabled())
    name = "Leak of returned object when using garbage collection";
  else if (getLangOptions().getGCMode() == LangOptions::HybridGC)
    name = "Leak of returned object when not using garbage collection (GC) in "
    "dual GC/non-GC code";
  else {
    assert(getLangOptions().getGCMode() == LangOptions::NonGC);
    name = "Leak of returned object";
  }
  
  leakAtReturn = new LeakAtReturn(this, name);
  BR.Register(leakAtReturn);
  
  // Second, register leaks within a function/method.
  if (isGCEnabled())
    name = "Leak of object when using garbage collection";  
  else if (getLangOptions().getGCMode() == LangOptions::HybridGC)
    name = "Leak of object when not using garbage collection (GC) in "
    "dual GC/non-GC code";
  else {
    assert(getLangOptions().getGCMode() == LangOptions::NonGC);
    name = "Leak";
  }
  
  leakWithinFunction = new LeakWithinFunction(this, name);
  BR.Register(leakWithinFunction);
  
  // Save the reference to the BugReporter.
  this->BR = &BR;
}

static const char* Msgs[] = {
  // GC only
  "Code is compiled to only use garbage collection",    
  // No GC.
  "Code is compiled to use reference counts",
  // Hybrid, with GC.
  "Code is compiled to use either garbage collection (GC) or reference counts"
  " (non-GC).  The bug occurs with GC enabled",  
  // Hybrid, without GC
  "Code is compiled to use either garbage collection (GC) or reference counts"
  " (non-GC).  The bug occurs in non-GC mode"
};

std::pair<const char**,const char**> CFRefReport::getExtraDescriptiveText() {
  CFRefCount& TF = static_cast<CFRefBug&>(getBugType()).getTF();
  
  switch (TF.getLangOptions().getGCMode()) {
    default:
      assert(false);
      
    case LangOptions::GCOnly:
      assert (TF.isGCEnabled());
      return std::make_pair(&Msgs[0], &Msgs[0]+1);      
      
    case LangOptions::NonGC:
      assert (!TF.isGCEnabled());
      return std::make_pair(&Msgs[1], &Msgs[1]+1);
      
    case LangOptions::HybridGC:
      if (TF.isGCEnabled())
        return std::make_pair(&Msgs[2], &Msgs[2]+1);
      else
        return std::make_pair(&Msgs[3], &Msgs[3]+1);
  }
}

static inline bool contains(const llvm::SmallVectorImpl<ArgEffect>& V,
                            ArgEffect X) {
  for (llvm::SmallVectorImpl<ArgEffect>::const_iterator I=V.begin(), E=V.end();
       I!=E; ++I)
    if (*I == X) return true;
  
  return false;
}

PathDiagnosticPiece* CFRefReport::VisitNode(const ExplodedNode<GRState>* N,
                                            const ExplodedNode<GRState>* PrevN,
                                            BugReporterContext& BRC) {
  
  if (!isa<PostStmt>(N->getLocation()))
    return NULL;
  
  // Check if the type state has changed.
  GRStateManager &StMgr = BRC.getStateManager();
  GRStateRef PrevSt(PrevN->getState(), StMgr);
  GRStateRef CurrSt(N->getState(), StMgr);
  
  const RefVal* CurrT = CurrSt.get<RefBindings>(Sym);  
  if (!CurrT) return NULL;
  
  const RefVal& CurrV = *CurrT;
  const RefVal* PrevT = PrevSt.get<RefBindings>(Sym);
  
  // Create a string buffer to constain all the useful things we want
  // to tell the user.
  std::string sbuf;
  llvm::raw_string_ostream os(sbuf);
  
  // This is the allocation site since the previous node had no bindings
  // for this symbol.
  if (!PrevT) {
    Stmt* S = cast<PostStmt>(N->getLocation()).getStmt();
    
    if (CallExpr *CE = dyn_cast<CallExpr>(S)) {
      // Get the name of the callee (if it is available).
      SVal X = CurrSt.GetSValAsScalarOrLoc(CE->getCallee());
      if (const FunctionDecl* FD = X.getAsFunctionDecl())
        os << "Call to function '" << FD->getNameAsString() <<'\'';
      else
        os << "function call";      
    }          
    else {
      assert (isa<ObjCMessageExpr>(S));
      os << "Method";
    }
    
    if (CurrV.getObjKind() == RetEffect::CF) {
      os << " returns a Core Foundation object with a ";
    }
    else {
      assert (CurrV.getObjKind() == RetEffect::ObjC);
      os << " returns an Objective-C object with a ";
    }
    
    if (CurrV.isOwned()) {
      os << "+1 retain count (owning reference).";
      
      if (static_cast<CFRefBug&>(getBugType()).getTF().isGCEnabled()) {
        assert(CurrV.getObjKind() == RetEffect::CF);
        os << "  "
        "Core Foundation objects are not automatically garbage collected.";
      }
    }
    else {
      assert (CurrV.isNotOwned());
      os << "+0 retain count (non-owning reference).";
    }
    
    PathDiagnosticLocation Pos(S, BRC.getSourceManager());
    return new PathDiagnosticEventPiece(Pos, os.str());
  }
  
  // Gather up the effects that were performed on the object at this
  // program point
  llvm::SmallVector<ArgEffect, 2> AEffects;
  
  if (const RetainSummary *Summ =
        TF.getSummaryOfNode(BRC.getNodeResolver().getOriginalNode(N))) {
    // We only have summaries attached to nodes after evaluating CallExpr and
    // ObjCMessageExprs.
    Stmt* S = cast<PostStmt>(N->getLocation()).getStmt();
    
    if (CallExpr *CE = dyn_cast<CallExpr>(S)) {
      // Iterate through the parameter expressions and see if the symbol
      // was ever passed as an argument.
      unsigned i = 0;
      
      for (CallExpr::arg_iterator AI=CE->arg_begin(), AE=CE->arg_end();
           AI!=AE; ++AI, ++i) {
        
        // Retrieve the value of the argument.  Is it the symbol
        // we are interested in?
        if (CurrSt.GetSValAsScalarOrLoc(*AI).getAsLocSymbol() != Sym)
          continue;
        
        // We have an argument.  Get the effect!
        AEffects.push_back(Summ->getArg(i));
      }
    }
    else if (ObjCMessageExpr *ME = dyn_cast<ObjCMessageExpr>(S)) {      
      if (Expr *receiver = ME->getReceiver())
        if (CurrSt.GetSValAsScalarOrLoc(receiver).getAsLocSymbol() == Sym) {
          // The symbol we are tracking is the receiver.
          AEffects.push_back(Summ->getReceiverEffect());
        }
    }
  }
  
  do {
    // Get the previous type state.
    RefVal PrevV = *PrevT;
    
    // Specially handle -dealloc.
    if (!TF.isGCEnabled() && contains(AEffects, Dealloc)) {
      // Determine if the object's reference count was pushed to zero.
      assert(!(PrevV == CurrV) && "The typestate *must* have changed.");
      // We may not have transitioned to 'release' if we hit an error.
      // This case is handled elsewhere.
      if (CurrV.getKind() == RefVal::Released) {
        assert(CurrV.getCombinedCounts() == 0);
        os << "Object released by directly sending the '-dealloc' message";
        break;
      }
    }
    
    // Specially handle CFMakeCollectable and friends.
    if (contains(AEffects, MakeCollectable)) {
      // Get the name of the function.
      Stmt* S = cast<PostStmt>(N->getLocation()).getStmt();
      SVal X = CurrSt.GetSValAsScalarOrLoc(cast<CallExpr>(S)->getCallee());
      const FunctionDecl* FD = X.getAsFunctionDecl();
      const std::string& FName = FD->getNameAsString();
      
      if (TF.isGCEnabled()) {
        // Determine if the object's reference count was pushed to zero.
        assert(!(PrevV == CurrV) && "The typestate *must* have changed.");
        
        os << "In GC mode a call to '" << FName
        <<  "' decrements an object's retain count and registers the "
        "object with the garbage collector. ";
        
        if (CurrV.getKind() == RefVal::Released) {
          assert(CurrV.getCount() == 0);
          os << "Since it now has a 0 retain count the object can be "
          "automatically collected by the garbage collector.";
        }
        else
          os << "An object must have a 0 retain count to be garbage collected. "
          "After this call its retain count is +" << CurrV.getCount()
          << '.';
      }
      else 
        os << "When GC is not enabled a call to '" << FName
        << "' has no effect on its argument.";
      
      // Nothing more to say.
      break;
    }
    
    // Determine if the typestate has changed.  
    if (!(PrevV == CurrV))
      switch (CurrV.getKind()) {
        case RefVal::Owned:
        case RefVal::NotOwned:
          
          if (PrevV.getCount() == CurrV.getCount()) {
            // Did an autorelease message get sent?
            if (PrevV.getAutoreleaseCount() == CurrV.getAutoreleaseCount())
              return 0;
            
            assert(PrevV.getAutoreleaseCount() < CurrV.getAutoreleaseCount());
            os << "Object sent -autorelease message";
            break;
          }
          
          if (PrevV.getCount() > CurrV.getCount())
            os << "Reference count decremented.";
          else
            os << "Reference count incremented.";
          
          if (unsigned Count = CurrV.getCount())
            os << " The object now has a +" << Count << " retain count.";
          
          if (PrevV.getKind() == RefVal::Released) {
            assert(TF.isGCEnabled() && CurrV.getCount() > 0);
            os << " The object is not eligible for garbage collection until the "
            "retain count reaches 0 again.";
          }
          
          break;
          
        case RefVal::Released:
          os << "Object released.";
          break;
          
        case RefVal::ReturnedOwned:
          os << "Object returned to caller as an owning reference (single retain "
          "count transferred to caller).";
          break;
          
        case RefVal::ReturnedNotOwned:
          os << "Object returned to caller with a +0 (non-owning) retain count.";
          break;
          
        default:
          return NULL;
      }
    
    // Emit any remaining diagnostics for the argument effects (if any).
    for (llvm::SmallVectorImpl<ArgEffect>::iterator I=AEffects.begin(),
         E=AEffects.end(); I != E; ++I) {
      
      // A bunch of things have alternate behavior under GC.
      if (TF.isGCEnabled())
        switch (*I) {
          default: break;
          case Autorelease:
            os << "In GC mode an 'autorelease' has no effect.";
            continue;
          case IncRefMsg:
            os << "In GC mode the 'retain' message has no effect.";
            continue;
          case DecRefMsg:
            os << "In GC mode the 'release' message has no effect.";
            continue;
        }
    }
  } while(0);
  
  if (os.str().empty())
    return 0; // We have nothing to say!

  Stmt* S = cast<PostStmt>(N->getLocation()).getStmt();
  PathDiagnosticLocation Pos(S, BRC.getSourceManager());
  PathDiagnosticPiece* P = new PathDiagnosticEventPiece(Pos, os.str());
  
  // Add the range by scanning the children of the statement for any bindings
  // to Sym.
  for (Stmt::child_iterator I = S->child_begin(), E = S->child_end(); I!=E; ++I)
    if (Expr* Exp = dyn_cast_or_null<Expr>(*I))
      if (CurrSt.GetSValAsScalarOrLoc(Exp).getAsLocSymbol() == Sym) {
        P->addRange(Exp->getSourceRange());
        break;
      }
  
  return P;
}

namespace {
  class VISIBILITY_HIDDEN FindUniqueBinding :
  public StoreManager::BindingsHandler {
    SymbolRef Sym;
    const MemRegion* Binding;
    bool First;
    
  public:
    FindUniqueBinding(SymbolRef sym) : Sym(sym), Binding(0), First(true) {}
    
    bool HandleBinding(StoreManager& SMgr, Store store, const MemRegion* R,
                       SVal val) {
      
      SymbolRef SymV = val.getAsSymbol();    
      if (!SymV || SymV != Sym)
        return true;
      
      if (Binding) {
        First = false;
        return false;
      }
      else
        Binding = R;
      
      return true;    
    }
    
    operator bool() { return First && Binding; }
    const MemRegion* getRegion() { return Binding; }
  };  
}

static std::pair<const ExplodedNode<GRState>*,const MemRegion*>
GetAllocationSite(GRStateManager& StateMgr, const ExplodedNode<GRState>* N,
                  SymbolRef Sym) {
  
  // Find both first node that referred to the tracked symbol and the
  // memory location that value was store to.
  const ExplodedNode<GRState>* Last = N;
  const MemRegion* FirstBinding = 0;  
  
  while (N) {
    const GRState* St = N->getState();
    RefBindings B = St->get<RefBindings>();
    
    if (!B.lookup(Sym))
      break;
    
    FindUniqueBinding FB(Sym);
    StateMgr.iterBindings(St, FB);      
    if (FB) FirstBinding = FB.getRegion();      
    
    Last = N;
    N = N->pred_empty() ? NULL : *(N->pred_begin());    
  }
  
  return std::make_pair(Last, FirstBinding);
}

PathDiagnosticPiece*
CFRefReport::getEndPath(BugReporterContext& BRC,
                        const ExplodedNode<GRState>* EndN) {
  // Tell the BugReporterContext to report cases when the tracked symbol is
  // assigned to different variables, etc.
  BRC.addNotableSymbol(Sym);
  return RangedBugReport::getEndPath(BRC, EndN);
}

PathDiagnosticPiece*
CFRefLeakReport::getEndPath(BugReporterContext& BRC,
                            const ExplodedNode<GRState>* EndN){
  
  // Tell the BugReporterContext to report cases when the tracked symbol is
  // assigned to different variables, etc.
  BRC.addNotableSymbol(Sym);
  
  // We are reporting a leak.  Walk up the graph to get to the first node where
  // the symbol appeared, and also get the first VarDecl that tracked object
  // is stored to.
  const ExplodedNode<GRState>* AllocNode = 0;
  const MemRegion* FirstBinding = 0;
  
  llvm::tie(AllocNode, FirstBinding) =
    GetAllocationSite(BRC.getStateManager(), EndN, Sym);
  
  // Get the allocate site.  
  assert(AllocNode);
  Stmt* FirstStmt = cast<PostStmt>(AllocNode->getLocation()).getStmt();
  
  SourceManager& SMgr = BRC.getSourceManager();
  unsigned AllocLine =SMgr.getInstantiationLineNumber(FirstStmt->getLocStart());
  
  // Compute an actual location for the leak.  Sometimes a leak doesn't
  // occur at an actual statement (e.g., transition between blocks; end
  // of function) so we need to walk the graph and compute a real location.
  const ExplodedNode<GRState>* LeakN = EndN;
  PathDiagnosticLocation L;
  
  while (LeakN) {
    ProgramPoint P = LeakN->getLocation();
    
    if (const PostStmt *PS = dyn_cast<PostStmt>(&P)) {
      L = PathDiagnosticLocation(PS->getStmt()->getLocStart(), SMgr);
      break;
    }
    else if (const BlockEdge *BE = dyn_cast<BlockEdge>(&P)) {
      if (const Stmt* Term = BE->getSrc()->getTerminator()) {
        L = PathDiagnosticLocation(Term->getLocStart(), SMgr);
        break;
      }
    }
    
    LeakN = LeakN->succ_empty() ? 0 : *(LeakN->succ_begin());
  }
  
  if (!L.isValid()) {
    const Decl &D = BRC.getCodeDecl();
    L = PathDiagnosticLocation(D.getBodyRBrace(BRC.getASTContext()), SMgr);
  }
  
  std::string sbuf;
  llvm::raw_string_ostream os(sbuf);
  
  os << "Object allocated on line " << AllocLine;
  
  if (FirstBinding)
    os << " and stored into '" << FirstBinding->getString() << '\'';  
  
  // Get the retain count.
  const RefVal* RV = EndN->getState()->get<RefBindings>(Sym);
  
  if (RV->getKind() == RefVal::ErrorLeakReturned) {
    // FIXME: Per comments in rdar://6320065, "create" only applies to CF
    // ojbects.  Only "copy", "alloc", "retain" and "new" transfer ownership
    // to the caller for NS objects.
    ObjCMethodDecl& MD = cast<ObjCMethodDecl>(BRC.getCodeDecl());
    os << " is returned from a method whose name ('"
       << MD.getSelector().getAsString()
    << "') does not contain 'copy' or otherwise starts with"
    " 'new' or 'alloc'.  This violates the naming convention rules given"
    " in the Memory Management Guide for Cocoa (object leaked)";
  }
  else if (RV->getKind() == RefVal::ErrorGCLeakReturned) {
    ObjCMethodDecl& MD = cast<ObjCMethodDecl>(BRC.getCodeDecl());
    os << " and returned from method '" << MD.getSelector().getAsString()
       << "' is potentially leaked when using garbage collection.  Callers "
          "of this method do not expect a returned object with a +1 retain "
          "count since they expect the object to be managed by the garbage "
          "collector";
  }
  else
    os << " is no longer referenced after this point and has a retain count of"
          " +" << RV->getCount() << " (object leaked)";
  
  return new PathDiagnosticEventPiece(L, os.str());
}

CFRefLeakReport::CFRefLeakReport(CFRefBug& D, const CFRefCount &tf,
                                 ExplodedNode<GRState> *n,
                                 SymbolRef sym, GRExprEngine& Eng)
: CFRefReport(D, tf, n, sym)
{
  
  // Most bug reports are cached at the location where they occured.
  // With leaks, we want to unique them by the location where they were
  // allocated, and only report a single path.  To do this, we need to find
  // the allocation site of a piece of tracked memory, which we do via a
  // call to GetAllocationSite.  This will walk the ExplodedGraph backwards.
  // Note that this is *not* the trimmed graph; we are guaranteed, however,
  // that all ancestor nodes that represent the allocation site have the
  // same SourceLocation.
  const ExplodedNode<GRState>* AllocNode = 0;
  
  llvm::tie(AllocNode, AllocBinding) =  // Set AllocBinding.
    GetAllocationSite(Eng.getStateManager(), getEndNode(), getSymbol());
  
  // Get the SourceLocation for the allocation site.
  ProgramPoint P = AllocNode->getLocation();
  AllocSite = cast<PostStmt>(P).getStmt()->getLocStart();
  
  // Fill in the description of the bug.
  Description.clear();
  llvm::raw_string_ostream os(Description);
  SourceManager& SMgr = Eng.getContext().getSourceManager();
  unsigned AllocLine = SMgr.getInstantiationLineNumber(AllocSite);
  os << "Potential leak ";
  if (tf.isGCEnabled()) {
    os << "(when using garbage collection) ";
  }  
  os << "of an object allocated on line " << AllocLine;
  
  // FIXME: AllocBinding doesn't get populated for RegionStore yet.
  if (AllocBinding)
    os << " and stored into '" << AllocBinding->getString() << '\'';
}

//===----------------------------------------------------------------------===//
// Main checker logic.
//===----------------------------------------------------------------------===//

/// GetReturnType - Used to get the return type of a message expression or
///  function call with the intention of affixing that type to a tracked symbol.
///  While the the return type can be queried directly from RetEx, when
///  invoking class methods we augment to the return type to be that of
///  a pointer to the class (as opposed it just being id).
static QualType GetReturnType(Expr* RetE, ASTContext& Ctx) {

  QualType RetTy = RetE->getType();

  // FIXME: We aren't handling id<...>.
  const PointerType* PT = RetTy->getAsPointerType();
  if (!PT)
    return RetTy;
    
  // If RetEx is not a message expression just return its type.
  // If RetEx is a message expression, return its types if it is something
  /// more specific than id.
  
  ObjCMessageExpr* ME = dyn_cast<ObjCMessageExpr>(RetE);
  
  if (!ME || !Ctx.isObjCIdStructType(PT->getPointeeType()))
    return RetTy;
  
  ObjCInterfaceDecl* D = ME->getClassInfo().first;  

  // At this point we know the return type of the message expression is id.
  // If we have an ObjCInterceDecl, we know this is a call to a class method
  // whose type we can resolve.  In such cases, promote the return type to
  // Class*.  
  return !D ? RetTy : Ctx.getPointerType(Ctx.getObjCInterfaceType(D));
}


void CFRefCount::EvalSummary(ExplodedNodeSet<GRState>& Dst,
                             GRExprEngine& Eng,
                             GRStmtNodeBuilder<GRState>& Builder,
                             Expr* Ex,
                             Expr* Receiver,
                             const RetainSummary& Summ,
                             ExprIterator arg_beg, ExprIterator arg_end,
                             ExplodedNode<GRState>* Pred) {
  
  // Get the state.
  GRStateManager& StateMgr = Eng.getStateManager();
  GRStateRef state(Builder.GetState(Pred), StateMgr);
  ASTContext& Ctx = StateMgr.getContext();
  ValueManager &ValMgr = Eng.getValueManager();

  // Evaluate the effect of the arguments.
  RefVal::Kind hasErr = (RefVal::Kind) 0;
  unsigned idx = 0;
  Expr* ErrorExpr = NULL;
  SymbolRef ErrorSym = 0;                                        
  
  for (ExprIterator I = arg_beg; I != arg_end; ++I, ++idx) {    
    SVal V = state.GetSValAsScalarOrLoc(*I);    
    SymbolRef Sym = V.getAsLocSymbol();

    if (Sym)
      if (RefBindings::data_type* T = state.get<RefBindings>(Sym)) {
        state = Update(state, Sym, *T, Summ.getArg(idx), hasErr);
        if (hasErr) {
          ErrorExpr = *I;
          ErrorSym = Sym;
          break;
        }        
        continue;
      }

    if (isa<Loc>(V)) {
      if (loc::MemRegionVal* MR = dyn_cast<loc::MemRegionVal>(&V)) {
        if (Summ.getArg(idx) == DoNothingByRef)
          continue;
        
        // Invalidate the value of the variable passed by reference.
        
        // FIXME: Either this logic should also be replicated in GRSimpleVals
        //  or should be pulled into a separate "constraint engine."
        
        // FIXME: We can have collisions on the conjured symbol if the
        //  expression *I also creates conjured symbols.  We probably want
        //  to identify conjured symbols by an expression pair: the enclosing
        //  expression (the context) and the expression itself.  This should
        //  disambiguate conjured symbols. 
        
        const TypedRegion* R = dyn_cast<TypedRegion>(MR->getRegion());

        if (R) {
          // Are we dealing with an ElementRegion?  If the element type is
          // a basic integer type (e.g., char, int) and the underying region
          // is a variable region then strip off the ElementRegion.
          // FIXME: We really need to think about this for the general case
          //   as sometimes we are reasoning about arrays and other times
          //   about (char*), etc., is just a form of passing raw bytes.
          //   e.g., void *p = alloca(); foo((char*)p);
          if (const ElementRegion *ER = dyn_cast<ElementRegion>(R)) {
            // Checking for 'integral type' is probably too promiscuous, but
            // we'll leave it in for now until we have a systematic way of
            // handling all of these cases.  Eventually we need to come up
            // with an interface to StoreManager so that this logic can be
            // approriately delegated to the respective StoreManagers while
            // still allowing us to do checker-specific logic (e.g.,
            // invalidating reference counts), probably via callbacks.
            if (ER->getElementType()->isIntegralType()) {
              const MemRegion *superReg = ER->getSuperRegion();
              if (isa<VarRegion>(superReg) || isa<FieldRegion>(superReg) ||
                  isa<ObjCIvarRegion>(superReg))
                R = cast<TypedRegion>(superReg);
            }

            // FIXME: What about layers of ElementRegions?
          }
          
          // Is the invalidated variable something that we were tracking?
          SymbolRef Sym = state.GetSValAsScalarOrLoc(R).getAsLocSymbol();
          
          // Remove any existing reference-count binding.
          if (Sym) state = state.remove<RefBindings>(Sym);
          
          if (R->isBoundable(Ctx)) {
            // Set the value of the variable to be a conjured symbol.
            unsigned Count = Builder.getCurrentBlockCount();
            QualType T = R->getValueType(Ctx);
          
            if (Loc::IsLocType(T) || (T->isIntegerType() && T->isScalarType())){
              ValueManager &ValMgr = Eng.getValueManager();
              SVal V = ValMgr.getConjuredSymbolVal(*I, T, Count);
              state = state.BindLoc(Loc::MakeVal(R), V);
            }
            else if (const RecordType *RT = T->getAsStructureType()) {
              // Handle structs in a not so awesome way.  Here we just
              // eagerly bind new symbols to the fields.  In reality we
              // should have the store manager handle this.  The idea is just
              // to prototype some basic functionality here.  All of this logic
              // should one day soon just go away.
              const RecordDecl *RD = RT->getDecl()->getDefinition(Ctx);
              
              // No record definition.  There is nothing we can do.
              if (!RD)
                continue;
              
              MemRegionManager &MRMgr = state.getManager().getRegionManager();
              
              // Iterate through the fields and construct new symbols.
              for (RecordDecl::field_iterator FI=RD->field_begin(Ctx),
                   FE=RD->field_end(Ctx); FI!=FE; ++FI) {
                
                // For now just handle scalar fields.
                FieldDecl *FD = *FI;
                QualType FT = FD->getType();
                
                if (Loc::IsLocType(FT) || 
                    (FT->isIntegerType() && FT->isScalarType())) {
                  const FieldRegion* FR = MRMgr.getFieldRegion(FD, R);

                  SVal V = ValMgr.getConjuredSymbolVal(*I, FT, Count);
                  state = state.BindLoc(Loc::MakeVal(FR), V);
                }                
              }
            } else if (const ArrayType *AT = Ctx.getAsArrayType(T)) {
              // Set the default value of the array to conjured symbol.
              StoreManager& StoreMgr = Eng.getStateManager().getStoreManager();
              SVal V = ValMgr.getConjuredSymbolVal(*I, AT->getElementType(),
                                                   Count);
              state = GRStateRef(StoreMgr.setDefaultValue(state, R, V), 
                                 StateMgr);
            } else {
              // Just blast away other values.
              state = state.BindLoc(*MR, UnknownVal());
            }
          }
        }
        else
          state = state.BindLoc(*MR, UnknownVal());
      }
      else {
        // Nuke all other arguments passed by reference.
        state = state.Unbind(cast<Loc>(V));
      }
    }
    else if (isa<nonloc::LocAsInteger>(V))
      state = state.Unbind(cast<nonloc::LocAsInteger>(V).getLoc());
  } 
  
  // Evaluate the effect on the message receiver.  
  if (!ErrorExpr && Receiver) {
    SymbolRef Sym = state.GetSValAsScalarOrLoc(Receiver).getAsLocSymbol();
    if (Sym) {
      if (const RefVal* T = state.get<RefBindings>(Sym)) {
        state = Update(state, Sym, *T, Summ.getReceiverEffect(), hasErr);
        if (hasErr) {
          ErrorExpr = Receiver;
          ErrorSym = Sym;
        }
      }
    }
  }
  
  // Process any errors.  
  if (hasErr) {
    ProcessNonLeakError(Dst, Builder, Ex, ErrorExpr, Pred, state,
                        hasErr, ErrorSym);
    return;
  }
  
  // Consult the summary for the return value.  
  RetEffect RE = Summ.getRetEffect();
  
  if (RE.getKind() == RetEffect::OwnedWhenTrackedReceiver) {
    assert(Receiver);
    SVal V = state.GetSValAsScalarOrLoc(Receiver);
    bool found = false;
    if (SymbolRef Sym = V.getAsLocSymbol())
      if (state.get<RefBindings>(Sym)) {
        found = true;
        RE = Summaries.getObjAllocRetEffect();
      }

    if (!found)
      RE = RetEffect::MakeNoRet();
  } 
  
  switch (RE.getKind()) {
    default:
      assert (false && "Unhandled RetEffect."); break;
      
    case RetEffect::NoRet: {
      
      // Make up a symbol for the return value (not reference counted).
      // FIXME: This is basically copy-and-paste from GRSimpleVals.  We 
      //  should compose behavior, not copy it.
      
      // FIXME: We eventually should handle structs and other compound types
      // that are returned by value.
      
      QualType T = Ex->getType();
      
      if (Loc::IsLocType(T) || (T->isIntegerType() && T->isScalarType())) {
        unsigned Count = Builder.getCurrentBlockCount();
        ValueManager &ValMgr = Eng.getValueManager();
        SVal X = ValMgr.getConjuredSymbolVal(Ex, T, Count);
        state = state.BindExpr(Ex, X, false);
      }      
      
      break;
    }
      
    case RetEffect::Alias: {
      unsigned idx = RE.getIndex();
      assert (arg_end >= arg_beg);
      assert (idx < (unsigned) (arg_end - arg_beg));
      SVal V = state.GetSValAsScalarOrLoc(*(arg_beg+idx));
      state = state.BindExpr(Ex, V, false);
      break;
    }
      
    case RetEffect::ReceiverAlias: {
      assert (Receiver);
      SVal V = state.GetSValAsScalarOrLoc(Receiver);
      state = state.BindExpr(Ex, V, false);
      break;
    }
      
    case RetEffect::OwnedAllocatedSymbol:
    case RetEffect::OwnedSymbol: {
      unsigned Count = Builder.getCurrentBlockCount();
      ValueManager &ValMgr = Eng.getValueManager();      
      SymbolRef Sym = ValMgr.getConjuredSymbol(Ex, Count);
      QualType RetT = GetReturnType(Ex, ValMgr.getContext());      
      state = state.set<RefBindings>(Sym, RefVal::makeOwned(RE.getObjKind(),
                                                            RetT));
      state = state.BindExpr(Ex, ValMgr.makeRegionVal(Sym), false);

      // FIXME: Add a flag to the checker where allocations are assumed to
      // *not fail.
#if 0
      if (RE.getKind() == RetEffect::OwnedAllocatedSymbol) {
        bool isFeasible;
        state = state.Assume(loc::SymbolVal(Sym), true, isFeasible);
        assert(isFeasible && "Cannot assume fresh symbol is non-null.");        
      }
#endif
      
      break;
    }
    
    case RetEffect::GCNotOwnedSymbol:
    case RetEffect::NotOwnedSymbol: {
      unsigned Count = Builder.getCurrentBlockCount();
      ValueManager &ValMgr = Eng.getValueManager();
      SymbolRef Sym = ValMgr.getConjuredSymbol(Ex, Count);
      QualType RetT = GetReturnType(Ex, ValMgr.getContext());      
      state = state.set<RefBindings>(Sym, RefVal::makeNotOwned(RE.getObjKind(),
                                                               RetT));
      state = state.BindExpr(Ex, ValMgr.makeRegionVal(Sym), false);
      break;
    }
  }
  
  // Generate a sink node if we are at the end of a path.
  GRExprEngine::NodeTy *NewNode =
    Summ.isEndPath() ? Builder.MakeSinkNode(Dst, Ex, Pred, state)
                     : Builder.MakeNode(Dst, Ex, Pred, state);
  
  // Annotate the edge with summary we used.
  if (NewNode) SummaryLog[NewNode] = &Summ;
}


void CFRefCount::EvalCall(ExplodedNodeSet<GRState>& Dst,
                          GRExprEngine& Eng,
                          GRStmtNodeBuilder<GRState>& Builder,
                          CallExpr* CE, SVal L,
                          ExplodedNode<GRState>* Pred) {
  const FunctionDecl* FD = L.getAsFunctionDecl();
  RetainSummary* Summ = !FD ? Summaries.getDefaultSummary() 
                        : Summaries.getSummary(const_cast<FunctionDecl*>(FD));
  
  assert(Summ);
  EvalSummary(Dst, Eng, Builder, CE, 0, *Summ,
              CE->arg_begin(), CE->arg_end(), Pred);
}

void CFRefCount::EvalObjCMessageExpr(ExplodedNodeSet<GRState>& Dst,
                                     GRExprEngine& Eng,
                                     GRStmtNodeBuilder<GRState>& Builder,
                                     ObjCMessageExpr* ME,
                                     ExplodedNode<GRState>* Pred) {  
  RetainSummary* Summ = 0;
  
  if (Expr* Receiver = ME->getReceiver()) {
    // We need the type-information of the tracked receiver object
    // Retrieve it from the state.
    const ObjCInterfaceDecl* ID = 0;

    // FIXME: Wouldn't it be great if this code could be reduced?  It's just
    // a chain of lookups.
    // FIXME: Is this really working as expected?  There are cases where
    //  we just use the 'ID' from the message expression.
    const GRState* St = Builder.GetState(Pred);
    SVal V = Eng.getStateManager().GetSValAsScalarOrLoc(St, Receiver);

    SymbolRef Sym = V.getAsLocSymbol();
    if (Sym) {
      if (const RefVal* T  = St->get<RefBindings>(Sym)) {
        QualType Ty = T->getType();
        
        if (const PointerType* PT = Ty->getAsPointerType()) {
          QualType PointeeTy = PT->getPointeeType();
          
          if (ObjCInterfaceType* IT = dyn_cast<ObjCInterfaceType>(PointeeTy))
            ID = IT->getDecl();
        }
      }
    }

    // FIXME: this is a hack.  This may or may not be the actual method
    //  that is called.
    if (!ID) {
      if (const PointerType *PT = Receiver->getType()->getAsPointerType())
        if (const ObjCInterfaceType *p =
            PT->getPointeeType()->getAsObjCInterfaceType())
          ID = p->getDecl();
    }

    // FIXME: The receiver could be a reference to a class, meaning that
    //  we should use the class method.
    Summ = Summaries.getInstanceMethodSummary(ME, ID);

    // Special-case: are we sending a mesage to "self"?
    //  This is a hack.  When we have full-IP this should be removed.
    if (isa<ObjCMethodDecl>(&Eng.getGraph().getCodeDecl())) {      
      if (Expr* Receiver = ME->getReceiver()) {
        SVal X = Eng.getStateManager().GetSValAsScalarOrLoc(St, Receiver);
        if (loc::MemRegionVal* L = dyn_cast<loc::MemRegionVal>(&X))
          if (L->getRegion() == Eng.getStateManager().getSelfRegion(St)) {
            // Update the summary to make the default argument effect
            // 'StopTracking'.
            Summ = Summaries.copySummary(Summ);
            Summ->setDefaultArgEffect(StopTracking);
          }
      }
    }
  }
  else
    Summ = Summaries.getClassMethodSummary(ME);

  if (!Summ)
    Summ = Summaries.getDefaultSummary();

  EvalSummary(Dst, Eng, Builder, ME, ME->getReceiver(), *Summ,
              ME->arg_begin(), ME->arg_end(), Pred);
}

namespace {
class VISIBILITY_HIDDEN StopTrackingCallback : public SymbolVisitor {
  GRStateRef state;
public:
  StopTrackingCallback(GRStateRef st) : state(st) {}
  GRStateRef getState() { return state; }

  bool VisitSymbol(SymbolRef sym) {
    state = state.remove<RefBindings>(sym);
    return true;
  }
  
  const GRState* getState() const { return state.getState(); }
};
} // end anonymous namespace
  

void CFRefCount::EvalBind(GRStmtNodeBuilderRef& B, SVal location, SVal val) {  
  // Are we storing to something that causes the value to "escape"?  
  bool escapes = false;
  
  // A value escapes in three possible cases (this may change):
  //
  // (1) we are binding to something that is not a memory region.
  // (2) we are binding to a memregion that does not have stack storage
  // (3) we are binding to a memregion with stack storage that the store
  //     does not understand.  
  GRStateRef state = B.getState();

  if (!isa<loc::MemRegionVal>(location))
    escapes = true;
  else {
    const MemRegion* R = cast<loc::MemRegionVal>(location).getRegion();
    escapes = !B.getStateManager().hasStackStorage(R);
    
    if (!escapes) {
      // To test (3), generate a new state with the binding removed.  If it is
      // the same state, then it escapes (since the store cannot represent
      // the binding).
      escapes = (state == (state.BindLoc(cast<Loc>(location), UnknownVal())));
    }
  }

  // If our store can represent the binding and we aren't storing to something
  // that doesn't have local storage then just return and have the simulation
  // state continue as is.
  if (!escapes)
      return;

  // Otherwise, find all symbols referenced by 'val' that we are tracking
  // and stop tracking them.
  B.MakeNode(state.scanReachableSymbols<StopTrackingCallback>(val).getState());
}


 // Return statements.

void CFRefCount::EvalReturn(ExplodedNodeSet<GRState>& Dst,
                            GRExprEngine& Eng,
                            GRStmtNodeBuilder<GRState>& Builder,
                            ReturnStmt* S,
                            ExplodedNode<GRState>* Pred) {
  
  Expr* RetE = S->getRetValue();
  if (!RetE)
    return;
  
  GRStateRef state(Builder.GetState(Pred), Eng.getStateManager());
  SymbolRef Sym = state.GetSValAsScalarOrLoc(RetE).getAsLocSymbol();
  
  if (!Sym)
    return;
  
  // Get the reference count binding (if any).
  const RefVal* T = state.get<RefBindings>(Sym);
  
  if (!T)
    return;
  
  // Change the reference count.  
  RefVal X = *T;  
  
  switch (X.getKind()) { 
    case RefVal::Owned: { 
      unsigned cnt = X.getCount();
      assert (cnt > 0);
      X.setCount(cnt - 1);
      X = X ^ RefVal::ReturnedOwned;
      break;
    }
      
    case RefVal::NotOwned: {
      unsigned cnt = X.getCount();
      if (cnt) {
        X.setCount(cnt - 1);
        X = X ^ RefVal::ReturnedOwned;
      }
      else {
        X = X ^ RefVal::ReturnedNotOwned;
      }
      break;
    }
      
    default: 
      return;
  }
  
  // Update the binding.
  state = state.set<RefBindings>(Sym, X);
  Pred = Builder.MakeNode(Dst, S, Pred, state);
  
  // Did we cache out?
  if (!Pred)
    return;
  
  // Update the autorelease counts.
  static unsigned autoreleasetag = 0;
  GenericNodeBuilder Bd(Builder, S, &autoreleasetag);
  bool stop = false;
  llvm::tie(Pred, state) = HandleAutoreleaseCounts(state , Bd, Pred, Eng, Sym,
                                                   X, stop);
  
  // Did we cache out?
  if (!Pred || stop)
    return;
  
  // Get the updated binding.
  T = state.get<RefBindings>(Sym);
  assert(T);
  X = *T;
    
  // Any leaks or other errors?
  if (X.isReturnedOwned() && X.getCount() == 0) {
    const Decl *CD = &Eng.getStateManager().getCodeDecl();    
    if (const ObjCMethodDecl* MD = dyn_cast<ObjCMethodDecl>(CD)) {      
      const RetainSummary &Summ = *Summaries.getMethodSummary(MD);
      RetEffect RE = Summ.getRetEffect();
      bool hasError = false;

      if (RE.getKind() != RetEffect::NoRet) {
        if (isGCEnabled() && RE.getObjKind() == RetEffect::ObjC) {
          // Things are more complicated with garbage collection.  If the
          // returned object is suppose to be an Objective-C object, we have
          // a leak (as the caller expects a GC'ed object) because no
          // method should return ownership unless it returns a CF object.
          X = X ^ RefVal::ErrorGCLeakReturned;
          
          // Keep this false until this is properly tested.
          hasError = true;
        }
        else if (!RE.isOwned()) {
          // Either we are using GC and the returned object is a CF type
          // or we aren't using GC.  In either case, we expect that the
          // enclosing method is expected to return ownership.        
          hasError = true;
          X = X ^ RefVal::ErrorLeakReturned;
        }
      }
      
      if (hasError) {        
        // Generate an error node.
        static int ReturnOwnLeakTag = 0;
        state = state.set<RefBindings>(Sym, X);
        ExplodedNode<GRState> *N =
          Builder.generateNode(PostStmt(S, &ReturnOwnLeakTag), state, Pred);
        if (N) {
          CFRefReport *report =
            new CFRefLeakReport(*static_cast<CFRefBug*>(leakAtReturn), *this,
                                N, Sym, Eng);
          BR->EmitReport(report);
        }
      }
    } 
  }
  else if (X.isReturnedNotOwned()) {
    const Decl *CD = &Eng.getStateManager().getCodeDecl();    
    if (const ObjCMethodDecl* MD = dyn_cast<ObjCMethodDecl>(CD)) {
      const RetainSummary &Summ = *Summaries.getMethodSummary(MD);
      if (Summ.getRetEffect().isOwned()) {
        // Trying to return a not owned object to a caller expecting an
        // owned object.
        
        static int ReturnNotOwnedForOwnedTag = 0;
        state = state.set<RefBindings>(Sym, X ^ RefVal::ErrorReturnedNotOwned);
        if (ExplodedNode<GRState> *N =
              Builder.generateNode(PostStmt(S, &ReturnNotOwnedForOwnedTag),
                                   state, Pred)) {
            CFRefReport *report =
                new CFRefReport(*static_cast<CFRefBug*>(returnNotOwnedForOwned),
                                *this, N, Sym);
            BR->EmitReport(report);
        }
      }
    }
  }
}

// Assumptions.

const GRState* CFRefCount::EvalAssume(GRStateManager& VMgr,
                                         const GRState* St,
                                         SVal Cond, bool Assumption,
                                         bool& isFeasible) {

  // FIXME: We may add to the interface of EvalAssume the list of symbols
  //  whose assumptions have changed.  For now we just iterate through the
  //  bindings and check if any of the tracked symbols are NULL.  This isn't
  //  too bad since the number of symbols we will track in practice are 
  //  probably small and EvalAssume is only called at branches and a few
  //  other places.
  RefBindings B = St->get<RefBindings>();
  
  if (B.isEmpty())
    return St;
  
  bool changed = false;
  
  GRStateRef state(St, VMgr);
  RefBindings::Factory& RefBFactory = state.get_context<RefBindings>();

  for (RefBindings::iterator I=B.begin(), E=B.end(); I!=E; ++I) {    
    // Check if the symbol is null (or equal to any constant).
    // If this is the case, stop tracking the symbol.
    if (VMgr.getSymVal(St, I.getKey())) {
      changed = true;
      B = RefBFactory.Remove(B, I.getKey());
    }
  }
  
  if (changed)
    state = state.set<RefBindings>(B);
  
  return state;
}

GRStateRef CFRefCount::Update(GRStateRef state, SymbolRef sym,
                              RefVal V, ArgEffect E,
                              RefVal::Kind& hasErr) {

  // In GC mode [... release] and [... retain] do nothing.
  switch (E) {
    default: break;
    case IncRefMsg: E = isGCEnabled() ? DoNothing : IncRef; break;
    case DecRefMsg: E = isGCEnabled() ? DoNothing : DecRef; break;
    case MakeCollectable: E = isGCEnabled() ? DecRef : DoNothing; break;
    case NewAutoreleasePool: E = isGCEnabled() ? DoNothing : 
                                                 NewAutoreleasePool; break;
  }
  
  // Handle all use-after-releases.
  if (!isGCEnabled() && V.getKind() == RefVal::Released) {
    V = V ^ RefVal::ErrorUseAfterRelease;
    hasErr = V.getKind();
    return state.set<RefBindings>(sym, V);
  }      
  
  switch (E) {
    default:
      assert (false && "Unhandled CFRef transition.");
      
    case Dealloc:
      // Any use of -dealloc in GC is *bad*.
      if (isGCEnabled()) {
        V = V ^ RefVal::ErrorDeallocGC;
        hasErr = V.getKind();
        break;
      }
      
      switch (V.getKind()) {
        default:
          assert(false && "Invalid case.");
        case RefVal::Owned:
          // The object immediately transitions to the released state.
          V = V ^ RefVal::Released;
          V.clearCounts();
          return state.set<RefBindings>(sym, V);
        case RefVal::NotOwned:
          V = V ^ RefVal::ErrorDeallocNotOwned;
          hasErr = V.getKind();
          break;
      }      
      break;

    case NewAutoreleasePool:
      assert(!isGCEnabled());
      return state.add<AutoreleaseStack>(sym);
      
    case MayEscape:
      if (V.getKind() == RefVal::Owned) {
        V = V ^ RefVal::NotOwned;
        break;
      }

      // Fall-through.
      
    case DoNothingByRef:
    case DoNothing:
      return state;

    case Autorelease:
      if (isGCEnabled())
        return state;
      
      // Update the autorelease counts.
      state = SendAutorelease(state, ARCountFactory, sym);
      V = V.autorelease();
      break;

    case StopTracking:
      return state.remove<RefBindings>(sym);

    case IncRef:      
      switch (V.getKind()) {
        default:
          assert(false);

        case RefVal::Owned:
        case RefVal::NotOwned:
          V = V + 1;
          break;          
        case RefVal::Released:
          // Non-GC cases are handled above.
          assert(isGCEnabled());
          V = (V ^ RefVal::Owned) + 1;
          break;
      }      
      break;
      
    case SelfOwn:
      V = V ^ RefVal::NotOwned;
      // Fall-through.
    case DecRef:
      switch (V.getKind()) {
        default:
          // case 'RefVal::Released' handled above.
          assert (false);

        case RefVal::Owned:
          assert(V.getCount() > 0);
          if (V.getCount() == 1) V = V ^ RefVal::Released;
          V = V - 1;
          break;
          
        case RefVal::NotOwned:
          if (V.getCount() > 0)
            V = V - 1;
          else {
            V = V ^ RefVal::ErrorReleaseNotOwned;
            hasErr = V.getKind();
          }          
          break;
          
        case RefVal::Released:
          // Non-GC cases are handled above.
          assert(isGCEnabled());
          V = V ^ RefVal::ErrorUseAfterRelease;
          hasErr = V.getKind();
          break;  
      }      
      break;
  }
  return state.set<RefBindings>(sym, V);
}

//===----------------------------------------------------------------------===//
// Handle dead symbols and end-of-path.
//===----------------------------------------------------------------------===//

std::pair<ExplodedNode<GRState>*, GRStateRef>
CFRefCount::HandleAutoreleaseCounts(GRStateRef state, GenericNodeBuilder Bd,
                                    ExplodedNode<GRState>* Pred,
                                    GRExprEngine &Eng,
                                    SymbolRef Sym, RefVal V, bool &stop) {
 
  unsigned ACnt = V.getAutoreleaseCount();
  stop = false;

  // No autorelease counts?  Nothing to be done.
  if (!ACnt)
    return std::make_pair(Pred, state);
  
  assert(!isGCEnabled() && "Autorelease counts in GC mode?");  
  unsigned Cnt = V.getCount();
  
  // FIXME: Handle sending 'autorelease' to already released object.

  if (V.getKind() == RefVal::ReturnedOwned)
    ++Cnt;
  
  if (ACnt <= Cnt) {
    if (ACnt == Cnt) {
      V.clearCounts();
      if (V.getKind() == RefVal::ReturnedOwned)
        V = V ^ RefVal::ReturnedNotOwned;
      else
        V = V ^ RefVal::NotOwned;
    }
    else {
      V.setCount(Cnt - ACnt);
      V.setAutoreleaseCount(0);
    }
    state = state.set<RefBindings>(Sym, V);
    ExplodedNode<GRState> *N = Bd.MakeNode(state, Pred);
    stop = (N == 0);
    return std::make_pair(N, state);
  }    

  // Woah!  More autorelease counts then retain counts left.
  // Emit hard error.
  stop = true;
  V = V ^ RefVal::ErrorOverAutorelease;
  state = state.set<RefBindings>(Sym, V);

  if (ExplodedNode<GRState> *N = Bd.MakeNode(state, Pred)) {
    N->markAsSink();
    
    std::string sbuf;
    llvm::raw_string_ostream os(sbuf);
    os << "Object over-autoreleased: object was sent -autorelease";
    if (V.getAutoreleaseCount() > 1)
      os << V.getAutoreleaseCount() << " times";
    os << " but the object has ";
    if (V.getCount() == 0)
      os << "zero (locally visible)";
    else
      os << "+" << V.getCount();
    os << " retain counts";
    
    CFRefReport *report =
      new CFRefReport(*static_cast<CFRefBug*>(overAutorelease),
                      *this, N, Sym, os.str().c_str());
    BR->EmitReport(report);
  }
  
  return std::make_pair((ExplodedNode<GRState>*)0, state);
}

GRStateRef
CFRefCount::HandleSymbolDeath(GRStateRef state, SymbolRef sid, RefVal V,
                              llvm::SmallVectorImpl<SymbolRef> &Leaked) {
  
  bool hasLeak = V.isOwned() || 
  ((V.isNotOwned() || V.isReturnedOwned()) && V.getCount() > 0);
  
  if (!hasLeak)
    return state.remove<RefBindings>(sid);
  
  Leaked.push_back(sid);
  return state.set<RefBindings>(sid, V ^ RefVal::ErrorLeak);
}

ExplodedNode<GRState>*
CFRefCount::ProcessLeaks(GRStateRef state,
                         llvm::SmallVectorImpl<SymbolRef> &Leaked,
                         GenericNodeBuilder &Builder,
                         GRExprEngine& Eng,
                         ExplodedNode<GRState> *Pred) {
  
  if (Leaked.empty())
    return Pred;
  
  // Generate an intermediate node representing the leak point.
  ExplodedNode<GRState> *N = Builder.MakeNode(state, Pred);
  
  if (N) {
    for (llvm::SmallVectorImpl<SymbolRef>::iterator
         I = Leaked.begin(), E = Leaked.end(); I != E; ++I) {
      
      CFRefBug *BT = static_cast<CFRefBug*>(Pred ? leakWithinFunction 
                                                 : leakAtReturn);
      assert(BT && "BugType not initialized.");
      CFRefLeakReport* report = new CFRefLeakReport(*BT, *this, N, *I, Eng);
      BR->EmitReport(report);
    }
  }
  
  return N;
}

void CFRefCount::EvalEndPath(GRExprEngine& Eng,
                             GREndPathNodeBuilder<GRState>& Builder) {
  
  GRStateRef state(Builder.getState(), Eng.getStateManager());
  GenericNodeBuilder Bd(Builder);
  RefBindings B = state.get<RefBindings>();  
  ExplodedNode<GRState> *Pred = 0;

  for (RefBindings::iterator I = B.begin(), E = B.end(); I != E; ++I) {
    bool stop = false;
    llvm::tie(Pred, state) = HandleAutoreleaseCounts(state, Bd, Pred, Eng,
                                                     (*I).first,
                                                     (*I).second, stop);   

    if (stop)
      return;
  }
  
  B = state.get<RefBindings>();  
  llvm::SmallVector<SymbolRef, 10> Leaked;  
  
  for (RefBindings::iterator I = B.begin(), E = B.end(); I != E; ++I)
    state = HandleSymbolDeath(state, (*I).first, (*I).second, Leaked);

  ProcessLeaks(state, Leaked, Bd, Eng, Pred);
}

void CFRefCount::EvalDeadSymbols(ExplodedNodeSet<GRState>& Dst,
                                 GRExprEngine& Eng,
                                 GRStmtNodeBuilder<GRState>& Builder,
                                 ExplodedNode<GRState>* Pred,
                                 Stmt* S,
                                 const GRState* St,
                                 SymbolReaper& SymReaper) {

  GRStateRef state(St, Eng.getStateManager());
  RefBindings B = state.get<RefBindings>();
  
  // Update counts from autorelease pools
  for (SymbolReaper::dead_iterator I = SymReaper.dead_begin(),
       E = SymReaper.dead_end(); I != E; ++I) {
    SymbolRef Sym = *I;
    if (const RefVal* T = B.lookup(Sym)){
      // Use the symbol as the tag.
      // FIXME: This might not be as unique as we would like.
      GenericNodeBuilder Bd(Builder, S, Sym);
      bool stop = false;
      llvm::tie(Pred, state) = HandleAutoreleaseCounts(state, Bd, Pred, Eng,
                                                       Sym, *T, stop);
      if (stop)
        return;
    }
  }
  
  B = state.get<RefBindings>();
  llvm::SmallVector<SymbolRef, 10> Leaked;
  
  for (SymbolReaper::dead_iterator I = SymReaper.dead_begin(),
       E = SymReaper.dead_end(); I != E; ++I) {    
      if (const RefVal* T = B.lookup(*I))
        state = HandleSymbolDeath(state, *I, *T, Leaked);
  }    
  
  static unsigned LeakPPTag = 0;
  {
    GenericNodeBuilder Bd(Builder, S, &LeakPPTag);
    Pred = ProcessLeaks(state, Leaked, Bd, Eng, Pred);
  }
  
  // Did we cache out?
  if (!Pred)
    return;
  
  // Now generate a new node that nukes the old bindings.
  RefBindings::Factory& F = state.get_context<RefBindings>();
  
  for (SymbolReaper::dead_iterator I = SymReaper.dead_begin(),
       E = SymReaper.dead_end(); I!=E; ++I) B = F.Remove(B, *I);
  
  state = state.set<RefBindings>(B);
  Builder.MakeNode(Dst, S, Pred, state);
}

void CFRefCount::ProcessNonLeakError(ExplodedNodeSet<GRState>& Dst,
                                     GRStmtNodeBuilder<GRState>& Builder,
                                     Expr* NodeExpr, Expr* ErrorExpr,                        
                                     ExplodedNode<GRState>* Pred,
                                     const GRState* St,
                                     RefVal::Kind hasErr, SymbolRef Sym) {
  Builder.BuildSinks = true;
  GRExprEngine::NodeTy* N  = Builder.MakeNode(Dst, NodeExpr, Pred, St);
  
  if (!N)
    return;
  
  CFRefBug *BT = 0;
  
  switch (hasErr) {
    default:
      assert(false && "Unhandled error.");
      return;
    case RefVal::ErrorUseAfterRelease:
      BT = static_cast<CFRefBug*>(useAfterRelease);
      break;      
    case RefVal::ErrorReleaseNotOwned:
      BT = static_cast<CFRefBug*>(releaseNotOwned);
      break;
    case RefVal::ErrorDeallocGC:
      BT = static_cast<CFRefBug*>(deallocGC);
      break;
    case RefVal::ErrorDeallocNotOwned:
      BT = static_cast<CFRefBug*>(deallocNotOwned);
      break;
  }
    
  CFRefReport *report = new CFRefReport(*BT, *this, N, Sym);
  report->addRange(ErrorExpr->getSourceRange());
  BR->EmitReport(report);
}

//===----------------------------------------------------------------------===//
// Transfer function creation for external clients.
//===----------------------------------------------------------------------===//

GRTransferFuncs* clang::MakeCFRefCountTF(ASTContext& Ctx, bool GCEnabled,
                                         const LangOptions& lopts) {
  return new CFRefCount(Ctx, GCEnabled, lopts);
}  
