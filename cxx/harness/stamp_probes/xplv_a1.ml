(* S570: a record pattern's variable is a binding of its own, so its `int`
   is generic and `print_int a` links a COPY, not the saved node *)
type t = { a : int }
let { a } = { a = 42 }
let () = print_int a
