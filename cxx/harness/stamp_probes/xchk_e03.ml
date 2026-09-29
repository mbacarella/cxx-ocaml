module type S = sig
  module M : sig val s : unit end
  module F : functor (S : sig end ) -> sig type t end
 end
let x = (module struct
    module M = struct let s = () end
    module F (_ : sig end) = struct type t end
  end : S)
module X = (val x)
module Y = X.M
module Z = Y
let _ = Z.s
