module B = Bool
module type S = sig
  open Set.Make(B)
  type u = t
end
let x = 1
