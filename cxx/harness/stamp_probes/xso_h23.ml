module type S = sig
  open Set.Make(Bool)
  type u = A : t -> u
end
let x = 1
