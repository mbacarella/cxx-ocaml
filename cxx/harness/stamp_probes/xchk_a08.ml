module type S = sig
  module F : functor (S : sig end) -> sig end
 end
let x = (module struct
    module F (S : sig end) = struct end
  end : S)
