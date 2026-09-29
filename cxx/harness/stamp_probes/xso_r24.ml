module type S = sig
  open Set.Make(Bool)
  type u = t list * t
end
let x = 1
