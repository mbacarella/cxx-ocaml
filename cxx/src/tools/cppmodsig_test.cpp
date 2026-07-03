// Unit tests for modsig::compute_coercion / trusted (P2 stage 1).  Standalone,
// header-only: modsig.hpp depends only on the standard library, so this links
// against nothing (no cppcaml_core -> no static-lib ar gotcha).
#include <cstdio>

#include "cppcaml/modsig.hpp"

using namespace cppcaml::modsig;
using From = Coercion::Field::From;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

// Build a numbered Sig from items (push applies shadowing, number assigns pos).
static SigPtr make(std::vector<Item> items) {
  auto s = std::make_shared<Sig>();
  for (auto& it : items) s->push(std::move(it));
  s->number();
  return s;
}
static Item val(std::string n) { return {.ns = NS::Value, .name = std::move(n)}; }
static Item cls(std::string n) { return {.ns = NS::Class, .name = std::move(n)}; }
static Item unk(std::string n) { return {.ns = NS::Unknown, .name = std::move(n)}; }
static Item prim(std::string n, std::string p, int ar) {
  return {.ns = NS::Value, .name = std::move(n), .runtime = false,
          .is_prim = true, .prim = std::move(p), .prim_arity = ar};
}
static Item mod(std::string n, SigPtr sub) {
  return {.ns = NS::Module, .name = std::move(n), .sub = std::move(sub)};
}
static Item alias(std::string n) {  // elided module alias: no runtime slot
  return {.ns = NS::Module, .name = std::move(n), .runtime = false};
}

int main() {
  {  // identity: same members, same order
    auto c = compute_coercion(*make({val("a"), val("b")}),
                              *make({val("a"), val("b")}));
    check(c.ok && c.identity && c.fields.size() == 2, "identity");
  }
  {  // reorder: fields map to swapped source positions, NOT identity
    auto c = compute_coercion(*make({val("a"), val("b")}),
                              *make({val("b"), val("a")}));
    check(c.ok && !c.identity && c.fields[0].src_pos == 1 &&
              c.fields[1].src_pos == 0,
          "reorder");
  }
  {  // narrowing: drop a member; src longer => not identity even though in order
    auto c = compute_coercion(*make({val("a"), val("b"), val("c")}),
                              *make({val("a"), val("c")}));
    check(c.ok && !c.identity && c.fields.size() == 2 &&
              c.fields[0].src_pos == 0 && c.fields[1].src_pos == 2,
          "narrowing");
  }
  {  // PR#5098: fields are 0,1.. with no subs but src has an EXTRA field
    auto c = compute_coercion(*make({val("a"), val("b")}),
                              *make({val("a")}));
    check(c.ok && !c.identity && c.fields.size() == 1 &&
              c.fields[0].src_pos == 0,
          "PR#5098 in-order-but-longer is not identity");
  }
  {  // prim -> val: src external (no slot) exposed as a value => eta-stub
    auto c = compute_coercion(*make({prim("f", "%addint", 2)}),
                              *make({val("f")}));
    check(c.ok && c.fields.size() == 1 &&
              c.fields[0].from == From::PrimStub &&
              c.fields[0].prim == "%addint" && c.fields[0].prim_arity == 2,
          "prim->val eta-stub");
  }
  {  // alias materialization: src elided alias exposed as a runtime module
    auto c = compute_coercion(*make({alias("E")}), *make({mod("E", nullptr)}));
    check(c.ok && c.fields.size() == 1 &&
              c.fields[0].from == From::AliasValue,
          "alias materialization");
  }
  {  // nested submodule reorder: sub coercion is non-identity, attached
    auto src = make({mod("M", make({val("x"), val("y")}))});
    auto tgt = make({mod("M", make({val("y"), val("x")}))});
    auto c = compute_coercion(*src, *tgt);
    check(c.ok && c.fields.size() == 1 && c.fields[0].sub &&
              c.fields[0].sub->fields[0].src_pos == 1 &&
              c.fields[0].sub->fields[1].src_pos == 0,
          "nested submodule reorder");
  }
  {  // nested identity sub is DROPPED and the whole thing is identity
    auto src = make({mod("M", make({val("x")}))});
    auto tgt = make({mod("M", make({val("x")}))});
    auto c = compute_coercion(*src, *tgt);
    check(c.ok && c.identity && c.fields.size() == 1 && !c.fields[0].sub,
          "nested identity sub dropped");
  }
  {  // cross-ns same name: src has `class c` AND `let c`; tgt wants the VALUE c.
    // Must pick the value's field (pos 1), never the class's (pos 0).
    auto src = make({cls("c"), val("c")});  // class pos 0, value pos 1
    auto tgt = make({val("c")});
    auto c = compute_coercion(*src, *tgt);
    check(c.ok && c.fields.size() == 1 && c.fields[0].from == From::SrcField &&
              c.fields[0].src_pos == 1 && c.unknown_pairings == 0,
          "cross-ns same name picks value by namespace");
  }
  {  // and symmetrically, tgt wanting the CLASS c picks pos 0
    auto src = make({cls("c"), val("c")});
    auto tgt = make({cls("c")});
    auto c = compute_coercion(*src, *tgt);
    check(c.ok && c.fields.size() == 1 && c.fields[0].src_pos == 0,
          "cross-ns same name picks class by namespace");
  }
  {  // Unknown-side pairing (flat splice): pairs by name alone, counted
    auto c = compute_coercion(*make({unk("a"), unk("b")}),
                              *make({val("b"), val("a")}));
    check(c.ok && c.fields[0].src_pos == 1 && c.fields[1].src_pos == 0 &&
              c.unknown_pairings == 2,
          "Unknown-side pairing by name");
  }
  {  // absent member => not ok, error names it
    auto c = compute_coercion(*make({val("a")}), *make({val("a"), val("z")}));
    check(!c.ok && c.error == "z", "absent member => ok=false with name");
  }
  {  // absent NESTED member => dotted error path
    auto src = make({mod("M", make({val("x")}))});
    auto tgt = make({mod("M", make({val("y")}))});
    auto c = compute_coercion(*src, *tgt);
    check(!c.ok && c.error == "M.y", "absent nested member => dotted path");
  }
  {  // non-runtime target items (types) take no field and skip presence checks
    auto tgt = make({{.ns = NS::Type, .name = "t", .runtime = false}, val("a")});
    auto c = compute_coercion(*make({val("a")}), *tgt);
    check(c.ok && c.identity && c.fields.size() == 1, "non-runtime tgt skipped");
  }
  {  // trusted(): all-known => true; any Unknown (even nested) => false
    check(trusted(*make({val("a"), mod("M", make({val("x")}))})),
          "trusted: fully namespaced");
    check(!trusted(*make({val("a"), unk("b")})),
          "trusted: Unknown item => false");
    check(!trusted(*make({mod("M", make({unk("x")}))})),
          "trusted: nested Unknown => false");
  }

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
