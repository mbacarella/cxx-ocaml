module type S = sig
  include sig open Set.Make(Bool) type u = t end
end
let x = 1
