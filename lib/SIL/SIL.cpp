//===--- SIL.cpp - Implements random SIL functionality --------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILUndef.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Mangle.h"
#include "swift/AST/Pattern.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Basic/Fallthrough.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
using namespace swift;

void ValueBase::replaceAllUsesWith(ValueBase *RHS) {
  assert(this != RHS && "Cannot RAUW a value with itself");
  assert(getNumTypes() == RHS->getNumTypes() &&
         "An instruction and the value base that it is being replaced by "
         "must have the same number of types");

  while (!use_empty()) {
    Operand *Op = *use_begin();
    Op->set(SILValue(RHS, Op->get().getResultNumber()));
  }
}


SILUndef *SILUndef::get(SILType Ty, SILModule *M) {
  // Unique these.
  SILUndef *&Entry = M->UndefValues[Ty];
  if (Entry == nullptr)
    Entry = new (*M) SILUndef(Ty);
  return Entry;
}



static unsigned getFuncNaturalUncurryLevel(AnyFunctionRef AFR) {
  assert(AFR.getBodyParamPatterns().size() >= 1 && "no arguments for func?!");
  unsigned Level = AFR.getBodyParamPatterns().size() - 1;
  // Functions with captures have an extra uncurry level for the capture
  // context.
  if (AFR.getCaptureInfo().hasLocalCaptures())
    Level += 1;
  return Level;
}

SILDeclRef::SILDeclRef(ValueDecl *vd, SILDeclRef::Kind kind,
                       unsigned atUncurryLevel, bool isForeign)
  : loc(vd), kind(kind), isForeign(isForeign), defaultArgIndex(0)
{
  unsigned naturalUncurryLevel;

  // FIXME: restructure to use a "switch".

  if (auto *func = dyn_cast<FuncDecl>(vd)) {
    assert(kind == Kind::Func &&
           "can only create a Func SILDeclRef for a func decl");
    naturalUncurryLevel = getFuncNaturalUncurryLevel(func);
  } else if (isa<ConstructorDecl>(vd)) {
    assert((kind == Kind::Allocator || kind == Kind::Initializer)
           && "can only create Allocator or Initializer SILDeclRef for ctor");
    naturalUncurryLevel = 1;
  } else if (auto *ed = dyn_cast<EnumElementDecl>(vd)) {
    assert(kind == Kind::EnumElement
           && "can only create EnumElement SILDeclRef for enum element");
    naturalUncurryLevel = ed->hasArgumentType() ? 1 : 0;
  } else if (isa<DestructorDecl>(vd)) {
    assert((kind == Kind::Destroyer || kind == Kind::Deallocator)
           && "can only create destroyer/deallocator SILDeclRef for dtor");
    naturalUncurryLevel = 0;
  } else if (isa<ClassDecl>(vd)) {
    assert((kind == Kind::IVarInitializer || kind == Kind::IVarDestroyer) &&
           "can only create ivar initializer/destroyer SILDeclRef for class");
    naturalUncurryLevel = 1;
  } else if (auto *var = dyn_cast<VarDecl>(vd)) {
    assert(kind == Kind::GlobalAccessor &&
           "can only create GlobalAccessor SILDeclRef for var");

    naturalUncurryLevel = 0;
    assert(!var->getDeclContext()->isLocalContext() &&
           "can't reference local var as global var");
    assert(var->hasStorage() && "can't reference computed var as global var");
  } else {
    llvm_unreachable("Unhandled ValueDecl for SILDeclRef");
  }
  
  assert((atUncurryLevel == ConstructAtNaturalUncurryLevel
          || atUncurryLevel <= naturalUncurryLevel)
         && "can't emit SILDeclRef below natural uncurry level");
  uncurryLevel = atUncurryLevel == ConstructAtNaturalUncurryLevel
    ? naturalUncurryLevel
    : atUncurryLevel;
  isCurried = uncurryLevel != naturalUncurryLevel;
}

SILDeclRef::SILDeclRef(SILDeclRef::Loc baseLoc, unsigned atUncurryLevel,
                       bool asForeign) 
 : defaultArgIndex(0)
{
  unsigned naturalUncurryLevel;
  if (ValueDecl *vd = baseLoc.dyn_cast<ValueDecl*>()) {
    if (FuncDecl *fd = dyn_cast<FuncDecl>(vd)) {
      // Map FuncDecls directly to Func SILDeclRefs.
      loc = fd;
      kind = Kind::Func;
      naturalUncurryLevel = getFuncNaturalUncurryLevel(fd);
    }
    // Map ConstructorDecls to the Allocator SILDeclRef of the constructor.
    else if (ConstructorDecl *cd = dyn_cast<ConstructorDecl>(vd)) {
      loc = cd;
      kind = Kind::Allocator;
      naturalUncurryLevel = 1;

      // FIXME: Should we require the caller to think about this?
      asForeign = false;
    }
    // Map EnumElementDecls to the EnumElement SILDeclRef of the element.
    else if (EnumElementDecl *ed = dyn_cast<EnumElementDecl>(vd)) {
      loc = ed;
      kind = Kind::EnumElement;
      naturalUncurryLevel = ed->hasArgumentType() ? 1 : 0;
    }
    // VarDecl constants require an explicit kind.
    else if (isa<VarDecl>(vd)) {
      llvm_unreachable("must create SILDeclRef for VarDecl with explicit kind");
    }
    // Map DestructorDecls to the Deallocator of the destructor.
    else if (auto dtor = dyn_cast<DestructorDecl>(vd)) {
      loc = dtor;
      kind = Kind::Deallocator;
      naturalUncurryLevel = 0;
    }
    else {
      llvm_unreachable("invalid loc decl for SILDeclRef!");
    }
  } else if (auto *ACE = baseLoc.dyn_cast<AbstractClosureExpr *>()) {
    loc = ACE;
    kind = Kind::Func;
    assert(ACE->getParamPatterns().size() >= 1 &&
           "no param patterns for function?!");
    naturalUncurryLevel = getFuncNaturalUncurryLevel(ACE);
  } else {
    llvm_unreachable("impossible SILDeclRef loc");
  }

  // Set the uncurry level.
  assert((atUncurryLevel == ConstructAtNaturalUncurryLevel
          || atUncurryLevel <= naturalUncurryLevel)
         && "can't emit SILDeclRef below natural uncurry level");
  uncurryLevel = atUncurryLevel == ConstructAtNaturalUncurryLevel
    ? naturalUncurryLevel
    : atUncurryLevel;
  
  isCurried = uncurryLevel != naturalUncurryLevel;  
  isForeign = asForeign;
}

SILDeclRef SILDeclRef::getDefaultArgGenerator(Loc loc,
                                              unsigned defaultArgIndex) {
  SILDeclRef result;
  result.loc = loc;
  result.kind = Kind::DefaultArgGenerator;
  result.defaultArgIndex = defaultArgIndex;
  return result;
}

/// \brief True if the function should be treated as transparent.
bool SILDeclRef::isTransparent() const {
  if (isEnumElement())
    return true;

  return hasDecl() ? getDecl()->isTransparent() : false;
}

bool SILDeclRef::isForeignThunk() const {
  // Non-decl entry points are never thunks.
  if (!hasDecl())
    return false;
  // Otherwise, match whether we have a clang node with whether we're foreign.
  if (isa<FuncDecl>(getDecl()) && getDecl()->hasClangNode())
    return !isForeign;
  return false;
}

static void mangleConstant(SILDeclRef c, llvm::raw_ostream &buffer,
                           ResilienceExpansion expansion) {
  using namespace Mangle;
  Mangler mangler(buffer);

  // Almost everything below gets one of the common prefixes:
  //   mangled-name ::= '_T' global     // Native symbol
  //   mangled-name ::= '_TTo' global   // ObjC interop thunk
  //   mangled-name ::= '_TTO' global   // Foreign function thunk
  StringRef introducer = "_T";
  if (c.isForeign)
    introducer = "_TTo";
  else if (c.isForeignThunk())
    introducer = "_TTO";
  
  switch (c.kind) {
  //   entity ::= declaration                     // other declaration
  case SILDeclRef::Kind::Func:
    if (!c.hasDecl()) {
      buffer << introducer;
      mangler.mangleClosureEntity(c.getAbstractClosureExpr(),
                                  expansion,
                                  c.uncurryLevel);
      return;
    }

    // As a special case, functions can have external asm names.
    // Use the asm name only for the original non-thunked, non-curried entry
    // point.
    if (!c.getDecl()->getAttrs().AsmName.empty()
        && !c.isForeignThunk() && !c.isCurried) {
      buffer << c.getDecl()->getAttrs().AsmName;
      return;
    }
      
    if (auto *FD = dyn_cast<FuncDecl>(c.getDecl())) {
      // Accessors are mangled specially.

      //   entity ::= declaration 'g'                 // getter
      //   entity ::= declaration 's'                 // setter
      //   entity ::= declaration 'w'                 // willSet
      //   entity ::= declaration 'W'                 // didSet
      char AccessorLetter;
      switch (FD->getAccessorKind()) {
      case AccessorKind::NotAccessor: AccessorLetter = '\0'; break;
      case AccessorKind::IsGetter:    AccessorLetter = 'g'; break;
      case AccessorKind::IsSetter:    AccessorLetter = 's'; break;
      case AccessorKind::IsWillSet:   AccessorLetter = 'w'; break;
      case AccessorKind::IsDidSet:    AccessorLetter = 'W'; break;
      }

      if (AccessorLetter) {
        buffer << introducer;
        mangler.mangleAccessorEntity(AccessorLetter,
                                     FD->getAccessorStorageDecl(),
                                     expansion);
        return;
      }
    }

    // Otherwise, fall through into the 'other decl' case.
    SWIFT_FALLTHROUGH;

  case SILDeclRef::Kind::EnumElement:
    // As a special case, Clang functions and globals don't get mangled at all.
    // FIXME: When we can import C++, use Clang's mangler.
    if (auto clangDecl = c.getDecl()->getClangDecl()) {
      if (!c.isForeignThunk() && !c.isCurried) {
        if (auto namedClangDecl = dyn_cast<clang::DeclaratorDecl>(clangDecl)) {
          if (auto asmLabel = namedClangDecl->getAttr<clang::AsmLabelAttr>()) {
            buffer << '\01' << asmLabel->getLabel();
          } else {
            buffer << namedClangDecl->getName();
          }
          return;
        }
      }
    }

    buffer << introducer;
    mangler.mangleEntity(c.getDecl(), expansion, c.uncurryLevel);
    return;
      
  //   entity ::= context 'D'                     // deallocating destructor
  case SILDeclRef::Kind::Deallocator:
    buffer << introducer;
    mangler.mangleDestructorEntity(cast<DestructorDecl>(c.getDecl()),
                                   /*isDeallocating*/ true);
    return;

  //   entity ::= context 'd'                     // destroying destructor
  case SILDeclRef::Kind::Destroyer:
    buffer << introducer;
    mangler.mangleDestructorEntity(cast<DestructorDecl>(c.getDecl()),
                                   /*isDeallocating*/ false);
    return;

  //   entity ::= context 'C' type                // allocating constructor
  case SILDeclRef::Kind::Allocator:
    buffer << introducer;
    mangler.mangleConstructorEntity(cast<ConstructorDecl>(c.getDecl()),
                                    /*allocating*/ true,
                                    expansion,
                                    c.uncurryLevel);
    return;

  //   entity ::= context 'c' type                // initializing constructor
  case SILDeclRef::Kind::Initializer:
    buffer << introducer;
    mangler.mangleConstructorEntity(cast<ConstructorDecl>(c.getDecl()),
                                    /*allocating*/ false,
                                    expansion,
                                    c.uncurryLevel);
    return;

  //   entity ::= declaration 'e'                 // ivar initializer
  //   entity ::= declaration 'E'                 // ivar destroyer
  case SILDeclRef::Kind::IVarInitializer:
  case SILDeclRef::Kind::IVarDestroyer:
    buffer << introducer;
    mangler.mangleIVarInitDestroyEntity(
      cast<ClassDecl>(c.getDecl()),
      c.kind == SILDeclRef::Kind::IVarDestroyer);
    return;

  //   entity ::= declaration 'a'                 // addressor
  case SILDeclRef::Kind::GlobalAccessor:
    buffer << introducer;
    mangler.mangleAddressorEntity(c.getDecl());
    return;

  //   entity ::= context 'e' index           // default arg generator
  case SILDeclRef::Kind::DefaultArgGenerator:
    buffer << introducer;
    mangler.mangleDefaultArgumentEntity(cast<AbstractFunctionDecl>(c.getDecl()),
                                        c.defaultArgIndex);
    return;
  }

  llvm_unreachable("bad entity kind!");
}

StringRef SILDeclRef::mangle(SmallVectorImpl<char> &buffer,
                             ResilienceExpansion expansion) const {
  assert(buffer.empty());
  llvm::raw_svector_ostream stream(buffer);
  mangleConstant(*this, stream, expansion);
  return stream.str();
}

static FormalLinkage getGenericClauseLinkage(ArrayRef<GenericParam> params) {
  FormalLinkage result = FormalLinkage::Top;
  for (auto &param : params) {
    for (auto proto : param.getAsTypeParam()->getProtocols())
      result ^= getTypeLinkage(CanType(proto->getDeclaredType()));
    if (auto superclass = param.getAsTypeParam()->getSuperclass())
      result ^= getTypeLinkage(superclass->getCanonicalType());
  }
  return result;
}

FormalLinkage swift::getDeclLinkage(Decl *D) {
  DeclContext *DC = D->getDeclContext();
  while (!DC->isModuleScopeContext()) {
    if (DC->isLocalContext())
      return FormalLinkage::Private;
    DC = DC->getParent();
  }

  // Clang declarations are public and can't be assured of having a
  // unique defining location.
  if (isa<ClangModuleUnit>(DC))
    return FormalLinkage::PublicNonUnique;

  // TODO: access control
  return FormalLinkage::PublicUnique;
}

FormalLinkage swift::getTypeLinkage(CanType type) {
  FormalLinkage result = FormalLinkage::Top;

  // Merge all nominal types from the structural type.
  (void) type.findIf([&](Type _type) {
    CanType type = CanType(_type);

    // For any nominal type reference, look at the type declaration.
    if (auto nominal = type->getAnyNominal()) {
      result ^= getDeclLinkage(nominal);

    // For polymorphic function types, look at the generic parameters.
    // FIXME: findIf should do this, once polymorphic function types can be
    // canonicalized and re-formed properly.
    } else if (auto polyFn = dyn_cast<PolymorphicFunctionType>(type)) {
      result ^= getGenericClauseLinkage(polyFn->getGenericParameters());
    }

    return false; // continue searching
  });

  return result;
}

FormalLinkage swift::getConformanceLinkage(const ProtocolConformance *conf) {
  // FIXME
  return FormalLinkage::PublicUnique;
}

/// Returns true if we are able to find an address projection path from V1 to
/// V2. Inserts the found path into Path.
bool
swift::
findAddressProjectionPathBetweenValues(SILValue V1, SILValue V2,
                                       SmallVectorImpl<Projection> &Path) {
  // If V1 == V2, there is a "trivial" address projection in between the
  // two. This is represented by returning true, but putting nothing into Path.
  if (V1 == V2)
    return true;

  // Otherwise see if V2 can be projection extracted from V1. First see if
  // V2 is a projection at all.
  auto Iter = V2;
  while (Projection::isAddressProjection(Iter) && V1 != Iter) {
    if (auto *SEA = dyn_cast<StructElementAddrInst>(Iter.getDef()))
      Path.push_back(Projection(SEA));
    else if (auto *TEA = dyn_cast<TupleElementAddrInst>(Iter.getDef()))
      Path.push_back(Projection(TEA));
    else
      Path.push_back(Projection(cast<RefElementAddrInst>(&*Iter)));
    Iter = cast<SILInstruction>(*Iter).getOperand(0);
  }

  // Return true if we have a non-empty projection list and if V1 == Iter.
  return !Path.empty() && V1 == Iter;
}
