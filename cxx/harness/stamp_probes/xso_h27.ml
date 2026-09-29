module type S = sig
  open Set.Make(Bool)
  type u = t
  open Set.Make(Int)
end
let x = 1
