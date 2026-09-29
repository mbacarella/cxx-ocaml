module type S = sig
  open Set.Make(Bool)
  module M : sig open Set.Make(Int) type u = t end
  type v = t
end
let x = 1
