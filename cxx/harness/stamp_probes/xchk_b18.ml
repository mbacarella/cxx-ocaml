module type S = sig
  module F : functor (S : sig type t end) -> sig end
 end
module X : S = struct
    module F (S : sig type t end) = struct end
  end
