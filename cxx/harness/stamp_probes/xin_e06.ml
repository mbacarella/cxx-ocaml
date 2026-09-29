module type S = sig type t = private string val of_string : string -> t end
module Make (M : sig val module_name : string end) : S = struct
type t = string let of_string s = s end
let () = let module Bar = Make(struct let module_name="Bar" end) in
let bar = Bar.of_string "bar" in ignore (bar :> string)
