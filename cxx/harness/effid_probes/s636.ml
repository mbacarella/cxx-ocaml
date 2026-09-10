(* S421 PIN (burn ledger 3a -- a LIVE MISCOMPILE, not a byte difference).
   Writing the include-strengthened alias Mp_absent gives it no runtime field,
   so this functor argument's coercion selected field 0 (the submodule M)
   instead of field 1 (Set), and M.Set.cardinal read out of the wrong block --
   it printed a pointer-shaped integer.  testsuite/tests/basic-modules/main.ml
   is the upstream case.  NOINCALIASPRESENT=1 restores the wrong answer. *)
module F (M : sig
            type t
            module Set : Set.S with type elt = t
          end) =
struct
  let test set = Printf.printf "%d\n" (M.Set.cardinal set)
end

module M = F (S636n)

let () = M.test (S636n.M.Set.singleton "42")
let () = M.test (S636n.M.Set.of_list [ "a"; "b"; "a" ])
