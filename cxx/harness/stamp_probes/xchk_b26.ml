module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
let f () = let module X = (struct
    module F (_ : sig end) = struct end
  end : S) in ()
