module type S = sig
  module F : functor (S : sig type t end) -> sig end
 end
module X = (struct
    module F (S : sig type t end) = struct end
  end : S)
