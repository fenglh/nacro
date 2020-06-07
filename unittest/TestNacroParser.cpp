#include "NacroParsers.h"
#include "LexingTestFixture.h"

using namespace clang;

class NacroParserTest : public NacroLexingTest {
protected:
  NacroParserTest() = default;

  // Simple wrapper around CreatePP
  std::unique_ptr<Preprocessor> GetPP(StringRef Source) {
    TrivialModuleLoader ModLoader;
    return CreatePP(Source, ModLoader);
  }
};

TEST_F(NacroParserTest, TestRuleParseArgList) {
  auto PP = GetPP("a:$expr, b:$stmt, c:$expr*)");
  NacroRuleParser Parser(*PP, {});

  ASSERT_TRUE(Parser.ParseArgList());
  auto& Rule = Parser.getNacroRule();
  ASSERT_EQ(Rule.replacements_size(), 3);

  auto RI = Rule.replacement_begin();
  ASSERT_EQ(RI->Type, NacroRule::ReplacementTy::Expr);
  ++RI;
  ASSERT_EQ(RI->Type, NacroRule::ReplacementTy::Stmt);
  ++RI;
  ASSERT_EQ(RI->Type, NacroRule::ReplacementTy::Expr);
  ASSERT_TRUE(RI->VarArgs);
}

TEST_F(NacroParserTest, TestRuleSimpleStmts) {
  auto PP = GetPP("{if(1){ puts(\"hello\"); }}");
  NacroRuleParser Parser(*PP, {});

  Parser.Advance();
  ASSERT_TRUE(Parser.ParseStmts());
  auto& Rule = Parser.getNacroRule();
  ASSERT_GT(Rule.token_size(), 2);
}

TEST_F(NacroParserTest, TestRuleBasicLoop) {
  auto PP = GetPP("$loop($i in $iter){ puts($i); }");
  NacroRuleParser Parser(*PP, {});

  Parser.Advance();
  ASSERT_TRUE(Parser.ParseLoop());

  auto& Rule = Parser.getNacroRule();
  auto LI = Rule.loop_begin();

  auto* IV = LI->InductionVar;
  ASSERT_TRUE(IV && IV->isStr("$i"));

  auto* IR = LI->IterRange;
  ASSERT_TRUE(IR && IR->isStr("$iter"));

  ++LI;
  ASSERT_EQ(LI, Rule.loop_end());
}

TEST_F(NacroParserTest, TestRuleSourceLocation) {
  auto PP = GetPP("(a:$expr, b:$expr) -> {\n"
                  "  puts(a);\n"
                  "  return b + 97;\n"
                  "}");
  auto& SM = PP->getSourceManager();
  NacroRuleParser Parser(*PP, {});
  ASSERT_TRUE(Parser.Parse());

  auto& Rule = Parser.getNacroRule();
  auto SR = Rule.getSourceRange();
  auto B = SR.getBegin();
  auto E = SR.getEnd();

  auto BL = SM.getSpellingLineNumber(B),
       EL = SM.getSpellingLineNumber(E);
  ASSERT_EQ(EL - BL, 3);
  // Ending can't be the last token - needs to be
  // one token over the last one
  ASSERT_GT(SM.getSpellingColumnNumber(E), 1);
}
