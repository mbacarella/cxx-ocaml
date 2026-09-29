module type S = sig
  module type T = functor (S : sig end ) -> sig end
 end
let x = (module struct
    module type T = functor (S : sig end ) -> sig end
  end : S)
