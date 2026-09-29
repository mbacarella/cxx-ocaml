module type S = sig
  module F : functor (S : sig end ) -> sig type t end
 end
let f () = (module struct
    module F (S : sig end) = struct type t end
  end : S)
