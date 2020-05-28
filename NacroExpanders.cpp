#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "NacroExpanders.h"
#include <iterator>
#include <vector>

using namespace clang;
using llvm::ArrayRef;
using llvm::Error;
using llvm::SmallVector;
using llvm::Twine;

static
DefMacroDirective* CreateMacroDirective(Preprocessor& PP, IdentifierInfo* Name,
                                        SourceLocation BeginLoc,
                                        ArrayRef<IdentifierInfo*> Args,
                                        ArrayRef<Token> Body,
                                        bool isVaradic = false) {
  auto* MI = PP.AllocateMacroInfo(BeginLoc);
  MI->setIsFunctionLike();
  if(isVaradic) MI->setIsGNUVarargs();

  for(auto Tok : Body) {
    MI->AddTokenToBody(Tok);
  }

  MI->setParameterList(Args, PP.getPreprocessorAllocator());
  return PP.appendDefMacroDirective(Name, MI);
}

Error NacroRuleExpander::ReplacementProtecting() {
  using namespace llvm;
  DenseMap<IdentifierInfo*, typename NacroRule::Replacement> IdentMap;
  for(auto& R : Rule->replacements()) {
    if(R.Identifier && !R.VarArgs) {
      IdentMap.insert({R.Identifier, R});
    }
  }
  if(IdentMap.empty()) return Error::success();

  for(auto TI = Rule->token_begin(); TI != Rule->token_end();) {
    auto Tok = *TI;
    auto TokLoc = Tok.getLocation();
    if(Tok.is(tok::identifier)) {
      auto* II = Tok.getIdentifierInfo();
      assert(II);
      if(IdentMap.count(II)) {
        auto& R = IdentMap[II];
        using RTy = typename NacroRule::ReplacementTy;
        switch(R.Type) {
        case RTy::Expr: {
          ++TI;
          Token LParen, RParen;
          RParen.startToken();
          RParen.setKind(tok::r_paren);
          RParen.setLength(1);
          RParen.setLocation(TokLoc.getLocWithOffset(1));
          TI = Rule->insert_token(TI, RParen);
          --TI;
          LParen.startToken();
          LParen.setKind(tok::l_paren);
          LParen.setLength(1);
          LParen.setLocation(TokLoc.getLocWithOffset(-1));
          TI = Rule->insert_token(TI, LParen);
          // skip expr and r_paren
          for(int i = 0; i < 3; ++i) ++TI;
          break;
        }
        case RTy::Stmt: {
          ++TI;
          Token Semi;
          Semi.startToken();
          Semi.setKind(tok::semi);
          Semi.setLength(1);
          Semi.setLocation(TokLoc.getLocWithOffset(1));
          TI = Rule->insert_token(TI, Semi);
          ++TI;
          break;
        }
        default: ++TI;
        }
        continue;
      }
    }
    ++TI;
  }
  return Error::success();
}

namespace {
struct LoopExpandingPPCallbacks : public PPCallbacks {
  LoopExpandingPPCallbacks(std::unique_ptr<NacroRule>&& R, Preprocessor& PP)
    : Rule(std::move(R)), PP(PP) {
    assert(Rule->hasVAArgs() && "No loops to expand from arguments");
    llvm::transform(Rule->replacements(), std::back_inserter(UnexpArgsII),
                    [](NacroRule::Replacement& R) {
                      return R.Identifier;
                    });
  }

  void ExpandsLoop(const NacroRule::Loop& LoopInfo,
                   ArrayRef<Token> LoopBody,
                   ArrayRef<std::vector<Token>> ExpVAArgs,
                   SmallVectorImpl<Token>& OutputBuffer) {
    auto* IndVarII = LoopInfo.InductionVar;
    for(const auto& Arg : ExpVAArgs) {
      for(auto& Tok : LoopBody) {
        if(Tok.is(tok::identifier) &&
           Tok.getIdentifierInfo() == IndVarII) {
          for(auto ArgTok : Arg) {
            ArgTok.setLocation(Tok.getLocation());
            OutputBuffer.push_back(ArgTok);
          }
        } else {
          OutputBuffer.push_back(Tok);
        }
      }
    }
  }

  void MacroExpands(const Token& MacroNameToken,
                    const MacroDefinition& MD,
                    SourceRange Range,
                    const MacroArgs* ConstArgs) override {
    // FIXME: Is this safe?
    auto* Args = const_cast<MacroArgs*>(ConstArgs);
    auto* MacroII = MacroNameToken.getIdentifierInfo();
    assert(MacroII);
    if(MacroII != Rule->getName()) return;
    // Number of un-expanded arguments
    assert(Args->getNumMacroArguments() == Rule->replacements_size());

    // If there is a VAArgs, it must be the last (formal) argument
    auto VAArgsIdx = Rule->replacements_size() - 1;
    auto& VAReplacement = Rule->getReplacement(VAArgsIdx);
    assert(VAReplacement.VarArgs);
    const auto& RawExpVAArgs
      = Args->getPreExpArgument(VAArgsIdx, PP);
    SmallVector<std::vector<Token>, 4> ExpVAArgs;
    std::vector<Token> VABuffer;
    for(const auto& Tok : RawExpVAArgs) {
      if(!Tok.isOneOf(tok::eof, tok::comma)) {
        VABuffer.push_back(Tok);
      } else {
        ExpVAArgs.push_back(VABuffer);
        VABuffer.clear();
      }
    }

    // Create a new MacroInfo for this iteration number
    SmallVector<Token, 16> ExpTokens;
    for(auto TokIdx = 0; TokIdx < Rule->token_size();) {
      auto Tok = Rule->getToken(TokIdx);
      if(Tok.is(tok::annot_pragma_loop_hint)) {
        auto LPI = Rule->FindLoop(TokIdx);
        assert(LPI != Rule->loop_end() &&
               "Not in the loop map?");
        assert(LPI.start() == TokIdx &&
               "Not pointing to the first loop token?");
        const auto& LP = LPI.value();
        assert(VAReplacement.Identifier == LP.IterRange &&
               "Iterating on non VAArgs variable");

        // Extract loop body
        SmallVector<Token, 8> LoopBody;
        for(auto E = LPI.stop(); TokIdx <= E; ++TokIdx) {
          Tok = Rule->getToken(TokIdx);
          if(Tok.isNot(tok::annot_pragma_loop_hint)) {
            LoopBody.push_back(Tok);
          }
        }
        ExpandsLoop(LP, LoopBody, ExpVAArgs, ExpTokens);
      } else {
        ExpTokens.push_back(Tok);
        ++TokIdx;
      }
    }

    auto* MI = MD.getMacroInfo();
    llvm::for_each(ExpTokens, [&MI](const Token& Tok) {
                    MI->AddTokenToBody(Tok);
                   });

    // Create an empty macro for next expansion
    CreateMacroDirective(PP, MacroII,
                         Rule->getBeginLoc(),
                         UnexpArgsII, {}, true);
  }

private:
  std::unique_ptr<NacroRule> Rule;
  Preprocessor& PP;
  SmallVector<IdentifierInfo*, 4> UnexpArgsII;
};
} // end anonymous namespace

Error NacroRuleExpander::Expand() {
  //if(auto E = ReplacementProtecting())
    //return E;

  SmallVector<IdentifierInfo*, 2> ReplacementsII;
  llvm::transform(Rule->replacements(), std::back_inserter(ReplacementsII),
                  [](NacroRule::Replacement& R) {
                    return R.Identifier;
                  });
  if(!Rule->needsPPHooks()) {
    // export as a normal macro function
    CreateMacroDirective(PP, Rule->getName(), Rule->getBeginLoc(),
                         ReplacementsII,
                         ArrayRef<Token>(Rule->token_begin(),
                                         Rule->token_end()));
  }

  if(!Rule->loop_empty()) {
    // Create a placeholder macro first
    CreateMacroDirective(PP, Rule->getName(), Rule->getBeginLoc(),
                         ReplacementsII, {}, true);
    PP.addPPCallbacks(
      std::make_unique<LoopExpandingPPCallbacks>(std::move(Rule), PP));
  }

  return Error::success();
}
