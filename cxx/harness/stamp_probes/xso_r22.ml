module type S = sig
  open Set.Make(Bool)
  module M : sig open Set.Make(Int) end
  type v = t
end
let x = 1
