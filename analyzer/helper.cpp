#include "helper.hpp"
#include <clang/Basic/Version.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PreprocessingRecord.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/Support/Casting.h>
#include <cctype>
#include <sstream>

using namespace clang;
using namespace clang::tooling;
using json = nlohmann::json;

std::mutex mutex;
std::set<std::string> existing_filenames;
static std::set<std::string> existing_macro_keys;

static MacroDefinition fetch_macro_definition(MacroDefinitionRecord *record) {
#if defined(CLANG_VERSION_MAJOR) && CLANG_VERSION_MAJOR >= 15
  return record->getMacroDefinition();
#else
  return record->getDefinition();
#endif
}

static std::string get_real_path(const SourceManager &srcMgr,
                                 SourceLocation loc) {
  if (loc.isInvalid())
    return "";

  SourceLocation spellingLoc = srcMgr.getSpellingLoc(loc);
  if (spellingLoc.isInvalid())
    return "";

  FileID fileId = srcMgr.getFileID(spellingLoc);
  if (fileId.isInvalid())
    return "";

  if (const FileEntry *fileEntry = srcMgr.getFileEntryForID(fileId)) {
    std::string path = fileEntry->tryGetRealPathName().str();
    if (path.empty())
      path = fileEntry->getName().str();
    return path;
  }

  std::string printed = spellingLoc.printToString(srcMgr);
  size_t pos = printed.find(':');
  if (pos != std::string::npos)
    printed = printed.substr(0, pos);
  return printed;
}

static std::string get_path_with_line(const SourceManager &srcMgr,
                                      SourceLocation loc) {
  if (loc.isInvalid())
    return "";

  SourceLocation spellingLoc = srcMgr.getSpellingLoc(loc);
  if (spellingLoc.isInvalid())
    return "";

  std::string path = get_real_path(srcMgr, spellingLoc);
  if (path.empty())
    return "";

  unsigned lineNumber = srcMgr.getSpellingLineNumber(spellingLoc);
  std::ostringstream oss;
  oss << path << ":" << lineNumber;
  return oss.str();
}

static std::string get_filename_from_path(const std::string &path) {
  if (path.empty())
    return "";

  // Manually extract the filename to avoid depending on std::filesystem,
  // which may not be available with the configured compiler flags.
  size_t lastSlash = path.find_last_of("/\\");
  if (lastSlash == std::string::npos)
    return path;

  size_t filenameStart = lastSlash + 1;
  if (filenameStart >= path.size())
    return "";

  return path.substr(filenameStart);
}

std::string get_decl_code(const NamedDecl *decl) {
  SourceManager &srcMgr = decl->getASTContext().getSourceManager();
  SourceLocation startLoc = decl->getBeginLoc();
  SourceLocation endLoc = decl->getEndLoc();

  if (!startLoc.isInvalid() && !endLoc.isInvalid()) {
    // Convert the source locations to file locations
    startLoc = srcMgr.getSpellingLoc(startLoc);
    endLoc = srcMgr.getSpellingLoc(endLoc);

    // Get file path and line number
    std::string filePath = srcMgr.getFilename(startLoc).str();
    unsigned int lineNumber = srcMgr.getSpellingLineNumber(startLoc);

    // Extract the source code text
    bool invalid = false;
    StringRef text =
        Lexer::getSourceText(CharSourceRange::getTokenRange(startLoc, endLoc),
                             srcMgr, LangOptions(), &invalid);

    if (!invalid) {
      std::string sourceCode = text.str();
      // Now you have filePath, lineNumber, and sourceCode
      // Store or process them as needed
      return sourceCode;
    }
  }
  return "";
}

void output_decl(const NamedDecl *decl, std::string output_file_name,
                 bool is_typedef, std::string alias_name) {
  // Add a lock
  std::lock_guard<std::mutex> lock(mutex);

  auto name = decl->getNameAsString();
  std::string sourceCode = get_decl_code(decl);

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  json j;
  j["name"] = name;
  j["source"] = sourceCode;

  // Get the SourceLocation for the beginning of the declaration
  SourceLocation beginLoc = decl->getBeginLoc();

  // Retrieve the SourceManager from the AST context
  SourceManager &sourceManager = decl->getASTContext().getSourceManager();

  std::stringstream filenameWithLine;
  if (const FileEntry *fileEntry =
          sourceManager.getFileEntryForID(sourceManager.getFileID(beginLoc))) {
    filenameWithLine << fileEntry->tryGetRealPathName().str();
  } else {
    filenameWithLine << decl->getBeginLoc().printToString(
        decl->getASTContext().getSourceManager());
  }
  // Append line number
  unsigned lineNumber = sourceManager.getSpellingLineNumber(beginLoc);
  filenameWithLine << ":" << lineNumber;

  std::string filename = filenameWithLine.str();
  std::string key_name =
      filename + "+" + name + "+" + output_file_name + "+" + alias_name;
  if (existing_filenames.find(key_name) == existing_filenames.end()) {
    existing_filenames.insert(key_name);
  } else {
    return;
  }
  j["filename"] = filename;

  if (is_typedef) {
    j["alias"] = alias_name;
  }

  auto json_str = j.dump();
  output_file << json_str << std::endl;
  output_file.flush();
  output_file.close();
}

void output_enum_values(const EnumDecl *decl, std::string output_file_name) {
  std::lock_guard<std::mutex> lock(mutex);

  std::string enumName = decl->getNameAsString();
  if (enumName.empty())
    return;

  SourceLocation beginLoc = decl->getBeginLoc();
  SourceManager &sourceManager = decl->getASTContext().getSourceManager();

  std::stringstream filenameWithLine;
  if (const FileEntry *fileEntry =
          sourceManager.getFileEntryForID(sourceManager.getFileID(beginLoc))) {
    filenameWithLine << fileEntry->tryGetRealPathName().str();
  } else {
    filenameWithLine << beginLoc.printToString(sourceManager);
  }
  unsigned lineNumber = sourceManager.getSpellingLineNumber(beginLoc);
  filenameWithLine << ":" << lineNumber;

  std::string filename = filenameWithLine.str();
  std::string key_name = filename + "+" + enumName + "+" + output_file_name;
  if (existing_filenames.find(key_name) != existing_filenames.end())
    return;
  existing_filenames.insert(key_name);

  json values = json::object();
  for (const auto *ecd : decl->enumerators()) {
    llvm::APSInt val = ecd->getInitVal();
    values[ecd->getNameAsString()] = val.getSExtValue();
  }

  json j;
  j[enumName] = values;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  output_file << j.dump() << std::endl;
  output_file.flush();
  output_file.close();
}

static void collect_nested_structs(QualType qt, std::set<std::string> &nested,
                                   std::set<const RecordDecl *> &visited) {
  qt = qt.getCanonicalType();

  if (const auto *pt = qt->getAs<PointerType>()) {
    collect_nested_structs(pt->getPointeeType(), nested, visited);
    return;
  }
  if (const auto *at = qt->getAsArrayTypeUnsafe()) {
    collect_nested_structs(at->getElementType(), nested, visited);
    return;
  }
  if (const auto *et = qt->getAs<ElaboratedType>()) {
    collect_nested_structs(et->getNamedType(), nested, visited);
    return;
  }
  if (const auto *tt = qt->getAs<TypedefType>()) {
    auto *td = tt->getDecl();
    std::string aliasName = td->getNameAsString();
    QualType underlying = td->getUnderlyingType();

    if (const auto *rt = underlying->getAs<RecordType>()) {
      const RecordDecl *rd = rt->getDecl();
      std::string name = aliasName.empty() ? rd->getNameAsString() : aliasName;
      if (name.empty()) {
        if (const TypedefNameDecl *anon = rd->getTypedefNameForAnonDecl())
          name = anon->getNameAsString();
      }
      if (!name.empty())
        nested.insert(name);
      if (const RecordDecl *def = rd->getDefinition()) {
        if (visited.insert(def).second) {
          for (const FieldDecl *field : def->fields())
            collect_nested_structs(field->getType(), nested, visited);
        }
      }
      return;
    }

    collect_nested_structs(underlying, nested, visited);
    return;
  }
  if (const auto *rt = qt->getAs<RecordType>()) {
    const RecordDecl *rd = rt->getDecl();
    std::string name = rd->getNameAsString();
    if (name.empty()) {
      if (const TypedefNameDecl *anon = rd->getTypedefNameForAnonDecl())
        name = anon->getNameAsString();
    }
    if (!name.empty())
      nested.insert(name);
    if (const RecordDecl *def = rd->getDefinition()) {
      if (visited.insert(def).second) {
        for (const FieldDecl *field : def->fields())
          collect_nested_structs(field->getType(), nested, visited);
      }
    }
  }
}

void output_struct_relations(const RecordDecl *decl,
                             std::string output_file_name,
                             std::string struct_name) {
  std::lock_guard<std::mutex> lock(mutex);

  std::string structName = struct_name;
  if (structName.empty()) {
    structName = decl->getNameAsString();
    if (structName.empty()) {
      if (const TypedefNameDecl *anon = decl->getTypedefNameForAnonDecl())
        structName = anon->getNameAsString();
    }
  }
  if (structName.empty())
    return;

  SourceLocation beginLoc = decl->getBeginLoc();
  SourceManager &sourceManager = decl->getASTContext().getSourceManager();

  std::stringstream filenameWithLine;
  if (const FileEntry *fileEntry =
          sourceManager.getFileEntryForID(sourceManager.getFileID(beginLoc))) {
    filenameWithLine << fileEntry->tryGetRealPathName().str();
  } else {
    filenameWithLine << beginLoc.printToString(sourceManager);
  }
  unsigned lineNumber = sourceManager.getSpellingLineNumber(beginLoc);
  filenameWithLine << ":" << lineNumber;

  std::string filename = filenameWithLine.str();
  std::string key_name = filename + "+" + structName + "+" + output_file_name;
  if (existing_filenames.find(key_name) != existing_filenames.end())
    return;
  existing_filenames.insert(key_name);

  std::set<std::string> nested;
  std::set<const RecordDecl *> visited;
  visited.insert(decl);
  for (const FieldDecl *field : decl->fields())
    collect_nested_structs(field->getType(), nested, visited);

  json arr = json::array();
  for (const auto &name : nested)
    arr.push_back(name);

  json j;
  j[structName] = arr;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  output_file << j.dump() << std::endl;
  output_file.flush();
  output_file.close();
}

static void collect_called_functions(const Stmt *stmt,
                                     std::vector<std::string> &ordered,
                                     std::set<std::string> &seen) {
  if (!stmt)
    return;

  if (const auto *call = dyn_cast<CallExpr>(stmt)) {
    if (const FunctionDecl *callee = call->getDirectCallee()) {
      std::string calleeName = callee->getNameAsString();
      if (!calleeName.empty() &&
          calleeName.rfind("__compiletime_assert_", 0) != 0) {
        if (seen.insert(calleeName).second)
          ordered.push_back(calleeName);
      }
    }
  }

  for (const Stmt *child : stmt->children())
    collect_called_functions(child, ordered, seen);
}

static QualType peel_type(QualType qt) {
  while (true) {
    if (const auto *pt = qt->getAs<PointerType>()) {
      qt = pt->getPointeeType();
      continue;
    }
    if (const auto *at = qt->getAsArrayTypeUnsafe()) {
      qt = at->getElementType();
      continue;
    }
    if (const auto *rt = qt->getAs<ReferenceType>()) {
      qt = rt->getPointeeType();
      continue;
    }
    if (const auto *pt = qt->getAs<ParenType>()) {
      qt = pt->getInnerType();
      continue;
    }
    if (const auto *attr = qt->getAs<AttributedType>()) {
      qt = attr->getModifiedType();
      continue;
    }
    if (const auto *macro = qt->getAs<MacroQualifiedType>()) {
      qt = macro->getUnderlyingType();
      continue;
    }
    if (const auto *et = qt->getAs<ElaboratedType>()) {
      qt = et->getNamedType();
      continue;
    }
    break;
  }

  return qt;
}

static const NamedDecl *get_type_definition(QualType qt) {
  qt = peel_type(qt);

  if (const auto *tt = qt->getAs<TypedefType>())
    return tt->getDecl();

  QualType canonical = peel_type(qt.getCanonicalType());

  if (const auto *tt = canonical->getAs<TypedefType>())
    return tt->getDecl();

  if (const auto *tag = canonical->getAs<TagType>()) {
    const TagDecl *tagDecl = tag->getDecl();
    if (const TagDecl *def = tagDecl->getDefinition())
      tagDecl = def;
    return dyn_cast<NamedDecl>(tagDecl);
  }

  return nullptr;
}

void output_func_params(const FunctionDecl *decl,
                        std::string output_file_name) {
  std::lock_guard<std::mutex> lock(mutex);

  std::string funcName = decl->getNameAsString();
  if (funcName.empty() ||
      funcName.rfind("__compiletime_assert_", 0) == 0)
    return;

  const SourceManager &sourceManager = decl->getASTContext().getSourceManager();

  std::string locationKey = get_path_with_line(sourceManager, decl->getBeginLoc());
  if (locationKey.empty())
    locationKey = funcName;

  std::string key_name = locationKey + "+" + funcName + "+" + output_file_name;
  if (existing_filenames.find(key_name) != existing_filenames.end())
    return;
  existing_filenames.insert(key_name);

  const FunctionDecl *definition = decl->getDefinition();
  if (!definition)
    definition = decl;

  const FunctionDecl *firstDecl = decl->getCanonicalDecl();
  if (!firstDecl)
    firstDecl = decl;

  std::string declHeaderPath = get_real_path(sourceManager, firstDecl->getBeginLoc());
  std::string declHeaderName = get_filename_from_path(declHeaderPath);

  std::string defPath = get_real_path(sourceManager, definition->getBeginLoc());
  std::string defFileName = get_filename_from_path(defPath);
  std::string defCode = get_decl_code(definition);

  json params = json::array();
  for (unsigned index = 0; index < decl->getNumParams(); ++index) {
    const ParmVarDecl *param = decl->getParamDecl(index);
    json pj;
    pj["name"] = param->getNameAsString();

    const ParmVarDecl *declParam = nullptr;
    if (firstDecl && firstDecl->getNumParams() > index)
      declParam = firstDecl->getParamDecl(index);

    std::string paramHeaderPath;
    if (declParam)
      paramHeaderPath =
          get_real_path(declParam->getASTContext().getSourceManager(),
                        declParam->getBeginLoc());
    if (paramHeaderPath.empty())
      paramHeaderPath = get_real_path(sourceManager, param->getBeginLoc());
    std::string paramHeaderName = get_filename_from_path(paramHeaderPath);
    pj["param_decl_header"] =
        paramHeaderName.empty() ? json(nullptr) : json(paramHeaderName);

    QualType qt = param->getType();
    pj["type_spelling"] = qt.getAsString();

    const NamedDecl *typeDecl = get_type_definition(qt);
    if (typeDecl) {
      std::string qualifiedName = typeDecl->getQualifiedNameAsString();
      if (qualifiedName.empty())
        qualifiedName = typeDecl->getNameAsString();
      pj["type_decl_qualified_name"] =
          qualifiedName.empty() ? json(nullptr) : json(qualifiedName);

      const SourceManager &typeSourceManager =
          typeDecl->getASTContext().getSourceManager();
      std::string typeHeaderPath =
          get_real_path(typeSourceManager, typeDecl->getBeginLoc());
      std::string typeHeaderName = get_filename_from_path(typeHeaderPath);
      pj["type_decl_header"] =
          typeHeaderName.empty() ? json(nullptr) : json(typeHeaderName);

      std::string typeCode = get_decl_code(typeDecl);
      pj["type_decl_code"] = typeCode.empty() ? json(nullptr) : json(typeCode);
    } else {
      pj["type_decl_qualified_name"] = nullptr;
      pj["type_decl_header"] = nullptr;
      pj["type_decl_code"] = nullptr;
    }
    params.push_back(pj);
  }

  json j;
  j["function_name"] = funcName;
  j["function_decl_header"] =
      declHeaderName.empty() ? json(nullptr) : json(declHeaderName);
  j["function_def_file"] =
      defFileName.empty() ? json(nullptr) : json(defFileName);
  j["function_def_code"] = defCode.empty() ? json(nullptr) : json(defCode);
  j["params"] = params;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  output_file << j.dump() << std::endl;
  output_file.flush();
  output_file.close();
}

void output_func_calls(const FunctionDecl *decl, std::string output_file_name) {
  if (!decl || !decl->doesThisDeclarationHaveABody())
    return;

  std::lock_guard<std::mutex> lock(mutex);

  std::string funcName = decl->getNameAsString();
  if (funcName.empty() ||
      funcName.rfind("__compiletime_assert_", 0) == 0)
    return;

  const SourceManager &sourceManager = decl->getASTContext().getSourceManager();
  std::string locationKey = get_path_with_line(sourceManager, decl->getBeginLoc());
  if (locationKey.empty())
    locationKey = funcName;

  std::string key_name = locationKey + "+" + funcName + "+" + output_file_name;
  if (existing_filenames.find(key_name) != existing_filenames.end())
    return;
  existing_filenames.insert(key_name);

  std::vector<std::string> ordered;
  std::set<std::string> seen;
  collect_called_functions(decl->getBody(), ordered, seen);

  json callees = json::array();
  for (const auto &name : ordered)
    callees.push_back(name);

  json j;
  j[funcName] = callees;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  output_file << j.dump() << std::endl;
  output_file.flush();
  output_file.close();
}

void output_func_locations(const FunctionDecl *decl,
                           std::string output_file_name) {
  if (!decl)
    return;

  const FunctionDecl *definition = decl->getDefinition();
  if (!definition)
    definition = decl;

  if (!definition->doesThisDeclarationHaveABody())
    return;

  std::string funcName = definition->getNameAsString();
  if (funcName.empty() ||
      funcName.rfind("__compiletime_assert_", 0) == 0)
    return;

  ASTContext &context = definition->getASTContext();
  const SourceManager &sourceManager = context.getSourceManager();

  SourceLocation beginLoc = sourceManager.getSpellingLoc(definition->getBeginLoc());
  SourceLocation endLoc = sourceManager.getSpellingLoc(definition->getEndLoc());

  if (beginLoc.isInvalid() || endLoc.isInvalid())
    return;

  std::string filePath = get_real_path(sourceManager, beginLoc);
  if (filePath.empty())
    filePath = sourceManager.getFilename(beginLoc).str();

  if (filePath.empty())
    return;

  unsigned startLine = sourceManager.getSpellingLineNumber(beginLoc);
  unsigned startCol = sourceManager.getSpellingColumnNumber(beginLoc);
  unsigned endLine = sourceManager.getSpellingLineNumber(endLoc);
  unsigned endCol = sourceManager.getSpellingColumnNumber(endLoc);

  unsigned tokenLength = Lexer::MeasureTokenLength(endLoc, sourceManager,
                                                   context.getLangOpts());
  if (tokenLength > 0)
    endCol += tokenLength - 1;

  std::lock_guard<std::mutex> lock(mutex);

  std::ostringstream keyBuilder;
  keyBuilder << filePath << ':' << startLine << ':' << startCol << ':'
             << funcName << '+' << output_file_name;
  std::string key = keyBuilder.str();
  if (existing_filenames.find(key) != existing_filenames.end())
    return;
  existing_filenames.insert(key);

  json j;
  j["name"] = funcName;
  j["filename"] = filePath;
  j["startLine"] = startLine;
  j["endLine"] = endLine;
  j["startCol"] = startCol;
  j["endCol"] = endCol;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  output_file << j.dump() << std::endl;
  output_file.flush();
  output_file.close();
}

void output_macro_definitions(CompilerInstance &compiler,
                              std::string output_file_name) {
  std::lock_guard<std::mutex> lock(mutex);

  Preprocessor &pp = compiler.getPreprocessor();
  PreprocessingRecord *record = pp.getPreprocessingRecord();
  if (!record)
    return;

  SourceManager &sourceManager = compiler.getSourceManager();
  const LangOptions &langOpts = compiler.getLangOpts();

  std::vector<json> entries;
  entries.reserve(32);

  for (auto it = record->begin(); it != record->end(); ++it) {
    const PreprocessedEntity *entity = *it;
    if (!entity || entity->getKind() != PreprocessedEntity::MacroDefinitionKind)
      continue;

    const auto *macroRecord = llvm::cast<MacroDefinitionRecord>(entity);
    const IdentifierInfo *identifier = macroRecord->getName();
    if (!identifier)
      continue;

    std::string macroName = identifier->getName().str();
    if (macroName.empty())
      continue;

    MacroDefinition macroDef = fetch_macro_definition(macroRecord);
    const MacroInfo *macroInfo = macroDef.getMacroInfo();
    if (!macroInfo)
      continue;

    SourceLocation defLoc = sourceManager.getSpellingLoc(macroInfo->getDefinitionLoc());
    if (defLoc.isInvalid())
      continue;

    if (sourceManager.isWrittenInBuiltinFile(defLoc) ||
        sourceManager.isWrittenInCommandLineFile(defLoc) ||
        sourceManager.isInSystemHeader(defLoc))
      continue;

    std::string locationKey = get_path_with_line(sourceManager, defLoc);
    if (locationKey.empty())
      locationKey = macroName;

    std::string key = locationKey + "+" + macroName + "+" + output_file_name;
    if (existing_macro_keys.find(key) != existing_macro_keys.end())
      continue;
    existing_macro_keys.insert(key);

    std::string body;
    for (const Token &token : macroInfo->tokens()) {
      if (token.is(tok::eod))
        continue;

      bool invalid = false;
      std::string spelling = Lexer::getSpelling(token, sourceManager, langOpts, &invalid);
      if (invalid)
        continue;

      if (!body.empty() && (token.hasLeadingSpace() ||
                            (isalnum(static_cast<unsigned char>(body.back())) &&
                             isalnum(static_cast<unsigned char>(spelling.front())))))
        body.push_back(' ');

      body += spelling;
    }

    json j;
    j["name"] = macroName;
    j["source"] = body;
    entries.push_back(std::move(j));
  }

  if (entries.empty())
    return;

  std::ofstream output_file;
  output_file.open(output_file_name, std::ios_base::app);
  for (const auto &entry : entries)
    output_file << entry.dump() << std::endl;
  output_file.flush();
  output_file.close();
}
