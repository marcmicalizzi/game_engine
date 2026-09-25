#include "model.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace schemac {

namespace {

const char* const k_primitives[] = {"bool", "u8",   "u16",  "u32",  "u64",    "i8",    "i16",
                                    "i32",  "i64",  "f32",  "f64",  "string", "bytes", "id128",
                                    "vec2", "vec3", "vec4", "quat", "json"};
const char* const k_integers[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};

struct Token {
  enum class Kind { Ident, Int, Float, String, Punct, Doc, End };
  Kind kind = Kind::End;
  std::string text;  // identifier, literal text (strings unescaped), or the punctuation char
  int line = 0;
};

class Lexer {
 public:
  Lexer(const std::string& text, const std::string& file) : text_(text), file_(file) {}

  bool run(std::vector<Token>& out, std::string& error) {
    while (true) {
      skip_ws();
      if (pos_ >= text_.size()) {
        out.push_back(Token{Token::Kind::End, "", line_});
        return true;
      }
      const char c = text_[pos_];
      if (c == '/' && peek(1) == '/') {
        if (peek(2) == '/') {
          pos_ += 3;
          if (pos_ < text_.size() && text_[pos_] == ' ') ++pos_;
          const size_t start = pos_;
          while (pos_ < text_.size() && text_[pos_] != '\n')
            ++pos_;
          out.push_back(Token{Token::Kind::Doc, text_.substr(start, pos_ - start), line_});
        } else {
          while (pos_ < text_.size() && text_[pos_] != '\n')
            ++pos_;
        }
        continue;
      }
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        const size_t start = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
                                       text_[pos_] == '_' || text_[pos_] == '.')) {
          ++pos_;
        }
        out.push_back(Token{Token::Kind::Ident, text_.substr(start, pos_ - start), line_});
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(c)) ||
          (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
        const size_t start = pos_;
        if (c == '-') ++pos_;
        bool is_float = false;
        if (text_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
          pos_ += 2;
          while (pos_ < text_.size() && std::isxdigit(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        } else {
          while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
          if (pos_ < text_.size() && text_[pos_] == '.' &&
              std::isdigit(static_cast<unsigned char>(peek(1)))) {
            is_float = true;
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
              ++pos_;
          }
          if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
              ++pos_;
          }
        }
        out.push_back(Token{is_float ? Token::Kind::Float : Token::Kind::Int,
                            text_.substr(start, pos_ - start), line_});
        continue;
      }
      if (c == '"') {
        ++pos_;
        std::string s;
        while (true) {
          if (pos_ >= text_.size() || text_[pos_] == '\n') {
            error = file_ + ":" + std::to_string(line_) + ": unterminated string";
            return false;
          }
          const char d = text_[pos_++];
          if (d == '"') break;
          if (d == '\\') {
            if (pos_ >= text_.size()) break;
            const char e = text_[pos_++];
            switch (e) {
              case 'n': s.push_back('\n'); break;
              case 't': s.push_back('\t'); break;
              case '"': s.push_back('"'); break;
              case '\\': s.push_back('\\'); break;
              default:
                error = file_ + ":" + std::to_string(line_) + ": invalid escape in string";
                return false;
            }
          } else {
            s.push_back(d);
          }
        }
        out.push_back(Token{Token::Kind::String, s, line_});
        continue;
      }
      if (std::string("{}:=,<>[]?@()").find(c) != std::string::npos) {
        out.push_back(Token{Token::Kind::Punct, std::string(1, c), line_});
        ++pos_;
        continue;
      }
      error = file_ + ":" + std::to_string(line_) + ": unexpected character '" + std::string(1, c) +
              "'";
      return false;
    }
  }

 private:
  char peek(size_t ahead) const { return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0'; }
  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\n') {
        ++line_;
        ++pos_;
      } else if (c == ' ' || c == '\t' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  const std::string& text_;
  const std::string& file_;
  size_t pos_ = 0;
  int line_ = 1;
};

class Parser {
 public:
  Parser(std::vector<Token> tokens, const std::string& file, SchemaFile& out)
      : tokens_(std::move(tokens)), file_(file), out_(out) {}

  bool run(std::string& error) {
    while (!at(Token::Kind::End)) {
      std::string doc = take_docs();
      if (is_ident("namespace")) {
        advance();
        if (!at(Token::Kind::Ident)) return fail("expected a namespace name", error);
        out_.ns = advance().text;
        continue;
      }
      if (is_ident("import")) {
        advance();
        if (!at(Token::Kind::String))
          return fail("expected a quoted file name after import", error);
        std::string imp = advance().text;
        const size_t slash = imp.find_last_of("/\\");
        if (slash != std::string::npos) imp = imp.substr(slash + 1);
        const size_t dot = imp.rfind('.');
        if (dot != std::string::npos) imp = imp.substr(0, dot);
        out_.imports.push_back(imp);
        continue;
      }
      if (is_ident("enum")) {
        if (!parse_enum(doc, error)) return false;
        continue;
      }
      if (is_ident("struct")) {
        if (!parse_struct(doc, error)) return false;
        continue;
      }
      if (is_ident("materialize")) {
        if (!parse_materialize(doc, error)) return false;
        continue;
      }
      return fail("expected namespace, import, enum, struct, or materialize", error);
    }
    if (out_.ns.empty()) {
      error = file_ + ":1: file declares no namespace";
      return false;
    }
    return true;
  }

 private:
  const Token& cur() const { return tokens_[pos_]; }
  bool at(Token::Kind k) const { return cur().kind == k; }
  bool at_punct(char c) const { return cur().kind == Token::Kind::Punct && cur().text[0] == c; }
  bool is_ident(const char* s) const { return cur().kind == Token::Kind::Ident && cur().text == s; }
  const Token& advance() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }
  SourceLoc loc() const { return SourceLoc{file_, cur().line}; }

  bool fail(const std::string& message, std::string& error) const {
    error = file_ + ":" + std::to_string(cur().line) + ": " + message;
    return false;
  }

  bool expect_punct(char c, std::string& error) {
    if (!at_punct(c)) return fail(std::string("expected '") + c + "'", error);
    advance();
    return true;
  }

  std::string take_docs() {
    std::string doc;
    while (at(Token::Kind::Doc)) {
      if (!doc.empty()) doc.push_back('\n');
      doc += advance().text;
    }
    return doc;
  }

  bool parse_attrs(std::vector<Attribute>& attrs, std::string& error) {
    while (at_punct('@')) {
      Attribute a;
      a.loc = loc();
      advance();
      if (!at(Token::Kind::Ident)) return fail("expected an attribute name after '@'", error);
      a.name = advance().text;
      if (at_punct('(')) {
        advance();
        while (!at_punct(')')) {
          if (at(Token::Kind::Int) || at(Token::Kind::Float) || at(Token::Kind::String) ||
              at(Token::Kind::Ident)) {
            a.args.push_back(advance().text);
          } else {
            return fail("expected an attribute argument", error);
          }
          if (at_punct(',')) advance();
        }
        advance();  // ')'
      }
      attrs.push_back(std::move(a));
    }
    return true;
  }

  bool parse_type(TypeExpr& type, std::string& error) {
    if (is_ident("map")) {
      advance();
      if (!expect_punct('<', error)) return false;
      type.kind = TypeExpr::Kind::Map;
      type.key = std::make_shared<TypeExpr>();
      type.element = std::make_shared<TypeExpr>();
      if (!parse_type(*type.key, error)) return false;
      if (!expect_punct(',', error)) return false;
      if (!parse_type(*type.element, error)) return false;
      if (!expect_punct('>', error)) return false;
    } else {
      if (!at(Token::Kind::Ident)) return fail("expected a type name", error);
      type.name = advance().text;
      type.kind = is_primitive_name(type.name) ? TypeExpr::Kind::Primitive : TypeExpr::Kind::Named;
    }
    // Postfix modifiers wrap the type just parsed.
    while (at_punct('?') || at_punct('[')) {
      TypeExpr wrapped = std::move(type);
      type = TypeExpr{};
      type.element = std::make_shared<TypeExpr>(std::move(wrapped));
      if (at_punct('?')) {
        advance();
        type.kind = TypeExpr::Kind::Optional;
      } else {
        advance();  // '['
        if (at(Token::Kind::Int)) {
          type.kind = TypeExpr::Kind::FixedArray;
          type.count = std::strtoull(advance().text.c_str(), nullptr, 0);
          if (type.count == 0) return fail("fixed array length must be positive", error);
        } else {
          type.kind = TypeExpr::Kind::Array;
        }
        if (!expect_punct(']', error)) return false;
      }
    }
    return true;
  }

  bool parse_default(Field& f, std::string& error) {
    if (at(Token::Kind::Int)) {
      f.default_kind = DefaultKind::Int;
      f.default_text = advance().text;
    } else if (at(Token::Kind::Float)) {
      f.default_kind = DefaultKind::Float;
      f.default_text = advance().text;
    } else if (at(Token::Kind::String)) {
      f.default_kind = DefaultKind::String;
      f.default_text = advance().text;
    } else if (is_ident("true") || is_ident("false")) {
      f.default_kind = DefaultKind::Bool;
      f.default_text = advance().text;
    } else if (is_ident("null")) {
      f.default_kind = DefaultKind::Null;
      advance();
    } else if (at(Token::Kind::Ident)) {
      f.default_kind = DefaultKind::Ident;
      f.default_text = advance().text;
    } else if (at_punct('[')) {
      advance();
      if (!expect_punct(']', error)) return false;
      f.default_kind = DefaultKind::EmptyArray;
    } else {
      return fail("expected a default value", error);
    }
    return true;
  }

  bool parse_enum(const std::string& doc, std::string& error) {
    EnumDecl e;
    e.doc = doc;
    e.loc = loc();
    advance();  // 'enum'
    if (!at(Token::Kind::Ident)) return fail("expected an enum name", error);
    e.name = advance().text;
    if (at_punct(':')) {
      advance();
      if (!at(Token::Kind::Ident)) return fail("expected an underlying integer type", error);
      e.underlying = advance().text;
    }
    if (!parse_attrs(e.attrs, error)) return false;
    if (!expect_punct('{', error)) return false;
    int64_t next_value = 0;
    while (!at_punct('}')) {
      EnumValue v;
      v.doc = take_docs();
      v.loc = loc();
      if (!at(Token::Kind::Ident)) return fail("expected an enumerator name", error);
      v.name = advance().text;
      if (at_punct('=')) {
        advance();
        if (!at(Token::Kind::Int)) return fail("expected an integer enumerator value", error);
        v.value = std::strtoll(advance().text.c_str(), nullptr, 0);
      } else {
        v.value = next_value;
      }
      next_value = v.value + 1;
      std::vector<Attribute> value_attrs;
      if (!parse_attrs(value_attrs, error)) return false;
      for (const Attribute& a : value_attrs) {
        if (a.name == "doc" && !a.args.empty()) v.doc = a.args[0];
      }
      if (at_punct(',')) advance();
      e.values.push_back(std::move(v));
    }
    advance();  // '}'
    for (const Attribute& a : e.attrs) {
      if (a.name == "doc" && !a.args.empty()) e.doc = a.args[0];
      if (a.name == "kind" && !a.args.empty()) e.tag = a.args[0];
    }
    out_.enums.push_back(std::move(e));
    return true;
  }

  bool parse_struct(const std::string& doc, std::string& error) {
    StructDecl s;
    s.doc = doc;
    s.loc = loc();
    advance();  // 'struct'
    if (!at(Token::Kind::Ident)) return fail("expected a struct name", error);
    s.name = advance().text;
    if (!parse_attrs(s.attrs, error)) return false;
    for (const Attribute& a : s.attrs) {
      if (a.name == "version" && !a.args.empty())
        s.version = static_cast<uint16_t>(std::strtoul(a.args[0].c_str(), nullptr, 0));
      if (a.name == "doc" && !a.args.empty()) s.doc = a.args[0];
      if (a.name == "kind" && !a.args.empty()) s.tag = a.args[0];
      if (a.name == "transient") s.transient = true;
    }
    if (!expect_punct('{', error)) return false;
    while (!at_punct('}')) {
      Field f;
      f.doc = take_docs();
      f.loc = loc();
      if (!at(Token::Kind::Ident)) return fail("expected a field name", error);
      f.name = advance().text;
      if (!expect_punct(':', error)) return false;
      if (!parse_type(f.type, error)) return false;
      if (at_punct('=')) {
        advance();
        if (!parse_default(f, error)) return false;
      }
      if (!parse_attrs(f.attrs, error)) return false;
      for (const Attribute& a : f.attrs) {
        if (a.name == "since" && !a.args.empty())
          f.since = static_cast<uint16_t>(std::strtoul(a.args[0].c_str(), nullptr, 0));
        if (a.name == "transient") f.transient = true;
        if (a.name == "deprecated") f.deprecated = true;
        if (a.name == "doc" && !a.args.empty()) f.doc = a.args[0];
        if (a.name == "unit" && !a.args.empty()) f.unit = a.args[0];
      }
      if (at_punct(',')) advance();
      s.fields.push_back(std::move(f));
    }
    advance();  // '}'
    out_.structs.push_back(std::move(s));
    return true;
  }

  // materialize <RecordType> [@tiers(...)] [@doc("...")] {
  //   Component.field = property [@writeback]
  //   Component
  //   parent = ChildOf | none
  // }
  //
  // The lexer reads `Component.field` as one identifier (a dotted name is one token everywhere in
  // the language), so the left side splits at its last dot: `engine.world.Transform.position` is
  // the qualified component `engine.world.Transform` and the field `position`. Names are resolved
  // and every rule is checked in resolve(); this only builds the rows.
  bool parse_materialize(const std::string& doc, std::string& error) {
    MaterializeDecl m;
    m.doc = doc;
    m.loc = loc();
    advance();  // 'materialize'
    if (!at(Token::Kind::Ident))
      return fail("expected a record type name after materialize", error);
    m.record = advance().text;
    if (!parse_attrs(m.attrs, error)) return false;
    for (const Attribute& a : m.attrs) {
      if (a.name == "doc" && !a.args.empty()) m.doc = a.args[0];
    }
    if (!expect_punct('{', error)) return false;
    while (!at_punct('}')) {
      take_docs();  // a row's own `///` comment documents the source, not the table
      if (at_punct('}')) break;
      if (at(Token::Kind::End)) return fail("unterminated materialize block", error);
      MaterializeRow row;
      row.loc = loc();
      if (!at(Token::Kind::Ident)) {
        return fail("expected 'Component.field = property', a component, or 'parent = ChildOf'",
                    error);
      }
      const std::string lhs = advance().text;
      if (at_punct('=')) {
        advance();
        if (!at(Token::Kind::Ident)) return fail("expected a name after '='", error);
        const std::string rhs = advance().text;
        if (lhs == "parent") {
          row.kind = MaterializeRow::Kind::Parent;
          row.property = rhs;
        } else {
          const size_t dot = lhs.rfind('.');
          if (dot == std::string::npos || dot == 0 || dot + 1 == lhs.size()) {
            error = file_ + ":" + std::to_string(row.loc.line) +
                    ": expected Component.field on "
                    "the left of '=', got '" +
                    lhs + "'";
            return false;
          }
          row.kind = MaterializeRow::Kind::Field;
          row.component = lhs.substr(0, dot);
          row.field = lhs.substr(dot + 1);
          row.property = rhs;
        }
      } else {
        row.kind = MaterializeRow::Kind::Component;
        row.component = lhs;
      }
      std::vector<Attribute> attrs;
      if (!parse_attrs(attrs, error)) return false;
      for (const Attribute& a : attrs) {
        if (a.name == "writeback" && row.kind == MaterializeRow::Kind::Field) {
          row.writeback = true;
          continue;
        }
        error = a.loc.file + ":" + std::to_string(a.loc.line) + ": '@" + a.name +
                "' is not an attribute of this materialize row (a field row takes @writeback)";
        return false;
      }
      if (at_punct(',')) advance();
      m.rows.push_back(std::move(row));
    }
    advance();  // '}'
    out_.materializations.push_back(std::move(m));
    return true;
  }

  std::vector<Token> tokens_;
  const std::string& file_;
  SchemaFile& out_;
  size_t pos_ = 0;
};

}  // namespace

bool is_primitive_name(const std::string& name) {
  for (const char* p : k_primitives) {
    if (name == p) return true;
  }
  return false;
}

bool is_integer_primitive(const std::string& name) {
  for (const char* p : k_integers) {
    if (name == p) return true;
  }
  return false;
}

bool parse_schema(const std::string& path, const std::string& text, SchemaFile& out,
                  std::string& error) {
  out.path = path;
  std::string stem = path;
  const size_t slash = stem.find_last_of("/\\");
  if (slash != std::string::npos) stem = stem.substr(slash + 1);
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos) stem = stem.substr(0, dot);
  out.stem = stem;

  std::vector<Token> tokens;
  Lexer lexer(text, path);
  if (!lexer.run(tokens, error)) return false;
  Parser parser(std::move(tokens), path, out);
  return parser.run(error);
}

}  // namespace schemac
