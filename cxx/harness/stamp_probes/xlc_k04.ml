module type S = sig
  module X : sig type t end
end
module F (X : S) = X.X
module M = struct
  module X = struct type t = int end
end
module N = F(M)
