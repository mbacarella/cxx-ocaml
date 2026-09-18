module type S = sig
  open Set.Make(Bool)
  open Set.Make(Int)
end
let x = 1
