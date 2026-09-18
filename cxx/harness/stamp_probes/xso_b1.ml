module type S = sig
  val v : int Hashtbl.Make(Bool).t
end
let x = 1
