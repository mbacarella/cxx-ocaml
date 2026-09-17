module type S = sig
  module F : functor (S : sig end ) -> sig type t end
 end
let x = (module struct
    module F (_ : sig end) = struct type t end
  end : S)
