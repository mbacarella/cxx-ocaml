module type S = sig
  module type T
  module X : T
end
module F (X : S) = X.X
module M = struct
  module type T = sig type t end
  module X = struct type t = int end
end
module N = F(M)
type t = N.t
