(** Check that rebinding module preserves private type aliases *)
module String_id : sig module type S = sig type t = private string
val of_string : string -> t end include S
module Make (M : sig val module_name : string end) : S end = struct
module type S = sig type t = private string val of_string : string -> t end
module String = struct type t = string end
module Make (M : sig val module_name : string end) = struct include String
let of_string s = Printf.printf "converting %s\n" M.module_name; s end
include Make (struct let module_name = "String_id" end) end
let () = let module Bar = String_id.Make(struct let module_name="Bar" end) in
let bar = Bar.of_string "bar" in Printf.printf "bar = %s " (bar :> string)
