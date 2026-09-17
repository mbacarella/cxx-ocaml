module type S = sig
  module F : functor (S : sig end) (T : sig end) -> sig end
 end
let x = (module struct
    module F (S : sig end) (T : sig end) = struct end
  end : S)
