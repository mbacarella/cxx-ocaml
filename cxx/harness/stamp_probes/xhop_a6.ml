module type S = sig type t end
module M = struct
  module type U = functor (X : S) (Y : S) -> sig type t = X.t * Y.t end
end
