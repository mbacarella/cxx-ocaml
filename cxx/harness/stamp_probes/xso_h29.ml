module type S = sig
  open Set.Make(Bool)
  module type T = sig type u = t end
end
let x = 1
