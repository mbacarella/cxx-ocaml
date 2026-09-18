module type S = sig
  open Hashtbl.Make(Bool)
end
let x = 1
