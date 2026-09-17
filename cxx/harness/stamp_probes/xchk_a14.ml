module type S = sig
  module F : functor (S : sig type t end) -> sig end
 end
let x = (module struct
    module F (S : sig type t end) = struct end
  end : S)
