(* TEST *)

(* Marshaling (cf. PR#5436) *)

(* Note: this test must *not* be made a toplevel or expect-style test,
   because then the Obj.id counter of the compiler implementation
   (called by the bytecode read-eval-print loop) would be the same as
   the Obj.id counter of the test code below. In particular, any
   change to the compiler implementation to use more objects or
   exceptions would change the numbers below, making the test very
   fragile. *)

let r = ref 0;;
let id o = Oo.id o - !r;;
r := Oo.id (object end);;

assert (id (object end) = 1);;
assert (id (object end) = 2);;
let o = object end in
  let s = Marshal.to_string o [] in
  let o' : < > = Marshal.from_string s 0 in
  let o'' : < > = Marshal.from_string s 0 in
  assert ((id o, id o', id o'') = (3, 4, 5));
