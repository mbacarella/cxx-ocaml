module String = struct type t = string end
module Make (M : sig val module_name : string end) = struct include String
let of_string s = s end
include Make (struct let module_name = "String_id" end)
