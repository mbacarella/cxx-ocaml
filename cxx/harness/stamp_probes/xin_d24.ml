module String_id = struct module type S = sig type t = private string
val of_string : string -> t end module String = struct type t = string end
module Make (M : sig val module_name : string end) = struct include String
let of_string s = s end
include Make (struct let module_name = "String_id" end) end
let () = let foo = String_id.of_string "foo" in Printf.printf "foo = %s
" (foo :> string)
