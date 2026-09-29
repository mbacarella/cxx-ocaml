module type S = sig
  module type T
  module X : T
end
module F (X : S) = X.X
module M = struct
  module type T = sig type t module Y : sig type z end end
  module X = struct type t = int module Y = struct type z end end
end
