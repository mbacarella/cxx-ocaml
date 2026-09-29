module type S = sig
  type u = int Hashtbl.Make(Bool).t
end
let x = 1
