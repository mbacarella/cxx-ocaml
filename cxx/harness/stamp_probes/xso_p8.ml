module M = struct
  module type S = sig
    open Set.Make(Bool)
    type u = t
  end
end
let x = 1
