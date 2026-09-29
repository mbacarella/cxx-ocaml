module type S = sig
  open Set.Make(Bool)
  type u = A of t | B
end
let x = 1
