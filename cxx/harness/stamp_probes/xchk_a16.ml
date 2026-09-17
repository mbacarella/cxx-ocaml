module type S = sig
  module F : functor (S : sig end) -> sig type t end
 end
let x = (module struct
    module F (S : sig end) = struct type t type u end
  end : S)
