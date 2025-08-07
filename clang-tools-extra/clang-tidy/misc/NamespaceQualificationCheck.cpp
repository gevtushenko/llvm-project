//===--- NamespaceQualificationCheck.cpp - clang-tidy ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NamespaceQualificationCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang::ast_matchers;

namespace clang::tidy::misc {

NamespaceQualificationCheck::NamespaceQualificationCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      RequireGlobalIn(Options.get("RequireGlobalIn", "")),
      ForbidGlobalIn(Options.get("ForbidGlobalIn", "")) {
  // Parse 'Namespaces' as a ';' separated list; fall back to single 'Namespace'.
  llvm::StringRef Multi = Options.get("Namespaces", "");
  if (!Multi.empty()) {
    llvm::SmallVector<llvm::StringRef, 8> Parts;
    Multi.split(Parts, ';', /*MaxSplit*/ -1, /*KeepEmpty*/ false);
    for (auto P : Parts)
      TargetNamespaces.emplace_back(P.str());
  }
  if (TargetNamespaces.empty()) {
    TargetNamespaces.emplace_back(Options.get("Namespace", "cuda").str());
  }
  
}

void NamespaceQualificationCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  if (!TargetNamespaces.empty()) {
    std::string Joined;
    for (size_t i = 0; i < TargetNamespaces.size(); ++i) {
      if (i)
        Joined += ';';
      Joined += TargetNamespaces[i];
    }
    Options.store(Opts, "Namespaces", Joined);
    Options.store(Opts, "Namespace", TargetNamespaces.front());
  }
  Options.store(Opts, "RequireGlobalIn", RequireGlobalIn.getItems().empty()
                                       ? ""
                                       : Options.get("RequireGlobalIn", ""));
  Options.store(Opts, "ForbidGlobalIn", ForbidGlobalIn.getItems().empty()
                                      ? ""
                                      : Options.get("ForbidGlobalIn", ""));
}

void NamespaceQualificationCheck::registerMatchers(MatchFinder *Finder) {
  if (!(getLangOpts().CPlusPlus || getLangOpts().CUDA))
    return;

  // Match all nested name specifier locations
  Finder->addMatcher(nestedNameSpecifierLoc().bind("NNS"), this);
  
  // Also match the TU to allow a lexical fallback if AST matching is not
  // producing results in some dialects.
  Finder->addMatcher(translationUnitDecl().bind("TU"), this);
}

bool NamespaceQualificationCheck::hasLeadingGlobal(
    const NestedNameSpecifierLoc &Loc) {
  for (NestedNameSpecifierLoc P = Loc.getPrefix(); P; P = P.getPrefix()) {
    if (P.getNestedNameSpecifier()->getKind() ==
        NestedNameSpecifier::Global)
      return true;
  }
  return false;
}

bool NamespaceQualificationCheck::isRootTargetNamespace(
    const NestedNameSpecifierLoc &Loc) const {
  // Find the leftmost non-global namespace in the chain
  NestedNameSpecifierLoc Current = Loc;
  NestedNameSpecifierLoc LeftmostNamespace;
  
  // Walk backwards to find all namespaces
  while (Current) {
    auto *NNS = Current.getNestedNameSpecifier();
    if (NNS && NNS->getKind() == NestedNameSpecifier::Namespace) {
      LeftmostNamespace = Current;
    }
    Current = Current.getPrefix();
  }
  
  if (!LeftmostNamespace)
    return false;
    
  // Check if the leftmost namespace matches any target
  auto *NNS = LeftmostNamespace.getNestedNameSpecifier();
  const auto *NS = NNS->getAsNamespace();
  if (!NS)
    return false;
    
  for (const auto &T : TargetNamespaces) {
    if (NS->getName() == T) {
      return true;
    }
  }
  return false;
}

NamespaceQualificationCheck::Rule
NamespaceQualificationCheck::ruleForPath(StringRef Path) const {
  if (Path.empty())
    return Rule::None;
  
  // If both match, prefer RequireGlobal.
  if (RequireGlobalIn.contains(Path))
    return Rule::RequireGlobal;
  if (ForbidGlobalIn.contains(Path))
    return Rule::ForbidGlobal;
  return Rule::None;
}

void NamespaceQualificationCheck::check(
    const MatchFinder::MatchResult &Result) {
  if (const auto *TU = Result.Nodes.getNodeAs<TranslationUnitDecl>("TU")) {
    // Fallback: lexical scan of the main file for policy violations.
    const auto &SM = *Result.SourceManager;
    FileID FID = SM.getMainFileID();
    if (FID.isValid()) {
      StringRef Path = SM.getFilename(SM.getLocForStartOfFile(FID));
      Rule R = ruleForPath(Path);
      if (R == Rule::None)
        return; // Skip files without applicable rules
      bool Forbid = (R == Rule::ForbidGlobal);
      bool Require = (R == Rule::RequireGlobal);
      StringRef Code = SM.getBufferData(FID);
      if (Forbid) {
        // Find '::cuda::'
        size_t Pos = 0;
        while ((Pos = Code.find("::cuda::", Pos)) != StringRef::npos) {
          SourceLocation L = SM.getLocForStartOfFile(FID).getLocWithOffset(Pos);
          diag(L, "avoid global qualification '::%0::' here") << "cuda";
          Pos += 2; // advance
        }
      } else if (Require) {
        // Find 'cuda::' not preceded by ':'
        size_t Pos = 0;
        while ((Pos = Code.find("cuda::", Pos)) != StringRef::npos) {
          if (Pos == 0 || Code[Pos - 1] != ':') {
            SourceLocation L = SM.getLocForStartOfFile(FID).getLocWithOffset(Pos);
            diag(L, "use global qualification '::%0::' for namespace usage") << "cuda";
          }
          Pos += 2;
        }
      }
    }
    return;
  }

  const auto *NNS = Result.Nodes.getNodeAs<NestedNameSpecifierLoc>("NNS");
  if (!NNS)
    return;

  SourceLocation Begin =
      Result.SourceManager->getSpellingLoc(NNS->getBeginLoc());
  if (!Begin.isValid())
    return;

  // Use the spelling location to determine which file's rules to apply
  // This ensures headers get their own rules, not the including file's rules
  llvm::StringRef FilePath = Result.SourceManager->getFilename(Begin);
  Rule R = ruleForPath(FilePath);
  
  // Skip if no rule applies to this file
  if (R == Rule::None)
    return;

  // Only act at the root namespace segment that matches a configured target.
  if (!isRootTargetNamespace(*NNS))
    return;

  bool HasGlobal = hasLeadingGlobal(*NNS);

  if (R == Rule::RequireGlobal && !HasGlobal) {
    // Insert leading :: before the namespace token.
    auto *Cur = NNS->getNestedNameSpecifier()->getAsNamespace();
    auto RootName = Cur ? Cur->getName() : llvm::StringRef("namespace");
    diag(Begin, "use global qualification '::%0::' for namespace usage")
        << RootName
        << FixItHint::CreateInsertion(Begin, "::");
  } else if (R == Rule::ForbidGlobal && HasGlobal) {
    // Remove the leading global '::'. Find its source range via the prefix.
    NestedNameSpecifierLoc P = NNS->getPrefix();
    while (P && P.getNestedNameSpecifier()->getKind() !=
                   NestedNameSpecifier::Global) {
      P = P.getPrefix();
    }
    if (P && P.getNestedNameSpecifier()->getKind() ==
                 NestedNameSpecifier::Global) {
      SourceRange GlobalRange(P.getBeginLoc(), P.getEndLoc());
      // The loc for global is a single token '::'. Remove it.
      auto *Cur = NNS->getNestedNameSpecifier()->getAsNamespace();
      auto RootName = Cur ? Cur->getName() : llvm::StringRef("namespace");
      diag(P.getBeginLoc(), "avoid global qualification '::%0::' here")
          << RootName
          << FixItHint::CreateRemoval(CharSourceRange::getTokenRange(
                 Result.SourceManager->getSpellingLoc(GlobalRange.getBegin()),
                 Result.SourceManager->getSpellingLoc(GlobalRange.getEnd())));
    }
  }
}

} // namespace clang::tidy::misc
