module type S = sig
  open Set.Make(Bool)
  module M : sig type u = t end
end
let x = 1
