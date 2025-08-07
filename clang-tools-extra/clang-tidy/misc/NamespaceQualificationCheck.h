//===--- NamespaceQualificationCheck.h - clang-tidy ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_NAMESPACEQUALIFICATIONCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_NAMESPACEQUALIFICATIONCHECK_H

#include "../ClangTidyCheck.h"
#include "../GlobList.h"
#include <optional>
#include <string>
#include <vector>

namespace clang::tidy::misc {

// Enforces presence/absence of a leading global qualifier for a given
// namespace based on file path globs.
class NamespaceQualificationCheck : public ClangTidyCheck {
public:
  NamespaceQualificationCheck(StringRef Name, ClangTidyContext *Context);

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  void registerPPCallbacks(const SourceManager &SM, Preprocessor *, Preprocessor *) override {}

  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

private:
  // Returns true if the specifier has a global '::' prefix.
  static bool hasLeadingGlobal(const NestedNameSpecifierLoc &Loc);

  // Decides desired qualification for a given file path.
  enum class Rule { None, RequireGlobal, ForbidGlobal };
  Rule ruleForPath(StringRef Path) const;

  // One or more root namespaces that should be considered. A usage matches if
  // the left-most namespace in a qualified name equals any of these.
  std::vector<std::string> TargetNamespaces;
  CachedGlobList RequireGlobalIn;
  CachedGlobList ForbidGlobalIn;
  // True if this specifier is the left-most namespace in the chain and its
  // name equals one of TargetNamespaces.
  bool isRootTargetNamespace(const NestedNameSpecifierLoc &Loc) const;
};

} // namespace clang::tidy::misc

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_NAMESPACEQUALIFICATIONCHECK_H
