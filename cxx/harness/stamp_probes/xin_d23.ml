module String_id : sig module type S = sig type t = private string
val of_string : string -> t end include S
module Make (M : sig val module_name : string end) : S end = struct
module type S = sig type t = private string val of_string : string -> t end
module Make (M : sig val module_name : string end) = struct type t = string
let of_string s = s end
include Make (struct let module_name = "String_id" end) end
let () = let foo = String_id.of_string "foo" in Printf.printf "foo = %s
" (foo :> string)
