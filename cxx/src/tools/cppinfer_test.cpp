// Unit tests for the HM inference core (Slice 1).  Standalone, no corpus.
#include <cstdio>

#include "cppcaml/infer.hpp"

using namespace cppcaml::infer;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}
static bool throws(Engine& e, const TypePtr& a, const TypePtr& b) {
  try { e.unify(a, b); return false; } catch (const TypeError&) { return true; }
}

int main() {
  {  // var unifies with a concrete type
    Engine e;
    auto v = e.fresh_var();
    auto i = e.constr("int");
    e.unify(v, i);
    check(show(v) == "int", "unify('a, int) => int");
  }
  {  // arrow unification propagates to vars
    Engine e;
    auto a = e.fresh_var(), b = e.fresh_var();
    e.unify(e.arrow(a, b), e.arrow(e.constr("int"), e.constr("bool")));
    check(show(a) == "int" && show(b) == "bool", "unify('a->'b, int->bool)");
  }
  {  // occurs check rejects recursive binding
    Engine e;
    auto a = e.fresh_var(), b = e.fresh_var();
    check(throws(e, a, e.arrow(a, b)), "occurs check rejects 'a = 'a -> 'b");
  }
  {  // constructor clash
    Engine e;
    check(throws(e, e.constr("int"), e.constr("bool")), "int vs bool clashes");
  }
  {  // arity clash on a constructor
    Engine e;
    auto la = e.constr("list", {e.constr("int")});
    auto lb = e.constr("list", {e.constr("int"), e.constr("int")});
    check(throws(e, la, lb), "list arity clash");
  }
  {  // let-polymorphism: generalize then instantiate independently
    Engine e;
    e.enter_level();
    auto a = e.fresh_var();
    auto id = e.arrow(a, a);          // 'a -> 'a at an inner level
    e.leave_level();
    e.generalize(id);                  // => generic 'a -> 'a
    auto i1 = e.instantiate(id);
    auto i2 = e.instantiate(id);
    e.unify(i1, e.arrow(e.constr("int"), e.fresh_var()));
    e.unify(i2, e.arrow(e.constr("bool"), e.fresh_var()));
    // independent instances: i1 ~ int->int, i2 ~ bool->bool, no clash
    check(show(i1) == "int -> int" && show(i2) == "bool -> bool",
          "generalize/instantiate gives independent instances");
  }
  {  // a NON-generalized var is shared across instantiate (monomorphic)
    Engine e;
    auto a = e.fresh_var();            // level 0, never generalized
    auto f = e.arrow(a, a);
    auto i1 = e.instantiate(f);        // free var => shared, not copied
    e.unify(i1, e.arrow(e.constr("int"), e.fresh_var()));
    check(show(a) == "int", "free (non-generic) var stays monomorphic");
  }

  std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
