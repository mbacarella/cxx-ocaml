(* S570 control: an unused pattern variable was already right *)
type t = { u : int; v : string }
let { u; v } = { u = 1; v = "s" }
