open Set.Make(Int)
let x = 1
module type S = sig
  open Set.Make(Bool)
end
