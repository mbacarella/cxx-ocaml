module type S = sig
  module type T = sig
    open Set.Make(Bool)
    type u = t
  end
end
let x = 1
