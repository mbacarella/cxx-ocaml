(* S570: an alias binder is a pattern variable too -- `a` keeps the
   constructor argument's object, `z` the pattern's own *)
type t = A of int
let (A a as z) = A 42
let () = print_int a; (match z with A n -> print_int n)
