module type S = sig
  open Set.Make(Bool)
  type u = (t, int) Hashtbl.t
end
let x = 1
