module type S = sig
  open Hashtbl.Make(Int)
end
let x = 1
