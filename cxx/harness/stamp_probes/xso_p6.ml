module type S = sig
  module M : sig
    open Set.Make(Bool)
    type u = t
  end
end
let x = 1
