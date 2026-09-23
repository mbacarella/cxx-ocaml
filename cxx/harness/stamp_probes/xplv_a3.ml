(* S570: a constructor pattern's variable, same law *)
type t = A of int
let (A a) = A 42
let () = print_int a
