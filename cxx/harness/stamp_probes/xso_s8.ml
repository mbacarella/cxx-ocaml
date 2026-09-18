module type S = sig
  open Set.Make(Bool)
  open Hashtbl.Make(Bool)
  type u = int t
end
let x = 1
