module type S = sig
  open Set.Make(Bool)
  open Int
  type u = t
end
let x = 1
