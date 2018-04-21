//===--- TFDifferentiation.cpp - SIL Automatic Differentiation --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// SWIFT_ENABLE_TENSORFLOW
//
// This file implements reverse-mode automatic differentiation.
//
// NOTE: Although the AD feature is developed as part of the Swift for
// TensorFlow project, it is completely independent from TensorFlow support.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/AST/AutoDiff.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/SubstitutionList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"

using namespace swift;
using llvm::DenseMap;

static NominalTypeDecl *getStdlibTypeDecl(StringRef, ASTContext &);
static std::string mangleWRT(ArrayRef<unsigned>);
static std::string mangleADConfig(const SILReverseAutoDiffConfiguration &);

//===----------------------------------------------------------------------===//
// Auxiliary data structures
//===----------------------------------------------------------------------===//

namespace {
// A differentiation task, specifying the original function and the
// `[reverse_differentiable]` attribute on the function. PrimalGen and
// AdjointGen will synthesize the primal and the adjoint for this task, filling
// the primal and adjoint fields in the attribute.
struct DifferentiationTask {
  SILFunction *original = nullptr;
  SILReverseDifferentiableAttr *attr = nullptr;

  /*implicit*/ DifferentiationTask(SILFunction *original,
                                   SILReverseDifferentiableAttr *attr)
    : original(original), attr(attr) {}

  bool isEqual(const DifferentiationTask &other) const {
    return original == other.original && attr == other.attr;
  }

  SILReverseAutoDiffConfiguration getMasterConfig() const {
    return SILReverseAutoDiffConfiguration::getMaster(attr->getParamIndices());
  }
};
} // end anonymous namespace

namespace llvm {
using swift::SILFunction;
using swift::SILReverseDifferentiableAttr;

template<typename T> struct DenseMapInfo;

template<> struct DenseMapInfo<DifferentiationTask> {
  static DifferentiationTask getEmptyKey() {
    return {
      DenseMapInfo<SILFunction *>::getEmptyKey(),
      DenseMapInfo<SILReverseDifferentiableAttr *>::getEmptyKey()
    };
  }

  static DifferentiationTask getTombstoneKey() {
    return {
      DenseMapInfo<SILFunction *>::getTombstoneKey(),
      DenseMapInfo<SILReverseDifferentiableAttr *>::getTombstoneKey()
    };
  }

  static unsigned getHashValue(const DifferentiationTask &Val) {
    return hash_combine(
      DenseMapInfo<SILFunction *>::getHashValue(Val.original),
      DenseMapInfo<SILReverseDifferentiableAttr *>::getHashValue(Val.attr)
    );
  }

  static bool isEqual(const DifferentiationTask &LHS,
                      const DifferentiationTask &RHS) {
    return LHS.isEqual(RHS);
  }
};
} // end llvm namespace

namespace {
/// The kind of SIL value in the primal function.
enum class PrimalValueKind {
  /// Argument to the primal function.
  Argument,
  /// The value can be converted from its arguments using the same instruction.
  Conversion,
  /// Intentially discarded for rematerialization.
  ToRematerialize,
  /// The value is statically grouped into the primal value struct and can be
  /// accessed directly using `struct_extract`.
  StaticCheckpoint,
  /// The value is pushed onto the differentiation tape in the struct and can be
  /// accessed when popped from the tape.
  TapeCheckpoint
};

/// The result coming from PrimalGen.
struct PrimalFunctionInfo {
  /// The primal function.
  SILFunction *primal = nullptr;
  /// Checkpoint storage.
  StructDecl *checkpointStorageDecl = nullptr;
  /// A mapping from primal checkpoints to the declaration references in the
  /// checkpoints struct.
  DenseMap<SILValue, SILDeclRef> staticCheckpointMap;
  /// A mapping from primal checkpoints to the declaration references of tapes
  /// they belong to.
  DenseMap<SILValue, SILDeclRef> tapedCheckpointMap;
};

using GradientLookupKey = std::pair<SILFunction *,
                                    SILReverseAutoDiffConfiguration>;
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// ADContext - Per-module contextual information for the Differentiation pass.
//===----------------------------------------------------------------------===//

namespace {
class ADContext {
  friend class PrimalGen;
  friend class AdjointGen;
private:
  /// The module where Differentiation is performed on.
  SILModule &module;

  /// Shared pass manager.
  SILPassManager &passManager;

  /// A mapping from functions and AD configurations to gradient functions.
  /// NOTE: The parameter index array is hashed by reference, which is expected
  /// to point to [reverse_differentiable wrt ...]'s trailing index storage.
  DenseMap<GradientLookupKey, SILFunction *> gradientMap;

  /// Type converter.
  Lowering::TypeConverter typeConverter;

  /// The Differentiable protocol in the standard library.
  ProtocolDecl *differentiableProtocol = nullptr;
  /// The FloatingPoint protocol in the stanard library.
  ProtocolDecl *floatingPointProtocol = nullptr;

  /// Flag indicating whether an error occurred.
  bool errorOccurred = false;

  /// AD runtime declarations.
  /// struct _ADTape
  NominalTypeDecl *tapeDecl = nullptr;
  /// sil @_swift_autodiff_CreateTape
  SILFunction *createTapeFn = nullptr;
  /// sil @_swift_autodiff_PushToTape
  SILFunction *pushToTapeFn = nullptr;
  /// sil @_swift_autodiff_PopFromTape
  SILFunction *popFromTapeFn = nullptr;
  /// sil @_swift_autodiff_TapeElementCount
  SILFunction *tapeElementCountFn = nullptr;

  /// `Differentiable.combiningAsAdjoint(with:)` declaration.
  FuncDecl *combiningAsAdjoingFn = nullptr;

public:
  SILModule &getModule() const { return module; }
  ASTContext &getASTContext() const { return module.getASTContext(); }
  SILPassManager &getPassManager() const { return passManager; }

  /// Construct an ADContext for the given module.
  explicit ADContext(SILModule &module, SILPassManager &passManager);

  Lowering::TypeConverter &getTypeConverter() {
    return typeConverter;
  }

  ProtocolDecl *getDifferentiableProtocol() const {
    return differentiableProtocol;
  }
  ProtocolDecl *getFloatingPointProtocol() const {
    return floatingPointProtocol;
  }

  /// Determines whether the given type conforms to Differentiable.
  bool conformsToDifferentiable(Type type) const;

  /// Determines whether the given type is differentiable.
  bool isDifferentiableType(Type type) const;
  /// Determines whether the given value is differentiable.
  bool isDifferentiableOperation(SILValue value) const;

  void insertPrimal(SILFunction *original, ArrayRef<unsigned> paramIndices,
                    SILFunction *primal) {
    auto *attr = getOrCreateReverseDifferentiableAttr(original, paramIndices);
    attr->setPrimalName(primal->getName());
  }

  void insertAdjoint(SILFunction *original, ArrayRef<unsigned> paramIndices,
                     SILFunction *adjoint) {
    auto *attr = getOrCreateReverseDifferentiableAttr(original, paramIndices);
    attr->setAdjointName(adjoint->getName());
  }

  SILFunction *lookupPrimal(SILFunction *original,
                            ArrayRef<unsigned> paramIndices) const {
    if (auto *attr = lookupReverseDifferentiableAttr(original, paramIndices))
      return module.findFunction(attr->getPrimalName(), SILLinkage::Public);
    return nullptr;
  }

  SILFunction *lookupAdjoint(SILFunction *original,
                             ArrayRef<unsigned> paramIndices) const {
    if (auto *attr = lookupReverseDifferentiableAttr(original, paramIndices))
      return module.findFunction(attr->getAdjointName(), SILLinkage::Public);
    return nullptr;
  }

  SILFunction *lookupPrimal(const DifferentiationTask &task) const {
    auto name = task.attr->getPrimalName();
    if (name.empty()) return nullptr;
    return module.findFunction(name, SILLinkage::Public);
  }

  SILFunction *lookupAdjoint(const DifferentiationTask &task) const {
    auto name = task.attr->getAdjointName();
    if (name.empty()) return nullptr;
    return module.findFunction(name, SILLinkage::Public);
  }

  void insertGradient(const GradientLookupKey &key, SILFunction *gradient) {
    gradientMap.insert({key, gradient});
  }

  SILFunction *lookupCanonicalGradient(const DifferentiationTask &task) const {
    return gradientMap.lookup({task.original, task.getMasterConfig()});
  }

  SILFunction *lookupGradient(const GradientLookupKey &key) const {
    return gradientMap.lookup(key);
  }

  /// Finds the `[reverse_differentiable]` attribute on the specified original
  /// function corresponding to the specified parameter indices. Returns nullptr
  /// if it does not exist.
  ///
  /// TODO: Currently we are doing a O(n) lookup. This could be improved by
  /// hashing on SILFunction's side or maintaining a dictionary in ADContext.
  /// In any case, this is not performance-critical.
  SILReverseDifferentiableAttr *
  lookupReverseDifferentiableAttr(SILFunction *original,
                                  ArrayRef<unsigned> paramIndices) const {
    for (auto *attr : original->getReverseDifferentiableAttrs())
      if (attr->getParamIndices().data() == paramIndices.data() ||
          attr->getParamIndices().equals(paramIndices))
        return attr;
    return nullptr;
  }

  /// Finds or creates a `[reverse_differentiable]` attribute on the specified
  /// original function corresponding to the specified parameter indices.
  SILReverseDifferentiableAttr *
  getOrCreateReverseDifferentiableAttr(SILFunction *function,
                                       ArrayRef<unsigned> paramIndices) {
    if (auto *attr = lookupReverseDifferentiableAttr(function, paramIndices))
      return attr;
    auto *attr =
      SILReverseDifferentiableAttr::create(getModule(), paramIndices);
    function->addReverseDifferentiableAttr(attr);
    return attr;
  }

  template<typename...T, typename...U>
  InFlightDiagnostic
  diagnose(SourceLoc loc, Diag<T...> diag, U &&...args) {
    return getASTContext().Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  void setErrorOccurred() { errorOccurred = true; }
  bool hasErrorOccurred() const { return errorOccurred; }
};
} // end anonymous namespace

ADContext::ADContext(SILModule &module, SILPassManager &passManager)
  : module(module), passManager(passManager), typeConverter(module) {
  auto &ctx = getASTContext();

  // Link all from module.
  module.linkAllFromCurrentModule();

  // Cache commonly used declarations and function references.
  differentiableProtocol =
    ctx.getProtocol(KnownProtocolKind::Differentiable);
  floatingPointProtocol =
    cast<ProtocolDecl>(getStdlibTypeDecl("FloatingPoint", ctx));

  // FIXME: Load tape runtime functions.
#if false
  tapeDecl = getStdlibTypeDecl("_ADTape", ctx);
  createTapeFn = module.findFunction("_swift_autodiff_CreateTape",
                                     SILLinkage::Public);
  assert(createTapeFn && "_swift_autodiff_CreateTape doesn't exist?");
  pushToTapeFn = module.findFunction("_swift_autodiff_PushToTape",
                                     SILLinkage::Public);
  assert(pushToTapeFn && "_swift_autodiff_PushToTape doesn't exist?");
  popFromTapeFn = module.findFunction("_swift_autodiff_PopFromTape",
                                      SILLinkage::Public);
  assert(popFromTapeFn && "_swift_autodiff_PopFromTape doesn't exist?");
  tapeElementCountFn = module.findFunction("_swift_autodiff_TapeElementCount",
                                           SILLinkage::Public);
  assert(tapeElementCountFn &&
         "_swift_autodiff_TapeElementCount doesn't exist?");
#endif

  DeclName combineAsAdjointFnName(ctx, ctx.getIdentifier("combiningAsAdjoint"),
                                  { ctx.Id_with });
  auto caaLookup = differentiableProtocol->lookupDirect(combineAsAdjointFnName);
  for (auto *cand : caaLookup) {
    if (auto *fd = dyn_cast<FuncDecl>(cand)) {
      // TODO: Check type signature.
      combiningAsAdjoingFn = fd;
      break;
    }
  }
  assert(combiningAsAdjoingFn &&
         "Differentiable.combiningAsAdjoint(with:) doesn't exist?");
}

bool ADContext::conformsToDifferentiable(Type type) const {
  auto *swiftModule = module.getSwiftModule();
  if (auto lookup =
      swiftModule->lookupConformance(type, differentiableProtocol))
    return true;
  return false;
}

/// Type must be differentiable.
bool ADContext::isDifferentiableType(Type type) const {
  if (conformsToDifferentiable(type))
    return true;
  // FIXME: Add DifferentiationParameter.
  if (auto structTy = type->getAs<StructType>()) {
    for (auto *prop : structTy->getDecl()->getStoredProperties())
      if (!isDifferentiableType(prop->getInterfaceType()))
        return false;
    return true;
  } else if (auto tupleTy = type->getAs<TupleType>()) {
    for (auto &tup : tupleTy->getElements())
      if (!isDifferentiableType(tup.getType()))
        return false;
    return true;
  }
  return false;
}

bool ADContext::isDifferentiableOperation(SILValue value) const {
  // Before we decide whether the opreation is differentiable, we need to see
  // if the type of this value is differentiable.
  if (!isDifferentiableType(value->getType().getSwiftRValueType()))
    return false;
  // FIXME: Check instructions
  return true;
}

//===----------------------------------------------------------------------===//
// PrimalGen - generates primal functions for each differentiation task in
// the SIL module.
//===----------------------------------------------------------------------===//

namespace {
class PrimalGen {
private:
  /// A reference to the list of original functions to generate a primal
  /// function for.
  ArrayRef<DifferentiationTask> diffTasks;

  /// The global AD context.
  ADContext &context;

public:
  explicit PrimalGen(ArrayRef<DifferentiationTask> diffTasks,
                     ADContext &context)
    : diffTasks(diffTasks), context(context) {}

  using Result = DenseMap<std::pair<SILFunction *, ArrayRef<unsigned>>,
                                  PrimalFunctionInfo>;
  /// Perform primal generation, and indirectly returns a mapping from original
  /// functions to primal infos.
  void generate(Result &primalInfos);

private:
  /// Creates an empty primal function.
  SILFunction *createPrimalFunction(SILFunction *original,
                                    ArrayRef<unsigned> paramIndices);
  /// A task specifies the empty primal function to be filled in, and what its
  /// corresponding original and parameter indices are.
  struct Task {
    SILFunction *original;
    SILFunction *primal;
    ArrayRef<unsigned> paramIndices;
  };
  /// Processes an original function and generate its adjoint.
  void processTask(Task task,
                   SmallVectorImpl<Task> &worklist,
                   PrimalFunctionInfo &primalInfo);
};
} // end anonymous namespace

/// Creates a struct declaration (without contents) for storing contextual
/// information for a function. The newly created struct will have the same
/// generic parameters as the function.
///
/// This helper function will be used to create checkpointing structs,
/// specifically structs for control-independent checkpoints and tape groups.
static StructDecl *createContextStructForFunction(SILFunction *function,
                                                  StringRef name) {
  auto *swiftModule = function->getModule().getSwiftModule();
  auto &file = swiftModule->getMainFile(FileUnitKind::Derived);
  auto &ctx = file.getASTContext();
  auto *genParams = function->getDeclContext()->getGenericParamsOfContext();
  auto structId = ctx.getIdentifier(name);
  auto ctxStruct =
    new (ctx) StructDecl(/*StructLoc*/ function->getLocation().getSourceLoc(),
                         /*Name*/ structId,
                         /*NameLoc*/ function->getLocation().getSourceLoc(),
                         /*Inherited*/ {},
                         /*GenericParams*/ genParams,
                         /*DC*/ file.getLocalContext());
  ctx.addExternalDecl(ctxStruct);
  return ctxStruct;
}

/// Determine the kind of the given primal value. It is a BB argument, a
/// cost-free conversion like `struct_extract`, a value to be recomputed in
/// the adjoint, an control-independent checkpoint, or a tape checkpoint.
static PrimalValueKind classifyInstruction(SILValue value,
                                           DominanceInfo &domInfo) {
  auto *fn = value->getParentBlock()->getParent();
  auto *entry = fn->getEntryBlock();
  auto *bb = value->getParentBlock();
  switch (value->getKind()) {
    case ValueKind::SILFunctionArgument:
      return PrimalValueKind::Argument;
    case ValueKind::TupleInst:
    case ValueKind::StructInst:
    case ValueKind::TupleExtractInst:
    case ValueKind::StructExtractInst:
    case ValueKind::EnumInst:
    case ValueKind::FunctionRefInst:
    case ValueKind::ConvertFunctionInst:
    case ValueKind::ThinToThickFunctionInst:
    case ValueKind::StringLiteralInst:
    case ValueKind::BuiltinInst:
    case ValueKind::GlobalValueInst:
    case ValueKind::KeyPathInst:
    case ValueKind::MetatypeInst:
    case ValueKind::GradientInst:
      return PrimalValueKind::Conversion;
    default:
      return domInfo.dominates(entry, bb)
        ? PrimalValueKind::StaticCheckpoint
        : PrimalValueKind::TapeCheckpoint;
  }
}

namespace {
class PrimalGenCloner : public SILClonerWithScopes<PrimalGenCloner> {
public:
  explicit PrimalGenCloner(SILFunction &newFn) : SILClonerWithScopes(newFn) {}
};
} // end anonymous namespace

void PrimalGen::processTask(PrimalGen::Task task,
                            SmallVectorImpl<PrimalGen::Task> &worklist,
                            PrimalFunctionInfo &primalInfo) {
  auto *domAnalysis = context.getPassManager().getAnalysis<DominanceAnalysis>();
  auto *domInfo = domAnalysis->get(task.original);

  llvm_unreachable("Unimplemented");
}

/// Creates a primal function.
SILFunction *PrimalGen::createPrimalFunction(SILFunction *original,
                                             ArrayRef<unsigned> paramIndices) {
  auto &module = context.module;
  std::string primalName =
    original->getName().str() + "__primal_" + mangleWRT(paramIndices);
  // Create a `<fn_name>__Checkpoints` struct.
  auto checkpointStructName = original->getName().str() + "__Checkpoints";
  StructDecl *checkpointStorageDecl =
    createContextStructForFunction(original, checkpointStructName);
  auto csdType =
    checkpointStorageDecl->getInterfaceType()->getCanonicalType();
  auto objTy = SILType::getPrimitiveObjectType(csdType);
  auto resultConv = objTy.isLoadable(module)
    ? ResultConvention::Owned : ResultConvention::Unowned;
  SILResultInfo csResult(csdType, resultConv);
  // Create result info for checkpoints.
  auto originalTy = original->getLoweredFunctionType();
  auto primalTy = SILFunctionType::get(originalTy->getGenericSignature(),
                                       originalTy->getExtInfo(),
                                       originalTy->getCoroutineKind(),
                                       originalTy->getCalleeConvention(),
                                       originalTy->getParameters(),
                                       originalTy->getYields(),
                                       { csResult },
                                       originalTy->getErrorResult(),
                                       context.getASTContext());
  auto *primal = module.getOrCreateFunction(original->getLocation(),
                                            primalName,
                                            SILLinkage::Public,
                                            primalTy,
                                            original->isBare(),
                                            original->isTransparent(),
                                            original->isSerialized());
  return primal;
}

/// Starting from functions to be differentiated using the `gradient`
/// instruction, recursively generate a primal function for each original
/// function along the differentiation path.
void PrimalGen::generate(PrimalGen::Result &primalInfos) {
  SmallVector<Task, 16> worklist;
  // Push everything to the worklist.
  for (auto &task : diffTasks) {
    // Does this original function have a primal yet? If not, generate a new one
    // and push it to the work list so that its body will be filled.
    if (context.lookupPrimal(task))
      continue;
    auto *original = task.original;
    auto *diffAttr = task.attr;
    auto paramIndices = diffAttr->getParamIndices();
    auto *primal = createPrimalFunction(original, paramIndices);
    worklist.push_back({original, primal, paramIndices});
  }
  // Iterate through the worklist, look up existing adjoint. If an adjoint
  // exists for the task, do nothing. Otherwise, create a function and process
  // it.
  while (!worklist.empty()) {
    auto task = worklist.back();
    worklist.pop_back();
    PrimalFunctionInfo pi;
    pi.primal = task.primal;
    primalInfos.insert({{task.original, task.paramIndices}, pi});
    processTask(task, worklist, pi);
  }
}

//===----------------------------------------------------------------------===//
// AdjointGen - generates an adjoint function for each differentiation task
// in a SIL module.
//===----------------------------------------------------------------------===//

/// The adjoint generator for all gradient functions. Executed after PrimalGen.
namespace {
class AdjointGen {
private:
  /// A reference to the global differentiation worklist created in the
  /// Differentiation pass, storing the gradient instructions to process.
  ArrayRef<DifferentiationTask> diffTasks;
  /// The global AD context.
  ADContext &context;
  /// A mapping from original functions to their primal infos.
  PrimalGen::Result &primalInfos;

  /// Emit instructions to accumulate adjoint.
  SILValue accumulateAdjoint(SILValue oldAdjoint, SILValue newAdjoint,
                             SILBuilder &builder, SILLocation loc) const;

public:
  explicit AdjointGen(ArrayRef<DifferentiationTask> diffTasks,
                      ADContext &context, PrimalGen::Result &primalInfos)
    : diffTasks(diffTasks), context(context), primalInfos(primalInfos) {}
  void generate();

private:
  /// Creates an empty adjoint function.
  SILFunction *createAdjointFunction(SILFunction *original,
                                     CanType checkpointsType,
                                     ArrayRef<unsigned> paramIndices);
  /// A task specifies the empty adjoint function to be filled in, and what its
  /// corresponding original and parameter indices are.
  struct Task {
    SILFunction *original;
    SILFunction *adjoint;
    ArrayRef<unsigned> paramIndices;
  };
  /// Process an original function and generate its adjoint.
  void processTask(Task task, SmallVectorImpl<Task> &worklist);
};
} // end anonymous namespace

SILValue
AdjointGen::accumulateAdjoint(SILValue oldAdjoint, SILValue newAdjoint,
                              SILBuilder &builder, SILLocation loc) const {
  auto adjointTy = oldAdjoint->getType().getSwiftRValueType();
  auto silAdjointTy = SILType::getPrimitiveAddressType(adjointTy);
  assert(adjointTy->isEqual(oldAdjoint->getType().getSwiftRValueType()) &&
         "Adjoints must have equal types!");
  auto adjointTyDecl = adjointTy->getAnyNominal();

  if (context.conformsToDifferentiable(adjointTy)) {
    // If the type conforms to Differentiable, then combine them using
    // `Differentiable.combiningAsAdjoing(with:)`.
    auto *conformance = context.getASTContext().getConformance(
      adjointTy, context.differentiableProtocol, loc.getSourceLoc(),
      adjointTyDecl, ProtocolConformanceState::Complete);
    auto fnTy = context.combiningAsAdjoingFn->getInterfaceType();
    auto silFnTy = SILType::getPrimitiveObjectType(fnTy->getCanonicalType());
    SILDeclRef declRef(context.combiningAsAdjoingFn, SILDeclRef::Kind::Func);
    auto witnessMethod = builder.createWitnessMethod(
      loc, adjointTy, ProtocolConformanceRef(conformance), declRef, silFnTy);
    auto subMap =
      adjointTy->getMemberSubstitutionMap(context.getModule().getSwiftModule(),
                                          context.combiningAsAdjoingFn);
    SmallVector<Substitution, 1> subs;
    conformance->getGenericSignature()->getSubstitutions(subMap, subs);
    auto result = builder.createAllocStack(loc, silAdjointTy.getObjectType());
    auto resultAccess = builder.createBeginAccess(
      loc, result, SILAccessKind::Init, SILAccessEnforcement::Static);
    builder.createApply(loc, witnessMethod, subs,
                        { resultAccess, newAdjoint, oldAdjoint },
                        /*isNonThrowing*/false);
    builder.createEndAccess(loc, resultAccess, /*aborted*/false);
    return result;
  }

  // If the type does not conform to Differentiable, then it must be an
  // aggregate (tuple/struct).
  else if (auto *tupleTy = adjointTy->getAs<TupleType>()) {
    SmallVector<SILValue, 4> accumulatedElts;
    for (unsigned i = 0, n = tupleTy->getNumElements(); i != n; ++i) {
      auto *oldElemVal = builder.createTupleExtract(loc, oldAdjoint, i);
      auto *newElemVal = builder.createTupleExtract(loc, newAdjoint, i);
      auto result = accumulateAdjoint(oldElemVal, newElemVal, builder, loc);
      accumulatedElts.push_back(result);
    }
    return builder.createTuple(loc, accumulatedElts);
  }
  else if (auto *structTy = adjointTy->getAs<StructType>()) {
    SmallVector<SILValue, 4> accumulatedMembers;
    auto *decl = structTy->getDecl();
    for (auto *member : decl->getStoredProperties()) {
      auto *oldMemberVal = builder.createStructExtract(loc, oldAdjoint, member);
      auto *newMemberVal = builder.createStructExtract(loc, newAdjoint, member);
      auto result = accumulateAdjoint(oldMemberVal, newMemberVal, builder, loc);
      accumulatedMembers.push_back(result);
    }
    return builder.createStruct(loc, silAdjointTy, accumulatedMembers);
  }
  else {
    llvm_unreachable("Invalid adjoint type!");
  }
}

SILFunction *
AdjointGen::createAdjointFunction(SILFunction *original,
                                  CanType checkpointsType,
                                  ArrayRef<unsigned> paramIndices) {
  auto &module = context.getModule();

  // Given a canonical type, returns parameter info.
  auto getFormalParamInfo = [&module](CanType ty) -> SILParameterInfo {
    SILType silTy = SILType::getPrimitiveObjectType(ty);
    ParameterConvention conv;
    if (SILModuleConventions::isPassedIndirectlyInSIL(silTy, module))
      conv = ParameterConvention::Indirect_In;
    else if (silTy.isTrivial(module))
      conv = ParameterConvention::Direct_Unowned;
    else
      conv = ParameterConvention::Direct_Owned;
    return { ty, conv };
  };

  // Given a canonical type, returns result info.
  auto getFormalResultInfo = [&module](CanType ty) -> SILResultInfo {
    SILType silTy = SILType::getPrimitiveObjectType(ty);
    ResultConvention conv;
    if (SILModuleConventions::isPassedIndirectlyInSIL(silTy, module))
      conv = ResultConvention::Indirect;
    else if (silTy.isTrivial(module))
      conv = ResultConvention::Unowned;
    else
      conv = ResultConvention::Owned;
    return { ty, conv };
  };

  // Parameters of the adjoint include the original parameters, a value
  // representing primal checkpoints, and a seed.
  // Results of the adjoint have the same type as the original parameters.
  SmallVector<SILParameterInfo, 8> adjParams;
  SmallVector<SILResultInfo, 8> adjResults;
  auto origTy = original->getLoweredFunctionType();
  for (auto &param : origTy->getParameters()) {
    adjParams.push_back(param);
    adjResults.push_back(getFormalResultInfo(param.getType()));
  }
  adjParams.push_back(getFormalParamInfo(checkpointsType));
  adjParams.push_back(
    getFormalParamInfo(origTy->getSingleResult().getType()));
  auto adjName = original->getName().str() + "__adj_" + mangleWRT(paramIndices);
  auto adjType = SILFunctionType::get(origTy->getGenericSignature(),
                                      origTy->getExtInfo(),
                                      origTy->getCoroutineKind(),
                                      origTy->getCalleeConvention(),
                                      adjParams, {}, adjResults, None,
                                      original->getASTContext());
  auto *adjoint = module.createFunction(SILLinkage::Public, adjName, adjType,
                                        original->getGenericEnvironment(),
                                        original->getLocation(),
                                        original->isBare(),
                                        original->isTransparent(),
                                        original->isSerialized());
  adjoint->setDebugScope(
    new (module) SILDebugScope(original->getLocation(), adjoint));
  return adjoint;
}

void AdjointGen::processTask(AdjointGen::Task task,
                             SmallVectorImpl<AdjointGen::Task> &worklist) {
  llvm_unreachable("unimplemented");
}

void AdjointGen::generate() {
  SmallVector<Task, 16> worklist;
  // Push everything to the worklist.
  for (auto task : diffTasks) {
    if (context.lookupAdjoint(task))
      continue;
    auto *original = task.original;
    auto *diffAttr = task.attr;
    auto paramIndices = diffAttr->getParamIndices();
    auto *primal = context.lookupPrimal(task);
    assert(primal && "PrimalGen didn't run on this function before?!");
    auto primalTy = primal->getLoweredFunctionType();
    auto checkpointsTy = primalTy->getSingleResult().getType();
    auto *adjoint =
      createAdjointFunction(original, checkpointsTy, paramIndices);
    worklist.push_back({original, adjoint, paramIndices});
  }
  // Iterate over the worklist, look up existing adjoint. If an adjoint exists
  // for the task, do nothing. Otherwise, create a function and process it.
  while (!worklist.empty()) {
    auto task = worklist.back();
    worklist.pop_back();
    processTask(task, worklist);
  }
}

//===----------------------------------------------------------------------===//
// Local utilities
//===----------------------------------------------------------------------===//

template<typename T>
static void debugDump(T &v) {
  DEBUG(llvm::dbgs() << "==== BEGIN DEBUG DUMP ===="
        << v << "==== END DEBUG DUMP ====\n");
}

/// Finds a type declaration in the standard library.
static NominalTypeDecl *getStdlibTypeDecl(StringRef name, ASTContext &ctx) {
  SmallVector<ValueDecl *, 1> lookupResults;
  ctx.lookupInSwiftModule(name, lookupResults);
  assert(!lookupResults.empty() && "Name does not exist in stdlib?");
  return cast<NominalTypeDecl>(lookupResults[0]);
}

static
void lookupProtocolRequiredMember(NominalTypeDecl *typeDecl,
                                  ProtocolDecl *proto,
                                  DeclName name, ModuleDecl *module,
                                  SmallVectorImpl<ValueDecl *> &results) {
  // Make sure the given type conforms to the given protocol.
  SmallVector<ProtocolConformance *, 2> conformances;
  auto type = typeDecl->getDeclaredInterfaceType();
  DEBUG(llvm::dbgs() << "Looking up protocol required method: "
        << name << " in " << type << '\n');
  typeDecl->lookupConformance(module, proto, conformances);
  assert(!conformances.empty() && "Type doesn't conform to the protocol?");
  // Look up nominal type candidates and protocol requirement candidates.
  SmallVector<ValueDecl *, 2> lookupResults;
  typeDecl->lookupQualified(
    type, name, NLOptions::NL_ProtocolMembers, nullptr, lookupResults);
  // Append matches to results.
  for (ValueDecl *decl : lookupResults) {
    DEBUG(llvm::dbgs() << "Protocol requirement lookup result: "
          << decl->getFullName() << " : " << decl->getInterfaceType() << '\n');
    results.push_back(decl);
  }
}

static SILFunction *
findSILFunctionForRequiredProtocolMember(NominalTypeDecl *typeDecl,
                                         ProtocolDecl *proto, DeclName name,
                                         ModuleDecl *module,
                                         SILModule &silModule) {
  SmallVector<ValueDecl *, 4> results;
  lookupProtocolRequiredMember(typeDecl, proto, name, module, results);
  for (auto *result : results) {
    std::string name = SILDeclRef(result).mangle();
    if (auto *fn = silModule.findFunction(name, SILLinkage::PublicExternal))
      return fn;
  }
  return nullptr;
}

/// Looks through the definition of a function value. If the source that
/// produced this function value is `function_ref`, it return the concrete
/// `SILFunction`.
static SILFunction *findReferencedFunction(SILValue value) {
  auto *inst = value->getDefiningInstruction();
  if (!inst) return nullptr;
  if (auto *fri = dyn_cast<FunctionRefInst>(inst)) {
    auto *fn = fri->getReferencedFunction();
    if (&fn->getModule() == &inst->getModule() ||
        fn->isSerialized() == IsSerialized)
      return fn;
  }
  if (auto *thinToThink = dyn_cast<ThinToThickFunctionInst>(inst))
    return findReferencedFunction(thinToThink->getOperand());
  if (auto *convertFn = dyn_cast<ConvertFunctionInst>(inst))
    return findReferencedFunction(convertFn->getOperand());
  return nullptr;
}

/// Mangles a w.r.t. list.
static std::string mangleWRT(ArrayRef<unsigned> paramIndices) {
  std::string result = "wrt_";
  interleave(paramIndices,
             [&](unsigned idx) { result += std::to_string(idx); },
             [&]{ result += '_'; });
  return result;
}

/// Mangles an AD configuration.
static
std::string mangleADConfig(const SILReverseAutoDiffConfiguration &config) {
  std::string result = "grad_" + mangleWRT(config.parameterIndices);
  if (config.seedable)
    result += "_s";
  if (config.preservingResult)
    result += "_p";
  return result;
}

/// Creates arguments in the entry block based on the function type.
static void createEntryArguments(SILFunction *f) {
  auto *entry = f->getEntryBlock();
  auto conv = f->getConventions();
  assert((entry->getNumArguments() == 0 ||
         conv.getNumSILArguments() == 0) &&
         "Entry already has arguments?!");
  for (auto indResultTy : conv.getIndirectSILResultTypes())
    entry->createFunctionArgument(indResultTy.getAddressType());
  for (auto paramTy : conv.getParameterSILTypes())
    entry->createFunctionArgument(paramTy);
}

/// Build an Int.
static SILValue convertFromIntegerLiteral(intmax_t value,
                                          NominalTypeDecl *targetTypeDecl,
                                          SILLocation loc, SILBuilder &builder,
                                          SILModule &module) {
  auto &astCtx = module.getASTContext();
  auto targetTy =
    targetTypeDecl->getDeclaredInterfaceType()->getCanonicalType();
  // Initialize an Int from the given value.
  auto builtinIntTy = SILType::getBuiltinIntegerType(2048, astCtx);
  // %1 = integer_literal $Builtin.Int2048, <value>
  auto *builtinInt = builder.createIntegerLiteral(loc, builtinIntTy, value);
  auto metatypeTy = SILType::getPrimitiveObjectType(
    CanMetatypeType::get(targetTy, MetatypeRepresentation::Thin));
  auto *metatype = builder.createMetatype(loc, metatypeTy);
  // %2 = metatype $@thin Int.Type
  // `init(_builtinIntegerLiteral:)`
  DeclName builtinLitInitName(astCtx, astCtx.Id_init, {
    astCtx.getIdentifier("_builtinIntegerLiteral")
  });
  auto *expByBuiltinIntProto =
    astCtx.getProtocol(KnownProtocolKind::ExpressibleByBuiltinIntegerLiteral);
  SmallVector<ValueDecl *, 1> builtinLitInitMethods;
  lookupProtocolRequiredMember(targetTypeDecl, expByBuiltinIntProto,
                               builtinLitInitName, module.getSwiftModule(),
                               builtinLitInitMethods);
  auto builtinLitInit = cast<ConstructorDecl>(builtinLitInitMethods.front());
  auto initDeclRefName = SILDeclRef(builtinLitInit).mangle();
  auto *builtinLitInitFunc = module.findFunction(initDeclRefName,
                                                 SILLinkage::PublicExternal);
  assert(builtinLitInitFunc &&
         "Cannot find `init(_builtinIntegerLiteral)` in SIL?");
  // %3 = function_ref @<target type>.init(_builtinIntegerLiteral:)
  auto *builtinLitInitRef = builder.createFunctionRef(loc, builtinLitInitFunc);
  // %4 = apply %3(%1, %2) => $<target type>
  return builder.createApply(loc, builtinLitInitRef,
                             { builtinInt, metatype },
                             /*isNonThrowing*/ false);
}

/// Create a seed value by calling the `init(differentiationSeed:)` initializer.
static void convertToIndirectSeed(intmax_t value,
                                  NominalTypeDecl *targetTypeDecl,
                                  SILValue seedBuf, SILLocation loc,
                                  SILBuilder &builder, ADContext &context) {
  auto type = targetTypeDecl->getDeclaredInterfaceType()->getCanonicalType();
  auto silObjType = SILType::getPrimitiveObjectType(type);
  auto *diffableProto = context.getDifferentiableProtocol();
  DEBUG(llvm::dbgs() << "Creating seed for " << type << " type\n");
  auto &astCtx = context.getASTContext();
  auto &module = context.getModule();
  auto &typeConv = context.getTypeConverter();

  // FIXME: We are using a hack here for `Float` because protocol witness method
  // application doesn't work yet.
  // if (targetTypeDecl == astCtx.getFloatDecl())
  //   return convertFromIntegerLiteral(1, targetTypeDecl, loc, builder, module);
  // else {
  //   context.diagnose(loc.getSourceLoc(), diag::autodiff_unsupported_type,
  //                    targetTypeDecl->getDeclaredInterfaceType());
  //   return SILValue();
  // }

  // Create a currency value from the specified integer literal.
  DeclName currencyDeclName(astCtx.getIdentifier("DifferentiationCurrency"));
  auto currencyDeclLookupResult =
    targetTypeDecl->lookupDirect(currencyDeclName);
  auto *currencyAlias = cast<TypeAliasDecl>(currencyDeclLookupResult[0]);
  auto currencyTy =
    currencyAlias->getDeclaredInterfaceType()->getCanonicalType();
  auto currencyTyDecl = currencyTy.getAnyNominal();
  // %0 = ... : $<currency type>
  auto currencyVal =
    convertFromIntegerLiteral(value, currencyTyDecl, loc, builder, module);

  // %1 = metatype $Float.Type
  auto metatypeTy = SILType::getPrimitiveObjectType(
    CanMetatypeType::get(type, MetatypeRepresentation::Thick));
  auto *metatype = builder.createMetatype(loc, metatypeTy);

  // Call `init(differentiationSeed:)` through `Differentiable` protocol.
  DeclName initName(astCtx, astCtx.Id_init,
                    { astCtx.getIdentifier("differentiationSeed") });
  // Allocate buffer for passing the indirect currency value.
  // %2 = alloc_stack $<currency type>
  auto currencyValBuf =
    builder.createAllocStack(loc, typeConv.getLoweredType(currencyTy));
  SWIFT_DEFER {
    // dealloc_stack %2 : $<currency type>
    builder.createDeallocStack(loc, currencyValBuf);
  };
  auto currencySOQ = typeConv.getTypeLowering(currencyTy).isTrivial()
    ? StoreOwnershipQualifier::Trivial : StoreOwnershipQualifier::Init;
  // store %0 : $<currency type> to $*<currency type>
  builder.createStore(loc, currencyVal, currencyValBuf, currencySOQ);
  auto *reqr =
    cast<ConstructorDecl>(diffableProto->lookupDirect(initName).front());
  SILDeclRef reqrRef(reqr, SILDeclRef::Kind::Allocator);
  auto silInitTy = context.getTypeConverter().getConstantType(reqrRef);
  // Get conformance to `FloatingPoint`.
  auto conf = astCtx.getConformance(type, diffableProto,
                                    targetTypeDecl->getLoc(), targetTypeDecl,
                                    ProtocolConformanceState::Complete);
  ProtocolConformanceRef confRef(conf);
  // $4 = witness_method ...
  auto initFnRef =
    builder.createWitnessMethod(loc, type, confRef, reqrRef, silInitTy);
  DEBUG(llvm::dbgs() << "Calling init(differentiationSeed:) on "
        << silObjType << "\n");
  auto subMap =
    type->getMemberSubstitutionMap(module.getSwiftModule(), reqr,
                                   diffableProto->getGenericEnvironment());
  SmallVector<Substitution, 1> subs;
  diffableProto->getGenericSignature()->getSubstitutions(subMap, subs);
  // %5 = apply %4(%3, %2, %1)
  builder.createApply(loc, initFnRef, astCtx.AllocateCopy(subs),
                      { seedBuf, currencyValBuf, metatype },
                      /*isNonThrowing*/ false);
}

//===----------------------------------------------------------------------===//
// Differentiation pass implementation
//===----------------------------------------------------------------------===//

// Retrieve or create an empty gradient function based on a `gradient`
// instruction and replace all users of the `gradient` instruction with the
// gradient function. Returns the gradient function.
static SILFunction *getOrCreateGradient(
  ADContext &context, GradientInst *gradInst, SILFunction *original,
  SmallVectorImpl<DifferentiationTask> &worklist) {
  auto &module = original->getModule();
  auto &astCtx = module.getASTContext();
  auto origTy = original->getLoweredFunctionType();
  auto config = gradInst->getConfiguration();

  // Creates a gradient function based on the configuration.
  auto createGradFunction = [&](SILReverseAutoDiffConfiguration &config) {
    auto gradType = origTy->getGradientType(config, module);
    std::string gradName =
    original->getName().str() + "__" + mangleADConfig(config);
    auto gradNameId = astCtx.getIdentifier(gradName);
    auto *gradFn = module.createFunction(SILLinkage::Public,
                                         gradNameId.str(), gradType,
                                         original->getGenericEnvironment(),
                                         original->getLocation(),
                                         original->isBare(),
                                         original->isTransparent(),
                                         original->isSerialized());
    gradFn->setDebugScope(
      new (module) SILDebugScope(original->getLocation(), gradFn));
    return gradFn;
  };

  // Step 1: Make sure the `[differentiable]` attribute exists. Based on this
  // attribute, create a differentiation task.
  SILReverseDifferentiableAttr *attr =
    context.getOrCreateReverseDifferentiableAttr(original,
                                                 config.parameterIndices);
  DifferentiationTask newTask { original, attr };
  // Update config's parameter indices to not depend on GradientInst's storage
  // because it will be removed.
  config.parameterIndices = attr->getParamIndices();

  // Step 2: Get or create a seedable, result-preserving gradient function. If
  // this function exists, return it.
  SILFunction *canonicalGrad = nullptr;
  // If it already exists, we'll simply use the existing one.
  if (auto *existingGrad = context.lookupGradient({original, config}))
    canonicalGrad = existingGrad;
  // Otherwise, create an empty function and push a differentiation task to the
  // worklist.
  else {
    auto masterConfig =
      SILReverseAutoDiffConfiguration::getMaster(config.parameterIndices);
    canonicalGrad = createGradFunction(masterConfig);
    // Cache the canonical gradient.
    context.insertGradient({original, masterConfig}, canonicalGrad);
    worklist.push_back(newTask);
  }

  // Step 3. If the requested gradient is not *both seedable and
  // result-preserving*, emit wrapper function, emit a call to the canonical
  // gradient function inside, and cache it.
  SILFunction *gradFn = nullptr;
  if (config.isMaster())
    gradFn = canonicalGrad;
  else if (auto *existingGradFn = context.lookupGradient({original, config}))
    gradFn = existingGradFn;
  else {
    gradFn = createGradFunction(config);
    // Create entry basic block.
    auto *entry = gradFn->createBasicBlock();
    createEntryArguments(gradFn);
    // Build a call to the canonical gradient function.
    SILBuilder builder(entry);
    auto loc = gradFn->getLocation();
    SILFunctionConventions gradConv(gradFn->getLoweredFunctionType(), module),
                           origConv(origTy, module),
                           canGradConv(canonicalGrad->getLoweredFunctionType(),
                                       module);

    SmallVector<SILValue, 8> args;
    SmallVector<SILValue, 1> stackAllocsToCleanUp;
    // Prepare arguments.
    // The first few arguments are the original arguments.
    for (auto arg : gradFn->getArguments())
      args.push_back(arg);
    // If it's not seedable, we need to create a default seed.
    if (!config.seedable) {
      auto seedTy = origTy->getSingleResult().getType();
      auto seedSILTy = SILType::getPrimitiveObjectType(seedTy);
      auto *seedTyDecl = seedTy->getAnyNominal();
      // Call `<seed type>.init(differentiationSeed: 1)` to create a default
      // seed to feed into the canonical gradient.
      auto *seedBuf = builder.createAllocStack(loc, seedSILTy);
      convertToIndirectSeed(1, seedTyDecl, seedBuf, loc, builder, context);
      // If seed is address only, we'll clean up the buffer after calling the
      // canonical gradient Otherwise, we just load the seed and deallocate the
      // buffer.
      if (seedSILTy.isAddressOnly(module)) {
        stackAllocsToCleanUp.push_back(seedBuf);
      } else {
        auto loq = seedSILTy.isTrivial(module)
          ? LoadOwnershipQualifier::Trivial : LoadOwnershipQualifier::Take;
        auto seed = builder.createLoad(loc, seedBuf, loq);
        args.push_back(seed);
        builder.createDeallocStack(loc, seedBuf);
      }
    }
    // Call the canonical gradient function.
    // %0 = function_ref ...
    auto *canGradFnRef = builder.createFunctionRef(loc, canonicalGrad);
    SubstitutionList substList;
    if (auto *genEnv = gradFn->getGenericEnvironment())
      substList = genEnv->getForwardingSubstitutions();
    // %1 = apply %0(...)
    auto *resultAndGrad = builder.createApply(loc, canGradFnRef, substList,
                                              args, /*isNonThrowing*/ false);
    // Clean up stack allocations made by seed passing when seed is addr-only.
    for (auto alloc : stackAllocsToCleanUp)
      builder.createDeallocStack(loc, alloc);
    // If the config is result-preserving, or if the original result is
    // indirect, we can just return whatever direct results the canonical
    // gradient produces.
    if (config.preservingResult ||
        canGradConv.getResults()[0].isFormalIndirect()) {
      builder.createReturn(loc, resultAndGrad);
    }
    // Otherwise, return every tuple element of `resultAndGrad` except the
    // first. For this, we have to build a bunch of `tuple_extract`s and
    // re-gather them using `tuple`.
    else {
      unsigned numDirResults = canGradConv.getNumDirectSILResults();
      SILValue result;
      if (numDirResults == 2)
        result = builder.createTupleExtract(loc, resultAndGrad, 1);
      else {
        SmallVector<SILValue, 8> grads;
        for (unsigned i = 1; i != numDirResults; ++i)
          grads.push_back(builder.createTupleExtract(loc, resultAndGrad, i));
        result = builder.createTuple(loc, gradConv.getSILResultType(), grads);
      }
      builder.createReturn(loc, result);
    }

    // Cache the newly created gradient wrapper (non-canonical).
    context.insertGradient({original, config}, gradFn);
  }

  return gradFn;
}

/// Finish the canonical gradient function.
///
/// For the following original function type:
///   (a1, a2, ..., an) -> r
///
/// The canonical gradient has the following type:
///   (a1, a2, ..., an, seed) -> (r, a1, a0, ..., an)
///
/// In the canonical gradient function, we simply call the primal and the
/// adjoint, and return a tuple of the original result and the gradient values.
static void fillCanonicalGradient(SILFunction &canGrad,
                                  DifferentiationTask task,
                                  ADContext &context) {
  assert(canGrad.empty() && "The gradient function must be empty");
  auto &module = context.getModule();
  auto canGradTy = canGrad.getLoweredFunctionType();
  auto loc = canGrad.getLocation();
  auto *primal = context.lookupPrimal(task);
  assert(primal && "Primal does not exist?");
  auto primalTy = primal->getLoweredFunctionType();
  auto *adjoint = context.lookupAdjoint(task);
  assert(adjoint && "Adjoint does not exist?");
  auto adjointTy = adjoint->getLoweredFunctionType();
  SILFunctionConventions primalConv(primalTy, module),
                         adjointConv(adjointTy, module),
                         canGradConv(canGradTy, module);
  // Create an entry basic block.
  auto *entry = canGrad.createBasicBlock();
  createEntryArguments(&canGrad);
  // Initialize arguments.
  SILBuilder builder(entry);
  // Call primal with original arguments.
  SmallVector<SILValue, 8> stackAllocsToCleanUp;
  SmallVector<SILValue, 8> primalArgs;
  // Add indirect results.
  for (auto indResInfo : primalTy->getIndirectFormalResults()) {
    auto objTy = SILType::getPrimitiveObjectType(indResInfo.getType());
    auto resultBuf = builder.createAllocStack(loc, objTy);
    stackAllocsToCleanUp.push_back(resultBuf);
    primalArgs.push_back(resultBuf);
  }
  // Add original parameters. These are the canonical gradient's parameter
  // arguments except the seed, which is the last argument.
  for (auto arg : canGrad.getArgumentsWithoutIndirectResults().drop_back())
    primalArgs.push_back(arg);
  // %0 = function_ref @primal
  auto *primalRef = builder.createFunctionRef(loc, primal);
  // %1 = apply %0(...)
  auto *primalApply = builder.createApply(loc, primalRef,
                                          canGrad.getForwardingSubstitutions(),
                                          primalArgs, /*isNonThrowing*/ false);
  // Collect primal results (direct and indirect) in the original order, to be
  // ready to pass them to the adjoint.
  // TODO: Does `SILInstruction::getResults` give all results, including the
  // indirect ones?
  auto primalResults = primalApply->getResults();

  // Call adjoint with original arguments, the checkpoints value and the seed.
  SmallVector<SILValue, 8> adjointArgs;
  // Add indirect results and original parameters. These are the canonical
  // gradient's arguments except the seed, which is the last argument.
  for (auto arg : canGrad.getArguments().drop_back())
    adjointArgs.push_back(arg);
  // Add primal checkpoints.
  adjointArgs.append(primalResults.begin(), primalResults.end());
  // Add seed.
  adjointArgs.push_back(canGrad.getArguments().back());
  // %2 = function_ref @adjoint
  auto *adjRef = builder.createFunctionRef(loc, adjoint);
  // %3 = apply %2(...)
  auto *adjApply = builder.createApply(loc, adjRef,
                                       canGrad.getForwardingSubstitutions(),
                                       adjointArgs, /*isNonThrowing*/ false);
  // Clean up stack allocations.
  for (auto val : reversed(stackAllocsToCleanUp))
    builder.createDeallocStack(loc, val);
  // Return the original result and the adjoint result as a tuple. If either one
  // of the primal or the adjoint returns a tuple, join them in a flat tuple.
  //
  // NOTE: Currently we don't have a formal way to extract the primal result
  // from the checkpoints value, which can be a value of some nominal type, or a
  // tuple. The best way would be separating the original result from
  // checkpoints and also passing them separately to the adjoint. Today, we just
  // assume that the primal result is the checkpoints value. This only works for
  // primitive functions unfortunately.
  SmallVector<SILValue, 8> directResults = { primalApply };
  if (adjointConv.getNumDirectSILResults() == 1)
    directResults.push_back(adjApply);
  else {
    auto adjointElems =
      builder.createDestructureTuple(loc, adjApply)->getResults();
    directResults.append(adjointElems.begin(), adjointElems.end());
  }
  auto tupleRet =
    builder.createTuple(loc, canGradConv.getSILResultType(), directResults);
  builder.createReturn(loc, tupleRet);
}

/// The automatic differentiation pass.
namespace {
class Differentiation : public SILModuleTransform {
public:
  Differentiation() : SILModuleTransform() {}
  void run() override;
};
} // end anonymous namespace

/// AD pass entry.
void Differentiation::run() {
  auto &module = *getModule();

  // Collect gradient instructions to process.
  SmallVector<GradientInst *, 16> gradInsts;
  // Handle each `gradient` instruction in the module.
  for (SILFunction &f : module)
    for (SILBasicBlock &bb : f)
      for (SILInstruction &i : bb)
        if (auto *gi = dyn_cast<GradientInst>(&i))
          gradInsts.push_back(gi);

  // If there's no `gradient` instruction, there's no AD to do.
  if (gradInsts.empty()) return;

  // AD relies on stdlib (the Swift module). If it's not imported, it's an
  // internal error.
  if (!module.getSwiftModule()->getASTContext().getStdlibModule()) {
    llvm::errs() <<
      "Internal error: AD depends on the Swift module but it's not imported.\n";
    return;
  }

  // A global differentiation context.
  ADContext context(module, *PM);

  // A list of differentiation tasks, each of which specifies the
  // differentiation job to perform.
  SmallVector<DifferentiationTask, 16> worklist;

  // Lower each gradient instruction to a function reference and replaces its
  // uses with a function reference to its gradient.
  //
  // If the operand to the instruction traces back to a function reference that
  // the compiler can see into, then we do further processing, i.e. retrieving
  // or creating its gradient. Otherwise, it's differentiating an opaque
  // function whose body isn't visible to the compiler. We don't have
  // infrastructure support for this yet and currently it'll error out, but
  // we'll look into adding a new function convention so that the primal and the
  // adjoint can be passed along with the function.
  for (auto *gi : gradInsts) {
    SILFunction *parent = gi->getFunction();
    auto operand = gi->getOperand(0);
    SILFunction *gradFn = nullptr;
    // If it traces back to a `function_ref`, differentiate that.
    if (auto *original = findReferencedFunction(operand))
      gradFn = getOrCreateGradient(context, gi, original, worklist);
    // Differentiating opaque functions is not supported yet.
    else {
      if (auto loc = gi->getLoc()) {
        auto *expr = loc.castToASTNode<ReverseAutoDiffExpr>();
        context.diagnose(expr->getOriginalExpr()->getLoc(),
                         diag::autodiff_opaque_function_unsupported);
      }
      context.setErrorOccurred();
    }

    // If `gradFn` is still null, errors must have occurred.
    if (!gradFn) return;

    // Replace the `gradient` instruction with the reference to the specified
    // function.
    SILBuilder builder(gi);
    auto *gradFnRef = builder.createFunctionRef(gi->getLoc(), gradFn);
    gi->replaceAllUsesWith(gradFnRef);
    gi->dropAllReferences();
    gi->eraseFromParent();
    // We invalide analyses on the parent function because the `gradient`
    // instruciton is transformed.
    PM->invalidateAnalysis(parent, SILAnalysis::InvalidationKind::FunctionBody);
  }

  // If there were any error, back out.
  if (context.hasErrorOccurred())
    return;

  // Run primal generation.
  PrimalGen primalGen(worklist, context);
  PrimalGen::Result primalInfos;
  primalGen.generate(primalInfos);

  // If there were any error, back out.
  if (context.hasErrorOccurred())
    return;

  // Run adjoint generation.
  AdjointGen adjointGen(worklist, context, primalInfos);
  adjointGen.generate();

  // If there were any error, back out.
  if (context.hasErrorOccurred())
    return;

  // Fill the body of the empty canonical gradient function corresponding to
  // each differentiation task, and we are done!
  for (auto &task : worklist) {
    auto *canGradFn = context.lookupCanonicalGradient(task);
    assert(canGradFn && "Cannot find the canonical gradient function");
    fillCanonicalGradient(*canGradFn, task, context);
  }
}

//===----------------------------------------------------------------------===//
// Pass registration requirements
//===----------------------------------------------------------------------===//

SILTransform *swift::createDifferentiation() {
  return new Differentiation;
}