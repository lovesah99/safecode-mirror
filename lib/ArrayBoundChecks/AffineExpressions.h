//===- llvm/ArrayBoundChecks/AffineExpressions.h  - Expression Analysis Utils ---*- C++ -*--=//
//
// This file defines a package of expression analysis utilties:
//

// 
//===----------------------------------------------------------------------===//

#ifndef LLVM_C_EXPRESSIONS_H
#define LLVM_C_EXPRESSIONS_H

#include <assert.h>
#include <map>
#include <list>
#include <string>
#include "llvm/Instruction.h"
#include "llvm/iMemory.h"
#include "llvm/InstrTypes.h"
#include "llvm/iTerminators.h"
#include "llvm/iOther.h"
#include "llvm/iPHINode.h"
#include "llvm/iOperators.h"
#include "llvm/Constants.h"
#include "llvm/Analysis/PostDominators.h"
#include "Support/StringExtras.h"
#include "llvm/SlotCalculator.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Analysis/InductionVariable.h"

using namespace std;

class Value;
class BasicBlock;
class Function;

namespace cfg { class LoopInfo; }

typedef std::map<const PHINode *,InductionVariable*> IndVarMap;
typedef std::map<const Function *,BasicBlock *> ExitNodeMap;
typedef std::map<const Function *, PostDominanceFrontier *> PostDominanceFrontierMap;

typedef std::map<const Value*,int> CoefficientMap;
typedef std::map<const Value*,string> ValStringMap;
typedef std::list<const Value*> VarList;
typedef std::list<const Value*>::const_iterator VarListIt;
typedef std::list<const CallInst*> CallInstList;
typedef std::list<const CallInst*>::const_iterator CallInstListIt;
typedef std::list<const Instruction*> MemAccessInstListType;
typedef std::list<const Instruction*>::const_iterator  MemAccessInstListIt;

// LinearExpr - Represent an expression of the form CONST*VAR1+CONST*VAR2+ ..... 
// or simpler.  


class LinearExpr {

  int offSet;
  VarList* vList;
  CoefficientMap* cMap;
  ValStringMap* vsMap;
public:
enum ExpressionType {
    Linear,        // Expr is linear
    Unknown
  } exprTy;
  inline int getOffSet() { return offSet; };
  inline void setOffSet(int offset) {  offSet = offset; };
  inline ExpressionType getExprType() { return exprTy; };
  inline VarList* getVarList() { return vList; };
  inline CoefficientMap* getCMap() { return cMap; };
  inline ValStringMap* getVSMap() { return vsMap; };

  LinearExpr(const Value *Val,SlotCalculator &Tab);        // Create a linear expression
  void negate();
  void addLinearExpr(LinearExpr *);
  LinearExpr * mulLinearExpr(LinearExpr *);
  void mulByConstant(int);
  void print(ostream &out);
};

class Constraint {
  string var;
  LinearExpr *le;
  string rel; // can be < ,  >, <=, >= for now
public :
  Constraint(string v, LinearExpr *l, string r) {
    assert(l != 0 && "the rhs for this constraint is null");
    var = v;
    le = l;
    rel = r;
  }
  void print(ostream &out);
};



//This holds a set of ABCExprs 
class ABCExprTree {
  Constraint *constraint;
  ABCExprTree *right;
  ABCExprTree *left;
  string logOp; // can be && or || or 
 public:
  ABCExprTree(Constraint *c) {
    constraint = c;
    left  = 0;
    right = 0;
    logOp = "&&";
  };
  ABCExprTree(ABCExprTree *l, ABCExprTree *r, string op) {
    constraint = 0;
    left = l;
    right = r;
    logOp = op;
  }
  void print(ostream &out);
};

typedef std::map<const Value *, ABCExprTree *> InstConstraintMapType;



//This holds the local information of a function
class FuncLocalInfo {
  //Local cache for constraints
  InstConstraintMapType FuncLocalConstraints;

  //Storing all constraints which need proving 
  InstConstraintMapType FuncSafetyConstraints;

  //All array accesses in a function 
  MemAccessInstListType maiList;
  //This stores the Or of the arguments at
  //various call sites, so that can be computed only once for
  //different array accesses. 
  ABCExprTree *argConstraints;

  bool dependsOnArg;
public :
  FuncLocalInfo() {
    dependsOnArg = false;
    argConstraints = 0;
  }

  inline void setdependsOnArg() {
    dependsOnArg  = true;
  }

  inline bool isDependentOnArg() {
    return dependsOnArg;
  }

  inline void addMemAccessInst(Instruction *MAI) {
    maiList.push_back(MAI);
  }

  inline void addLocalConstraint(const Value *v, ABCExprTree *aet) {
    FuncLocalConstraints[v] = aet;
  }
  inline bool inLocalConstraints(const Value *v) {
    return (FuncLocalConstraints.count(v) > 0);
  }
  inline ABCExprTree * getLocalConstraint(const Value *v) {
    if (FuncLocalConstraints.count(v)) return FuncLocalConstraints[v];
    else return 0;
  }
  inline void addSafetyConstraint(const Value *v, ABCExprTree *aet) {
    FuncSafetyConstraints[v] = aet;
  }
  inline ABCExprTree* getSafetyConstraint(const Value *v) {
    return (FuncSafetyConstraints[v]);
  }
  inline MemAccessInstListType getMemAccessInstList() {
    return maiList;
  }

  inline void addArgumentConstraints(ABCExprTree *aet) {
    argConstraints = aet;
  }
  inline ABCExprTree * getArgumentConstraints() {
    return argConstraints;
  }
};

// We dont want identifier names with ., space, -  in them. 
// So we replace them with _
static string makeNameProper(string x) {
  string tmp;
  int len = 0;
  for (string::iterator sI = x.begin(), sEnd = x.end(); sI != sEnd; sI++) {
    if (len > 18) return tmp; //have to do something more meaningful
    len++;
    switch (*sI) {
    case '.': tmp += "d_"; len++;break;
    case ' ': tmp += "s_"; len++;break;
    case '-': tmp += "D_"; len++;break;
    case '_': tmp += "l_"; len++;break;
    default:  tmp += *sI;
    }
  }
  if (tmp == "in") return "in__1";
  return tmp;
 };


static string getValueNames( Value *V, SlotCalculator *Table) {
  if ( Constant *CPI = dyn_cast<Constant>(V)) {
    if ( ConstantSInt *CPI = dyn_cast<ConstantSInt>(V)) {
      return itostr(CPI->getValue());
    } else if ( ConstantUInt *CPI = dyn_cast<ConstantUInt>(V)) {
      return utostr(CPI->getValue());
    }
  }
  if (V->hasName()) {
    return makeNameProper(V->getName());
  }
  else {
    int Slot = Table->getValSlot(V);
    //    assert(Slot >= 0 && "Invalid value!");
    if (Slot >= 0) {
      return "l_" + itostr(Slot) + "_" + utostr(V->getType()->getUniqueID());
    } else return "Unknown";
  }
};



#endif
