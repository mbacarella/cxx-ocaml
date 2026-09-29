module type S = sig
  open Set.Make(Bool)
  type u = t * int
end
let x = 1
