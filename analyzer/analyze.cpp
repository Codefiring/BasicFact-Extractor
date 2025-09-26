#include "helper.hpp"
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>

using namespace clang;
using namespace clang::tooling;

class MacroCollector : public PPCallbacks {
public:
  explicit MacroCollector(Preprocessor &pp)
      : preprocessor(pp), langOpts(pp.getLangOpts()) {}

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override {
    const auto *macroInfo = MD->getMacroInfo();
    if (!macroInfo || macroInfo->isBuiltinMacro())
      return;

    const SourceManager &sourceManager = preprocessor.getSourceManager();
    SourceLocation beginLoc = MD->getLocation();
    if (beginLoc.isInvalid())
      return;

    SourceLocation spellingLoc = sourceManager.getSpellingLoc(beginLoc);
    if (sourceManager.isInSystemHeader(spellingLoc) ||
        sourceManager.isInSystemMacro(spellingLoc))
      return;

    std::string macroText = getMacroDefinitionText(*macroInfo, *MD);
    if (macroText.empty())
      return;

    std::string macroName = MacroNameTok.getIdentifierInfo()->getName().str();
    output_macro(macroName, macroText, sourceManager, beginLoc);
  }

private:
  std::string getMacroDefinitionText(const MacroInfo &macroInfo,
                                     const MacroDirective &directive) const {
    const SourceManager &sourceManager = preprocessor.getSourceManager();

    SourceLocation startLoc = directive.getLocation();
    SourceLocation endLoc = macroInfo.getDefinitionEndLoc();
    if (startLoc.isInvalid() || endLoc.isInvalid())
      return "";

    startLoc = sourceManager.getSpellingLoc(startLoc);
    endLoc = sourceManager.getSpellingLoc(endLoc);

    SourceLocation endOfToken =
        Lexer::getLocForEndOfToken(endLoc, 0, sourceManager, langOpts);
    if (endOfToken.isInvalid())
      return "";

    bool invalid = false;
    CharSourceRange range = CharSourceRange::getCharRange(startLoc, endOfToken);
    StringRef text = Lexer::getSourceText(range, sourceManager, langOpts, &invalid);
    if (invalid)
      return "";

    return text.str();
  }

  Preprocessor &preprocessor;
  const LangOptions &langOpts;
};

class StructVisitor : public RecursiveASTVisitor<StructVisitor> {
public:
  explicit StructVisitor(ASTContext *context, bool collect_enum = true,
                         bool collect_struct = true, bool collect_func = true,
                         bool collect_handler = true,
                         bool collect_typedef = true)
      : context(context), collect_enum(collect_enum),
        collect_struct(collect_struct), collect_func(collect_func),
        collect_handler(collect_handler), collect_typedef(collect_typedef) {}

  bool VisitFunctionDecl(FunctionDecl *funcDecl) {
    if (!collect_func)
      return true;
    if (funcDecl->isThisDeclarationADefinition()) {
      std::string funcName = funcDecl->getNameAsString();
      std::string sourceCode = get_decl_code(funcDecl);
      if (funcName != "")
        output_decl(funcDecl, "func.jsonl");
    }
    return true;
  }

  bool VisitRecordDecl(RecordDecl *recordDecl) {
    if (collect_struct) {
      if (recordDecl->isThisDeclarationADefinition()) {
        std::string structName = recordDecl->getNameAsString();
        std::string sourceCode = get_decl_code(recordDecl);
        if (structName != "")
          output_decl(recordDecl, "struct.jsonl");
      }
    }
    return true;
  }

  bool VisitEnumDecl(EnumDecl *enumDecl) {
    if (!collect_enum)
      return true;
    if (enumDecl->isThisDeclarationADefinition()) {
      std::string enumName = enumDecl->getNameAsString();
      std::string sourceCode = get_decl_code(enumDecl);

      // Output the enum definition
      if (enumName != "")
        output_decl(enumDecl, "enum.jsonl");
    }
    return true;
  }

  bool VisitTypedefDecl(TypedefDecl *typedefDecl) {
    QualType qt = typedefDecl->getUnderlyingType();
    if (collect_enum) {
      if (const EnumType *et = qt->getAs<EnumType>()) {
        EnumDecl *enumDecl = et->getDecl();
        std::string enumName = enumDecl->getNameAsString();
        std::string aliasName = typedefDecl->getNameAsString();

        // Output the typedef alias
        if (aliasName != "")
          output_decl(typedefDecl, "enum-typedef.jsonl", true, enumName);
      }
    }
    if (collect_struct) {
      if (const RecordType *rt = qt->getAs<RecordType>()) {
        RecordDecl *recordDecl = rt->getDecl();
        std::string structName = recordDecl->getNameAsString();
        std::string aliasName = typedefDecl->getNameAsString();

        // Output the typedef alias
        if (aliasName != "")
          output_decl(typedefDecl, "struct-typedef.jsonl", true, structName);
      }
    }
    if (collect_typedef) {
      if (const TypedefType *tt = qt->getAs<TypedefType>()) {
        TypedefNameDecl *typedefDecl = tt->getDecl();
        std::string typedefName = typedefDecl->getNameAsString();
        std::string aliasName = typedefDecl->getNameAsString();

        // Output the typedef alias
        if (aliasName != "")
          output_decl(typedefDecl, "typedef.jsonl", true, typedefName);
      }
    }
    return true;
  }

  bool VisitVarDecl(VarDecl *declaration) {
    if (!collect_handler)
      return true;

    // Check if the declaration is in the main file
    if (!context->getSourceManager().isInMainFile(declaration->getBeginLoc()))
      return true;

    if (declaration->hasInit()) {
      if (const auto *initList =
              dyn_cast<InitListExpr>(declaration->getInit())) {
        if (const auto *recordDecl =
                declaration->getType()->getAsRecordDecl()) {
          auto fieldIt = recordDecl->field_begin();
          for (unsigned i = 0; i < initList->getNumInits(); ++i, ++fieldIt) {
            // Ensure we have not run past the end of the fields
            if (fieldIt == recordDecl->field_end())
              break;

            const FieldDecl *fieldDecl = *fieldIt;
            auto fieldName = fieldDecl->getNameAsString();
            if (fieldName == "ioctl" || fieldName == "unlocked_ioctl") {
              auto sourceCode = get_decl_code(declaration);
              // Check whether the ioctl is in the source code
              if (sourceCode.find(".ioctl") == std::string::npos &&
                  sourceCode.find(".unlocked_ioctl") == std::string::npos)
                continue;

              output_decl(declaration, "ioctl.jsonl");
              break;
            }
          }
        }
      }
    }
    return true;
  }

private:
  ASTContext *context;
  bool collect_enum;
  bool collect_struct;
  bool collect_func;
  bool collect_handler;
  bool collect_typedef;
};

class StructConsumer : public clang::ASTConsumer {
public:
  explicit StructConsumer(ASTContext *context, bool collect_enum = true,
                          bool collect_struct = true, bool collect_func = true,
                          bool collect_handler = true,
                          bool collect_typedef = true)
      : visitor(context, collect_enum, collect_struct, collect_func,
                collect_handler, collect_typedef) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  StructVisitor visitor;
};

class StructAction : public clang::ASTFrontendAction {
public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef) override {
    compiler.getPreprocessor().addPPCallbacks(
        std::make_unique<MacroCollector>(compiler.getPreprocessor()));
    return std::make_unique<StructConsumer>(&compiler.getASTContext());
  }
};

int main(int argc, const char **argv) {
  llvm::cl::OptionCategory MyToolCategory("my-tool options");
  llvm::cl::opt<std::string> OptCompileCommands(
      "p", llvm::cl::desc("Specify path compile_commands.json"),
      llvm::cl::Required, llvm::cl::cat(MyToolCategory));
  llvm::cl::ParseCommandLineOptions(argc, argv);

  // Load compile_commands.json manually
  std::string ErrorMessage;
  auto CompilationDatabase = JSONCompilationDatabase::loadFromFile(
      OptCompileCommands, ErrorMessage,
      clang::tooling::JSONCommandLineSyntax::AutoDetect);

  if (!CompilationDatabase) {
    llvm::errs() << "Error loading compile_commands.json: " << ErrorMessage
                 << "\n";
    return 1;
  }

  // Extract source files from the loaded database
  std::vector<std::string> sources;
  for (const auto &command : CompilationDatabase->getAllCompileCommands()) {
    // Only add .c and .h files
    if (command.Filename.find(".c") == std::string::npos &&
        command.Filename.find(".h") == std::string::npos)
      continue;
    sources.push_back(command.Filename);
  }

  // Process each source file
  std::vector<std::future<void>> futures;
  auto frontendAction = newFrontendActionFactory<StructAction>();

  int maxThreads = 100;
  Semaphore sem(maxThreads);
  // Assuming 'sources' is a vector of strings containing source paths
  for (const auto &sourcePath : sources) {
    sem.wait(); // Wait for an available slot

    // Capture necessary variables by reference
    futures.push_back(
        std::async(std::launch::async, [&sem, &sourcePath, &CompilationDatabase,
                                        &frontendAction]() {
          std::cout << sourcePath << std::endl;

          // Processing logic with ClangTool
          std::vector<std::string> currentSource = {sourcePath};
          ClangTool tool(*CompilationDatabase, currentSource);
          tool.run(frontendAction.get());

          sem.notify(); // Signal that this thread is done
        }));
  }

  for (auto &fut : futures) {
    fut.wait();
  }
}
