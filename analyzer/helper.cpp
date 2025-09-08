#include "helper.hpp"
#include <llvm/ADT/APSInt.h>

using namespace clang;
using namespace clang::tooling;
using json = nlohmann::json;

std::mutex mutex;
std::set<std::string> existing_filenames;

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