#include "import_source.h"

#include <torch/csrc/jit/script/parser.h>

namespace torch {
namespace jit {
namespace script {

// this is a much simpler accessor that only handles modules, parameters, and
// and methods. It does not depend on python to work.
struct ModuleAccessorValue : public SugaredValue {
  ModuleAccessorValue(std::shared_ptr<Module> module)
      : module(std::move(module)) {}
  std::string kind() const override {
    return "module";
  }
  // select an attribute on it, e.g. `this.field`
  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Method& m,
      const std::string& field) override {
    if (std::shared_ptr<Module> v = module->find_module(field)) {
      return std::make_shared<ModuleAccessorValue>(std::move(v));
    } else if (NamedIValue* v = module->find_parameter(field)) {
      return std::make_shared<SimpleValue>(m.get_or_add_parameter(v->slot()));
    } else if (NamedIValue* v = module->find_buffer(field)) {
      return std::make_shared<SimpleValue>(m.get_or_add_parameter(v->slot()));
    } else if (script::NamedIValue* v = module->find_attribute(field)) {
      return std::make_shared<script::SimpleValue>(
          m.get_or_add_attribute(v->type(), v->slot()));
    } else if (Method* m = module->find_method(field)) {
      return std::make_shared<MethodValue>(shared_from_this(), *m);
    } else {
      throw ErrorReport(loc) << "unknown attr: " << field;
    }
  }

 private:
  std::shared_ptr<Module> module;
};

struct OpsValue : public SugaredValue {
  OpsValue(size_t version) : version_(version) {}
  std::string kind() const override {
    return "ops";
  }
  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Method& m,
      const std::string& field) override {
    return std::make_shared<BuiltinModule>(field, version_);
  }
  size_t version_;
};

struct ConstantValue : public SugaredValue {
  ConstantValue(IValue value) : value_(std::move(value)) {}
  IValue value_;
  std::string kind() const override {
    return "constant";
  }
  Value* asValue(const SourceRange& loc, Method& m) override {
    return m.graph()->insertConstant(value_);
  }
};

// This value maps attributes CONSTANTS.c0 CONSTANTS.c1 to entries
// in the 'constants' vector. This table is will be stored in a container format
// and given to the import_method when restoring the code.
struct ConstantTableValue : public SugaredValue {
  ConstantTableValue(ArrayRef<at::Tensor> constants) : constants_(constants) {}
  std::string kind() const override {
    return "CONSTANTS";
  }
  // select an attribute on it, e.g. `this.field`
  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Method& m,
      const std::string& field) override {
    const char* field_s = field.c_str();
    char* end;
    int64_t offset = std::strtoll(field_s + 1, &end, 10);
    if (field.size() < 2 || *end != 0)
      throw ErrorReport(loc) << "invalid constant specifier: " << field;
    if (offset < 0 || size_t(offset) >= constants_.size()) {
      throw ErrorReport(loc) << "constant index " << offset
                             << " is out of bounds (constant table has "
                             << constants_.size() << " entries).";
    }
    Value* value = m.graph()->insertConstant(constants_[offset], nullptr, loc);
    return std::make_shared<SimpleValue>(value);
  }

 private:
  ArrayRef<at::Tensor> constants_;
};

// Helper that contains the state for a parsing a TorchScript source string.
struct SourceImporter {
  SourceImporter(
      const std::string& src,
      const std::vector<at::Tensor>& constant_table)
      : parser_(src), constant_table_(constant_table) {
    const auto version = parseVersionNumber();
    env_ = {
        {"torch", std::make_shared<BuiltinModule>("aten", version)},
        {"ops", std::make_shared<OpsValue>(version)},
        {"CONSTANTS", std::make_shared<ConstantTableValue>(constant_table)},
        {"fork", std::make_shared<ForkValue>()},
        {"annotate", std::make_shared<AnnotateValue>()},
        {"inf",
         std::make_shared<ConstantValue>(
             std::numeric_limits<double>::infinity())},
        {"nan",
         std::make_shared<ConstantValue>(
             std::numeric_limits<double>::quiet_NaN())},
    };

    resolver_ = [&](const std::string& name,
                    Method& m,
                    const SourceRange& loc) -> std::shared_ptr<SugaredValue> {
      auto it = env_.find(name);
      if (it == env_.end()) {
        return nullptr;
      }
      return it->second;
    };
  }

  Parser parser_;
  // Constants present in the model. Used to resolve "CONSTANTS.n" to the actual
  // value
  const std::vector<at::Tensor>& constant_table_;
  std::unordered_map<std::string, std::shared_ptr<SugaredValue>> env_;
  std::function<std::shared_ptr<
      SugaredValue>(const std::string& name, Method& m, const SourceRange& loc)>
      resolver_;

  size_t parseVersionNumber() {
    auto& L = parser_.lexer();
    auto range = L.cur().range;
    auto name = L.expect(TK_IDENT).text();
    L.expect('=');
    std::string version_text = L.expect(TK_NUMBER).text();
    L.expect(TK_NEWLINE);
    auto version = Const::create(L.cur().range, version_text);
    if (name != "op_version_set")
      throw ErrorReport(range) << "expected an assignment to op_version_set";
    if (!version.isIntegral())
      throw ErrorReport(range)
          << "expected an integral version but found " << version.text();
    return size_t(version.asIntegral());
  }
};

void import_methods(
    const std::shared_ptr<Module>& mod,
    const std::string& src,
    const std::vector<at::Tensor>& constant_table) {
  SourceImporter importer(src, constant_table);
  auto& p = importer.parser_;

  std::vector<Def> definitions;
  std::vector<Resolver> resolvers;
  while (p.lexer().cur().kind != TK_EOF) {
    auto def = Def(p.parseFunction(/*is_method=*/true));
    definitions.emplace_back(def);
    resolvers.emplace_back(importer.resolver_);
  }
  auto self = std::make_shared<ModuleAccessorValue>(mod);
  defineMethodsInModule(mod, definitions, resolvers, Self(self));
}

void import_libs(
    const std::string& src,
    const std::vector<at::Tensor>& constant_table) {
  SourceImporter importer(src, constant_table);
  auto& p = importer.parser_;

  while (p.lexer().cur().kind != TK_EOF) {
    std::vector<Def> definitions;
    std::vector<Resolver> resolvers;
    auto class_def = ClassDef(p.parseClass());
    for (const auto& method_def : class_def.defs()) {
      definitions.emplace_back(method_def);
      resolvers.emplace_back(importer.resolver_);
    }

    auto mod = std::make_shared<Module>();
    Self self(ClassType::create(class_def.name().name(), mod));
    defineMethodsInModule(mod, definitions, resolvers, self);
  }
}

} // namespace script
} // namespace jit
} // namespace torch
