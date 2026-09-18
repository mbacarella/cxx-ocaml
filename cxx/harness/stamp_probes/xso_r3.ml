module type S = sig
  open Hashtbl.Make(Bool)
  type u = int t
end
let x = 1
